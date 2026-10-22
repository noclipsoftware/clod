// Server: the background process that owns every tab (ConPTY children, emulators, scrollback),
// composes frames for whichever client window is attached, and shows the tray icon.
// It outlives client windows; it ends on an explicit quit, or when no tabs remain and nobody is attached.

// The command that starts claude, or empty when Claude Code isn't installed. Looked up on every
// tab start, so installing it while clod runs works on the next restart (Enter in the tab).
static std::wstring FindClaude()
{
    // CLOD_CLAUDE replaces the claude command, e.g. "cmd.exe /k echo" for tests that kill servers.
    wchar_t Override[1024] = {};
    if(GetEnvironmentVariableW(L"CLOD_CLAUDE", Override, ArrayCount(Override))) { return Override; }
    wchar_t Path[MAX_PATH] = {};
    if(SearchPathW(0, L"claude.exe", 0, MAX_PATH, Path, 0)) { return std::wstring(L"\"") + Path + L"\""; }
    if(SearchPathW(0, L"claude.cmd", 0, MAX_PATH, Path, 0)) { return L"cmd.exe /c claude"; } // npm install
    // The native installer's location. Checked directly because this server's PATH was inherited
    // when it started and doesn't see installs made since.
    wchar_t Home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
    std::wstring Native = std::wstring(Home) + L"\\.local\\bin\\claude.exe";
    if(GetFileAttributesW(Native.c_str()) != INVALID_FILE_ATTRIBUTES) { return L"\"" + Native + L"\""; }
    return std::wstring();
}

static std::string NewUuid()
{
    u32 R[4] = {};
    for(u32& V : R) { rand_s(&V); }
    R[1] = (R[1] & 0xFFFF0FFF) | 0x00004000; // version 4
    R[2] = (R[2] & 0x3FFFFFFF) | 0x80000000; // RFC 4122 variant
    char Buf[40];
    snprintf(Buf, sizeof(Buf), "%08x-%04x-%04x-%04x-%04x%08x", R[0], R[1] >> 16, R[1] & 0xFFFF, R[2] >> 16, R[2] & 0xFFFF, R[3]);
    return Buf;
}

