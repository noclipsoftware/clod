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

// The environment markers a parent Claude Code session leaves behind. A claude that inherits them
// thinks it's a subagent (e.g. it stops saving its transcript), so tabs and the task view's report
// writer start without them.
static const wchar_t* ClaudeMarkers[] = {
    L"CLAUDECODE", L"CLAUDE_CODE_CHILD_SESSION", L"CLAUDE_CODE_SESSION_ID", L"CLAUDE_CODE_SESSION_ATTENDED",
    L"CLAUDE_CODE_ENTRYPOINT", L"CLAUDE_CODE_EXECPATH", L"CLAUDE_CODE_MESSAGING_SOCKET",
    L"CLAUDE_CODE_MESSAGING_TOKEN", L"CLAUDE_PID", L"AI_AGENT",
};

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

// The session a running claude registered in <ClaudeConfigDir>\sessions\<pid>.json. False when
// there's no such file, it names no session, or it's left over from an earlier process that had
// the same pid (its recorded start time differs).
static bool32 ReadClaudeSession(DWORD Pid, std::string& Id, std::string& Cwd)
{
    std::wstring Path = ClaudeConfigDir() + L"\\sessions\\" + std::to_wstring(Pid) + L".json";
    HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return false; }
    std::string Json = ReadFileRange(File, 0, 64 * 1024);
    CloseHandle(File);
    std::string Start;
    HANDLE Proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
    FILETIME Created = {}, Ignored[3] = {};
    bool32 Timed = Proc && GetProcessTimes(Proc, &Created, &Ignored[0], &Ignored[1], &Ignored[2]);
    if(Proc) { CloseHandle(Proc); }
    u64 CreatedFt = ((u64)Created.dwHighDateTime << 32) | Created.dwLowDateTime;
    if(JsonFindString(Json, "procStart", false, Start) && Timed && Start != std::to_string(CreatedFt)) { return false; }
    if(!JsonFindString(Json, "sessionId", false, Id) || Id.empty()) { return false; }
    Cwd.clear();
    JsonFindString(Json, "cwd", false, Cwd);
    return true;
}

// A process's current directory (where a shell has cd'd to), read from its process parameters
// (PEB -> RTL_USER_PROCESS_PARAMETERS.CurrentDirectory, 64-bit layout). Empty if it can't be read.
static std::string ProcessCurrentDirectory(DWORD Pid)
{
    typedef LONG (WINAPI *query_fn)(HANDLE, PROCESSINFOCLASS, void*, ULONG, ULONG*);
    static query_fn Query = (query_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    HANDLE Proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, Pid);
    if(!Proc || !Query) { if(Proc) { CloseHandle(Proc); } return std::string(); }
    PROCESS_BASIC_INFORMATION Basic = {};
    u64 Params = 0;
    UNICODE_STRING Dir = {};
    wchar_t Buf[MAX_PATH + 1] = {};
    bool32 Ok = Query(Proc, ProcessBasicInformation, &Basic, sizeof(Basic), 0) >= 0 &&
                ReadProcessMemory(Proc, (u8*)Basic.PebBaseAddress + 0x20, &Params, sizeof(Params), 0) &&
                ReadProcessMemory(Proc, (u8*)(uintptr_t)Params + 0x38, &Dir, sizeof(Dir), 0) &&
                Dir.Length > 0 && Dir.Length <= MAX_PATH * sizeof(wchar_t) &&
                ReadProcessMemory(Proc, Dir.Buffer, Buf, Dir.Length, 0);
    CloseHandle(Proc);
    if(!Ok) { return std::string(); }
    std::wstring Path(Buf, Dir.Length / sizeof(wchar_t));
    if(Path.size() > 3 && Path.back() == L'\\') { Path.pop_back(); } // "C:\foo\" -> "C:\foo", "C:\" stays
    return WideToUtf8(Path);
}

// One tab's answer from a process scan: the claude running somewhere under its cmd, if any, and
// the folder the cmd itself is in.
struct claude_find
{
    DWORD CmdPid;
    DWORD Pid;       // 0 = none
    std::string Id;
    std::string Cwd;
    std::string ShellCwd;
    u64 CpuUsed;     // CPU time the cmd and everything under it used since the last scan (100 ns units)
};

// The home screen's Claude Code sessions, re-read on their own thread (hundreds of transcripts):
// Known in (unchanged transcripts aren't read again), Sessions out, then Done and the loop's Wake.
struct sessions_scan
{
    std::vector<session_info> Known;
    std::vector<session_info> Sessions;
    HANDLE Wake;
    volatile LONG Done;
};

static DWORD WINAPI SessionsScanThread(void* Param)
{
    sessions_scan* S = (sessions_scan*)Param;
    S->Sessions = ScanSessions(S->Known);
    InterlockedExchange(&S->Done, 1);
    SetEvent(S->Wake);
    return 0;
}

// Tray apps with unread messages, for the command bar: settings.txt's `apps` (exe names, default
// "slack discord", "off" = none) want a look while their notification-area icon has a badge, a
// colored dot on an otherwise white icon. Neither app says so anywhere else (their window titles
// don't change), so this asks explorer for its tray buttons and reads their icons: on its own
// thread, since explorer may be slow to answer. Wanting out (in the setting's order).
struct apps_scan
{
    std::vector<tray_app> Wanting;
    HANDLE Wake;
    volatile LONG Done;
};

static DWORD WINAPI AppsScanThread(void* Param)
{
    apps_scan* S = (apps_scan*)Param;
    std::string Setting = ReadSetting("apps");
    if(Setting.empty()) { Setting = "slack discord"; }
    std::vector<std::string> Apps;
    for(size_t At = 0; Setting != "off" && At < Setting.size();)
    {
        size_t End = Setting.find_first_of(" ,", At);
        if(End == std::string::npos) { End = Setting.size(); }
        if(End > At) { Apps.push_back(Setting.substr(At, End - At)); }
        At = End + 1;
    }
    std::vector<tray_app> Badged;
    // The tray's buttons live in toolbars: under TrayNotifyWnd (Windows 11, hidden icons included),
    // its SysPager (Windows 10) and the overflow window's (Windows 10's hidden icons). A missing
    // parent is skipped: FindWindowEx with none would search the top-level windows.
    HWND Tray = FindWindowW(L"Shell_TrayWnd", 0);
    HWND Notify = Tray ? FindWindowExW(Tray, 0, L"TrayNotifyWnd", 0) : 0;
    HWND Pager = Notify ? FindWindowExW(Notify, 0, L"SysPager", 0) : 0;
    HWND Overflow = FindWindowW(L"NotifyIconOverflowWindow", 0);
    HWND Bars[3] = {Notify ? FindWindowExW(Notify, 0, L"ToolbarWindow32", 0) : 0,
                    Pager ? FindWindowExW(Pager, 0, L"ToolbarWindow32", 0) : 0,
                    Overflow ? FindWindowExW(Overflow, 0, L"ToolbarWindow32", 0) : 0};
    for(s32 B = 0; B < 3 && !Apps.empty(); ++B)
    {
        if(!Bars[B] || (B > 0 && Bars[B] == Bars[0])) { continue; }
        DWORD ExplorerPid = 0;
        GetWindowThreadProcessId(Bars[B], &ExplorerPid);
        HANDLE Explorer = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, ExplorerPid);
        void* Remote = Explorer ? VirtualAllocEx(Explorer, 0, sizeof(TBBUTTON), MEM_COMMIT, PAGE_READWRITE) : 0;
        DWORD_PTR Count = 0;
        if(Remote) { SendMessageTimeoutW(Bars[B], TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 500, &Count); }
        for(DWORD_PTR I = 0; I < Count && I < 256; ++I)
        {
            // Each button's data, in explorer's memory: owner window, id, message, two reserved words, icon.
            struct tray_data { HWND Owner; UINT Id; UINT Message; UINT Reserved[2]; HICON Icon; };
            TBBUTTON Button = {};
            tray_data Data = {};
            DWORD_PTR Got = 0;
            if(!SendMessageTimeoutW(Bars[B], TB_GETBUTTON, I, (LPARAM)Remote, SMTO_ABORTIFHUNG, 500, &Got) || !Got) { continue; }
            if(!ReadProcessMemory(Explorer, Remote, &Button, sizeof(Button), 0)) { continue; }
            if(!ReadProcessMemory(Explorer, (void*)Button.dwData, &Data, sizeof(Data), 0) || !Data.Icon) { continue; }
            // Whose icon: the owner window's exe, if it's one of the apps.
            DWORD OwnerPid = 0;
            GetWindowThreadProcessId(Data.Owner, &OwnerPid);
            HANDLE Owner = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, OwnerPid);
            wchar_t Path[MAX_PATH] = {};
            DWORD PathLen = MAX_PATH;
            if(Owner) { QueryFullProcessImageNameW(Owner, 0, Path, &PathLen); CloseHandle(Owner); }
            std::string Exe = WideToUtf8(PathFindFileNameW(Path));
            if(Exe.size() > 4) { Exe.resize(Exe.size() - 4); }
            bool32 Listed = false;
            for(const std::string& A : Apps) { Listed |= _stricmp(A.c_str(), Exe.c_str()) == 0; }
            if(!Listed) { continue; }
            // A badge: at least 1 in 20 of the icon's opaque pixels strongly colored (icons are shared
            // across processes, so its bitmap can be read here).
            ICONINFO Info = {};
            if(!GetIconInfo(Data.Icon, &Info)) { continue; }
            BITMAP Bm = {};
            GetObjectW(Info.hbmColor, sizeof(Bm), &Bm);
            std::vector<u32> Px((size_t)Max(Bm.bmWidth, 0) * Max(Bm.bmHeight, 0));
            BITMAPINFO Bi = {};
            Bi.bmiHeader.biSize = sizeof(Bi.bmiHeader);
            Bi.bmiHeader.biWidth = Bm.bmWidth;
            Bi.bmiHeader.biHeight = -Bm.bmHeight;
            Bi.bmiHeader.biPlanes = 1;
            Bi.bmiHeader.biBitCount = 32;
            HDC Dc = GetDC(0);
            s32 Opaque = 0;
            s32 Colored = 0;
            if(!Px.empty() && Px.size() <= 512 * 512 && GetDIBits(Dc, Info.hbmColor, 0, Bm.bmHeight, Px.data(), &Bi, DIB_RGB_COLORS))
            {
                for(u32 C : Px)
                {
                    s32 R = (C >> 16) & 255, G = (C >> 8) & 255, Bl = C & 255;
                    if((C >> 24) < 128) { continue; }
                    Opaque++;
                    if(Max(R, Max(G, Bl)) - Min(R, Min(G, Bl)) > 100) { Colored++; }
                }
            }
            ReleaseDC(0, Dc);
            if(Info.hbmColor) { DeleteObject(Info.hbmColor); }
            if(Info.hbmMask) { DeleteObject(Info.hbmMask); }
            if(Opaque && Colored * 20 >= Opaque) { Badged.push_back(tray_app{Exe, WideToUtf8(Path), (u64)Data.Icon}); }
        }
        if(Remote) { VirtualFreeEx(Explorer, Remote, 0, MEM_RELEASE); }
        if(Explorer) { CloseHandle(Explorer); }
    }
    for(const std::string& A : Apps)
    {
        for(const tray_app& T : Badged)
        {
            if(_stricmp(A.c_str(), T.Name.c_str()) == 0) { S->Wanting.push_back(T); break; }
        }
    }
    InterlockedExchange(&S->Done, 1);
    SetEvent(S->Wake);
    return 0;
}

