#pragma once

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include <deque>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  s32;
typedef int64_t  s64;
typedef int32_t  bool32;

#define ArrayCount(a) (sizeof(a) / sizeof((a)[0]))

static s32 Clamp(s32 V, s32 Lo, s32 Hi) { return V < Lo ? Lo : (V > Hi ? Hi : V); }
static s32 Min(s32 A, s32 B) { return A < B ? A : B; }
static s32 Max(s32 A, s32 B) { return A > B ? A : B; }

// Color: 0 = terminal default, otherwise tagged in the top byte.
#define COLOR_PALETTE 0x01000000u
#define COLOR_RGB     0x02000000u
#define COLOR_UI      0x03000000u // + a ui_color
#define COLOR_TAG(c)  ((c) & 0xFF000000u)
static u32 Palette(u32 Index) { return COLOR_PALETTE | (Index & 0xFF); }
static u32 Rgb(u32 R, u32 G, u32 B) { return COLOR_RGB | ((R & 0xFF) << 16) | ((G & 0xFF) << 8) | (B & 0xFF); }

// clod's own screen parts (bars, sessions page, pane rules) color by role, so clodw's themes can
// recolor them. Console clients get UiDefault (clod_app.cpp).
enum ui_color
{
    UI_BAR,           // tab bar and command bar background
    UI_BAR_TEXT,
    UI_BRIGHT,        // key names in the bar, a tab shown in another pane
    UI_DIM,           // hints, ages, an exited tab
    UI_SHOWN,         // background of a tab shown in another pane
    UI_ROW,           // the selected row on the sessions page
    UI_RULE,          // lines between panes
    UI_PROJECT,       // project names on the sessions page
    UI_ACCENT,        // the active tab, an armed prefix, the focused pane's edge
    UI_ON_ACCENT,     // text on the accent
    UI_ON_ACCENT_DIM,
    UI_NOTE,          // status messages
    UI_LIVE,          // a session that's open in a tab
    UI_COUNT,
};
static u32 Ui(u32 Role) { return COLOR_UI | Role; }

enum
{
    ATTR_BOLD      = 1 << 0,
    ATTR_DIM       = 1 << 1,
    ATTR_ITALIC    = 1 << 2,
    ATTR_UNDERLINE = 1 << 3,
    ATTR_BLINK     = 1 << 4,
    ATTR_INVERSE   = 1 << 5,
    ATTR_HIDDEN    = 1 << 6,
    ATTR_STRIKE    = 1 << 7,
};

enum
{
    CELL_WIDE      = 1 << 0, // first half of a double-width glyph
    CELL_WIDE_CONT = 1 << 1, // second half, renders nothing itself
};

// All-zero cell is a blank space in default colors.
struct cell
{
    u32 Codepoint;
    u32 Fg;
    u32 Bg;
    u16 Attr;
    u16 Flags;
};

static bool CellEqual(const cell& A, const cell& B)
{
    return A.Codepoint == B.Codepoint && A.Fg == B.Fg && A.Bg == B.Bg && A.Attr == B.Attr && A.Flags == B.Flags;
}

static void AppendUtf8(std::string& Out, u32 Cp)
{
    if(Cp < 0x80) { Out += (char)Cp; }
    else if(Cp < 0x800) { Out += (char)(0xC0 | (Cp >> 6)); Out += (char)(0x80 | (Cp & 0x3F)); }
    else if(Cp < 0x10000) { Out += (char)(0xE0 | (Cp >> 12)); Out += (char)(0x80 | ((Cp >> 6) & 0x3F)); Out += (char)(0x80 | (Cp & 0x3F)); }
    else { Out += (char)(0xF0 | (Cp >> 18)); Out += (char)(0x80 | ((Cp >> 12) & 0x3F)); Out += (char)(0x80 | ((Cp >> 6) & 0x3F)); Out += (char)(0x80 | (Cp & 0x3F)); }
}

// Decodes one UTF-8 codepoint at S[*I], advancing *I. Bad bytes give U+FFFD.
static u32 DecodeUtf8(const char* S, size_t Len, size_t* I)
{
    u8 B = (u8)S[*I];
    u32 Cp = 0xFFFD;
    s32 Need = 0;
    if(B < 0x80) { Cp = B; }
    else if((B & 0xE0) == 0xC0) { Cp = B & 0x1F; Need = 1; }
    else if((B & 0xF0) == 0xE0) { Cp = B & 0x0F; Need = 2; }
    else if((B & 0xF8) == 0xF0) { Cp = B & 0x07; Need = 3; }
    *I += 1;
    for(s32 K = 0; K < Need; ++K)
    {
        if(*I >= Len || ((u8)S[*I] & 0xC0) != 0x80) { return 0xFFFD; }
        Cp = (Cp << 6) | ((u8)S[*I] & 0x3F);
        *I += 1;
    }
    return Cp;
}

#include "clod_widths.h"

// Cell width as the system conhost lays it out (1 or 2, never 0). Child output reaches clod through
// ConPTY, which positions everything by these widths, so the emulator must agree exactly; using a
// different table (e.g. dropping zero-width characters) desyncs rows and leaves stale cells.
static s32 CharWidth(u32 Cp)
{
    if(Cp < 0x1100) { return 1; }
    s32 Lo = 0;
    s32 Hi = (s32)ArrayCount(ConhostWide) - 1;
    while(Lo <= Hi)
    {
        s32 Mid = (Lo + Hi) / 2;
        if(Cp < ConhostWide[Mid][0]) { Hi = Mid - 1; }
        else if(Cp > ConhostWide[Mid][1]) { Lo = Mid + 1; }
        else { return 2; }
    }
    return 1;
}

// Regional indicator letters: two in a row make a flag.
static bool32 IsRegionalIndicator(u32 Cp) { return Cp >= 0x1F1E6 && Cp <= 0x1F1FF; }

// Characters a grapheme-aware terminal (Windows Terminal) merges into the previous glyph instead of
// giving them a cell: combining marks, joiners, selectors, skin tones, tags, invisible formatting.
// Conhost gives each its own cell, so the renderer draws them as blanks to keep columns aligned.
static bool32 IsClusterExtender(u32 Cp)
{
    if(Cp < 0x300) { return false; }
    static const u32 Ranges[][2] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05C7}, {0x0610, 0x061A},
        {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06ED}, {0x0900, 0x0903}, {0x093A, 0x094F},
        {0x0E31, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
        {0x202A, 0x202E}, {0x2060, 0x206F}, {0x20D0, 0x20FF}, {0x302A, 0x302F}, {0xFE00, 0xFE0F},
        {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0x1F3FB, 0x1F3FF}, {0xE0000, 0xE0FFF},
    };
    for(u32 I = 0; I < ArrayCount(Ranges); ++I)
    {
        if(Cp >= Ranges[I][0] && Cp <= Ranges[I][1]) { return true; }
    }
    return false;
}