// (Re)starts claude for a tab: resume when the session file exists, otherwise claim the tab's id.
static void StartTab(tab& Tab, s32 Cols, s32 Rows, HANDLE Job, HANDLE Wake, app_state& App, u64 NowMs)
{
    VtInit(Tab.Term, Cols, Rows);
    Tab.ScrollOffset = 0;
    Tab.SeenPushed = 0;
    Tab.Exited = false;
    Tab.ExitCode = 0;
    Tab.TitleBusy = false;

    // --- no Claude Code on this machine: say so in the tab instead of failing obscurely ---
    std::wstring Claude = FindClaude();
    if(Claude.empty())
    {
        static const char Message[] =
            "\r\n  \x1b[1mClaude Code is not installed\x1b[0m (no claude.exe or claude.cmd found).\r\n\r\n"
            "  Install it from \x1b[4mhttps://claude.com/claude-code\x1b[0m, for example in PowerShell:\r\n\r\n"
            "      irm https://claude.ai/install.ps1 | iex\r\n\r\n"
            "  then press Enter here to start this tab.\r\n";
        VtFeed(Tab.Term, (const u8*)Message, sizeof(Message) - 1);
        Tab.Term.CursorHidden = true;
        Tab.Exited = true;
        Tab.ExitCode = (u32)-1;
        return;
    }

    bool32 Resume = SessionFileExists(Tab.SessionId);
    std::wstring Cmd = Claude + (Resume ? L" --resume " : L" --session-id ") + Utf8ToWide(Tab.SessionId);

    std::wstring Cwd = Utf8ToWide(Tab.Cwd);
    if(Cwd.empty() || GetFileAttributesW(Cwd.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        App.Status = "Directory missing: " + Tab.Cwd + " (starting in " + App.LaunchCwd + ")";
        App.StatusUntil = NowMs + 5000;
        Cwd = Utf8ToWide(App.LaunchCwd);
    }

    // --- environment: drop the markers a parent Claude Code session leaves behind ---
    // Inheriting them makes the child think it's a subagent (e.g. it stops saving its transcript).
    static const wchar_t* Strip[] = {
        L"CLAUDECODE", L"CLAUDE_CODE_CHILD_SESSION", L"CLAUDE_CODE_SESSION_ID", L"CLAUDE_CODE_SESSION_ATTENDED",
        L"CLAUDE_CODE_ENTRYPOINT", L"CLAUDE_CODE_EXECPATH", L"CLAUDE_CODE_MESSAGING_SOCKET",
        L"CLAUDE_CODE_MESSAGING_TOKEN", L"CLAUDE_PID", L"AI_AGENT",
    };
    std::wstring Env;
    wchar_t* Block = GetEnvironmentStringsW();
    for(const wchar_t* Var = Block; *Var; Var += wcslen(Var) + 1)
    {
        const wchar_t* Eq = wcschr(Var + 1, L'='); // +1: skips the leading '=' of "=C:=C:\..." entries
        size_t NameLen = Eq ? (size_t)(Eq - Var) : wcslen(Var);
        bool32 Drop = false;
        for(const wchar_t* S : Strip) { if(wcslen(S) == NameLen && _wcsnicmp(Var, S, NameLen) == 0) { Drop = true; } }
        if(!Drop) { Env.append(Var); Env.push_back(0); }
    }
    FreeEnvironmentStringsW(Block);
    Env.push_back(0);

    std::string Error;
    Tab.Pty = PtyStart(Cmd, Cwd, Env.c_str(), Cols, Rows, Job, Wake, Error);
    if(!Tab.Pty)
    {
        Tab.Exited = true;
        Tab.ExitCode = (u32)-1;
        App.Status = Error;
        App.StatusUntil = NowMs + 5000;
    }
}

static tab* OpenTab(app_state& App, const std::string& SessionId, const std::string& Cwd, const std::string& Project, const std::string& Title,
                    HANDLE Job, HANDLE Wake, u64 NowMs)
{
    tab* T = new tab();
    T->SessionId = SessionId.empty() ? NewUuid() : SessionId;
    T->Cwd = Cwd;
    T->Project = Project;
    T->Title = Title;
    StartTab(*T, App.Width, ContentRows(App.Height), Job, Wake, App, NowMs);
    App.Tabs.push_back(T);
    return T;
}

// A path typed in the folder prompt, made absolute: drops quotes, expands ~, resolves relative
// paths against Base and folds . and .. away. A trailing separator stays.
static std::wstring ResolveTypedPath(const std::string& Typed, const std::string& Base)
{
    std::wstring P;
    for(wchar_t C : Utf8ToWide(Typed)) { if(C != L'"') { P += C; } }
    while(!P.empty() && P.back() == L' ') { P.pop_back(); }
    while(!P.empty() && P[0] == L' ') { P.erase(0, 1); }
    if(!P.empty() && P[0] == L'~' && (P.size() == 1 || P[1] == L'\\' || P[1] == L'/'))
    {
        wchar_t Home[MAX_PATH] = {};
        GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
        P = Home + P.substr(1);
    }
    bool32 Absolute = (P.size() >= 2 && P[1] == L':') || (!P.empty() && (P[0] == L'\\' || P[0] == L'/'));
    if(!Absolute) { P = Utf8ToWide(Base) + L"\\" + P; }
    wchar_t Full[4096] = {};
    DWORD Len = GetFullPathNameW(P.c_str(), (DWORD)ArrayCount(Full), Full, 0);
    return (Len > 0 && Len < ArrayCount(Full)) ? std::wstring(Full, Len) : P;
}

// --- open tabs on disk, so a reboot or crash can bring them back with --resume ---

static std::wstring TabsFilePath()
{
    wchar_t Base[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", Base, MAX_PATH);
    std::wstring Dir = std::wstring(Base) + L"\\clod";
    CreateDirectoryW(Dir.c_str(), 0);
    return Dir + L"\\tabs" + InstanceSuffix() + L".txt";
}

static std::string TabsFileContents(const app_state& App)
{
    std::string Out = "active\t" + std::to_string(App.Active) + "\n";
    for(const tab* T : App.Tabs)
    {
        std::string Fields[4] = {T->SessionId, T->Cwd, T->Project, T->Title};
        Out += "tab";
        for(std::string& F : Fields)
        {
            for(char& C : F) { if(C == '\t' || C == '\n' || C == '\r') { C = ' '; } }
            Out += "\t" + F;
        }
        Out += "\n";
    }
    return Out;
}

static void WriteTabsFile(const std::string& Contents)
{
    std::wstring Path = TabsFilePath();
    std::wstring Temp = Path + L".tmp";
    HANDLE File = CreateFileW(Temp.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return; }
    DWORD Written = 0;
    WriteFile(File, Contents.data(), (DWORD)Contents.size(), &Written, 0);
    CloseHandle(File);
    MoveFileExW(Temp.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static std::string ReadTabsFile()
{
    HANDLE File = CreateFileW(TabsFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return std::string(); }
    std::string Out = ReadFileRange(File, 0, 1024 * 1024);
    CloseHandle(File);
    return Out;
}

static std::vector<std::string> SplitTabs(const std::string& Line)
{
    std::vector<std::string> Out;
    size_t Start = 0;
    for(;;)
    {
        size_t End = Line.find('\t', Start);
        Out.push_back(Line.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
        if(End == std::string::npos) { return Out; }
        Start = End + 1;
    }
}

// Sends the reason, waits for the client to read it, then hangs up.
static void DropClient(ipc_conn& Client, const std::string& Reason)
{
    if(!Client.Pipe) { return; }
    IpcSendString(Client, MSG_BYE, Reason);
    FlushFileBuffers(Client.Pipe);
    DisconnectNamedPipe(Client.Pipe);
    IpcClose(Client);
}

static void StartListening(HANDLE Listen, OVERLAPPED& Ov, HANDLE Event)
{
    Ov = OVERLAPPED{};
    Ov.hEvent = Event;
    ResetEvent(Event);
    if(!ConnectNamedPipe(Listen, &Ov) && GetLastError() == ERROR_PIPE_CONNECTED) { SetEvent(Event); }
}

static void LaunchClientWindow(s32 JumpToTab)
{
    // A plain new-console launch is what Windows hands to the user's default terminal app.
    wchar_t Exe[MAX_PATH] = {};
    GetModuleFileNameW(0, Exe, MAX_PATH);
    std::wstring Cmd = std::wstring(L"\"") + Exe + L"\"";
    if(JumpToTab) { Cmd += L" --tab " + std::to_wstring(JumpToTab - 1); }
    wchar_t Home[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
    STARTUPINFOW Si = {};
    Si.cb = sizeof(Si);
    PROCESS_INFORMATION Pi = {};
    if(CreateProcessW(0, &Cmd[0], 0, 0, FALSE, CREATE_NEW_CONSOLE, 0, Home, &Si, &Pi))
    {
        CloseHandle(Pi.hThread);
        CloseHandle(Pi.hProcess);
    }
}

static int ServerMain()
{
    HANDLE Listen = IpcCreatePipe(true);
    if(Listen == INVALID_HANDLE_VALUE) { return 0; } // another server already owns the pipe

    // Children die with us, even if we crash.
    HANDLE Job = CreateJobObjectW(0, 0);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits = {};
    Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits));

    HANDLE Wake = CreateEventW(0, FALSE, FALSE, 0);
    HANDLE ConnectEvent = CreateEventW(0, TRUE, FALSE, 0);
    OVERLAPPED ConnectOv = {};
    StartListening(Listen, ConnectOv, ConnectEvent);
    static ipc_conn Client = {};
    bool32 Attached = false; // Client has sent MSG_ATTACH
    HWND ClientWindow = 0;

    app_state App = {};
    App.Width = 120;
    App.Height = 40;
    {
        wchar_t Home[MAX_PATH] = {};
        GetEnvironmentVariableW(L"USERPROFILE", Home, MAX_PATH);
        App.LaunchCwd = WideToUtf8(Home);
    }
    App.Sessions = ScanSessions();

    // `clod --test`: a throwaway instance for trying UI changes next to the real one. No tray, no
    // restore file, and it ends together with its window.
    bool32 Ephemeral = GetEnvironmentVariableW(L"CLOD_EPHEMERAL", 0, 0) > 0;

    static tray_state Tray = {};
    if(!Ephemeral) { TrayCreate(Tray, &App); }

    // --- restore tabs left open by a server that didn't quit cleanly (reboot, crash, kill) ---
    if(!Ephemeral)
    {
        std::string Saved = ReadTabsFile();
        s32 SavedActive = 0;
        size_t At = 0;
        while(At < Saved.size())
        {
            size_t End = Saved.find('\n', At);
            if(End == std::string::npos) { End = Saved.size(); }
            std::vector<std::string> F = SplitTabs(Saved.substr(At, End - At));
            At = End + 1;
            if(F.size() >= 2 && F[0] == "active") { SavedActive = atoi(F[1].c_str()); }
            // A tab that never got a prompt has no session file; there is nothing to resume.
            if(F.size() >= 5 && F[0] == "tab" && SessionFileExists(F[1]))
            {
                OpenTab(App, F[1], F[2], F[3], F[4], Job, Wake, GetTickCount64());
            }
        }
        App.Active = Clamp(SavedActive, 0, (s32)App.Tabs.size());
    }
    std::string SavedTabs = TabsFileContents(App);
    if(!Ephemeral) { WriteTabsFile(SavedTabs); }

    frame Prev = {};
    frame Next = {};
    bool32 ForceFull = true;
    bool32 Dirty = true;
    u64 StartMs = GetTickCount64();
    u64 LastFrameMs = 0;
    u64 ResizeAtMs = 0;         // pending child resize, applied once the window settles
    u64 SyncSinceMs = 0;        // when the active child began a synchronized update
    u64 LaunchedWindowAtMs = 0; // tray opened a window that hasn't attached yet
    std::string OuterTitle;
    s32 OuterMouse = -1;        // mouse tracking mode currently requested from the client's terminal
    tab* FocusedTab = 0;        // compared by address only; may dangle after a close
    std::string TrayTip;
    std::vector<app_command> Cmds;
    std::string InBytes;
    std::string OutBytes;
    std::string Payload;

    while(!App.Quit)
    {
        HANDLE Waits[3] = {Wake, ConnectEvent, Client.ReadEvent};
        DWORD WaitCount = Client.Pipe ? 3 : 2;
        DWORD Timeout = (Dirty || ResizeAtMs) ? 8 : 250;
        MsgWaitForMultipleObjects(WaitCount, Waits, FALSE, Timeout, QS_ALLINPUT);
        u64 NowMs = GetTickCount64();

        // --- tray window messages ---
        MSG Msg = {};
        while(PeekMessageW(&Msg, 0, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
        if(Tray.Action == TRAY_QUIT)
        {
            App.Quit = true; // the tab list stays on disk, so the next launch reopens every tab
        }
        else if(Tray.Action == TRAY_OPEN)
        {
            if(Tray.ActionTab) { SwitchTab(App, Tray.ActionTab - 1); }
            if(Attached)
            {
                // The tray click gives us the right to hand foreground to the client's window.
                // In Windows Terminal the console window is a hidden placeholder owned by the real
                // terminal window, so walk up to the root owner (conhost's window is its own root).
                HWND Target = ClientWindow ? GetAncestor(ClientWindow, GA_ROOTOWNER) : 0;
                if(Target)
                {
                    if(IsIconic(Target)) { ShowWindow(Target, SW_RESTORE); }
                    if(!SetForegroundWindow(Target)) { FlashWindow(Target, TRUE); }
                }
                Dirty = true;
            }
            else if(NowMs - LaunchedWindowAtMs > 3000)
            {
                LaunchClientWindow(Tray.ActionTab);
                LaunchedWindowAtMs = NowMs;
            }
        }
        Tray.Action = TRAY_NONE;
        Tray.ActionTab = 0;
        if(App.Quit) { break; }

        // --- new connection: it takes over from any attached window ---
        if(WaitForSingleObject(ConnectEvent, 0) == WAIT_OBJECT_0)
        {
            DWORD Ignored = 0;
            if(GetOverlappedResult(Listen, &ConnectOv, &Ignored, FALSE) || GetLastError() == ERROR_PIPE_CONNECTED)
            {
                DropClient(Client, "attached in another window");
                Attached = false;
                IpcInit(Client, Listen);
                IpcPump(Client);
                Listen = IpcCreatePipe(false);
            }
            else
            {
                DisconnectNamedPipe(Listen);
            }
            StartListening(Listen, ConnectOv, ConnectEvent);
        }

        // --- client messages ---
        InBytes.clear();
        if(Client.Pipe)
        {
            bool32 Alive = IpcPump(Client);
            u32 Type = 0;
            while(IpcNextMessage(Client, Type, Payload))
            {
                if(Type == MSG_ATTACH && Payload.size() >= sizeof(msg_attach))
                {
                    msg_attach A = {};
                    memcpy(&A, Payload.data(), sizeof(A));
                    App.LaunchCwd = Payload.substr(sizeof(A));
                    App.Width = Max(A.Width, 1);
                    App.Height = Max(A.Height, 2);
                    if(A.JumpToTab) { SwitchTab(App, A.JumpToTab - 1); }
                    App.Sessions = ScanSessions();
                    ClientWindow = (HWND)(uintptr_t)A.ConsoleWindow;
                    Attached = true;
                    LaunchedWindowAtMs = 0;
                    // A fresh terminal: assume nothing about what it shows or which modes it has.
                    Prev = frame{};
                    OuterMouse = -1;
                    OuterTitle.clear();
                    ForceFull = true;
                    ResizeAtMs = NowMs;
                    Dirty = true;
                }
                else if(Type == MSG_INPUT && Attached)
                {
                    InBytes += Payload;
                }
                else if(Type == MSG_RESIZE && Payload.size() >= 8)
                {
                    s32 Size[2] = {};
                    memcpy(Size, Payload.data(), 8);
                    if(Size[0] != App.Width || Size[1] != App.Height)
                    {
                        App.Width = Max(Size[0], 1);
                        App.Height = Max(Size[1], 2);
                        ResizeAtMs = NowMs;
                        ForceFull = true;
                        Dirty = true;
                    }
                }
            }
            if(!Alive)
            {
                // Window closed: tabs keep running (a test instance ends with its window instead).
                IpcClose(Client);
                Attached = false;
                ClientWindow = 0;
                if(Ephemeral) { App.Quit = true; }
            }
        }

        // --- input to commands ---
        Cmds.clear();
        if(!InBytes.empty())
        {
            HandleInput(App, InBytes, NowMs, Cmds);
            Dirty = true;
        }

        for(app_command& C : Cmds)
        {
            switch(C.Kind)
            {
                case CMD_WRITE: PtyWrite(C.Tab->Pty, C.Bytes); break;
                case CMD_OPEN:
                {
                    OpenTab(App, C.SessionId, C.Cwd, C.Project, C.Title, Job, Wake, NowMs);
                    SwitchTab(App, (s32)App.Tabs.size());
                } break;
                case CMD_OPEN_FOLDER:
                {
                    tab* Base = ActiveTab(App);
                    std::wstring Path = ResolveTypedPath(C.Cwd, Base ? Base->Cwd : App.LaunchCwd);
                    while(Path.size() > 3 && (Path.back() == L'\\' || Path.back() == L'/')) { Path.pop_back(); } // keep "C:\"
                    DWORD Attr = GetFileAttributesW(Path.c_str());
                    std::string Cwd = WideToUtf8(Path);
                    if(Attr == INVALID_FILE_ATTRIBUTES || !(Attr & FILE_ATTRIBUTE_DIRECTORY))
                    {
                        App.Status = "Not a folder: " + Cwd; // the prompt stays open to fix it
                        App.StatusUntil = NowMs + 4000;
                        break;
                    }
                    App.Prompt = PROMPT_NONE;
                    App.PathInput.clear();
                    OpenTab(App, std::string(), Cwd, ProjectName(Cwd), std::string(), Job, Wake, NowMs);
                    SwitchTab(App, (s32)App.Tabs.size());
                } break;
                case CMD_COMPLETE_FOLDER:
                {
                    // --- extend the last path component to what all matching folders share ---
                    tab* Base = ActiveTab(App);
                    std::wstring Path = ResolveTypedPath(App.PathInput, Base ? Base->Cwd : App.LaunchCwd);
                    size_t Slash = Path.find_last_of(L"\\/");
                    std::wstring Dir = (Slash == std::wstring::npos) ? std::wstring() : Path.substr(0, Slash + 1);
                    std::wstring Prefix = Path.substr(Dir.size());
                    std::vector<std::wstring> Matches;
                    WIN32_FIND_DATAW Found = {};
                    HANDLE Find = FindFirstFileExW((Dir + Prefix + L"*").c_str(), FindExInfoBasic, &Found, FindExSearchLimitToDirectories, 0, 0);
                    if(Find != INVALID_HANDLE_VALUE)
                    {
                        do
                        {
                            std::wstring Name = Found.cFileName;
                            if(!(Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || Name == L"." || Name == L"..") { continue; }
                            Matches.push_back(Name);
                        } while(FindNextFileW(Find, &Found));
                        FindClose(Find);
                    }
                    if(Matches.empty())
                    {
                        App.Status = "No folder matches";
                        App.StatusUntil = NowMs + 2000;
                        break;
                    }
                    std::wstring Common = Matches[0];
                    for(const std::wstring& M : Matches)
                    {
                        size_t N = 0;
                        while(N < Common.size() && N < M.size() && towlower(Common[N]) == towlower(M[N])) { ++N; }
                        Common.resize(N);
                    }
                    App.PathInput = WideToUtf8(Dir + Common) + (Matches.size() == 1 ? "\\" : "");
                    if(Matches.size() > 1)
                    {
                        std::string List;
                        for(const std::wstring& M : Matches) { List += WideToUtf8(M) + "  "; }
                        App.Status = List;
                        App.StatusUntil = NowMs + 5000;
                    }
                } break;
                case CMD_RESTART:
                {
                    PtyClose(C.Tab->Pty);
                    C.Tab->Pty = 0;
                    StartTab(*C.Tab, App.Width, ContentRows(App.Height), Job, Wake, App, NowMs);
                } break;
                case CMD_CLOSE:
                {
                    auto It = std::find(App.Tabs.begin(), App.Tabs.end(), C.Tab);
                    if(It == App.Tabs.end()) { break; }
                    PtyClose(C.Tab->Pty);
                    delete C.Tab;
                    App.Tabs.erase(It);
                    App.Active = Min(App.Active, (s32)App.Tabs.size());
                } break;
                case CMD_RESCAN:
                {
                    App.Sessions = ScanSessions();
                    App.Selected = 0;
                    App.ListScroll = 0;
                } break;
                case CMD_DETACH:
                {
                    if(Ephemeral) { App.Quit = true; break; } // the shutdown below says goodbye
                    std::string N = std::to_string(App.Tabs.size());
                    DropClient(Client, "detached; " + N + " session" + (App.Tabs.size() == 1 ? "" : "s") + " still running (see the tray icon)");
                    Attached = false;
                    ClientWindow = 0;
                } break;
            }
        }

        // Nothing running and nobody watching: nothing left to serve. The grace period covers
        // the gap between a client spawning this server and connecting to it.
        if(!Client.Pipe && App.Tabs.empty() && NowMs - StartMs > 5000)
        {
            App.Quit = true;
            break;
        }

        // --- child output into emulators ---
        std::string Chunk;
        std::string ToClient; // pass-through sequences and bells for the attached terminal
        for(s32 I = 0; I < (s32)App.Tabs.size(); ++I)
        {
            tab* T = App.Tabs[I];
            if(!T->Pty) { continue; }
            EnterCriticalSection(&T->Pty->Lock);
            Chunk.swap(T->Pty->Pending);
            LeaveCriticalSection(&T->Pty->Lock);
            std::string Why;
            if(!Chunk.empty())
            {
                bool32 WasSync = T->Term.SyncOutput;
                VtFeed(T->Term, (const u8*)Chunk.data(), Chunk.size());
                Chunk.clear();
                PtyWrite(T->Pty, T->Term.Reply);
                T->Term.Reply.clear();
                ToClient += T->Term.PassThrough;
                T->Term.PassThrough.clear();
                TabSyncScroll(*T);
                if(T != ActiveTab(App)) { T->Activity = true; }
                if(T == ActiveTab(App) && T->Term.SyncOutput && !WasSync) { SyncSinceMs = NowMs; }
                Dirty = true;

                // --- reasons to notify: bell, OSC 9/777, or claude's title spinner stopping ---
                size_t TitleAt = 0;
                u32 First = T->Term.Title.empty() ? 0 : DecodeUtf8(T->Term.Title.data(), T->Term.Title.size(), &TitleAt);
                bool32 Busy = (First >= 0x25D0 && First <= 0x25D3); // ◐◑◒◓
                if(T->TitleBusy && !Busy && First == 0x2733) { Why = "Claude finished"; } // ✳
                T->TitleBusy = Busy;
                if(T->Term.Bell)
                {
                    ToClient += "\x07";
                    T->Term.Bell = false;
                    Why = "Claude needs your attention";
                }
                if(!T->Term.Notify.empty())
                {
                    Why = T->Term.Notify;
                    T->Term.Notify.clear();
                }
            }
            if(!T->Exited && WaitForSingleObject(T->Pty->Process, 0) == WAIT_OBJECT_0)
            {
                DWORD Code = 0;
                GetExitCodeProcess(T->Pty->Process, &Code);
                T->Exited = true;
                T->ExitCode = Code;
                Dirty = true;
            }

            bool32 Unseen = !Attached || T != ActiveTab(App);
            if(!Why.empty() && Unseen && NowMs - T->NotifiedAtMs > 10000)
            {
                TrayNotify(Tray, TabName(*T), Why, I + 1);
                T->NotifiedAtMs = NowMs;
            }
        }
        if(Attached && !ToClient.empty() && !IpcSendString(Client, MSG_OUTPUT, ToClient)) { IpcClose(Client); Attached = false; }

        // --- focus follows the active tab, for children that asked for focus reports ---
        tab* NowFocused = Attached ? ActiveTab(App) : 0;
        if(NowFocused != FocusedTab)
        {
            bool32 OldAlive = std::find(App.Tabs.begin(), App.Tabs.end(), FocusedTab) != App.Tabs.end();
            if(OldAlive && FocusedTab->Pty && FocusedTab->Term.FocusEvents) { PtyWrite(FocusedTab->Pty, "\x1b[O"); }
            if(NowFocused && NowFocused->Pty && NowFocused->Term.FocusEvents) { PtyWrite(NowFocused->Pty, "\x1b[I"); }
            FocusedTab = NowFocused;
        }

        // --- resize children once the window has been still for a moment ---
        if(ResizeAtMs && NowMs - ResizeAtMs >= 60)
        {
            ResizeAtMs = 0;
            for(tab* T : App.Tabs)
            {
                VtResize(T->Term, App.Width, ContentRows(App.Height));
                if(T->Pty)
                {
                    COORD Size = {(SHORT)Max(App.Width, 1), (SHORT)ContentRows(App.Height)};
                    ResizePseudoConsole(T->Pty->Console, Size);
                }
            }
            Dirty = true;
        }

        // --- tray tooltip and the restore file follow the tab set ---
        std::string Tip = "clod - " + std::to_string(App.Tabs.size()) + " session" + (App.Tabs.size() == 1 ? "" : "s");
        if(Tip != TrayTip)
        {
            TraySetTip(Tray, Tip);
            TrayTip = Tip;
        }
        std::string Tabs = TabsFileContents(App);
        if(Tabs != SavedTabs && !Ephemeral)
        {
            WriteTabsFile(Tabs);
            SavedTabs = Tabs;
        }

        // --- present to the attached window ---
        tab* Active = ActiveTab(App);
        bool32 HoldForSync = Active && Active->Term.SyncOutput && NowMs - SyncSinceMs < 100;
        if(Attached && Dirty && !HoldForSync && NowMs - LastFrameMs >= 8)
        {
            FILETIME Ft = {};
            GetSystemTimeAsFileTime(&Ft);
            u64 NowFt = ((u64)Ft.dwHighDateTime << 32) | Ft.dwLowDateTime;
            Compose(App, NowMs, NowFt, Next, App.TabHits);

            OutBytes.clear();
            // Track motion only when the active child wants hover events; otherwise clicks, drags and wheel.
            s32 WantMouse = App.MouseCaptureOff ? 0 : ((Active && Active->Term.MouseMode == 1003) ? 1003 : 1002);
            if(WantMouse != OuterMouse)
            {
                OutBytes += "\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?1006l";
                if(WantMouse) { OutBytes += "\x1b[?1000h\x1b[?" + std::to_string(WantMouse) + "h\x1b[?1006h"; }
                OuterMouse = WantMouse;
            }
            std::string Name = Ephemeral ? "clod TEST" : "clod";
            std::string Title = Active ? Name + " - " + TabName(*Active) : Name;
            if(Title != OuterTitle)
            {
                OutBytes += "\x1b]0;" + Title + "\x07";
                OuterTitle = Title;
            }
            RenderFrame(OutBytes, Prev, Next, ForceFull);
            if(!OutBytes.empty() && !IpcSendString(Client, MSG_OUTPUT, OutBytes)) { IpcClose(Client); Attached = false; }
            std::swap(Prev, Next);
            ForceFull = false;
            Dirty = false;
            LastFrameMs = NowMs;
        }
        // Status messages expire on their own.
        if(!App.Status.empty() && NowMs >= App.StatusUntil) { App.Status.clear(); Dirty = true; }
    }

    // --- shutdown: tell the window, then hang up every child at once so they exit in parallel ---
    // The tab list is left on disk on purpose: the next launch reopens every tab with --resume.
    std::string Count = std::to_string(App.Tabs.size()) + " tab" + (App.Tabs.size() == 1 ? "" : "s");
    DropClient(Client, Ephemeral ? "test instance ended" : "clod quit from the tray; " + Count + " will reopen the next time you run clod");
    if(!Ephemeral) { TrayDestroy(Tray); }
    for(tab* T : App.Tabs) { if(T->Pty) { ClosePseudoConsole(T->Pty->Console); T->Pty->Console = 0; } }
    for(tab* T : App.Tabs)
    {
        PtyClose(T->Pty);
        delete T;
    }
    CloseHandle(Listen);
    CloseHandle(ConnectEvent);
    CloseHandle(Wake);
    CloseHandle(Job);
    return 0;
}
