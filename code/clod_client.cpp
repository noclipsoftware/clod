// Client: the console window. Relays console input to the server and server output to the console.
// Closing the window only ends this process; the server keeps every tab running.

#include "clod_ambiguous.h"

static void ConsoleWrite(HANDLE Out, const std::string& S)
{
    size_t At = 0;
    while(At < S.size())
    {
        DWORD Written = 0;
        if(!WriteFile(Out, S.data() + At, (DWORD)(S.size() - At), &Written, 0) || Written == 0) { break; }
        At += Written;
    }
}

static void ConsoleSize(HANDLE Out, s32* W, s32* H)
{
    CONSOLE_SCREEN_BUFFER_INFO Info = {};
    if(GetConsoleScreenBufferInfo(Out, &Info))
    {
        *W = Info.srWindow.Right - Info.srWindow.Left + 1;
        *H = Info.srWindow.Bottom - Info.srWindow.Top + 1;
    }
}

static BOOL WINAPI ClientCtrlHandler(DWORD Type)
{
    // Ctrl+C reaches us as input (processed input is off); swallow the break events.
    return Type == CTRL_C_EVENT || Type == CTRL_BREAK_EVENT;
}

static int ClientMain(s32 JumpToTab)
{
    RaiseForTyping(GetCurrentProcess());
    HANDLE In = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE Out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD OldIn = 0, OldOut = 0;
    if(!GetConsoleMode(In, &OldIn) || !GetConsoleMode(Out, &OldOut))
    {
        fprintf(stderr, "clod needs a Windows console. From mintty/Git Bash run it via `winpty clod`, or use Windows Terminal or cmd.exe.\n");
        return 1;
    }

    std::string Error;
    wchar_t Self[MAX_PATH] = {};
    GetModuleFileNameW(0, Self, MAX_PATH);
    HANDLE Pipe = ConnectToServer(Self, Error);
    if(!Pipe)
    {
        fprintf(stderr, "clod: %s\n", Error.c_str());
        return 1;
    }
    static ipc_conn Server = {};
    IpcInit(Server, Pipe);

    UINT OldCp = GetConsoleOutputCP();
    UINT OldInCp = GetConsoleCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    if(!SetConsoleMode(Out, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN) ||
       !SetConsoleMode(In, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT))
    {
        SetConsoleMode(Out, OldOut);
        SetConsoleMode(In, OldIn);
        fprintf(stderr, "clod: this console doesn't support virtual terminal sequences (needs Windows 10 1809+).\n");
        return 1;
    }
    SetConsoleCtrlHandler(ClientCtrlHandler, TRUE);
    ConsoleWrite(Out, "\x1b[?1049h\x1b[?7l\x1b[?2004h");

    // --- attach ---
    s32 Width = 80, Height = 24;
    ConsoleSize(Out, &Width, &Height);
    {
        msg_attach A = {};
        A.Version = CLOD_PROTOCOL_VERSION;
        A.Width = Width;
        A.Height = Height;
        A.JumpToTab = JumpToTab;
        A.ConsoleWindow = (u64)(uintptr_t)GetConsoleWindow();
        wchar_t Cwd[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, Cwd);
        std::string Payload((const char*)&A, sizeof(A));
        Payload += WideToUtf8(Cwd);
        IpcSendString(Server, MSG_ATTACH, Payload);
    }

    // --- which ambiguous-width characters this console draws 2 cells wide ---
    // A classic console window sizes these by its font (Windows Terminal doesn't), while the consoles
    // behind the tabs make them 1 cell. Measured after attaching, in an off-screen buffer, so the
    // first frame isn't held up; the server repaints once it knows.
    {
        std::vector<u32> Wide;
        HANDLE Probe = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, 0, CONSOLE_TEXTMODE_BUFFER, 0);
        if(Probe != INVALID_HANDLE_VALUE)
        {
            COORD ProbeSize = {16, 2};
            SetConsoleScreenBufferSize(Probe, ProbeSize);
            for(u32 R = 0; R < ArrayCount(AmbiguousWidth); ++R)
            {
                for(u32 Cp = AmbiguousWidth[R][0]; Cp <= AmbiguousWidth[R][1]; ++Cp)
                {
                    if(CharWidth(Cp) != 1) { continue; }
                    wchar_t S[2] = {(wchar_t)Cp, 0};
                    DWORD Len = 1;
                    if(Cp >= 0x10000) { S[0] = (wchar_t)(0xD800 + ((Cp - 0x10000) >> 10)); S[1] = (wchar_t)(0xDC00 + ((Cp - 0x10000) & 0x3FF)); Len = 2; }
                    COORD Origin = {0, 0};
                    SetConsoleCursorPosition(Probe, Origin);
                    DWORD Written = 0;
                    WriteConsoleW(Probe, S, Len, &Written, 0);
                    CONSOLE_SCREEN_BUFFER_INFO Info = {};
                    GetConsoleScreenBufferInfo(Probe, &Info);
                    if(Info.dwCursorPosition.X == 2) { Wide.push_back(Cp); }
                }
            }
            CloseHandle(Probe);
        }
        IpcSend(Server, MSG_WIDTHS, Wide.data(), (u32)(Wide.size() * sizeof(u32)));
    }

    // CLOD_CAPTURE=<file> appends what this window writes, for replaying a rendering bug with
    // tools\replaycheck: records of u32 length + bytes; a length of 0xFFFFFFFF is a size record
    // followed by s32 width and height.
    FILE* Capture = 0;
    {
        wchar_t Path[MAX_PATH] = {};
        if(GetEnvironmentVariableW(L"CLOD_CAPTURE", Path, MAX_PATH)) { Capture = _wfsopen(Path, L"ab", _SH_DENYWR); } // readable while capturing
        if(Capture)
        {
            u32 Mark = 0xFFFFFFFF;
            s32 Size[2] = {Width, Height};
            fwrite(&Mark, 4, 1, Capture);
            fwrite(Size, 4, 2, Capture);
        }
    }

    std::string Reason = "lost connection to the clod server";
    std::string Monitor; // last reported to the server
    std::string InBytes;
    u32 Type = 0;
    std::string Payload;
    bool32 Running = IpcPump(Server);
    while(Running)
    {
        HANDLE Waits[2] = {In, Server.ReadEvent};
        WaitForMultipleObjects(2, Waits, FALSE, 250);

        // --- server output; messages that arrived just before a hang-up (like BYE) still count ---
        bool32 Alive = IpcPump(Server);
        while(IpcNextMessage(Server, Type, Payload))
        {
            if(Type == MSG_OUTPUT)
            {
                ConsoleWrite(Out, Payload);
                if(Capture)
                {
                    u32 Len = (u32)Payload.size();
                    fwrite(&Len, 4, 1, Capture);
                    fwrite(Payload.data(), 1, Payload.size(), Capture);
                    fflush(Capture);
                }
            }
            else if(Type == MSG_BYE) { Reason = Payload; Running = false; break; }
        }
        if(!Running || !Alive) { break; }

        // --- console input as one VT byte stream ---
        InBytes.clear();
        DWORD Count = 0;
        while(GetNumberOfConsoleInputEvents(In, &Count) && Count > 0)
        {
            INPUT_RECORD Recs[256];
            DWORD Read = 0;
            if(!ReadConsoleInputW(In, Recs, ArrayCount(Recs), &Read)) { break; }
            std::wstring Wide;
            for(DWORD I = 0; I < Read; ++I)
            {
                INPUT_RECORD& R = Recs[I];
                if(R.EventType == KEY_EVENT && R.Event.KeyEvent.bKeyDown && R.Event.KeyEvent.uChar.UnicodeChar)
                {
                    for(WORD K = 0; K < Max(R.Event.KeyEvent.wRepeatCount, 1); ++K) { Wide += R.Event.KeyEvent.uChar.UnicodeChar; }
                }
                else if(R.EventType == MOUSE_EVENT)
                {
                    // Consoles that report mouse as records instead of VT: synthesize SGR mouse sequences.
                    const MOUSE_EVENT_RECORD& M = R.Event.MouseEvent;
                    CONSOLE_SCREEN_BUFFER_INFO Info = {};
                    GetConsoleScreenBufferInfo(Out, &Info);
                    s32 MX = M.dwMousePosition.X - Info.srWindow.Left + 1;
                    s32 MY = M.dwMousePosition.Y - Info.srWindow.Top + 1;
                    wchar_t Buf[64] = {};
                    if(M.dwEventFlags & MOUSE_WHEELED)
                    {
                        swprintf(Buf, 64, L"\x1b[<%d;%d;%dM", ((s32)M.dwButtonState >> 16) > 0 ? 64 : 65, MX, MY);
                    }
                    else if(M.dwEventFlags == 0 || M.dwEventFlags == DOUBLE_CLICK)
                    {
                        bool32 Down = (M.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
                        swprintf(Buf, 64, L"\x1b[<0;%d;%d%c", MX, MY, Down ? L'M' : L'm');
                    }
                    Wide += Buf;
                }
                else if(R.EventType == FOCUS_EVENT)
                {
                    Wide += R.Event.FocusEvent.bSetFocus ? L"\x1b[I" : L"\x1b[O";
                }
            }
            InBytes += WideToUtf8(Wide);
        }
        if(!InBytes.empty() && !IpcSendString(Server, MSG_INPUT, InBytes)) { break; }

        // --- window size: polled, since not every console reports window-only resizes ---
        s32 NewW = Width, NewH = Height;
        ConsoleSize(Out, &NewW, &NewH);
        if(NewW != Width || NewH != Height)
        {
            Width = NewW;
            Height = NewH;
            s32 Size[2] = {Width, Height};
            if(Capture)
            {
                u32 Mark = 0xFFFFFFFF;
                fwrite(&Mark, 4, 1, Capture);
                fwrite(Size, 4, 2, Capture);
                fflush(Capture);
            }
            if(!IpcSend(Server, MSG_RESIZE, Size, sizeof(Size))) { break; }
        }

        // --- the monitor (each keeps its own layout); in Windows Terminal the console window is a
        // hidden placeholder owned by the real window ---
        std::string NewMonitor = MonitorKey(GetAncestor(GetConsoleWindow(), GA_ROOTOWNER));
        if(NewMonitor != Monitor)
        {
            Monitor = NewMonitor;
            if(!IpcSendString(Server, MSG_MONITOR, Monitor)) { break; }
        }
    }

    IpcClose(Server);
    if(Capture) { fclose(Capture); }
    ConsoleWrite(Out, "\x1b[?1003l\x1b[?1002l\x1b[?1000l\x1b[?1006l\x1b[?2004l\x1b[?7h\x1b[0m\x1b[0 q\x1b[?25h\x1b]0;\x07\x1b[?1049l");
    SetConsoleMode(Out, OldOut);
    SetConsoleMode(In, OldIn);
    SetConsoleOutputCP(OldCp);
    SetConsoleCP(OldInCp);
    if(!Reason.empty()) { fprintf(stderr, "clod: %s\n", Reason.c_str()); }
    return 0;
}
