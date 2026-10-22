// Replays a CLOD_CAPTURE file into the console it runs in, frame by frame, and after each frame
// compares what the console actually holds with clod's own emulator fed the same bytes. The first
// frame that leaves a printable character where clod's model has a blank is the rendering bug.
//
//   replaycheck <capture file> <report file>
//
// Run it inside the terminal to test, in a window at least the capture's size, e.g.
//   wt.exe -w new --maximized replaycheck.exe capture.bin report.txt
// It stops at the first resize in the capture (the window can't follow it).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "../code/clod_base.h"
#include "../code/clod_vt.cpp"

static std::string Escaped(const std::string& S)
{
    std::string Out;
    for(char C : S)
    {
        u8 B = (u8)C;
        if(B == 0x1B) { Out += "\\e"; }
        else if(B < 0x20 || B == 0x7F) { char H[8]; snprintf(H, sizeof(H), "\\x%02X", B); Out += H; }
        else { Out += C; }
    }
    return Out;
}

int wmain(int Argc, wchar_t** Argv)
{
    if(Argc < 3) { return 1; }
    FILE* In = 0;
    _wfopen_s(&In, Argv[1], L"rb");
    FILE* Rep = 0;
    _wfopen_s(&Rep, Argv[2], L"w");
    if(!In || !Rep) { return 1; }

    // --- the capture as records: frame bytes, or a size change ---
    struct record { bool32 Size; s32 W; s32 H; std::string Bytes; };
    std::vector<record> Recs;
    for(;;)
    {
        u32 Len = 0;
        if(fread(&Len, 4, 1, In) != 1) { break; }
        record R = {};
        if(Len == 0xFFFFFFFF)
        {
            s32 Size[2] = {};
            if(fread(Size, 4, 2, In) != 2) { break; }
            R.Size = true;
            R.W = Size[0];
            R.H = Size[1];
        }
        else
        {
            R.Bytes.resize(Len);
            if(Len && fread(&R.Bytes[0], 1, Len, In) != Len) { break; }
        }
        Recs.push_back(R);
    }
    fclose(In);
    if(Recs.empty() || !Recs[0].Size) { fprintf(Rep, "capture doesn't start with a size record\n"); fclose(Rep); return 1; }
    s32 W = Recs[0].W;
    s32 H = Recs[0].H;

    HANDLE Out = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleOutputCP(CP_UTF8);
    DWORD Mode = 0;
    GetConsoleMode(Out, &Mode);
    SetConsoleMode(Out, ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    // A classic console window can be sized to the capture; Windows Terminal ignores this. A freshly
    // launched window may still be growing (e.g. maximizing): give it a few seconds.
    CONSOLE_SCREEN_BUFFER_INFO Info = {};
    COORD BufSize = {(SHORT)W, (SHORT)H};
    SMALL_RECT WinRect = {0, 0, (SHORT)(W - 1), (SHORT)(H - 1)};
    SetConsoleScreenBufferSize(Out, BufSize);
    SetConsoleWindowInfo(Out, TRUE, &WinRect);
    s32 WinW = 0;
    s32 WinH = 0;
    for(s32 Try = 0; Try < 50 && (WinW < W || WinH < H); ++Try)
    {
        if(Try) { Sleep(100); }
        GetConsoleScreenBufferInfo(Out, &Info);
        WinW = Info.srWindow.Right - Info.srWindow.Left + 1;
        WinH = Info.srWindow.Bottom - Info.srWindow.Top + 1;
    }
    fprintf(Rep, "capture %dx%d, %zu records; window %dx%d\n", W, H, Recs.size(), WinW, WinH);
    // Frames position everything absolutely, so any window at least the capture's size will do.
    if(WinW < W || WinH < H) { fprintf(Rep, "window too small: needs at least %dx%d (try wt.exe --maximized)\n", W, H); fclose(Rep); return 1; }

    // Same setup the client does before its first frame. The alternate screen is a separate buffer,
    // so read back through CONOUT$, which follows the active one.
    std::string Setup = "\x1b[?1049h\x1b[?7l\x1b[?2004h";
    DWORD Written = 0;
    WriteFile(Out, Setup.data(), (DWORD)Setup.size(), &Written, 0);
    HANDLE Screen = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    vt_terminal Model = {};
    VtInit(Model, W, H);
    VtFeed(Model, (const u8*)Setup.data(), Setup.size());

    std::vector<CHAR_INFO> Cells((size_t)W * H);
    s32 Strays = 0;
    for(size_t I = 1; I < Recs.size() && !Strays; ++I)
    {
        if(Recs[I].Size) { fprintf(Rep, "record %zu: resize to %dx%d, stopping\n", I, Recs[I].W, Recs[I].H); break; }
        const std::string& B = Recs[I].Bytes;
        size_t At = 0;
        while(At < B.size())
        {
            if(!WriteFile(Out, B.data() + At, (DWORD)(B.size() - At), &Written, 0) || !Written) { break; }
            At += Written;
        }
        VtFeed(Model, (const u8*)B.data(), B.size());

        // --- compare: a printable host character over a model blank is a stray ---
        GetConsoleScreenBufferInfo(Screen, &Info);
        COORD Size = {(SHORT)W, (SHORT)H};
        COORD Zero = {0, 0};
        SMALL_RECT Rect = {Info.srWindow.Left, Info.srWindow.Top, (SHORT)(Info.srWindow.Left + W - 1), (SHORT)(Info.srWindow.Top + H - 1)}; // top-left W x H
        ReadConsoleOutputW(Screen, Cells.data(), Size, Zero, &Rect);
        const std::vector<cell>& M = Model.Screens[Model.AltActive ? 1 : 0];
        std::string Where;
        for(s32 Y = 0; Y < H; ++Y)
        {
            for(s32 X = 0; X < W; ++X)
            {
                wchar_t Host = Cells[(size_t)Y * W + X].Char.UnicodeChar;
                u32 Ours = M[(size_t)Y * W + X].Codepoint;
                bool32 OursBlank = (Ours == 0 || Ours == ' ') && !(M[(size_t)Y * W + X].Flags & CELL_WIDE_CONT);
                if(OursBlank && Host > 0x20 && Host < 0x7F)
                {
                    char Buf[64];
                    snprintf(Buf, sizeof(Buf), " (%d,%d)'%c'", X, Y, (char)Host);
                    Where += Buf;
                    Strays += 1;
                }
            }
        }
        if(Strays)
        {
            fprintf(Rep, "record %zu: %d stray cells:%s\n", I, Strays, Where.c_str());
            for(s32 Y = 0; Y < H; ++Y)
            {
                std::string HostRow, OurRow;
                for(s32 X = 0; X < W; ++X)
                {
                    wchar_t Hc = Cells[(size_t)Y * W + X].Char.UnicodeChar;
                    AppendUtf8(HostRow, Hc ? Hc : ' ');
                    const cell& C = M[(size_t)Y * W + X];
                    if(!(C.Flags & CELL_WIDE_CONT)) { AppendUtf8(OurRow, C.Codepoint ? C.Codepoint : ' '); }
                }
                if(HostRow != OurRow) { fprintf(Rep, "row %d host |%s|\nrow %d ours |%s|\n", Y, HostRow.c_str(), Y, OurRow.c_str()); }
            }
            fprintf(Rep, "--- record %zu bytes:\n%s\n", I - 1, Escaped(Recs[I - 1].Bytes).c_str());
            fprintf(Rep, "--- record %zu bytes:\n%s\n", I, Escaped(B).c_str());
        }
    }
    if(!Strays) { fprintf(Rep, "no strays\n"); }
    fclose(Rep);
    WriteFile(Out, "\x1b[?1049l", 8, &Written, 0);
    return 0;
}