// A process scan handed to its own thread: CmdPids in, Finds out, then Done and the loop's Wake.
struct claude_scan
{
    std::vector<DWORD> CmdPids;
    std::vector<claude_find> Finds;
    std::map<std::pair<DWORD, u64>, u64> Cpu; // in: each tab process's CPU time at the last scan, by pid and start time; out: now
    u64 PrevAtFt;  // in: when the last scan read them (0 = none yet)
    u64 AtFt;      // out: when this one did
    HANDLE Wake;
    volatile LONG Done;
};

// Claude Code writes <ClaudeConfigDir>\sessions\<pid>.json for each running instance. A tab is a
// claude tab while one of those pids lives somewhere under its cmd; the file names the session,
// so a claude typed in a shell (or switched with /resume) is tracked too. Also keeps the tabs'
// console hosts (ConPTY starts them as our children) and claudes above normal priority, like us.
static DWORD WINAPI ClaudeScanThread(void* Param)
{
    claude_scan* S = (claude_scan*)Param;
    std::vector<PROCESSENTRY32W> Procs = ListProcesses();
    FILETIME NowFt = {};
    GetSystemTimeAsFileTime(&NowFt);
    S->AtFt = ((u64)NowFt.dwHighDateTime << 32) | NowFt.dwLowDateTime;
    std::map<std::pair<DWORD, u64>, u64> Cpu;
    for(const PROCESSENTRY32W& P : Procs)
    {
        if(P.th32ParentProcessID != GetCurrentProcessId() || _wcsicmp(P.szExeFile, L"conhost.exe") != 0) { continue; }
        HANDLE Host = OpenProcess(PROCESS_SET_INFORMATION, FALSE, P.th32ProcessID);
        if(Host) { RaiseForTyping(Host); CloseHandle(Host); }
    }
    for(DWORD CmdPid : S->CmdPids)
    {
        std::vector<DWORD> Tree = {CmdPid};
        for(size_t K = 0; K < Tree.size() && Tree.size() < 256; ++K)
        {
            for(const PROCESSENTRY32W& P : Procs)
            {
                if(P.th32ParentProcessID == Tree[K] && P.th32ProcessID != Tree[K]) { Tree.push_back(P.th32ProcessID); }
            }
        }
        claude_find F = {};
        F.CmdPid = CmdPid;
        F.ShellCwd = ProcessCurrentDirectory(CmdPid);
        // --- the CPU the tree used since the last scan: each process's time now less what it had then,
        // all of it for one started since (a process that ended in between takes its last slice along) ---
        for(DWORD Pid : Tree)
        {
            HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
            FILETIME Created = {}, Ended = {}, Kernel = {}, User = {};
            if(Process && GetProcessTimes(Process, &Created, &Ended, &Kernel, &User))
            {
                u64 CreatedFt = ((u64)Created.dwHighDateTime << 32) | Created.dwLowDateTime;
                u64 Used = (((u64)Kernel.dwHighDateTime << 32) | Kernel.dwLowDateTime) + (((u64)User.dwHighDateTime << 32) | User.dwLowDateTime);
                std::pair<DWORD, u64> Key = {Pid, CreatedFt};
                auto Was = S->Cpu.find(Key);
                if(Was != S->Cpu.end()) { F.CpuUsed += Used > Was->second ? Used - Was->second : 0; }
                else if(S->PrevAtFt && CreatedFt >= S->PrevAtFt) { F.CpuUsed += Used; }
                Cpu[Key] = Used;
            }
            if(Process) { CloseHandle(Process); }
        }
        for(size_t K = 1; K < Tree.size() && !F.Pid; ++K)
        {
            if(ReadClaudeSession(Tree[K], F.Id, F.Cwd)) { F.Pid = Tree[K]; }
        }
        if(F.Pid)
        {
            HANDLE Claude = OpenProcess(PROCESS_SET_INFORMATION, FALSE, F.Pid);
            if(Claude) { RaiseForTyping(Claude); CloseHandle(Claude); }
        }
        S->Finds.push_back(F);
    }
    S->Cpu.swap(Cpu);
    InterlockedExchange(&S->Done, 1);
    SetEvent(S->Wake);
    return 0;
}

