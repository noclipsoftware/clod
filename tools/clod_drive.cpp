// Test driver: runs a command in a pseudo console, scripts keystrokes and resizes,
// and dumps the emulated screen as text. Used to exercise clod without a human at the keyboard.
//
//   clod_drive 100x30 "command line" step...
//     w<ms>          pump output for ms
//     k<keys>        send keys; escapes \r \n \e \t \xHH and \\ for a backslash
//     s<cols>x<rows> resize
//     d              dump screen
//     D              dump screen with scrollback

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>

#include "../code/clod_base.h"
#include "../code/clod_vt.cpp"
#include "../code/clod_ipc.cpp" // RaiseForTyping, LatencyMark
#include "../code/clod_pty.cpp"

static void Pump(pty* P, vt_terminal& T, u32 Ms)
{
    u64 End = GetTickCount64() + Ms;
    while(GetTickCount64() < End)
    {
        WaitForSingleObject(P->Wake, 10);
        std::string Chunk;
        EnterCriticalSection(&P->Lock);
        Chunk.swap(P->Pending);
        LeaveCriticalSection(&P->Lock);
        if(!Chunk.empty())
        {
            // CLOD_DRIVE_RAW=<file> appends the child's raw output, for inspecting the sequences it sends.
            static FILE* Raw = 0;
            static bool32 RawChecked = false;
            if(!RawChecked)
            {
                RawChecked = true;
                char Path[MAX_PATH] = {};
                if(GetEnvironmentVariableA("CLOD_DRIVE_RAW", Path, MAX_PATH)) { fopen_s(&Raw, Path, "ab"); }
            }
            if(Raw) { fwrite(Chunk.data(), 1, Chunk.size(), Raw); fflush(Raw); }
            VtFeed(T, (const u8*)Chunk.data(), Chunk.size());
            PtyWrite(P, T.Reply);
            T.Reply.clear();
        }
    }
}

static void DumpLine(const cell* Row, s32 Len)
{
    std::string S;
    for(s32 X = 0; X < Len; ++X)
    {
        if(Row[X].Flags & CELL_WIDE_CONT) { continue; }
        AppendUtf8(S, Row[X].Codepoint ? Row[X].Codepoint : ' ');
    }
    while(!S.empty() && S.back() == ' ') { S.pop_back(); }
    printf("|%s\n", S.c_str());
}

static void Dump(vt_terminal& T, bool32 WithScrollback)
{
    if(WithScrollback)
    {
        printf("---- scrollback %zu lines\n", T.Scrollback.size());
        for(auto& L : T.Scrollback) { DumpLine(L.data(), (s32)L.size()); }
    }
    printf("---- screen %dx%d cursor %d,%d%s alt=%d mouse=%d title=\"%s\"\n", T.Cols, T.Rows, T.CursorX, T.CursorY,
           T.CursorHidden ? " hidden" : "", T.AltActive, T.MouseMode, T.Title.c_str());
    for(s32 Y = 0; Y < T.Rows; ++Y) { DumpLine(T.Screens[T.AltActive ? 1 : 0].data() + (size_t)Y * T.Cols, T.Cols); }
    fflush(stdout);
}

static std::string Unescape(const char* S)
{
    std::string Out;
    for(; *S; ++S)
    {
        if(*S != '\\' || !S[1]) { Out += *S; continue; }
        ++S;
        switch(*S)
        {
            case 'r': Out += '\r'; break;
            case 'n': Out += '\n'; break;
            case 'e': Out += '\x1b'; break;
            case 't': Out += '\t'; break;
            case 'x': { char H[3] = {S[1], S[2], 0}; Out += (char)strtol(H, 0, 16); S += 2; } break;
            default: Out += *S; break;
        }
    }
    return Out;
}

int wmain(int Argc, wchar_t** Argv)
{
    SetConsoleOutputCP(CP_UTF8);
    if(Argc < 3) { fprintf(stderr, "usage: clod_drive COLSxROWS \"command\" steps...\n"); return 1; }
    s32 Cols = 0, Rows = 0;
    swscanf_s(Argv[1], L"%dx%d", &Cols, &Rows);
    HANDLE Wake = CreateEventW(0, FALSE, FALSE, 0);
    std::string Error;
    pty* P = PtyStart(Argv[2], L"", 0, Cols, Rows, 0, Wake, Error);
    if(!P) { fprintf(stderr, "%s\n", Error.c_str()); return 1; }
    vt_terminal T = {};
    VtInit(T, Cols, Rows);

    for(s32 I = 3; I < Argc; ++I)
    {
        std::wstring W = Argv[I];
        std::string Step;
        for(wchar_t C : W) { AppendUtf8(Step, C); }
        char Op = Step[0];
        const char* Arg = Step.c_str() + 1;
        if(Op == 'w') { Pump(P, T, (u32)atoi(Arg)); }
        else if(Op == 'k') { PtyWrite(P, Unescape(Arg)); Pump(P, T, 50); }
        else if(Op == 's')
        {
            sscanf_s(Arg, "%dx%d", &Cols, &Rows);
            VtResize(T, Cols, Rows);
            COORD Size = {(SHORT)Cols, (SHORT)Rows};
            ResizePseudoConsole(P->Console, Size);
        }
        else if(Op == 'd') { Dump(T, false); }
        else if(Op == 'D') { Dump(T, true); }
    }
    DWORD Code = 0;
    if(WaitForSingleObject(P->Process, 0) == WAIT_OBJECT_0) { GetExitCodeProcess(P->Process, &Code); printf("---- exited %lu\n", Code); }
    PtyClose(P);
    return 0;
}
