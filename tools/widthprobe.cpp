// Measures how wide the hosting console draws each codepoint / sequence, and reports where that
// disagrees with clod's CharWidth. Disagreements shift the rest of a row on screen, leaving stale
// cells that clod's diff renderer never repaints.
//
//   widthprobe <output file>
//
// Run it inside the terminal you care about (e.g. Windows Terminal); it measures that console host.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>

#include "../code/clod_base.h"

static std::wstring Utf32ToWide(const std::vector<u32>& Cps)
{
    std::wstring W;
    for(u32 Cp : Cps)
    {
        if(Cp >= 0x10000)
        {
            Cp -= 0x10000;
            W += (wchar_t)(0xD800 + (Cp >> 10));
            W += (wchar_t)(0xDC00 + (Cp & 0x3FF));
        }
        else { W += (wchar_t)Cp; }
    }
    return W;
}

// What clod's emulator does: each codepoint independently, zero-width ones dropped.
static s32 OurWidth(const std::vector<u32>& Cps)
{
    s32 W = 0;
    for(u32 Cp : Cps) { W += CharWidth(Cp); }
    return W;
}

static s32 HostWidth(HANDLE Buf, const std::vector<u32>& Cps)
{
    COORD Origin = {0, 0};
    SetConsoleCursorPosition(Buf, Origin);
    std::wstring S = L"[" + Utf32ToWide(Cps) + L"]";
    DWORD Written = 0;
    WriteConsoleW(Buf, S.data(), (DWORD)S.size(), &Written, 0);
    CONSOLE_SCREEN_BUFFER_INFO Info = {};
    GetConsoleScreenBufferInfo(Buf, &Info);
    return Info.dwCursorPosition.X - 2;
}

int wmain(int Argc, wchar_t** Argv)
{
    if(Argc < 2) { return 1; }
    FILE* Out = 0;
    _wfopen_s(&Out, Argv[1], L"w");
    if(!Out) { return 1; }

    HANDLE Buf = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, 0, CONSOLE_TEXTMODE_BUFFER, 0);
    if(Buf == INVALID_HANDLE_VALUE) { fprintf(Out, "no console\n"); return 1; }
    COORD Size = {200, 10};
    SetConsoleScreenBufferSize(Buf, Size);
    DWORD Mode = 0;
    GetConsoleMode(Buf, &Mode);
    SetConsoleMode(Buf, Mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // --- --table: every codepoint's width in this host, "XXXX width" per line, for table generation ---
    if(Argc > 2 && wcscmp(Argv[2], L"--table") == 0)
    {
        static const u32 All[][2] = {{0x00A0, 0xD7FF}, {0xF900, 0xFFFD}, {0x10000, 0x1FFFF}};
        for(u32 R = 0; R < ArrayCount(All); ++R)
        {
            for(u32 Cp = All[R][0]; Cp <= All[R][1]; ++Cp)
            {
                std::vector<u32> Cps = {Cp};
                fprintf(Out, "%X %d\n", Cp, HostWidth(Buf, Cps));
            }
        }
        fclose(Out);
        return 0;
    }

    // --- single codepoints across the ranges terminal UIs use ---
    static const u32 Ranges[][2] = {
        {0x00A0, 0x02FF}, {0x0300, 0x036F}, {0x2000, 0x2BFF}, {0x2E80, 0x2EFF}, {0x3000, 0x303F},
        {0xFE00, 0xFE6F}, {0xFF00, 0xFFEF}, {0x1F000, 0x1FAFF},
    };
    s32 Mismatches = 0;
    fprintf(Out, "# single codepoints: U+XXXX ours host\n");
    for(u32 R = 0; R < ArrayCount(Ranges); ++R)
    {
        for(u32 Cp = Ranges[R][0]; Cp <= Ranges[R][1]; ++Cp)
        {
            std::vector<u32> Cps = {Cp};
            s32 Ours = OurWidth(Cps);
            s32 Host = HostWidth(Buf, Cps);
            if(Ours != Host) { fprintf(Out, "U+%04X %d %d\n", Cp, Ours, Host); Mismatches++; }
        }
    }

    // --- sequences: emoji presentation selector, ZWJ, skin tone, flags, keycaps, combining ---
    fprintf(Out, "# base + U+FE0F (emoji presentation): U+XXXX ours host\n");
    for(u32 Cp = 0x2000; Cp <= 0x2BFF; ++Cp)
    {
        std::vector<u32> Cps = {Cp, 0xFE0F};
        s32 Ours = OurWidth(Cps);
        s32 Host = HostWidth(Buf, Cps);
        if(Ours != Host) { fprintf(Out, "U+%04X+FE0F %d %d\n", Cp, Ours, Host); Mismatches++; }
    }
    struct named { const char* Name; std::vector<u32> Cps; };
    named Seqs[] = {
        {"e + combining acute", {0x65, 0x301}},
        {"family ZWJ", {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467}},
        {"thumbs up + skin tone", {0x1F44D, 0x1F3FD}},
        {"flag US", {0x1F1FA, 0x1F1F8}},
        {"keycap 1", {0x31, 0xFE0F, 0x20E3}},
        {"heart + FE0F", {0x2764, 0xFE0F}},
        {"rainbow flag", {0x1F3F3, 0xFE0F, 0x200D, 0x1F308}},
    };
    fprintf(Out, "# named sequences: name ours host\n");
    for(named& S : Seqs)
    {
        s32 Ours = OurWidth(S.Cps);
        s32 Host = HostWidth(Buf, S.Cps);
        fprintf(Out, "%s %d %d%s\n", S.Name, Ours, Host, Ours != Host ? "  MISMATCH" : "");
    }
    fprintf(Out, "# total single/FE0F mismatches: %d\n", Mismatches);
    fclose(Out);
    return 0;
}