// Where a tab's recent output is kept across server restarts: %LOCALAPPDATA%\clod\history\<id>.txt.
static std::wstring HistoryPath(const std::string& Id)
{
    wchar_t Base[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", Base, MAX_PATH);
    return std::wstring(Base) + L"\\clod\\history" + InstanceSuffix() + L"\\" + Utf8ToWide(Id) + L".txt"; // per instance
}

// Saves a shell tab's last lines of output for the next server to show (claude tabs come back by
// --resume instead, so theirs is dropped). UTF-16 with a BOM: cmd's `type` prints that right in
// any code page.
static void SaveHistory(tab& T)
{
    std::string Text = T.SessionId.empty() ? HistoryText(T.Term, 200) : std::string();
    if(Text == T.HistorySaved) { return; }
    T.HistorySaved = Text;
    std::wstring Path = HistoryPath(T.HistoryId);
    if(Text.empty()) { DeleteFileW(Path.c_str()); return; }
    CreateDirectoryW(Path.substr(0, Path.find_last_of(L'\\')).c_str(), 0);
    std::wstring Wide = L"\xFEFF" + Utf8ToWide(Text);
    HANDLE File = CreateFileW(Path.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return; }
    DWORD Written = 0;
    WriteFile(File, Wide.data(), (DWORD)(Wide.size() * sizeof(wchar_t)), &Written, 0);
    CloseHandle(File);
}

// (Re)starts a tab's cmd, running the user's init.bat. With a SessionId, cmd then runs claude:
// resuming when the session file exists, otherwise claiming the id. Claude exiting leaves the shell.
static void StartTab(tab& Tab, s32 Cols, s32 Rows, HANDLE Job, HANDLE Wake, app_state& App, u64 NowMs)
{
    VtInit(Tab.Term, Cols, Rows);
    Tab.ScrollOffset = 0;
    Tab.SeenPushed = 0;
    Tab.Exited = false;
    Tab.TitleBusy = false;
    Tab.ClaudePid = 0;
    Tab.LaunchedAtMs = 0;

    std::wstring Cwd = Utf8ToWide(Tab.Cwd);
    if(Cwd.empty() || GetFileAttributesW(Cwd.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        App.Status = "Directory missing: " + Tab.Cwd + " (starting in " + App.LaunchCwd + ")";
        App.StatusUntil = NowMs + 5000;
        Cwd = Utf8ToWide(App.LaunchCwd);
    }

    // --- the command: cmd /k "call init.bat & clink inject & cd /d <cwd> & claude ..." ---
    // The user's own setup lives in ~\.clod (seeded from clod's defaults on first run): init.bat,
    // and the clink profile ~\.clod\clink, where clink itself finds clink_settings, .inputrc and Lua
    // scripts, and keeps history. init.bat runs before the inject, so it can also steer clink.
    // Clink (vendored, next to clod.exe) runs from a DLL cache in %TEMP%, so it never locks its files.
    wchar_t Exe[MAX_PATH] = {};
    if(!GetEnvironmentVariableW(L"ComSpec", Exe, MAX_PATH)) { wcscpy_s(Exe, L"cmd.exe"); }
    std::wstring ClodDir = UserClodDir();
    std::wstring Inner;
    std::wstring Init = ClodDir + L"init.bat";
    // A shell back after a restart first shows its saved output, above the new prompt.
    std::wstring History = HistoryPath(Tab.HistoryId);
    if(Tab.ReplayHistory && Tab.SessionId.empty() && GetFileAttributesW(History.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        Inner = L"type \"" + History + L"\"";
    }
    Tab.ReplayHistory = false;
    if(GetFileAttributesW(Init.c_str()) != INVALID_FILE_ATTRIBUTES) { Inner += (Inner.empty() ? L"" : L" & ") + std::wstring(L"call \"") + Init + L"\""; }
    std::wstring Clink = ExeDir() + L"clink\\clink_x64.exe";
    if(GetFileAttributesW(Clink.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        if(!Inner.empty()) { Inner += L" & "; }
        Inner += L"\"" + Clink + L"\" inject --quiet --profile \"" + ClodDir + L"clink\"";
    }
    if(!Tab.SessionId.empty())
    {
        if(!Inner.empty()) { Inner += L" & "; }
        std::wstring Claude = FindClaude();
        if(Claude.empty())
        {
            // No Claude Code on this machine: say so in the shell instead of failing obscurely.
            Inner += L"echo. & echo   Claude Code is not installed (no claude.exe or claude.cmd found). Install it, e.g. in PowerShell: & "
                     L"echo. & echo       irm https://claude.ai/install.ps1 ^| iex & echo.";
            Tab.SessionId.clear();
        }
        else
        {
            bool32 Resume = SessionFileExists(Tab.SessionId);
            Inner += L"cd /d \"" + Cwd + L"\" & " + Claude + (Resume ? L" --resume " : L" --session-id ") + Utf8ToWide(Tab.SessionId);
            Tab.LaunchedAtMs = NowMs;
        }
    }
    // cmd strips the outer quotes of a /k string that holds more than one quoted part.
    std::wstring Cmd = std::wstring(L"\"") + Exe + L"\"" + (Inner.empty() ? std::wstring() : L" /k \"" + Inner + L"\"");

    // --- environment: drop the markers a parent Claude Code session leaves behind (ClaudeMarkers) ---
    // Inherited clink config locations are dropped, so another clink setup never leaks into clod's.
    static const wchar_t* Strip[] = {
        L"CLINK_SETTINGS", L"CLINK_INPUTRC", L"CLINK_PATH", L"CLINK_PROFILE", L"CLINK_HISTORY_LABEL", L"CLOD_DIR",
    };
    std::wstring Env;
    bool32 Vs = ReadSetting("vs_env") != "off";
    std::vector<bool32> VsDone(App.VsSet.size(), false);
    wchar_t* Block = GetEnvironmentStringsW();
    for(const wchar_t* Var = Block; *Var; Var += wcslen(Var) + 1)
    {
        const wchar_t* Eq = wcschr(Var + 1, L'='); // +1: skips the leading '=' of "=C:=C:\..." entries
        size_t NameLen = Eq ? (size_t)(Eq - Var) : wcslen(Var);
        bool32 Drop = false;
        for(const wchar_t* S : Strip) { if(wcslen(S) == NameLen && _wcsnicmp(Var, S, NameLen) == 0) { Drop = true; } }
        for(const wchar_t* S : ClaudeMarkers) { if(wcslen(S) == NameLen && _wcsnicmp(Var, S, NameLen) == 0) { Drop = true; } }
        if(Drop) { continue; }
        // Visual Studio's build environment changes this one: set, or put around its own value.
        std::wstring Entry = Var;
        for(size_t I = 0; Vs && I < App.VsSet.size(); ++I)
        {
            if(App.VsSet[I].find(L'=') == NameLen && _wcsnicmp(Var, App.VsSet[I].c_str(), NameLen) == 0) { Entry = App.VsSet[I]; VsDone[I] = true; }
        }
        for(const std::wstring& P : App.VsPrepend)
        {
            if(Vs && P.find(L'=') == NameLen && _wcsnicmp(Var, P.c_str(), NameLen) == 0) { Entry = P + Entry.substr(NameLen + 1); }
        }
        for(const std::wstring& P : App.VsAppend)
        {
            if(Vs && P.find(L'=') == NameLen && _wcsnicmp(Var, P.c_str(), NameLen) == 0) { Entry += P.substr(NameLen + 1); }
        }
        Env.append(Entry);
        Env.push_back(0);
    }
    FreeEnvironmentStringsW(Block);
    for(size_t I = 0; Vs && I < App.VsSet.size(); ++I) { if(!VsDone[I]) { Env.append(App.VsSet[I]); Env.push_back(0); } }
    // Each tab keeps its own clink history (clink_history_<label> in the profile), so up-arrow in a
    // tab brought back after a restart recalls that tab's commands. The label is the tab's lasting
    // id without dashes (clink allows 32 letters and digits): the session, else the shell's history id.
    std::wstring Label;
    for(char Ch : Tab.SessionId.empty() ? Tab.HistoryId : Tab.SessionId) { if(isalnum((u8)Ch) && Label.size() < 32) { Label += (wchar_t)Ch; } }
    Env += L"CLINK_HISTORY_LABEL=" + Label;
    Env.push_back(0);
    // Tab completion for git, npm, winget, ... (vendored clink-completions, next to clod.exe). Clink
    // still loads the profile's own scripts; init.bat can add to CLINK_PATH or clear it.
    std::wstring Completions = ExeDir() + L"clink-completions";
    if(GetFileAttributesW(Completions.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        Env += L"CLINK_PATH=" + Completions;
        Env.push_back(0);
    }
    // Where clod.exe is (with a trailing backslash), so the shell can run `clod view` (clod.lua's alias).
    Env += L"CLOD_DIR=" + ExeDir();
    Env.push_back(0);
    Env.push_back(0);

    std::string Error;
    Tab.Pty = PtyStart(Cmd, Cwd, Env.c_str(), Cols, Rows, Job, Wake, Error);
    if(!Tab.Pty)
    {
        Tab.Exited = true;
        App.Status = Error;
        App.StatusUntil = NowMs + 5000;
    }
}

// Shell: a plain cmd. Otherwise claude runs in it, with SessionId or a brand new one.
static tab* OpenTab(app_state& App, bool32 Shell, const std::string& SessionId, const std::string& HistoryId, const std::string& Cwd, const std::string& Project, const std::string& Title,
                    HANDLE Job, HANDLE Wake, u64 NowMs)
{
    tab* T = new tab();
    T->SessionId = Shell ? std::string() : (SessionId.empty() ? NewUuid() : SessionId);
    T->Cwd = Cwd;
    T->Project = Project;
    T->Title = Title;
    // A shell restored after a restart shows its saved output again (when there is any).
    T->HistoryId = HistoryId.empty() ? NewUuid() : HistoryId;
    T->ReplayHistory = !HistoryId.empty();
    pane_rect R = PaneRect(App, App.Focus); // it opens in the focused pane
    StartTab(*T, R.W, R.H, Job, Wake, App, NowMs);
    App.Tabs.push_back(T);
    return T;
}

static void CloseTab(app_state& App, tab* T)
{
    auto It = std::find(App.Tabs.begin(), App.Tabs.end(), T);
    if(It == App.Tabs.end()) { return; }
    ForgetTab(App, T); // its pane takes the next tab nothing shows
    DeleteFileW(HistoryPath(T->HistoryId).c_str()); // a closed tab's output isn't coming back
    PtyClose(T->Pty);
    delete T;
    App.Tabs.erase(It);
    App.Active = TabIndex(App, App.PaneTab[App.Focus]);
}

// A program's whole output (stdout and stderr), run hidden; empty if it can't start. Input goes to
// its stdin; Env (a block of "NAME=value\0" entries) replaces our environment when given; Dir is its
// folder (empty = ours). A program still running after TimeoutMs is ended, with what it wrote so far.
static std::string RunCapture(std::wstring Cmd, const std::string& Input, const std::wstring& Env, const std::wstring& Dir, u32 TimeoutMs)
{
    SECURITY_ATTRIBUTES Sa = {sizeof(Sa), 0, TRUE};
    HANDLE Read = 0;
    HANDLE Write = 0;
    HANDLE InRead = 0;
    HANDLE InWrite = 0;
    if(!CreatePipe(&Read, &Write, &Sa, 0)) { return std::string(); }
    if(!CreatePipe(&InRead, &InWrite, &Sa, 1 << 16)) { CloseHandle(Read); CloseHandle(Write); return std::string(); }
    SetHandleInformation(Read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(InWrite, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW Si = {};
    Si.cb = sizeof(Si);
    Si.dwFlags = STARTF_USESTDHANDLES;
    Si.hStdInput = InRead;
    Si.hStdOutput = Write;
    Si.hStdError = Write;
    PROCESS_INFORMATION Pi = {};
    bool32 Started = CreateProcessW(0, &Cmd[0], 0, 0, TRUE, CREATE_NO_WINDOW | (Env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                                    Env.empty() ? 0 : (void*)Env.data(), Dir.empty() ? 0 : Dir.c_str(), &Si, &Pi);
    CloseHandle(Write);
    CloseHandle(InRead);
    DWORD Wrote = 0;
    if(Started && !Input.empty()) { WriteFile(InWrite, Input.data(), (DWORD)Input.size(), &Wrote, 0); }
    CloseHandle(InWrite);
    // --- read until it exits (or runs out of time); a child it started may hold the pipe open longer ---
    std::string Out;
    char Buf[4096];
    u64 StartMs = GetTickCount64();
    while(Started)
    {
        DWORD Avail = 0;
        DWORD Got = 0;
        if(!PeekNamedPipe(Read, 0, 0, 0, &Avail, 0)) { break; } // closed: everything's read
        if(Avail && ReadFile(Read, Buf, Min((s32)Avail, (s32)sizeof(Buf)), &Got, 0)) { Out.append(Buf, Got); continue; }
        if(WaitForSingleObject(Pi.hProcess, 50) == WAIT_OBJECT_0)
        {
            while(PeekNamedPipe(Read, 0, 0, 0, &Avail, 0) && Avail && ReadFile(Read, Buf, Min((s32)Avail, (s32)sizeof(Buf)), &Got, 0)) { Out.append(Buf, Got); }
            break;
        }
        if(GetTickCount64() - StartMs > TimeoutMs) { TerminateProcess(Pi.hProcess, 1); break; }
    }
    if(Started)
    {
        CloseHandle(Pi.hProcess);
        CloseHandle(Pi.hThread);
    }
    CloseHandle(Read);
    return Out;
}

// Visual Studio's x64 build environment for the tabs, so cl, link, cmake, ninja and msbuild work in
// any shell: what vcvars64.bat of the newest install with the C++ tools changes, as a difference from
// our own environment (set, put in front of, put after). Running it takes seconds, so the result is
// kept in %LOCALAPPDATA%\clod\vsenv.txt and only run again when a Visual Studio folder it names is
// gone (an update moved the tools). Nothing when there's no such install, or when we were started from
// a developer prompt already (the tabs inherit that).
static void LoadVsEnv(app_state& App)
{
    if(GetEnvironmentVariableW(L"VSINSTALLDIR", 0, 0)) { return; }
    wchar_t Local[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", Local, MAX_PATH);
    std::wstring CachePath = std::wstring(Local) + L"\\clod\\vsenv.txt";

    // --- the cache: "set|pre|post <tab> NAME=value" lines, kept while every VS folder in it exists ---
    std::string Cached = ReadWholeFile(CachePath);
    bool32 Valid = !Cached.empty();
    std::vector<std::wstring> Lists[3];
    for(size_t At = 0; At < Cached.size() && Valid;)
    {
        size_t End = Cached.find('\n', At);
        if(End == std::string::npos) { End = Cached.size(); }
        std::string Line = Cached.substr(At, End - At);
        At = End + 1;
        while(!Line.empty() && Line.back() == '\r') { Line.pop_back(); }
        size_t Tab = Line.find('\t');
        if(Tab == std::string::npos) { continue; }
        std::string Kind = Line.substr(0, Tab);
        std::wstring Entry = Utf8ToWide(Line.substr(Tab + 1));
        s32 List = (Kind == "set") ? 0 : (Kind == "pre") ? 1 : (Kind == "post") ? 2 : -1;
        if(List < 0) { continue; }
        Lists[List].push_back(Entry);
        // Folders of Visual Studio named in a list value must still be there.
        std::wstring Value = Entry.substr(Entry.find(L'=') + 1);
        for(size_t P = 0; P < Value.size() && Valid;)
        {
            size_t Semi = Value.find(L';', P);
            if(Semi == std::wstring::npos) { Semi = Value.size(); }
            std::wstring Dir = Value.substr(P, Semi - P);
            P = Semi + 1;
            if(Dir.find(L"Microsoft Visual Studio") != std::wstring::npos && GetFileAttributesW(Dir.c_str()) == INVALID_FILE_ATTRIBUTES) { Valid = false; }
        }
    }

    if(!Valid)
    {
        // --- find the newest install with the C++ x64 tools, run its vcvars64.bat, read back `set` ---
        for(std::vector<std::wstring>& L : Lists) { L.clear(); }
        wchar_t X86[MAX_PATH] = {};
        if(!GetEnvironmentVariableW(L"ProgramFiles(x86)", X86, MAX_PATH)) { return; }
        std::wstring Vswhere = std::wstring(X86) + L"\\Microsoft Visual Studio\\Installer\\vswhere.exe";
        if(GetFileAttributesW(Vswhere.c_str()) == INVALID_FILE_ATTRIBUTES) { return; }
        std::string Install = RunCapture(L"\"" + Vswhere + L"\" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath", std::string(), std::wstring(), std::wstring(), 60000);
        while(!Install.empty() && strchr("\r\n ", Install.back())) { Install.pop_back(); }
        std::wstring Vcvars = Utf8ToWide(Install) + L"\\VC\\Auxiliary\\Build\\vcvars64.bat";
        if(Install.empty() || GetFileAttributesW(Vcvars.c_str()) == INVALID_FILE_ATTRIBUTES) { return; }
        // cmd /u writes `set`'s output as UTF-16.
        std::string Raw = RunCapture(L"cmd.exe /d /u /c \"\"" + Vcvars + L"\" >nul 2>&1 && set\"", std::string(), std::wstring(), std::wstring(), 120000);
        std::wstring Text((const wchar_t*)Raw.data(), Raw.size() / sizeof(wchar_t));
        for(size_t At = 0; At < Text.size();)
        {
            size_t End = Text.find(L'\n', At);
            if(End == std::wstring::npos) { End = Text.size(); }
            std::wstring Line = Text.substr(At, End - At);
            At = End + 1;
            while(!Line.empty() && Line.back() == L'\r') { Line.pop_back(); }
            size_t Eq = Line.find(L'=');
            if(Eq == std::wstring::npos || Eq == 0) { continue; }
            std::wstring Name = Line.substr(0, Eq);
            std::wstring New = Line.substr(Eq + 1);
            DWORD Len = GetEnvironmentVariableW(Name.c_str(), 0, 0);
            std::wstring Old(Len, 0);
            if(Len) { Old.resize(GetEnvironmentVariableW(Name.c_str(), &Old[0], Len)); }
            if(New == Old) { continue; }
            size_t Pos = Old.empty() ? std::wstring::npos : New.find(Old);
            if(Pos == std::wstring::npos) { Lists[0].push_back(Name + L"=" + New); continue; }
            if(Pos > 0) { Lists[1].push_back(Name + L"=" + New.substr(0, Pos)); }
            if(Pos + Old.size() < New.size()) { Lists[2].push_back(Name + L"=" + New.substr(Pos + Old.size())); }
        }
        std::string Out;
        static const char* Kinds[3] = {"set", "pre", "post"};
        for(s32 K = 0; K < 3; ++K) { for(const std::wstring& E : Lists[K]) { Out += std::string(Kinds[K]) + "\t" + WideToUtf8(E) + "\n"; } }
        CreateDirectoryW((std::wstring(Local) + L"\\clod").c_str(), 0);
        FILE* File = 0;
        _wfopen_s(&File, CachePath.c_str(), L"wb");
        if(File) { fwrite(Out.data(), 1, Out.size(), File); fclose(File); }
    }
    App.VsSet = Lists[0];
    App.VsPrepend = Lists[1];
    App.VsAppend = Lists[2];
}

// The prefix key from settings.txt ("prefix = alt+`", "prefix = ctrl+]"), or Alt+` when unset or unusable.
static u32 LoadPrefixKey()
{
    s32 Key = PrefixFromName(ReadSetting("prefix"));
    return Key >= 0 ? (u32)Key : (PREFIX_ALT | '`');
}

// Shows an image over tab T (`clod view`): Path itself, or when Path is a folder its first image;
// then Step images on through that folder, in Explorer's order, wrapping, skipping files Windows
// can't read. Sets the tab's view (and its place among the folder's images); on failure says why
// in the command bar and leaves the view as it was.
static void ViewImageAt(app_state& App, tab& T, const std::string& Path, s32 Step, u64 NowMs)
{
    std::wstring P = Utf8ToWide(Path);
    while(P.size() > 3 && (P.back() == L'\\' || P.back() == L'/')) { P.pop_back(); }
    DWORD Attr = GetFileAttributesW(P.c_str());
    bool32 Folder = (Attr != INVALID_FILE_ATTRIBUTES) && (Attr & FILE_ATTRIBUTE_DIRECTORY);
    std::wstring Dir = Folder ? P : P.substr(0, P.find_last_of(L"\\/"));
    std::wstring Name = Folder ? std::wstring() : P.substr(P.find_last_of(L"\\/") + 1);

    // --- the folder's images by extension, sorted the way Explorer sorts names ---
    static const wchar_t* Exts[] = {L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".gif", L".bmp", L".dib", L".tif", L".tiff",
                                    L".ico", L".webp", L".heic", L".heif", L".avif", L".jxr", L".wdp", L".dds"};
    std::vector<std::wstring> Names;
    WIN32_FIND_DATAW Found = {};
    HANDLE Find = FindFirstFileW((Dir + L"\\*").c_str(), &Found);
    for(bool32 More = Find != INVALID_HANDLE_VALUE; More; More = FindNextFileW(Find, &Found))
    {
        if(Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { continue; }
        const wchar_t* Dot = wcsrchr(Found.cFileName, L'.');
        bool32 Image = false;
        for(const wchar_t* E : Exts) { if(Dot && _wcsicmp(Dot, E) == 0) { Image = true; } }
        if(Image) { Names.push_back(Found.cFileName); }
    }
    if(Find != INVALID_HANDLE_VALUE) { FindClose(Find); }
    std::sort(Names.begin(), Names.end(), [](const std::wstring& A, const std::wstring& B) { return StrCmpLogicalW(A.c_str(), B.c_str()) < 0; });
    s32 At = 0;
    if(!Folder)
    {
        // The file itself, even with an extension not on the list (WIC may still read it).
        At = -1;
        for(s32 I = 0; I < (s32)Names.size(); ++I) { if(_wcsicmp(Names[I].c_str(), Name.c_str()) == 0) { At = I; } }
        if(At < 0) { Names.insert(Names.begin(), Name); At = 0; }
    }
    if(Names.empty())
    {
        App.Status = "No images in " + WideToUtf8(Dir);
        App.StatusUntil = NowMs + 5000;
        return;
    }

    // --- decode: the one Step away, or the next readable one past it in the same direction ---
    s32 Count = (s32)Names.size();
    s32 Dirn = (Step < 0) ? -1 : 1;
    for(s32 Tries = 0; Tries < Count; ++Tries)
    {
        s32 Index = (((At + Step + Dirn * Tries) % Count) + Count) % Count;
        std::wstring File = Dir + L"\\" + Names[Index];
        std::vector<u32> Pixels;
        u32 W = 0, H = 0, FullW = 0, FullH = 0;
        if(!DecodeImage(File, 640, 400, Pixels, &W, &H, &FullW, &FullH)) { continue; }
        T.ViewImage = WideToUtf8(File);
        T.ViewPixels.swap(Pixels);
        T.ViewW = W;
        T.ViewH = H;
        T.ViewFullW = FullW;
        T.ViewFullH = FullH;
        T.ViewIndex = (u32)Index;
        T.ViewCount = (u32)Count;
        return;
    }
    App.Status = Folder || Count > 1 ? "No image Windows can read in " + WideToUtf8(Dir) : "Can't show " + WideToUtf8(Name) + ": not an image Windows can read";
    App.StatusUntil = NowMs + 5000;
}

// A frame for a client that draws cells itself (clodw): the rows that differ from Prev, or all of
// them when Full, plus cursor, title and the mouse reporting it should do. Returns false when there
// is nothing to send.
static bool32 EncodeCells(std::string& Out, const frame& Prev, const frame& Next, bool32 Full, s32 MouseMode, u32 PrefixKey,
                          const std::string& Title, bool32 TitleChanged)
{
    if(Prev.Width != Next.Width || Prev.Height != Next.Height || Prev.Cells.size() != Next.Cells.size()) { Full = true; }
    std::string Rows;
    u32 RowCount = 0;
    size_t RowBytes = (size_t)Next.Width * sizeof(cell);
    for(s32 Y = 0; Y < Next.Height; ++Y)
    {
        const cell* Row = Next.Cells.data() + (size_t)Y * Next.Width;
        if(!Full && memcmp(Row, Prev.Cells.data() + (size_t)Y * Next.Width, RowBytes) == 0) { continue; }
        Rows.append((const char*)&Y, sizeof(Y));
        Rows.append((const char*)Row, RowBytes);
        RowCount += 1;
    }
    bool32 CursorChanged = Full || Prev.CursorHidden != Next.CursorHidden || Prev.CursorX != Next.CursorX || Prev.CursorY != Next.CursorY ||
                           Prev.CursorShape != Next.CursorShape;
    bool32 PanesChanged = Prev.PaneCount != Next.PaneCount || memcmp(Prev.Panes, Next.Panes, sizeof(Next.Panes)) != 0 ||
                          Prev.PaneMouseTaken != Next.PaneMouseTaken || Prev.ImagePane != Next.ImagePane || Prev.ImagePath != Next.ImagePath ||
                          Prev.AppIcons != Next.AppIcons;
    if(!RowCount && !CursorChanged && !TitleChanged && !PanesChanged) { return false; }

    msg_cells H = {};
    H.Width = Next.Width;
    H.Height = Next.Height;
    H.CursorX = Next.CursorX;
    H.CursorY = Next.CursorY;
    H.Flags = (Full ? CELLS_FULL : 0) | (Next.CursorHidden ? CELLS_CURSOR_HIDDEN : 0);
    H.MouseMode = MouseMode;
    H.PrefixKey = PrefixKey;
    H.CursorShape = Next.CursorShape;
    H.PaneCount = Min(Next.PaneCount, 4);
    for(s32 P = 0; P < H.PaneCount; ++P)
    {
        H.Panes[P][0] = Next.Panes[P].X;
        H.Panes[P][1] = Next.Panes[P].Y;
        H.Panes[P][2] = Next.Panes[P].W;
        H.Panes[P][3] = Next.Panes[P].H;
    }
    H.PaneMouseTaken = Next.PaneMouseTaken;
    H.ImagePane = Next.ImagePane;
    H.ImagePathLen = (u32)Next.ImagePath.size();
    H.AppIconsLen = (u32)Next.AppIcons.size();
    H.TitleLen = (u32)Title.size();
    H.RowCount = RowCount;
    Out.append((const char*)&H, sizeof(H));
    Out += Title;
    Out += Next.ImagePath;
    Out += Next.AppIcons;
    Out += Rows;
    return true;
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
    // Also the layout, focus and what each pane shows (tab numbers, 0 = home screen), and the
    // layout each monitor last used.
    std::string Out = "active\t" + std::to_string(App.Active) + "\n";
    Out += "layout\t" + std::to_string(App.Layout) + "\t" + std::to_string(App.Focus);
    for(s32 P = 0; P < MAX_PANES; ++P) { Out += "\t" + std::to_string(TabIndex(App, App.PaneTab[P])); }
    Out += "\n";
    for(const monitor_layout& M : App.MonitorLayouts) { Out += "monitor\t" + std::to_string(M.Layout) + "\t" + M.Monitor + "\n"; }
    // Pages, when there's more than one: which is current, then each one's layout, focus and panes
    // (the current page from the live state; its entry in Pages is only refreshed when leaving it).
    if(App.Pages.size() > 1)
    {
        Out += "pages\t" + std::to_string(App.Page) + "\n";
        for(s32 G = 0; G < (s32)App.Pages.size(); ++G)
        {
            bool32 Current = (G == App.Page);
            const page& Pg = App.Pages[G];
            Out += "page\t" + std::to_string(Current ? App.Layout : Pg.Layout) + "\t" + std::to_string(Current ? App.Focus : Pg.Focus);
            for(s32 P = 0; P < MAX_PANES; ++P) { Out += "\t" + std::to_string(TabIndex(App, Current ? App.PaneTab[P] : Pg.PaneTab[P])); }
            Out += "\n";
        }
    }
    for(const tab* T : App.Tabs)
    {
        std::string Fields[4] = {T->SessionId.empty() ? T->HistoryId : T->SessionId, T->Cwd, T->Project, T->Title};
        Out += T->SessionId.empty() ? "shell" : "tab";
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
    // clod's own window when it's installed next to us; else a plain new-console launch, which
    // Windows hands to the user's default terminal app.
    wchar_t Exe[MAX_PATH] = {};
    GetModuleFileNameW(0, Exe, MAX_PATH);
    std::wstring Window = Exe;
    Window = Window.substr(0, Window.find_last_of(L'\\') + 1) + L"clodw.exe";
    bool32 Own = GetFileAttributesW(Window.c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring Cmd = std::wstring(L"\"") + (Own ? Window : std::wstring(Exe)) + L"\"";
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

// --- the task view: what each claude is doing, from its transcript, and reports on its stops ---

// A session's transcript, <ClaudeConfigDir>\projects\<project>\<id>.jsonl; empty when there's none.
static std::wstring FindTranscript(const std::string& SessionId)
{
    std::wstring Projects = ClaudeConfigDir() + L"\\projects\\";
    std::wstring Name = Utf8ToWide(SessionId) + L".jsonl";
    WIN32_FIND_DATAW Found = {};
    HANDLE Find = FindFirstFileW((Projects + L"*").c_str(), &Found);
    std::wstring Out;
    while(Find != INVALID_HANDLE_VALUE && Out.empty())
    {
        if((Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && Found.cFileName[0] != L'.')
        {
            std::wstring Path = Projects + Found.cFileName + L"\\" + Name;
            if(GetFileAttributesW(Path.c_str()) != INVALID_FILE_ATTRIBUTES) { Out = Path; }
        }
        if(!FindNextFileW(Find, &Found)) { break; }
    }
    if(Find != INVALID_HANDLE_VALUE) { FindClose(Find); }
    return Out;
}

// What the end of a claude transcript says: the last prompt the user typed, the step under
// way (its last tool call, else the text it's writing), and the text it wrote since whatever started
// its turn (a prompt, a task notification): its report when it stops. Subagents' lines are skipped.
struct transcript_look
{
    std::string Prompt;
    std::string Step;
    std::string Text;
};

static transcript_look LookAtTranscript(const std::wstring& Path)
{
    transcript_look L = {};
    HANDLE File = CreateFileW(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    if(File == INVALID_HANDLE_VALUE) { return L; }
    LARGE_INTEGER Size = {};
    GetFileSizeEx(File, &Size);
    u64 Take = Size.QuadPart < (1 << 19) ? (u64)Size.QuadPart : (1 << 19); // the last 512 KB
    std::string Tail = ReadFileRange(File, (u64)Size.QuadPart - Take, (u32)Take);
    CloseHandle(File);
    size_t At = (Take < (u64)Size.QuadPart) ? Tail.find('\n') + 1 : 0; // the first line is cut short
    while(At < Tail.size())
    {
        size_t End = Tail.find('\n', At);
        if(End == std::string::npos) { End = Tail.size(); }
        std::string Line = Tail.substr(At, End - At);
        At = End + 1;
        if(Line.find("\"isSidechain\":true") != std::string::npos) { continue; }
        std::string S;
        if(Line.find("\"type\":\"user\"") != std::string::npos)
        {
            // Something that starts a turn (not a tool's result, not context Claude Code adds). What the
            // user typed is the prompt; tagged text (a task notification, a slash command) isn't.
            if(Line.find("\"tool_result\"") != std::string::npos || Line.find("\"isMeta\":true") != std::string::npos) { continue; }
            L.Text.clear();
            L.Step.clear();
            bool32 Typed = JsonFindString(Line, "content", false, S) || JsonFindString(Line, "text", false, S);
            if(Typed && !S.empty() && S[0] != '<') { L.Prompt = OneLine(S); }
            continue;
        }
        if(Line.find("\"type\":\"assistant\"") == std::string::npos) { continue; }
        if(Line.find("\"type\":\"tool_use\"") != std::string::npos)
        {
            std::string Name;
            std::string Detail;
            JsonFindString(Line, "name", false, Name);
            static const char* Keys[] = {"description", "file_path", "command", "pattern", "url", "query", "prompt"};
            for(const char* Key : Keys) { if(Detail.empty()) { JsonFindString(Line, Key, false, Detail); } }
            L.Step = Name + (Detail.empty() ? std::string() : " \xC2\xB7 " + OneLine(Detail)); // ·
        }
        else if(Line.find("\"type\":\"text\"") != std::string::npos && JsonFindString(Line, "text", false, S))
        {
            L.Text += S + "\n";
            L.Step = "writing \xC2\xB7 " + OneLine(S);
        }
    }
    if(L.Text.size() > 6000)
    {
        size_t Cut = L.Text.size() - 6000;
        while(Cut < L.Text.size() && ((u8)L.Text[Cut] & 0xC0) == 0x80) { Cut += 1; } // not inside a character
        L.Text = L.Text.substr(Cut);
    }
    return L;
}

// The claude tabs' transcripts, looked at on their own thread every second while the task view is
// open: Ids (and Paths where known) in, Paths (found) and Looks out, then Done and the loop's Wake.
struct tasks_scan
{
    std::vector<std::string> Ids;
    std::vector<std::wstring> Paths;
    std::vector<transcript_look> Looks;
    HANDLE Wake;
    volatile LONG Done;
};

static DWORD WINAPI TasksScanThread(void* Param)
{
    tasks_scan* S = (tasks_scan*)Param;
    for(size_t I = 0; I < S->Ids.size(); ++I)
    {
        if(S->Paths[I].empty()) { S->Paths[I] = FindTranscript(S->Ids[I]); }
        S->Looks.push_back(S->Paths[I].empty() ? transcript_look{} : LookAtTranscript(S->Paths[I]));
    }
    InterlockedExchange(&S->Done, 1);
    SetEvent(S->Wake);
    return 0;
}

// A report on a claude's stop, written on its own thread: what it wrote since its turn began, put
// to a small model (`claude -p --model haiku`, no session kept) as a verdict and the one thing to
// know. Id, Path (if known) and Claude (FindClaude) in; Report out (no Text: nothing to report).
struct report_job
{
    std::string Id;
    std::wstring Path;
    std::wstring Claude;
    task_report Report;
    HANDLE Wake;
    volatile LONG Done;
};

static DWORD WINAPI ReportThread(void* Param)
{
    report_job* J = (report_job*)Param;
    std::wstring Path = J->Path.empty() ? FindTranscript(J->Id) : J->Path;
    transcript_look L = Path.empty() ? transcript_look{} : LookAtTranscript(Path);
    std::string Said = OneLine(L.Text);
    if(!Said.empty())
    {
        J->Report.Text = TruncateText(Said, 200); // if the model doesn't answer in shape: its own words
        if(!J->Claude.empty())
        {
            std::string Ask =
                "A coding agent (Claude Code) stopped and reported back to a developer who did not watch it work.\n\n"
                "The developer's request: " + L.Prompt + "\n\n"
                "The agent's report:\n" + L.Text + "\n\n"
                "Judge only from the report whether the work that was asked for got done: success = done and it works; "
                "mixed = partly done, or done with caveats or open problems; failed = not done, or broken; progress = the "
                "report says more work follows. Don't judge wording or style. Names in the report (projects, tools, files) "
                "are correct as written.\n\n"
                "Reply with exactly one line: the verdict word, a colon, then at most 180 characters saying the single most "
                "critical thing the developer needs to know (for success, the result in brief). If the agent asks the "
                "developer something, say what it needs. No markdown, no quotes.";
            // Without a parent session's markers, in %LOCALAPPDATA%\clod (no project's CLAUDE.md), no MCP servers,
            // none of the user's settings (their hooks and plugins double the time it takes).
            std::wstring Env;
            wchar_t* Block = GetEnvironmentStringsW();
            for(const wchar_t* Var = Block; *Var; Var += wcslen(Var) + 1)
            {
                const wchar_t* Eq = wcschr(Var + 1, L'=');
                size_t NameLen = Eq ? (size_t)(Eq - Var) : wcslen(Var);
                bool32 Drop = false;
                for(const wchar_t* M : ClaudeMarkers) { if(wcslen(M) == NameLen && _wcsnicmp(Var, M, NameLen) == 0) { Drop = true; } }
                if(!Drop) { Env.append(Var); Env.push_back(0); }
            }
            FreeEnvironmentStringsW(Block);
            Env.push_back(0);
            wchar_t Local[MAX_PATH] = {};
            GetEnvironmentVariableW(L"LOCALAPPDATA", Local, MAX_PATH);
            std::wstring Dir = std::wstring(Local) + L"\\clod";
            CreateDirectoryW(Dir.c_str(), 0);
            std::string Out = RunCapture(J->Claude + L" -p --model haiku --no-session-persistence --strict-mcp-config --setting-sources project", Ask, Env, Dir, 90000);
            // The first line that starts with a verdict and a colon.
            static const char* Verdicts[] = {"success", "mixed", "failed", "progress"};
            for(size_t At = 0; At < Out.size();)
            {
                size_t End = Out.find('\n', At);
                if(End == std::string::npos) { End = Out.size(); }
                std::string Line = OneLine(Out.substr(At, End - At));
                At = End + 1;
                for(s32 V = 0; V < 4; ++V)
                {
                    size_t Len = strlen(Verdicts[V]);
                    if(ToLower(Line).compare(0, Len, Verdicts[V]) != 0 || Line.size() <= Len || Line[Len] != ':') { continue; }
                    std::string Text = Line.substr(Len + 1);
                    while(!Text.empty() && Text[0] == ' ') { Text.erase(0, 1); }
                    if(Text.empty()) { continue; }
                    J->Report.Verdict = VERDICT_SUCCESS + V;
                    J->Report.Text = TruncateText(Text, 200);
                    At = Out.size();
                    break;
                }
            }
        }
    }
    InterlockedExchange(&J->Done, 1);
    SetEvent(J->Wake);
    return 0;
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

    RaiseForTyping(GetCurrentProcess());
    HANDLE Wake = CreateEventW(0, FALSE, FALSE, 0);
    HANDLE ConnectEvent = CreateEventW(0, TRUE, FALSE, 0);
    // Wakes the loop exactly when a held-back frame may go out (plain wait timeouts round up to ~16 ms).
    HANDLE FrameTimer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
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
    App.PrefixKey = LoadPrefixKey();
    LoadVsEnv(App); // before the saved tabs start
    App.Sessions = ScanSessions(std::vector<session_info>());

    SeedUserSetup(); // first run: the user's own copy of the default shell setup and settings
    InstallFonts();  // Iosevka Clod for every app, if it isn't installed yet

    // `clod --test`: a throwaway instance for trying UI changes next to the real one. No tray, no
    // restore file, and it ends together with its window.
    bool32 Ephemeral = GetEnvironmentVariableW(L"CLOD_EPHEMERAL", 0, 0) > 0;

    static tray_state Tray = {};
    if(!Ephemeral) { TrayCreate(Tray, &App); }

    // --- restore tabs left open by a server that didn't quit cleanly (reboot, crash, kill) ---
    if(!Ephemeral)
    {
        std::string Saved = ReadTabsFile();

        // --- let claudes from the previous server finish exiting first (a quick quit-and-reopen):
        // resumed next to a still-live process of the same session, Claude renames the session ---
        {
            std::vector<std::string> Resuming;
            for(size_t At = 0; At < Saved.size();)
            {
                size_t End = Saved.find('\n', At);
                if(End == std::string::npos) { End = Saved.size(); }
                std::vector<std::string> F = SplitTabs(Saved.substr(At, End - At));
                if(F.size() >= 2 && F[0] == "tab") { Resuming.push_back(F[1]); }
                At = End + 1;
            }
            std::vector<HANDLE> Still;
            WIN32_FIND_DATAW Found = {};
            HANDLE Find = Resuming.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((ClaudeConfigDir() + L"\\sessions\\*.json").c_str(), &Found);
            while(Find != INVALID_HANDLE_VALUE)
            {
                DWORD Pid = (DWORD)_wtoi(Found.cFileName);
                std::string Id;
                std::string Cwd;
                if(Pid && ReadClaudeSession(Pid, Id, Cwd) && std::find(Resuming.begin(), Resuming.end(), Id) != Resuming.end() && Still.size() < MAXIMUM_WAIT_OBJECTS)
                {
                    HANDLE Proc = OpenProcess(SYNCHRONIZE, FALSE, Pid);
                    if(Proc) { Still.push_back(Proc); }
                }
                if(!FindNextFileW(Find, &Found)) { FindClose(Find); break; }
            }
            if(!Still.empty()) { WaitForMultipleObjects((DWORD)Still.size(), Still.data(), TRUE, 15000); }
            for(HANDLE Proc : Still) { CloseHandle(Proc); }
        }
        s32 SavedActive = 0;
        s32 SavedPage = 0;
        std::vector<std::string> SavedLayout;
        std::vector<std::vector<std::string>> SavedPages;
        size_t At = 0;
        while(At < Saved.size())
        {
            size_t End = Saved.find('\n', At);
            if(End == std::string::npos) { End = Saved.size(); }
            std::vector<std::string> F = SplitTabs(Saved.substr(At, End - At));
            At = End + 1;
            if(F.size() >= 2 && F[0] == "active") { SavedActive = atoi(F[1].c_str()); }
            if(F.size() >= 3 + MAX_PANES && F[0] == "layout") { SavedLayout = F; }
            if(F.size() >= 2 && F[0] == "pages") { SavedPage = atoi(F[1].c_str()); }
            if(F.size() >= 3 + MAX_PANES && F[0] == "page") { SavedPages.push_back(F); }
            if(F.size() >= 3 && F[0] == "monitor") { App.MonitorLayouts.push_back(monitor_layout{F[2], Clamp(atoi(F[1].c_str()), 0, LAYOUT_MAIN)}); }
            // A claude tab resumes its session. A shell can't be resumed; it comes back as a fresh
            // one in the same folder, as does a claude that never got a prompt (no session file).
            if(F.size() >= 5 && (F[0] == "tab" || F[0] == "shell"))
            {
                bool32 Claude = (F[0] == "tab" && SessionFileExists(F[1]));
                OpenTab(App, !Claude, F[1], Claude ? std::string() : F[1], F[2], F[3], Claude ? F[4] : std::string(), Job, Wake, GetTickCount64());
            }
        }
        // --- the saved layout, with each pane's tab when that tab came back (else the home screen) ---
        if(!SavedLayout.empty())
        {
            App.Layout = Clamp(atoi(SavedLayout[1].c_str()), 0, LAYOUT_MAIN);
            for(s32 P = 0; P < PaneCount(App.Layout); ++P)
            {
                s32 Index = atoi(SavedLayout[3 + P].c_str());
                tab* T = (Index > 0 && Index <= (s32)App.Tabs.size()) ? App.Tabs[Index - 1] : 0;
                App.PaneTab[P] = (PaneShowing(App, T) < 0) ? T : 0;
            }
            FocusPane(App, atoi(SavedLayout[2].c_str()));
        }
        else
        {
            SwitchTab(App, Clamp(SavedActive, 0, (s32)App.Tabs.size()));
        }
        // --- the saved pages: each pane's tab when it came back and no earlier pane took it ---
        if(SavedPages.size() > 1)
        {
            std::vector<tab*> Taken;
            for(const std::vector<std::string>& F : SavedPages)
            {
                page Pg = {};
                Pg.Layout = Clamp(atoi(F[1].c_str()), 0, LAYOUT_MAIN);
                Pg.Focus = Clamp(atoi(F[2].c_str()), 0, PaneCount(Pg.Layout) - 1);
                for(s32 P = 0; P < PaneCount(Pg.Layout); ++P)
                {
                    s32 Index = atoi(F[3 + P].c_str());
                    tab* T = (Index > 0 && Index <= (s32)App.Tabs.size()) ? App.Tabs[Index - 1] : 0;
                    if(T && std::find(Taken.begin(), Taken.end(), T) == Taken.end()) { Pg.PaneTab[P] = T; Taken.push_back(T); }
                }
                App.Pages.push_back(Pg);
            }
            App.Page = Clamp(SavedPage, 0, (s32)App.Pages.size() - 1);
            LoadPage(App, App.Pages[App.Page]);
        }
    }
    if(!ActiveTab(App)) { App.HomeOpen = true; } // nothing in the focused pane: start on the home screen
    std::string SavedTabs = TabsFileContents(App);
    if(!Ephemeral) { WriteTabsFile(SavedTabs); }

    // --- saved output of tabs that aren't coming back ---
    if(!Ephemeral)
    {
        std::wstring Dir = HistoryPath("*");
        Dir = Dir.substr(0, Dir.find_last_of(L'\\') + 1);
        WIN32_FIND_DATAW Found = {};
        HANDLE Find = FindFirstFileW((Dir + L"*.txt").c_str(), &Found);
        while(Find != INVALID_HANDLE_VALUE)
        {
            std::wstring Name = Found.cFileName;
            bool32 Kept = false;
            for(tab* T : App.Tabs) { if(Utf8ToWide(T->HistoryId) + L".txt" == Name) { Kept = true; } }
            if(!Kept) { DeleteFileW((Dir + Name).c_str()); }
            if(!FindNextFileW(Find, &Found)) { FindClose(Find); break; }
        }
    }

    frame Prev = {};
    frame Next = {};
    bool32 ForceFull = true;
    bool32 Dirty = true;
    u64 StartMs = GetTickCount64();
    u64 LastFrameUs = 0;        // last frame that carried changes (QpcUs)
    u64 ResizeAtMs = 0;         // pending child resize, applied once the window settles
    u64 SyncSinceMs = 0;        // when the active child began a synchronized update
    u64 LaunchedWindowAtMs = 0; // tray opened a window that hasn't attached yet
    u64 ClaudeScanMs = 0;       // last look for claude processes inside the tabs
    claude_scan* Scan = 0;      // a look under way on its own thread
    std::map<std::pair<DWORD, u64>, u64> CpuKnown; // the tabs' processes' CPU times at the last scan (tab::Cpu)
    u64 CpuAtFt = 0;
    u64 SessionsScanMs = 0;     // last re-read of the Claude Code sessions (SessionsScanThread)
    sessions_scan* SessionsScan = 0;
    u64 TasksScanMs = 0;        // last look at the claude tabs' transcripts (TasksScanThread, task view open)
    tasks_scan* TasksScan = 0;
    report_job* ReportJob = 0;  // a report being written (ReportThread), one at a time
    std::vector<std::pair<std::string, u64>> ReportQueue; // sessions that stopped, and when to start their report
    u64 AppsScanMs = 0;         // last look at the tray apps' icons (AppsScanThread)
    apps_scan* AppsScan = 0;
    u64 HistorySavedMs = GetTickCount64(); // last save of the shell tabs' output (not at once: restored tabs still have to type theirs)
    std::string OuterTitle;
    std::vector<u32> OuterWide;  // sorted codepoints the attached console draws 2 cells wide where CharWidth says 1
    bool32 ClientCells = false;  // the attached client is clodw: it gets MSG_CELLS instead of VT
    s32 OuterMouse = -1;        // mouse tracking mode currently requested from the client's terminal
    tab* FocusedTab = 0;        // compared by address only; may dangle after a close
    std::string TrayTip;
    std::vector<app_command> Cmds;
    std::string InBytes;
    std::string OutBytes;
    std::string Payload;

    while(!App.Quit)
    {
        HANDLE Waits[4] = {Wake, ConnectEvent, FrameTimer, Client.ReadEvent};
        DWORD WaitCount = Client.Pipe ? 4 : 3;
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
                u32 Version = 0;
                if(Type == MSG_ATTACH && Payload.size() >= sizeof(Version)) { memcpy(&Version, Payload.data(), sizeof(Version)); }
                if(Type == MSG_ATTACH && (Version != CLOD_PROTOCOL_VERSION || Payload.size() < sizeof(msg_attach)))
                {
                    // A window from another build would misread everything that follows.
                    DropClient(Client, "this window is from a different clod build than the running server; quit clod from the tray and open it again");
                    Attached = false;
                    ClientWindow = 0;
                    break;
                }
                if(Type == MSG_ATTACH)
                {
                    msg_attach A = {};
                    memcpy(&A, Payload.data(), sizeof(A));
                    App.LaunchCwd = Payload.substr(sizeof(A));
                    App.PrefixKey = LoadPrefixKey(); // settings.txt may have changed since
                    App.Width = Max(A.Width, 1);
                    App.Height = Max(A.Height, 2);
                    if(A.JumpToTab) { SwitchTab(App, A.JumpToTab - 1); }
                    App.Sessions = ScanSessions(App.Sessions);
                    ClientWindow = (HWND)(uintptr_t)A.ConsoleWindow;
                    ClientCells = (A.Flags & ATTACH_CELLS) != 0;
                    Attached = true;
                    App.WindowUnfocused = false; // a window that just attached has the focus
                    LaunchedWindowAtMs = 0;
                    // A fresh terminal: assume nothing about what it shows or which modes it has.
                    Prev = frame{};
                    OuterMouse = -1;
                    OuterTitle.clear();
                    OuterWide.clear();
                    ForceFull = true;
                    ResizeAtMs = NowMs;
                    Dirty = true;
                }
                else if(Type == MSG_MONITOR && Attached && Payload != App.Monitor)
                {
                    // The window is on another monitor: use the layout that monitor last had.
                    App.Monitor = Payload;
                    for(const monitor_layout& M : App.MonitorLayouts)
                    {
                        if(M.Monitor == App.Monitor && M.Layout != App.Layout) { SetLayout(App, M.Layout); }
                    }
                    Dirty = true;
                }
                else if(Type == MSG_WIDTHS && Attached)
                {
                    // The window's own character widths: repaint everything with them.
                    OuterWide.assign((const u32*)Payload.data(), (const u32*)(Payload.data() + Payload.size() / 4 * 4));
                    std::sort(OuterWide.begin(), OuterWide.end());
                    ForceFull = true;
                    Dirty = true;
                }
                else if(Type == MSG_INPUT && Attached)
                {
                    LatencyMark("server_in");
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
                case CMD_WRITE: PtyWrite(C.Tab->Pty, C.Bytes); LatencyMark("server_write"); break;
                case CMD_OPEN:
                {
                    OpenTab(App, false, C.SessionId, std::string(), C.Cwd, C.Project, C.Title, Job, Wake, NowMs);
                    SwitchTab(App, (s32)App.Tabs.size());
                } break;
                case CMD_OPEN_SHELL:
                {
                    OpenTab(App, true, std::string(), std::string(), C.Cwd, ProjectName(C.Cwd), std::string(), Job, Wake, NowMs);
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
                    OpenTab(App, false, std::string(), std::string(), Cwd, ProjectName(Cwd), std::string(), Job, Wake, NowMs);
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
                    StartTab(*C.Tab, Max(C.Tab->Term.Cols, 1), Max(C.Tab->Term.Rows, 1), Job, Wake, App, NowMs);
                } break;
                case CMD_CLOSE: CloseTab(App, C.Tab); break;
                case CMD_VIEW_STEP: if(!C.Tab->ViewImage.empty()) { ViewImageAt(App, *C.Tab, C.Tab->ViewImage, C.Step, NowMs); } break;
                case CMD_FOCUS_APP:
                {
                    // The app's main window (Electron's, with a title; hidden while it sits in the tray):
                    // shown, restored and brought to the front. The clicking window allowed the switch
                    // (AllowSetForegroundWindow), since this server is never in the foreground itself.
                    if(C.Step < 0 || C.Step >= (s32)App.AppsWanting.size()) { break; }
                    struct find_app { std::wstring Path; HWND Visible; HWND Hidden; };
                    find_app Find = {Utf8ToWide(App.AppsWanting[C.Step].Path), 0, 0};
                    EnumWindows([](HWND Wnd, LPARAM Param) -> BOOL
                    {
                        find_app& F = *(find_app*)Param;
                        wchar_t Class[64] = {};
                        GetClassNameW(Wnd, Class, ArrayCount(Class));
                        if(wcscmp(Class, L"Chrome_WidgetWin_1") != 0 || GetWindow(Wnd, GW_OWNER) || !GetWindowTextLengthW(Wnd)) { return TRUE; }
                        DWORD Pid = 0;
                        GetWindowThreadProcessId(Wnd, &Pid);
                        HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);
                        wchar_t Path[MAX_PATH] = {};
                        DWORD PathLen = MAX_PATH;
                        if(Process) { QueryFullProcessImageNameW(Process, 0, Path, &PathLen); CloseHandle(Process); }
                        if(_wcsicmp(Path, F.Path.c_str()) != 0) { return TRUE; }
                        if(IsWindowVisible(Wnd)) { F.Visible = Wnd; return FALSE; }
                        if(!F.Hidden) { F.Hidden = Wnd; }
                        return TRUE;
                    }, (LPARAM)&Find);
                    HWND Wnd = Find.Visible ? Find.Visible : Find.Hidden;
                    if(!Wnd) { break; }
                    if(!IsWindowVisible(Wnd)) { ShowWindow(Wnd, SW_SHOW); }
                    if(IsIconic(Wnd)) { ShowWindow(Wnd, SW_RESTORE); }
                    SetForegroundWindow(Wnd);
                } break;
                case CMD_RESCAN:
                {
                    App.Sessions = ScanSessions(App.Sessions);
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
        std::vector<tab*> Ended;
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
                // --- `clod view <file or folder>` ran in this tab: the image covers the tab until a key
                // or click; arrows step through the folder's images (CMD_VIEW_STEP) ---
                if(!T->Term.View.empty())
                {
                    ViewImageAt(App, *T, T->Term.View, 0, NowMs);
                    T->Term.View.clear();
                }
                // --- new output off screen (the home screen says so): only when it added lines or moved
                // the cursor to another row since the tab was last seen; clink redrawing its prompt in
                // place (git status arriving) or ConPTY repainting after a resize doesn't count ---
                if(PaneShowing(App, T) >= 0 || NowMs < T->RepaintUntilMs)
                {
                    T->SeenLinesPushed = T->Term.ScrollbackPushed;
                    T->SeenCursorY = T->Term.CursorY;
                }
                else if(T->Term.ScrollbackPushed != T->SeenLinesPushed || T->Term.CursorY != T->SeenCursorY) { T->Activity = true; }
                if(T == ActiveTab(App) && T->Term.SyncOutput && !WasSync) { SyncSinceMs = NowMs; }
                Dirty = true;

                // --- reasons to notify: bell, OSC 9/777, or claude's title spinner stopping ---
                size_t TitleAt = 0;
                u32 First = T->Term.Title.empty() ? 0 : DecodeUtf8(T->Term.Title.data(), T->Term.Title.size(), &TitleAt);
                bool32 Busy = (First >= 0x25D0 && First <= 0x25D3); // ◐◑◒◓
                if(T->TitleBusy && !Busy && First == 0x2733) { Why = "Claude finished"; } // ✳
                if(Busy && !T->TitleBusy) { T->NeedsInput = false; } // a new turn: whatever it asked is settled
                if(Busy != T->TitleBusy) { T->TurnSinceMs = NowMs; }
                // Every stop gets a report for the task view, once its last words are in the transcript.
                if(T->TitleBusy && !Busy && !T->SessionId.empty() && ReadSetting("task_summaries") != "off")
                {
                    ReportQueue.push_back({T->SessionId, NowMs + 2000});
                    T->Summarizing = true;
                }
                T->TitleBusy = Busy;
                if(T->Term.Bell)
                {
                    ToClient += "\x07";
                    T->Term.Bell = false;
                    Why = T->SessionId.empty() ? "Bell" : "Claude needs your attention";
                }
                if(!T->Term.Notify.empty())
                {
                    Why = T->Term.Notify;
                    T->Term.Notify.clear();
                }
            }
            // cmd ending (exit typed in the shell) ends the tab.
            if(!T->Exited && WaitForSingleObject(T->Pty->Process, 0) == WAIT_OBJECT_0)
            {
                T->Exited = true;
                Dirty = true;
                Ended.push_back(T);
            }

            // Unseen: no window, the tab isn't in a pane, or the window is behind others. clodw also
            // flashes its taskbar button (it does that on BEL while not in the foreground).
            HWND ClientRoot = ClientWindow ? GetAncestor(ClientWindow, GA_ROOTOWNER) : 0;
            bool32 Unseen = !Attached || PaneShowing(App, T) < 0 || (ClientRoot && GetForegroundWindow() != ClientRoot);
            if(!Why.empty() && ClientCells && Why != "Bell" && Why != "Claude needs your attention") { ToClient += "\x07"; }
            // Claude ringing or notifying (a permission prompt, a question) waits for the user: the home
            // screen lists it under "Needs input" until something is typed into it or a turn starts.
            if(!T->SessionId.empty() && !Why.empty() && Why != "Claude finished")
            {
                if(!T->NeedsInput) { T->WantsSinceMs = NowMs; } // waiting since the first ask
                T->NeedsInput = true;
            }
            // Off screen it waits on the home screen and the top bar until some pane shows it.
            if(!Why.empty() && PaneShowing(App, T) < 0)
            {
                if(T->Notice.empty() && !T->NeedsInput) { T->WantsSinceMs = NowMs; }
                T->Notice = (Why == "Claude finished") ? "finished" : (Why == "Claude needs your attention") ? "needs you" : (Why == "Bell") ? "bell" : Why;
            }
            if(!Why.empty() && Unseen && NowMs - T->NotifiedAtMs > 10000)
            {
                if(ReadSetting("notifications") != "off") { TrayNotify(Tray, TabName(*T), Why, I + 1); } // clodw's window menu toggles it
                T->NotifiedAtMs = NowMs;
            }
        }
        for(tab* T : Ended) { CloseTab(App, T); }

        // --- which tabs are running claude (ClaudeScanThread) ---
        // A snapshot of every process takes tens of milliseconds, so it runs on its own thread and
        // the loop (and typing) never waits for it; its answer is applied once it's in.
        if(Scan && Scan->Done)
        {
            // Each tab's CPU, as a share of the whole machine since the last scan.
            u64 Elapsed = Scan->PrevAtFt && Scan->AtFt > Scan->PrevAtFt ? Scan->AtFt - Scan->PrevAtFt : 0;
            u64 Cores = Max((s32)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1);
            CpuKnown.swap(Scan->Cpu);
            CpuAtFt = Scan->AtFt;
            for(tab* T : App.Tabs)
            {
                if(!T->Pty || T->Exited) { continue; }
                const claude_find* Find = 0;
                for(const claude_find& F : Scan->Finds) { if(F.CmdPid == GetProcessId(T->Pty->Process)) { Find = &F; } }
                if(!Find) { continue; } // opened after the scan started
                if(Elapsed)
                {
                    float Cpu = (float)(Find->CpuUsed * 100.0 / ((double)Elapsed * Cores));
                    if((s32)(Cpu + 0.5f) != (s32)(T->Cpu + 0.5f)) { Dirty = true; }
                    T->Cpu = Cpu;
                }
                // A shell's folder follows its cd, so it comes back there after a restart (tabs.txt).
                if(T->SessionId.empty() && !Find->Pid && !Find->ShellCwd.empty() && Find->ShellCwd != T->Cwd)
                {
                    T->Cwd = Find->ShellCwd;
                    T->Project = ProjectName(Find->ShellCwd);
                    Dirty = true;
                }
                if(Find->Pid)
                {
                    if(Find->Id != T->SessionId)
                    {
                        T->SessionId = Find->Id;
                        T->Title.clear();
                        T->TranscriptPath.clear();
                        T->TaskPrompt.clear();
                        T->TaskStep.clear();
                        Dirty = true;
                    }
                    if(!Find->Cwd.empty() && Find->Cwd != T->Cwd) { T->Cwd = Find->Cwd; T->Project = ProjectName(Find->Cwd); Dirty = true; }
                    T->ClaudePid = Find->Pid;
                    T->LaunchedAtMs = 0;
                }
                else if(T->ClaudePid || (T->LaunchedAtMs && NowMs - T->LaunchedAtMs > 30000))
                {
                    // Claude left (or the one the server started never showed up): back to a shell.
                    T->ClaudePid = 0;
                    T->LaunchedAtMs = 0;
                    T->SessionId.clear();
                    T->Title.clear();
                    T->TitleBusy = false;
                    Dirty = true;
                }
            }
            delete Scan;
            Scan = 0;
        }
        if(!Scan && !App.Tabs.empty() && NowMs - ClaudeScanMs >= 1000)
        {
            ClaudeScanMs = NowMs;
            Scan = new claude_scan();
            Scan->Wake = Wake;
            Scan->Cpu = CpuKnown;
            Scan->PrevAtFt = CpuAtFt;
            for(tab* T : App.Tabs) { if(T->Pty && !T->Exited) { Scan->CmdPids.push_back(GetProcessId(T->Pty->Process)); } }
            HANDLE Thread = CreateThread(0, 0, ClaudeScanThread, Scan, 0, 0);
            if(Thread) { CloseHandle(Thread); }
            else { delete Scan; Scan = 0; }
        }
        // --- the Claude Code sessions, re-read every 5 s while a window is attached, so the home
        // screen is current when it opens; the selection stays on the same session ---
        if(SessionsScan && SessionsScan->Done)
        {
            std::vector<session_info>& New = SessionsScan->Sessions;
            bool32 Changed = New.size() != App.Sessions.size();
            for(size_t I = 0; I < New.size() && !Changed; ++I)
            {
                Changed = New[I].Id != App.Sessions[I].Id || New[I].ModifiedTime != App.Sessions[I].ModifiedTime || New[I].Title != App.Sessions[I].Title;
            }
            if(Changed)
            {
                s32 Running = HomeTabCount(App);
                std::vector<s32> Filtered = FilterSessions(App);
                s32 Index = App.Selected - Running;
                std::string Kept = (Index >= 0 && Index < (s32)Filtered.size()) ? App.Sessions[Filtered[Index]].Id : std::string();
                App.Sessions.swap(New);
                if(!Kept.empty())
                {
                    Filtered = FilterSessions(App);
                    for(s32 I = 0; I < (s32)Filtered.size(); ++I) { if(App.Sessions[Filtered[I]].Id == Kept) { App.Selected = Running + I; } }
                }
                Dirty = true;
            }
            delete SessionsScan;
            SessionsScan = 0;
        }
        if(!SessionsScan && Attached && NowMs - SessionsScanMs >= 5000)
        {
            SessionsScanMs = NowMs;
            SessionsScan = new sessions_scan();
            SessionsScan->Known = App.Sessions;
            SessionsScan->Wake = Wake;
            HANDLE Thread = CreateThread(0, 0, SessionsScanThread, SessionsScan, 0, 0);
            if(Thread) { CloseHandle(Thread); }
            else { delete SessionsScan; SessionsScan = 0; }
        }
        // --- the task view: each claude's prompt and step from its transcript, every second while it's
        // open (TasksScanThread) ---
        if(TasksScan && TasksScan->Done)
        {
            for(size_t I = 0; I < TasksScan->Ids.size(); ++I)
            {
                for(tab* T : App.Tabs)
                {
                    if(T->SessionId != TasksScan->Ids[I]) { continue; }
                    const transcript_look& L = TasksScan->Looks[I];
                    if(L.Prompt != T->TaskPrompt || L.Step != T->TaskStep) { Dirty = true; }
                    T->TranscriptPath = WideToUtf8(TasksScan->Paths[I]);
                    T->TaskPrompt = L.Prompt;
                    T->TaskStep = L.Step;
                }
            }
            delete TasksScan;
            TasksScan = 0;
        }
        if(!TasksScan && Attached && App.TasksOpen && NowMs - TasksScanMs >= 1000)
        {
            TasksScanMs = NowMs;
            TasksScan = new tasks_scan();
            TasksScan->Wake = Wake;
            for(tab* T : App.Tabs)
            {
                if(T->SessionId.empty()) { continue; }
                TasksScan->Ids.push_back(T->SessionId);
                TasksScan->Paths.push_back(Utf8ToWide(T->TranscriptPath));
            }
            HANDLE Thread = CreateThread(0, 0, TasksScanThread, TasksScan, 0, 0);
            if(Thread) { CloseHandle(Thread); }
            else { delete TasksScan; TasksScan = 0; }
        }
        // --- reports on the claudes' stops (ReportThread), one at a time, each a few seconds after its
        // stop; they queue on the tab as unread ---
        if(ReportJob && ReportJob->Done)
        {
            for(tab* T : App.Tabs)
            {
                if(T->SessionId != ReportJob->Id) { continue; }
                bool32 More = false;
                for(const std::pair<std::string, u64>& Q : ReportQueue) { More = More || Q.first == T->SessionId; }
                T->Summarizing = More;
                if(!ReportJob->Report.Text.empty())
                {
                    ReportJob->Report.AtMs = NowMs;
                    T->Reports.push_back(ReportJob->Report);
                    if(T->Reports.size() > 20) { T->Reports.erase(T->Reports.begin()); }
                }
                Dirty = true;
            }
            delete ReportJob;
            ReportJob = 0;
        }
        if(!ReportJob && !ReportQueue.empty() && NowMs >= ReportQueue[0].second)
        {
            ReportJob = new report_job();
            ReportJob->Id = ReportQueue[0].first;
            ReportJob->Claude = FindClaude();
            ReportJob->Wake = Wake;
            for(tab* T : App.Tabs) { if(T->SessionId == ReportJob->Id) { ReportJob->Path = Utf8ToWide(T->TranscriptPath); } }
            ReportQueue.erase(ReportQueue.begin());
            HANDLE Thread = CreateThread(0, 0, ReportThread, ReportJob, 0, 0);
            if(Thread) { CloseHandle(Thread); }
            else { delete ReportJob; ReportJob = 0; }
        }
        // --- tray apps with unread messages (AppsScanThread), every 2 s while a window is attached ---
        if(AppsScan && AppsScan->Done)
        {
            if(AppsScan->Wanting != App.AppsWanting) { App.AppsWanting.swap(AppsScan->Wanting); Dirty = true; }
            delete AppsScan;
            AppsScan = 0;
        }
        if(!AppsScan && Attached && NowMs - AppsScanMs >= 2000)
        {
            AppsScanMs = NowMs;
            AppsScan = new apps_scan();
            AppsScan->Wake = Wake;
            HANDLE Thread = CreateThread(0, 0, AppsScanThread, AppsScan, 0, 0);
            if(Thread) { CloseHandle(Thread); }
            else { delete AppsScan; AppsScan = 0; }
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

        // --- child sizes: a tab on screen matches its pane (layout and pane changes apply at once);
        // after the window has been still for a moment, tabs off screen take the focused pane's size,
        // where they'd appear next ---
        bool32 Settled = ResizeAtMs && NowMs - ResizeAtMs >= 60;
        if(!ResizeAtMs || Settled)
        {
            ResizeAtMs = 0;
            pane_rect FocusRect = PaneRect(App, App.Focus);
            for(tab* T : App.Tabs)
            {
                s32 Pane = PaneShowing(App, T);
                if(Pane < 0 && !Settled) { continue; }
                pane_rect R = (Pane >= 0) ? PaneRect(App, Pane) : FocusRect;
                if(T->Term.Cols == R.W && T->Term.Rows == R.H) { continue; }
                VtResize(T->Term, R.W, R.H);
                T->SeenLinesPushed = T->Term.ScrollbackPushed;
                T->SeenCursorY = T->Term.CursorY;
                T->RepaintUntilMs = NowMs + 500;
                if(T->Pty)
                {
                    COORD Size = {(SHORT)R.W, (SHORT)R.H};
                    ResizePseudoConsole(T->Pty->Console, Size);
                }
                Dirty = true;
            }
        }

        // --- tray tooltip and the restore file follow the tab set ---
        std::string Tip = "clod - " + std::to_string(App.Tabs.size()) + " session" + (App.Tabs.size() == 1 ? "" : "s");
        if(Tip != TrayTip)
        {
            TraySetTip(Tray, Tip);
            TrayTip = Tip;
        }
        std::string Tabs = TabsFileContents(App);
        if(!Ephemeral && NowMs - HistorySavedMs >= 30000)
        {
            for(tab* T : App.Tabs) { SaveHistory(*T); } // also covers a crash or reboot
            HistorySavedMs = NowMs;
        }
        if(Tabs != SavedTabs && !Ephemeral)
        {
            WriteTabsFile(Tabs);
            SavedTabs = Tabs;
        }

        // --- present to the attached window ---
        tab* Active = ActiveTab(App);
        bool32 HoldForSync = Active && Active->Term.SyncOutput && NowMs - SyncSinceMs < 100;
        // At most one frame per 8 ms (a flood of output), but a frame after a quiet spell (an echo)
        // goes out at once; frames that changed nothing don't count.
        u64 NowUs = QpcUs();
        bool32 FrameDue = NowUs - LastFrameUs >= 8000;
        if(Attached && Dirty && !HoldForSync && !FrameDue)
        {
            LARGE_INTEGER Due = {};
            Due.QuadPart = -(s64)(8000 - (NowUs - LastFrameUs)) * 10; // relative, in 100 ns
            SetWaitableTimer(FrameTimer, &Due, 0, 0, 0, FALSE);
        }
        if(Attached && Dirty && !HoldForSync && FrameDue)
        {
            FILETIME Ft = {};
            GetSystemTimeAsFileTime(&Ft);
            u64 NowFt = ((u64)Ft.dwHighDateTime << 32) | Ft.dwLowDateTime;
            App.WindowDrawsImages = ClientCells;
            Compose(App, NowMs, NowFt, Next, App.BarHits);

            OutBytes.clear();
            // Track motion only when the active child wants hover events; otherwise clicks, drags and wheel.
            s32 WantMouse = App.MouseCaptureOff ? 0 : ((Active && Active->Term.MouseMode == 1003) ? 1003 : 1002);
            std::string Name = Ephemeral ? "clod TEST" : "clod";
            std::string Title = Active ? Name + " - " + TabName(*Active) : Name;
            bool32 HeaderChanged = (WantMouse != OuterMouse || Title != OuterTitle);
            if(ClientCells)
            {
                // clodw draws the cells itself: no VT, no width guessing.
                if(EncodeCells(OutBytes, Prev, Next, ForceFull, WantMouse, App.PrefixKey, Title, HeaderChanged))
                {
                    LatencyMark("server_frame");
                    if(!IpcSendString(Client, MSG_CELLS, OutBytes)) { IpcClose(Client); Attached = false; }
                }
            }
            else
            {
                if(WantMouse != OuterMouse)
                {
                    OutBytes += "\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?1006l";
                    if(WantMouse) { OutBytes += "\x1b[?1000h\x1b[?" + std::to_string(WantMouse) + "h\x1b[?1006h"; }
                }
                if(Title != OuterTitle) { OutBytes += "\x1b]0;" + Title + "\x07"; }
                RenderFrame(OutBytes, Prev, Next, ForceFull, OuterWide);
                if(!OutBytes.empty() && !IpcSendString(Client, MSG_OUTPUT, OutBytes)) { IpcClose(Client); Attached = false; }
            }
            OuterMouse = WantMouse;
            OuterTitle = Title;
            std::swap(Prev, Next);
            ForceFull = false;
            Dirty = false;
            if(!OutBytes.empty()) { LastFrameUs = NowUs; }
        }
        // Status messages expire on their own.
        if(!App.Status.empty() && NowMs >= App.StatusUntil) { App.Status.clear(); Dirty = true; }
        // The command bar's clock, in the user's own date and time formats; redrawn when the minute turns.
        {
            SYSTEMTIME Local = {};
            GetLocalTime(&Local);
            wchar_t Day[64] = {};
            wchar_t Time[64] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &Local, L"ddd MMM d", Day, ArrayCount(Day), 0);
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &Local, 0, Time, ArrayCount(Time));
            std::string Clock = WideToUtf8(std::wstring(Day) + L"  " + Time);
            if(Clock != App.Clock) { App.Clock = Clock; Dirty = true; }
        }
        if(App.CycleShownUntil && NowMs >= App.CycleShownUntil) { App.CycleShownUntil = 0; Dirty = true; } // the tab switcher box goes
    }

    // --- shutdown: tell the window, then hang up every child at once so they exit in parallel ---
    // The tab list is left on disk on purpose: the next launch reopens every tab with --resume,
    // and shell tabs show their saved output again.
    if(!Ephemeral) { for(tab* T : App.Tabs) { SaveHistory(*T); } }
    std::string Count = std::to_string(App.Tabs.size()) + " tab" + (App.Tabs.size() == 1 ? "" : "s");
    DropClient(Client, Ephemeral ? "test instance ended" : "clod quit from the tray; " + Count + " will reopen the next time you run clod");
    if(!Ephemeral) { TrayDestroy(Tray); }
    // Let go of the pipe now: a quick reopen starts the next server at once instead of waiting
    // on this one (which then waits out these claudes before resuming the same sessions).
    CloseHandle(Listen);
    Listen = INVALID_HANDLE_VALUE;
    for(tab* T : App.Tabs) { if(T->Pty) { ClosePseudoConsole(T->Pty->Console); T->Pty->Console = 0; } }
    // Give each claude a moment to exit on its own (saving, and removing its session file) before
    // the job takes down whatever is left.
    std::vector<HANDLE> Claudes;
    for(tab* T : App.Tabs)
    {
        HANDLE Proc = T->ClaudePid ? OpenProcess(SYNCHRONIZE, FALSE, T->ClaudePid) : 0;
        if(Proc && Claudes.size() < MAXIMUM_WAIT_OBJECTS) { Claudes.push_back(Proc); }
        else if(Proc) { CloseHandle(Proc); }
    }
    if(!Claudes.empty()) { WaitForMultipleObjects((DWORD)Claudes.size(), Claudes.data(), TRUE, 5000); }
    for(HANDLE Proc : Claudes) { CloseHandle(Proc); }
    for(tab* T : App.Tabs)
    {
        PtyClose(T->Pty);
        delete T;
    }
    CloseHandle(ConnectEvent);
    CloseHandle(Wake);
    CloseHandle(Job);
    return 0;
}
