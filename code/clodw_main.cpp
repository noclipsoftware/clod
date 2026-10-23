// clodw's window: attaches to the clod server as a cells client, draws the grid the server composes,
// and turns keyboard and mouse input into the VT bytes a terminal would send. No text layout happens
// here beyond drawing each cell's glyph: the server's grid is the truth, so nothing can drift.

struct clodw
{
    HWND Window;
    renderer R;
    font_state F;
    glyph_cache G;
    ipc_conn* Server;
    std::wstring FontFamily;
    float FontPt;
    u32 Dpi;

    // --- the grid as the server last sent it ---
    s32 GridW;
    s32 GridH;
    std::vector<cell> Grid;
    s32 CursorX;
    s32 CursorY;
    bool32 CursorHidden;
    s32 MouseMode;
    u32 PrefixKey;   // the server's command prefix
    s32 CursorShape; // DECSCUSR from the server (0 default, 1/2 block, 3/4 underline, 5/6 bar; odd blinks)
    s32 PaneCount;
    s32 Panes[4][4]; // pane rectangles {x, y, w, h}: a selection stays inside one
    u32 PaneMouseTaken; // bit per pane: the program there (or the home screen) takes the mouse
    std::string Title;
    s32 ImagePane;         // pane + 1 showing the image `clod view` asked for; 0 = none
    std::string ImagePath; // that image, decoded into ImagePixels and the renderer
    std::vector<u32> ImagePixels;
    u32 ImageW;
    u32 ImageH;

    // --- selection, in cells (inclusive): drag, or Shift+drag while a program has the mouse ---
    bool32 Selecting;      // a drag is under way
    bool32 HasSelection;
    s32 SelPane[4];        // the pane rectangle the selection is confined to
    s32 SelAx;             // anchor
    s32 SelAy;
    s32 SelBx;             // moving end
    s32 SelBy;
    s32 Clicks;            // 1, 2, 3: click, double-click (word), triple-click (line)
    u64 ClickAtMs;
    s32 ClickX;
    s32 ClickY;
    bool32 BlinkOff;      // a blinking cursor is in its hidden phase
    u64 BlinkAtMs;         // last blink toggle; input restarts it visible

    std::string InBytes;   // input not yet sent
    bool32 Dirty;          // the window needs a redraw
    s32 SentW;             // size last told to the server, in cells
    s32 SentH;
    std::string Monitor;   // monitor last told to the server (each keeps its own layout)
    wchar_t HighSurrogate; // first half of a WM_CHAR surrogate pair
    s32 MouseHeld;         // SGR button number + 1 while a button is down; 0 = none
    s32 MouseX;            // last reported cell, 1-based
    s32 MouseY;
    s32 WheelAccum;
    bool32 Quit;
    bool32 CopyOnSelectOff; // settings "copy_on_select = off": a finished selection isn't copied
    bool32 KeepActiveOff;  // settings "keep_active = off": don't hold off Windows' idle (Slack's away)
    s32 Transparency;      // 100 - settings "opacity" (percent): 0 = opaque
    HMENU OpacityMenu;     // the window menu's Opacity submenu
    u32 Theme;             // index into Themes (settings "theme"); 0 = Clod
    HMENU ThemeMenu;
    bool32 Keeping;       // keeping the user active right now (the bar's light shows it)
    std::string ByeReason;
    std::vector<gpu_cell> Gpu;
};

// A color theme, all 0xRRGGBB: the terminal's default colors, its 16 colors, the selection and
// clod's own screen parts (ui_color order). Picked from the window menu's Theme submenu.
struct theme
{
    const char* Name;
    bool32 Light;  // light title bar
    u32 Fg;
    u32 Bg;
    u32 Selection;
    u32 Ansi[16];  // black red green yellow blue magenta cyan white, then the bright ones
    u32 Ui[UI_COUNT];
};

static const theme Themes[] = {
    // Clod: warm dark, around Claude's orange.
    {"Clod", false, 0xD8D2C6, 0x1C1A18, 0x4A3A30,
     {0x2B2826, 0xE5636F, 0x9CBF7E, 0xE3B65E, 0x78A6D8, 0xC590C8, 0x6FBFB2, 0xC9C2B5,
      0x6E6861, 0xF4848E, 0xB5D69A, 0xF2CC84, 0x9BC0EF, 0xDCAEDD, 0x93D8CD, 0xF3EFE7},
     {0x262321, 0xB8B0A3, 0xF3EFE7, 0x7D766D, 0x403B36, 0x302C29, 0x4A4540, 0x8FB3D9,
      0xD97757, 0x1C1A18, 0x5A2A1C, 0xF0C878, 0x9CBF7E}},
    // Clod's background and bars with stronger text colors: vivid, earthy, neon.
    {"Clod Vivid", false, 0xD8D2C6, 0x1C1A18, 0x4A3A30,
     {0x2B2826, 0xF2545B, 0x6CC24A, 0xF2B33D, 0x4C9AFF, 0xD670D6, 0x3FC1C9, 0xC9C2B5,
      0x7A736B, 0xFF6E74, 0x8BDB66, 0xFFCC5C, 0x6EB0FF, 0xE68FE6, 0x5FD7DD, 0xF5F1EA},
     {0x262321, 0xB8B0A3, 0xF3EFE7, 0x7D766D, 0x403B36, 0x302C29, 0x4A4540, 0x6EB0FF,
      0xD97757, 0x1C1A18, 0x5A2A1C, 0xF2B33D, 0x6CC24A}},
    {"Clod Earth", false, 0xD8D2C6, 0x1C1A18, 0x4A3A30,
     {0x2B2826, 0xD9483B, 0x8DB33A, 0xE6A532, 0x4F8FCC, 0xB86BB0, 0x3AA6A0, 0xC9C2B5,
      0x756E66, 0xF0624F, 0xA6CC4F, 0xF5BE4F, 0x6EA8E0, 0xCF88C8, 0x52C2BB, 0xF3EFE7},
     {0x262321, 0xB8B0A3, 0xF3EFE7, 0x7D766D, 0x403B36, 0x302C29, 0x4A4540, 0x6EA8E0,
      0xD97757, 0x1C1A18, 0x5A2A1C, 0xF5BE4F, 0x8DB33A}},
    {"Clod Neon", false, 0xE2DCD0, 0x1C1A18, 0x4A3A30,
     {0x2B2826, 0xFF5C57, 0x5AF78E, 0xF3F99D, 0x57C7FF, 0xFF6AC1, 0x9AEDFE, 0xD8D2C6,
      0x7A736B, 0xFF7A75, 0x7CFFA6, 0xF8FCBE, 0x80D4FF, 0xFF8DD0, 0xB8F3FF, 0xF5F1EA},
     {0x262321, 0xB8B0A3, 0xF3EFE7, 0x7D766D, 0x403B36, 0x302C29, 0x4A4540, 0x57C7FF,
      0xD97757, 0x1C1A18, 0x5A2A1C, 0xF3F99D, 0x5AF78E}},
    // Clod Light: Claude's cream paper, the same orange.
    {"Clod Light", true, 0x2E2B26, 0xF7F5F0, 0xF2D3C4,
     {0x2E2B26, 0xC4363F, 0x4A7C2F, 0x9A6A00, 0x2F62B5, 0x8A4BA0, 0x1F7F78, 0x8F897F,
      0x777168, 0xD94F58, 0x5C9440, 0xB58000, 0x4A7FD1, 0xA465BA, 0x2E9A92, 0xB9B3A8},
     {0xEBE7DE, 0x4F4A42, 0x1F1C18, 0x8C857B, 0xDCD6CA, 0xE6E0D4, 0xCFC8BB, 0x2F62B5,
      0xD97757, 0x1C1A18, 0x5A2A1C, 0x9A5A00, 0x3F8F3A}},
    {"Tokyo Night", false, 0xC0CAF5, 0x1A1B26, 0x33467C,
     {0x15161E, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xA9B1D6,
      0x414868, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xC0CAF5},
     {0x16161E, 0xA9B1D6, 0xC0CAF5, 0x565F89, 0x292E42, 0x283457, 0x3B4261, 0x7DCFFF,
      0x7AA2F7, 0x15161E, 0x283457, 0xE0AF68, 0x9ECE6A}},
    {"Catppuccin Mocha", false, 0xCDD6F4, 0x1E1E2E, 0x45475A,
     {0x45475A, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xBAC2DE,
      0x585B70, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xA6ADC8},
     {0x181825, 0xBAC2DE, 0xCDD6F4, 0x6C7086, 0x313244, 0x313244, 0x45475A, 0x89B4FA,
      0xCBA6F7, 0x1E1E2E, 0x4C3A66, 0xF9E2AF, 0xA6E3A1}},
    {"Nord", false, 0xD8DEE9, 0x2E3440, 0x434C5E,
     {0x3B4252, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x88C0D0, 0xE5E9F0,
      0x4C566A, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x8FBCBB, 0xECEFF4},
     {0x3B4252, 0xD8DEE9, 0xECEFF4, 0x7B88A1, 0x4C566A, 0x3B4252, 0x4C566A, 0x81A1C1,
      0x88C0D0, 0x2E3440, 0x3E5C69, 0xEBCB8B, 0xA3BE8C}},
    {"Catppuccin Latte", true, 0x4C4F69, 0xEFF1F5, 0xCCD0DA,
     {0x5C5F77, 0xD20F39, 0x40A02B, 0xDF8E1D, 0x1E66F5, 0xEA76CB, 0x179299, 0xACB0BE,
      0x6C6F85, 0xD20F39, 0x40A02B, 0xDF8E1D, 0x1E66F5, 0xEA76CB, 0x179299, 0xBCC0CC},
     {0xE6E9EF, 0x5C5F77, 0x4C4F69, 0x8C8FA1, 0xCCD0DA, 0xDCE0E8, 0xBCC0CC, 0x1E66F5,
      0x8839EF, 0xEFF1F5, 0xD8C3F8, 0xC4700F, 0x40A02B}},
    {"GitHub Light", true, 0x1F2328, 0xFFFFFF, 0xB6E3FF,
     {0x24292F, 0xCF222E, 0x116329, 0x4D2D00, 0x0969DA, 0x8250DF, 0x1B7C83, 0x6E7781,
      0x57606A, 0xA40E26, 0x1A7F37, 0x633C01, 0x218BFF, 0xA475F9, 0x3192AA, 0x8C959F},
     {0xF6F8FA, 0x424A53, 0x1F2328, 0x6E7781, 0xEAEEF2, 0xDDF4FF, 0xD0D7DE, 0x0969DA,
      0x0969DA, 0xFFFFFF, 0xC5DEFB, 0x9A6700, 0x1A7F37}},
    // Campbell: Windows Terminal's default, which clodw used before themes.
    {"Campbell", false, 0xCCCCCC, 0x0C0C0C, 0x264F78,
     {0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
      0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2},
     {0x303030, 0xBCBCBC, 0xEEEEEE, 0x808080, 0x585858, 0x3A3A3A, 0x585858, 0x87AFD7,
      0xD77757, 0x141414, 0x462319, 0xF0C878, 0x6EC878}},
};

// Packs 0xRRGGBB-style components as the shader reads them (0x00BBGGRR).
static u32 PackRgb(u32 R, u32 G, u32 B) { return R | (G << 8) | (B << 16); }
static u32 PackHex(u32 Hex) { return PackRgb((Hex >> 16) & 255, (Hex >> 8) & 255, Hex & 255); }

// A cell color (clod_base.h tags) as a packed RGB in theme T; untagged = DefaultHex.
static u32 ResolveColor(u32 C, u32 DefaultHex, const theme& T)
{
    if(COLOR_TAG(C) == COLOR_RGB || COLOR_TAG(C) == COLOR_UI)
    {
        return PackHex(COLOR_TAG(C) == COLOR_RGB ? C & 0xFFFFFF : T.Ui[Min((s32)(C & 255), UI_COUNT - 1)]);
    }
    if(COLOR_TAG(C) != COLOR_PALETTE) { return PackHex(DefaultHex); }
    u32 I = C & 255;
    if(I < 16) { return PackHex(T.Ansi[I]); }
    if(I < 232)
    {
        static const u32 Level[6] = {0, 95, 135, 175, 215, 255};
        u32 N = I - 16;
        return PackRgb(Level[N / 36], Level[(N / 6) % 6], Level[N % 6]);
    }
    u32 Gray = 8 + 10 * (I - 232);
    return PackRgb(Gray, Gray, Gray);
}

#define DEFAULT_FONT_PT 12.0f
#define IDM_FONT 0x0100 // window menu command (system menu ids keep the low 4 bits clear)
#define IDM_OPACITY 0x0200 // + 16 * index into OpacitySteps
static const s32 OpacitySteps[] = {100, 95, 90, 85, 80, 75, 70, 60, 50};

// Ticks the Opacity menu's entry for the current opacity (none for a value set by hand in settings.txt).
static void CheckOpacity(const clodw& C)
{
    for(u32 I = 0; I < ArrayCount(OpacitySteps); ++I)
    {
        CheckMenuItem(C.OpacityMenu, IDM_OPACITY + I * 16, MF_BYCOMMAND | (OpacitySteps[I] == 100 - C.Transparency ? MF_CHECKED : MF_UNCHECKED));
    }
}

#define IDM_THEME 0x0300 // + 16 * index into Themes
#define IDM_GOOD_PROGRAMMER 0x0400 // "Be a good programmer" on/off

// "Be a good programmer" lives in the user's global CLAUDE.md, which every Claude Code session on the
// machine reads (in clod or not): on = the principles (~\.clod\good-programmer.md) sit there between
// these markers, off = they don't. The rest of the file is the user's and is kept as it is.
static const char GoodStart[] = "<!-- clod: Be a good programmer (start). The clod window menu adds and removes this block. -->";
static const char GoodEnd[] = "<!-- clod: Be a good programmer (end) -->";

// <CLAUDE_CONFIG_DIR or ~\.claude>\CLAUDE.md
static std::wstring GlobalClaudeMd()
{
    wchar_t Dir[MAX_PATH] = {};
    if(!GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", Dir, MAX_PATH))
    {
        GetEnvironmentVariableW(L"USERPROFILE", Dir, MAX_PATH);
        wcscat_s(Dir, L"\\.claude");
    }
    return std::wstring(Dir) + L"\\CLAUDE.md";
}

static std::string ReadWholeFile(const std::wstring& Path)
{
    std::string Out;
    FILE* File = 0;
    _wfopen_s(&File, Path.c_str(), L"rb");
    char Buf[4096];
    for(size_t N = 0; File && (N = fread(Buf, 1, sizeof(Buf), File)) > 0;) { Out.append(Buf, N); }
    if(File) { fclose(File); }
    return Out;
}

// Puts the principles block into the global CLAUDE.md (On) or takes it out. False when the file
// couldn't be written or there are no principles to add.
static bool32 SetGoodProgrammer(bool32 On)
{
    std::wstring Path = GlobalClaudeMd();
    std::string Md = ReadWholeFile(Path);

    // --- drop the block (and the blank line clod put before it), wherever it is ---
    size_t Start = Md.find(GoodStart);
    size_t End = (Start == std::string::npos) ? std::string::npos : Md.find(GoodEnd, Start);
    if(End != std::string::npos)
    {
        End += sizeof(GoodEnd) - 1;
        while(End < Md.size() && (Md[End] == '\r' || Md[End] == '\n')) { ++End; }
        while(Start > 0 && (Md[Start - 1] == '\r' || Md[Start - 1] == '\n')) { --Start; }
        Md.erase(Start, End - Start);
        if(!Md.empty()) { Md += "\r\n"; }
    }

    // --- add it at the end, from the user's copy (else clod's default) ---
    if(On)
    {
        std::string Principles = ReadWholeFile(UserClodDir() + L"good-programmer.md");
        if(Principles.empty()) { Principles = ReadWholeFile(ExeDir() + L"shell\\good-programmer.md"); }
        if(Principles.empty()) { return false; }
        while(!Principles.empty() && strchr("\r\n", Principles.back())) { Principles.pop_back(); }
        Md += (Md.empty() ? "" : "\r\n") + std::string(GoodStart) + "\r\n" + Principles + "\r\n" + GoodEnd + "\r\n";
    }

    // --- write beside it, then swap in, so a session reading it never sees half a file ---
    std::wstring Dir = Path.substr(0, Path.find_last_of(L'\\'));
    CreateDirectoryW(Dir.c_str(), 0);
    std::wstring Temp = Path + L".clod-tmp";
    FILE* File = 0;
    _wfopen_s(&File, Temp.c_str(), L"wb");
    if(!File) { return false; }
    bool32 Ok = fwrite(Md.data(), 1, Md.size(), File) == Md.size();
    fclose(File);
    return Ok && MoveFileExW(Temp.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Ticks the Theme menu's entry for the current theme.
static void CheckTheme(const clodw& C)
{
    for(u32 I = 0; I < ArrayCount(Themes); ++I)
    {
        CheckMenuItem(C.ThemeMenu, IDM_THEME + I * 16, MF_BYCOMMAND | (I == C.Theme ? MF_CHECKED : MF_UNCHECKED));
    }
}

// The title bar in the theme: dark or light, and on Windows 11 the bars' own colors.
static void ApplyTitleBar(const clodw& C)
{
    const theme& T = Themes[C.Theme];
    BOOL Dark = !T.Light;
    DwmSetWindowAttribute(C.Window, DWMWA_USE_IMMERSIVE_DARK_MODE, &Dark, sizeof(Dark));
    COLORREF Caption = PackHex(T.Ui[UI_BAR]); // COLORREF is 0x00BBGGRR too
    COLORREF Text = PackHex(T.Ui[UI_BAR_TEXT]);
    DwmSetWindowAttribute(C.Window, DWMWA_CAPTION_COLOR, &Caption, sizeof(Caption));
    DwmSetWindowAttribute(C.Window, DWMWA_TEXT_COLOR, &Text, sizeof(Text));
}

// Font size in pixels at the window's DPI; resets the glyph atlas.
static void ApplyFont(clodw& C)
{
    FontSetup(C.F, C.G, C.R, C.FontFamily, C.FontPt * (float)C.Dpi / 72.0f);
    C.Dirty = true;
}

// Writes the font lines of settings.txt (the Font... dialog, zoom).
static void SaveFontSettings(const std::wstring& Family, float Pt)
{
    char Face[LF_FACESIZE * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, Family.c_str(), -1, Face, sizeof(Face), 0, 0);
    char Size[32];
    snprintf(Size, sizeof(Size), "%g", Pt);
    WriteSetting("font", Face);
    WriteSetting("font_size", Size);
}

// The selection's start and end in reading order (anchor and moving end may be either way round).
static void SelectionRange(const clodw& C, s32* Sx, s32* Sy, s32* Ex, s32* Ey)
{
    bool32 Forward = C.SelAy < C.SelBy || (C.SelAy == C.SelBy && C.SelAx <= C.SelBx);
    *Sx = Forward ? C.SelAx : C.SelBx;
    *Sy = Forward ? C.SelAy : C.SelBy;
    *Ex = Forward ? C.SelBx : C.SelAx;
    *Ey = Forward ? C.SelBy : C.SelAy;
}

// Puts UTF-8 text on the clipboard (a selection copy, or a program's OSC 52).
static void SetClipboardText(HWND Window, const std::string& Utf8)
{
    int Len = MultiByteToWideChar(CP_UTF8, 0, Utf8.data(), (int)Utf8.size(), 0, 0);
    HGLOBAL Mem = GlobalAlloc(GMEM_MOVEABLE, (size_t)(Len + 1) * sizeof(wchar_t));
    if(!Mem) { return; }
    wchar_t* Wide = (wchar_t*)GlobalLock(Mem);
    MultiByteToWideChar(CP_UTF8, 0, Utf8.data(), (int)Utf8.size(), Wide, Len);
    Wide[Len] = 0;
    GlobalUnlock(Mem);
    if(!OpenClipboard(Window)) { GlobalFree(Mem); return; }
    EmptyClipboard();
    if(!SetClipboardData(CF_UNICODETEXT, Mem)) { GlobalFree(Mem); }
    CloseClipboard();
}

// Puts the selection on the clipboard: rows clipped to its pane, trailing blanks dropped, CRLF between.
static void CopySelection(const clodw& C)
{
    s32 Sx = 0, Sy = 0, Ex = 0, Ey = 0;
    SelectionRange(C, &Sx, &Sy, &Ex, &Ey);
    std::string Text;
    for(s32 Y = Max(Sy, 0); Y <= Ey && Y < C.GridH; ++Y)
    {
        s32 X0 = (Y == Sy) ? Sx : C.SelPane[0];
        s32 X1 = (Y == Ey) ? Ex : C.SelPane[0] + C.SelPane[2] - 1;
        std::string Line;
        for(s32 X = Max(X0, 0); X <= X1 && X < C.GridW; ++X)
        {
            const cell& K = C.Grid[(size_t)Y * C.GridW + X];
            if(K.Flags & CELL_WIDE_CONT) { continue; }
            AppendUtf8(Line, K.Codepoint >= 0x20 ? K.Codepoint : ' ');
        }
        while(!Line.empty() && Line.back() == ' ') { Line.pop_back(); }
        Text += Line;
        if(Y < Ey) { Text += "\r\n"; }
    }
    SetClipboardText(C.Window, Text);
}

// Sends the clipboard's text as a bracketed paste with CR line ends.
static void PasteClipboard(clodw& C)
{
    if(!OpenClipboard(C.Window)) { return; }
    HANDLE Data = GetClipboardData(CF_UNICODETEXT);
    const wchar_t* Text = Data ? (const wchar_t*)GlobalLock(Data) : 0;
    if(Text)
    {
        int Len = WideCharToMultiByte(CP_UTF8, 0, Text, -1, 0, 0, 0, 0);
        std::string Utf8(Max(Len, 1), 0);
        WideCharToMultiByte(CP_UTF8, 0, Text, -1, &Utf8[0], Len, 0, 0);
        Utf8.resize(strlen(Utf8.c_str()));
        std::string Paste;
        for(size_t I = 0; I < Utf8.size(); ++I)
        {
            if(Utf8[I] == '\r' && I + 1 < Utf8.size() && Utf8[I + 1] == '\n') { continue; }
            Paste += (Utf8[I] == '\n') ? '\r' : Utf8[I];
        }
        C.InBytes += "\x1b[200~" + Paste + "\x1b[201~";
        GlobalUnlock(Data);
    }
    CloseClipboard();
}

// Where the window's place is kept between runs: %LOCALAPPDATA%\clod\window<instance>.txt.
static std::wstring WindowFilePath()
{
    wchar_t Base[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", Base, MAX_PATH);
    return std::wstring(Base) + L"\\clod\\window" + InstanceSuffix() + L".txt";
}

// One round of work: send input and size, apply what the server sent, redraw if anything changed.
// Runs from the main loop, and from a timer while Windows holds the thread in a move/resize loop.
static void Tick(clodw& C)
{
    // --- size in cells follows the client area ---
    RECT Client = {};
    GetClientRect(C.Window, &Client);
    s32 Cols = Max(1, (s32)(Client.right - Client.left) / C.F.CellW);
    s32 Rows = Max(2, (s32)(Client.bottom - Client.top) / C.F.CellH);
    if((Client.right > 0 && Client.bottom > 0) && (Cols != C.SentW || Rows != C.SentH))
    {
        s32 Size[2] = {Cols, Rows};
        if(!IpcSend(*C.Server, MSG_RESIZE, Size, sizeof(Size))) { C.Quit = true; }
        C.SentW = Cols;
        C.SentH = Rows;
    }
    std::string Monitor = MonitorKey(C.Window);
    if(Monitor != C.Monitor)
    {
        if(!IpcSendString(*C.Server, MSG_MONITOR, Monitor)) { C.Quit = true; }
        C.Monitor = Monitor;
    }
    if(!C.InBytes.empty())
    {
        LatencyMark("client_send");
        if(!IpcSendString(*C.Server, MSG_INPUT, C.InBytes)) { C.Quit = true; }
        C.InBytes.clear();
    }

    // --- server messages ---
    bool32 Alive = IpcPump(*C.Server);
    u32 Type = 0;
    std::string Payload;
    while(IpcNextMessage(*C.Server, Type, Payload))
    {
        if(Type == MSG_CELLS && Payload.size() >= sizeof(msg_cells))
        {
            LatencyMark("client_cells");
            msg_cells H = {};
            memcpy(&H, Payload.data(), sizeof(H));
            size_t At = sizeof(H);
            std::string Title = Payload.substr(At, H.TitleLen);
            At += H.TitleLen;
            std::string Image = Payload.substr(Min((s32)At, (s32)Payload.size()), H.ImagePathLen);
            At += H.ImagePathLen;
            C.ImagePane = H.ImagePane;
            if(Image != C.ImagePath)
            {
                // A new image (or none): decode it at up to 4096 pixels a side; the GPU's mipmaps scale it down.
                C.ImagePath = Image;
                u32 FullW = 0, FullH = 0;
                C.ImageW = C.ImageH = 0;
                C.ImagePixels.clear();
                if(!Image.empty())
                {
                    std::wstring Wide(Image.size() + 1, 0);
                    Wide.resize(MultiByteToWideChar(CP_UTF8, 0, Image.data(), (int)Image.size(), &Wide[0], (int)Wide.size()));
                    DecodeImage(Wide, 4096, 4096, C.ImagePixels, &C.ImageW, &C.ImageH, &FullW, &FullH);
                }
                RendererSetImage(C.R, C.ImagePixels.data(), C.ImageW, C.ImageH);
            }
            // A new grid or new panes: the selection no longer points at what was selected.
            bool32 Reshaped = (H.Flags & CELLS_FULL) || H.Width != C.GridW || H.Height != C.GridH ||
                              H.PaneCount != C.PaneCount || memcmp(H.Panes, C.Panes, sizeof(C.Panes)) != 0;
            if(Reshaped && (C.HasSelection || C.Selecting)) { C.HasSelection = false; C.Selecting = false; }
            if((H.Flags & CELLS_FULL) || H.Width != C.GridW || H.Height != C.GridH)
            {
                C.GridW = H.Width;
                C.GridH = H.Height;
                C.Grid.assign((size_t)Max(H.Width, 0) * Max(H.Height, 0), cell{});
            }
            size_t RowBytes = (size_t)C.GridW * sizeof(cell);
            for(u32 I = 0; I < H.RowCount && At + sizeof(s32) + RowBytes <= Payload.size(); ++I)
            {
                s32 Y = 0;
                memcpy(&Y, Payload.data() + At, sizeof(Y));
                At += sizeof(Y);
                if(Y >= 0 && Y < C.GridH) { memcpy(C.Grid.data() + (size_t)Y * C.GridW, Payload.data() + At, RowBytes); }
                At += RowBytes;
            }
            C.CursorX = H.CursorX;
            C.CursorY = H.CursorY;
            C.CursorHidden = (H.Flags & CELLS_CURSOR_HIDDEN) != 0;
            C.MouseMode = H.MouseMode;
            C.PrefixKey = H.PrefixKey;
            C.CursorShape = H.CursorShape;
            C.PaneCount = Clamp(H.PaneCount, 0, 4);
            memcpy(C.Panes, H.Panes, sizeof(C.Panes));
            C.PaneMouseTaken = H.PaneMouseTaken;
            if(Title != C.Title)
            {
                C.Title = Title;
                std::wstring Wide(Title.size() + 1, 0);
                Wide.resize(MultiByteToWideChar(CP_UTF8, 0, Title.data(), (int)Title.size(), &Wide[0], (int)Wide.size()));
                SetWindowTextW(C.Window, Wide.c_str());
            }
            C.Dirty = true;
        }
        else if(Type == MSG_OUTPUT)
        {
            // Pass-through bytes meant for a terminal: the bell, and OSC 52 "set the clipboard"
            // (how Claude Code copies), "ESC ] 52 ; <targets> ; <base64> ESC \".
            if(Payload.find('\x07') != std::string::npos && GetForegroundWindow() != C.Window)
            {
                FLASHWINFO Flash = {sizeof(Flash), C.Window, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
                FlashWindowEx(&Flash);
            }
            for(size_t At = Payload.find("\x1b]52;"); At != std::string::npos; At = Payload.find("\x1b]52;", At + 1))
            {
                size_t Semi = Payload.find(';', At + 5);
                size_t End = Payload.find("\x1b\\", At);
                if(Semi == std::string::npos || End == std::string::npos || Semi > End) { continue; }
                std::string Text;
                u32 Bits = 0;
                s32 BitCount = 0;
                for(size_t I = Semi + 1; I < End; ++I)
                {
                    char Ch = Payload[I];
                    s32 V = (Ch >= 'A' && Ch <= 'Z') ? Ch - 'A' : (Ch >= 'a' && Ch <= 'z') ? Ch - 'a' + 26 :
                            (Ch >= '0' && Ch <= '9') ? Ch - '0' + 52 : (Ch == '+') ? 62 : (Ch == '/') ? 63 : -1;
                    if(V < 0) { continue; } // '=' padding; a "?" query has no data and sets nothing
                    Bits = (Bits << 6) | (u32)V;
                    BitCount += 6;
                    if(BitCount >= 8) { BitCount -= 8; Text += (char)((Bits >> BitCount) & 0xFF); }
                }
                if(!Text.empty()) { SetClipboardText(C.Window, Text); }
            }
        }
        else if(Type == MSG_BYE)
        {
            C.ByeReason = Payload;
            C.Quit = true;
        }
    }
    if(!Alive && !C.Quit)
    {
        C.ByeReason = "lost connection to the clod server";
        C.Quit = true;
    }

    // --- keep active: while this window is in front, Windows' idle timer (what Slack's "away"
    // goes by) never reaches its limit; a one-pixel nudge and back resets it. Nothing happens when
    // the machine is locked or another window is in front ---
    bool32 Keeping = !C.KeepActiveOff && GetForegroundWindow() == C.Window;
    if(Keeping)
    {
        LASTINPUTINFO Last = {sizeof(Last), 0};
        GetLastInputInfo(&Last);
        if(GetTickCount() - Last.dwTime > 2 * 60 * 1000)
        {
            INPUT Nudge[2] = {};
            Nudge[0].type = INPUT_MOUSE;
            Nudge[0].mi.dx = 1;
            Nudge[0].mi.dwFlags = MOUSEEVENTF_MOVE;
            Nudge[1] = Nudge[0];
            Nudge[1].mi.dx = -1;
            SendInput(2, Nudge, sizeof(INPUT));
        }
    }
    if(Keeping != C.Keeping) { C.Keeping = Keeping; C.Dirty = true; }

    // --- a blinking cursor (odd DECSCUSR shapes) toggles every 530 ms ---
    u64 NowMs = GetTickCount64();
    if((C.CursorShape & 1) && !C.CursorHidden && NowMs - C.BlinkAtMs >= 530)
    {
        C.BlinkOff = !C.BlinkOff;
        C.BlinkAtMs = NowMs;
        C.Dirty = true;
    }

    if(!C.Dirty || C.Grid.empty() || C.Quit) { return; }
    C.Dirty = false;

    // --- the grid as GPU cells: colors resolved, glyph tiles looked up (drawn on first use) ---
    const theme& Theme = Themes[C.Theme];
    // A full atlas empties the glyph cache and the pass runs again.
    size_t N = C.Grid.size();
    C.Gpu.assign(N, gpu_cell{});
    for(s32 Pass = 0; Pass < 2; ++Pass)
    {
        bool32 AtlasFull = false;
        for(s32 Y = 0; Y < C.GridH && !AtlasFull; ++Y)
        {
            for(s32 X = 0; X < C.GridW && !AtlasFull; ++X)
            {
                size_t I = (size_t)Y * C.GridW + X;
                const cell& K = C.Grid[I];
                u32 Fg = ResolveColor(K.Fg, Theme.Fg, Theme);
                u32 Bg = ResolveColor(K.Bg, Theme.Bg, Theme);
                if(K.Attr & ATTR_INVERSE) { u32 T = Fg; Fg = Bg; Bg = T; }
                if(K.Attr & ATTR_DIM) { Fg = ((Fg >> 1) & 0x7F7F7F) + ((Bg >> 1) & 0x7F7F7F); } // halfway to the background, dark or light
                u32 Flags = ((K.Attr & ATTR_UNDERLINE) ? GPU_UNDERLINE : 0) | ((K.Attr & ATTR_STRIKE) ? GPU_STRIKE : 0);
                bool32 Wide = (K.Flags & CELL_WIDE) && X + 1 < C.GridW;
                u32 Cp = K.Codepoint;
                // Blanks, controls, orphaned second halves, hidden text, and marks or joiners with no
                // base before them.
                bool32 Blank = Cp <= ' ' || (K.Flags & CELL_WIDE_CONT) || (K.Attr & ATTR_HIDDEN) || IsClusterExtender(Cp) ||
                               ((K.Flags & CELL_WIDE) && !Wide);
                const cell* Row = C.Grid.data() + (size_t)Y * C.GridW;

                // --- the cluster starting here: the base plus the cells after it that join it (marks,
                // selectors, joiners and what they join, skin tones, keycaps, a flag's second letter).
                // ConPTY gives each part its own cell; the cluster is drawn as one glyph across them ---
                u32 Cps[GLYPH_MAX_CPS] = {};
                u32 Count = 0;
                s32 Span = Wide ? 2 : 1;
                s32 MaxSpan = Min(GLYPH_MAX_CELLS, (s32)C.G.TilesX); // a glyph's tiles share one atlas row
                if(!Blank)
                {
                    Cps[Count++] = Cp;
                    bool32 Flag = IsRegionalIndicator(Cp);
                    for(;;)
                    {
                        s32 Nx = X + Span;
                        if(Nx >= C.GridW || Count >= GLYPH_MAX_CPS) { break; }
                        u32 Next = Row[Nx].Codepoint;
                        s32 Add = ((Row[Nx].Flags & CELL_WIDE) && Nx + 1 < C.GridW) ? 2 : 1;
                        // After a joiner only an emoji or symbol joins, never e.g. a pane's separator line.
                        bool32 AfterZwj = Cps[Count - 1] == 0x200D && (Next >= 0x1F000 || (Next >= 0x2600 && Next <= 0x27BF));
                        bool32 Joins = Next > ' ' && (IsClusterExtender(Next) || AfterZwj || (Flag && Count == 1 && IsRegionalIndicator(Next)));
                        if(!Joins || Span + Add > MaxSpan) { break; }
                        Cps[Count++] = Next;
                        Span += Add;
                    }
                }

                u32 Tile = 0;
                bool32 Color = false;
                if(!Blank)
                {
                    u32 Style = ((K.Attr & ATTR_BOLD) ? STYLE_BOLD : 0) | ((K.Attr & ATTR_ITALIC) ? STYLE_ITALIC : 0);
                    bool32 Overflow = false;
                    if(!GlyphLookup(C.G, C.F, C.R, Cps, Count, Style, Span, &Tile, &Color, &Overflow)) { AtlasFull = true; break; }
                    // A glyph wider than its cells (symbols from a fallback font, emoji a bit over two cells)
                    // may spill into a following blank cell of the same style and draw full size;
                    // otherwise it stays squeezed.
                    s32 Nx = X + Span;
                    bool32 UnderCursor = !C.CursorHidden && C.CursorY == Y && C.CursorX == Nx; // the block would cut the glyph
                    bool32 Room = Overflow && !UnderCursor && Nx < C.GridW && Span < MaxSpan && (Row[Nx].Codepoint == 0 || Row[Nx].Codepoint == ' ') &&
                                  !Row[Nx].Flags && Row[Nx].Fg == K.Fg && Row[Nx].Bg == K.Bg && Row[Nx].Attr == K.Attr;
                    if(Room)
                    {
                        Span += 1;
                        if(!GlyphLookup(C.G, C.F, C.R, Cps, Count, Style, Span, &Tile, &Color, &Overflow)) { AtlasFull = true; break; }
                    }
                }
                if(Color) { Flags |= GPU_COLOR; }
                if(COLOR_TAG(K.Bg) == 0 && !(K.Attr & ATTR_INVERSE)) { Flags |= GPU_SEE_THROUGH; } // the terminal's own background
                for(s32 Part = 0; Part < Span; ++Part)
                {
                    C.Gpu[I + Part] = gpu_cell{Tile ? Tile + (u32)Part : 0, Fg, Bg, Flags};
                }
                X += Span - 1;
            }
        }
        if(!AtlasFull) { break; }
        GlyphCacheReset(C.G);
    }

    // --- the keep-active light, in the bar's last cell (the server leaves it blank): green while
    // this window keeps the user active, grey otherwise ---
    if(!C.KeepActiveOff && C.GridW > 0 && C.GridH > 0)
    {
        size_t I = (size_t)C.GridH * C.GridW - 1;
        C.Gpu[I].Tile = 0;
        C.Gpu[I].Flags |= GPU_DOT;
        C.Gpu[I].Fg = C.Keeping ? PackRgb(90, 210, 110) : PackRgb(100, 100, 100);
    }

    // --- the selection: rows from start to end, each clipped to the selection's pane ---
    if(C.HasSelection)
    {
        s32 Sx = 0, Sy = 0, Ex = 0, Ey = 0;
        SelectionRange(C, &Sx, &Sy, &Ex, &Ey);
        for(s32 Y = Max(Sy, 0); Y <= Ey && Y < C.GridH; ++Y)
        {
            s32 X0 = (Y == Sy) ? Sx : C.SelPane[0];
            s32 X1 = (Y == Ey) ? Ex : C.SelPane[0] + C.SelPane[2] - 1;
            for(s32 X = Max(X0, 0); X <= X1 && X < C.GridW; ++X)
            {
                C.Gpu[(size_t)Y * C.GridW + X].Bg = PackHex(Theme.Selection);
                C.Gpu[(size_t)Y * C.GridW + X].Flags &= ~GPU_SEE_THROUGH;
            }
        }
    }

    // --- cursor, in the shape the program asked for (DECSCUSR): a block swaps the cell's colors
    // (both halves of a wide one), an underline or bar is drawn by the shader; odd shapes blink ---
    bool32 Blinks = (C.CursorShape & 1) != 0;
    if(!C.CursorHidden && !(Blinks && C.BlinkOff) && C.CursorX >= 0 && C.CursorX < C.GridW && C.CursorY >= 0 && C.CursorY < C.GridH)
    {
        size_t I = (size_t)C.CursorY * C.GridW + C.CursorX;
        size_t Count = (C.Grid[I].Flags & CELL_WIDE) && C.CursorX + 1 < C.GridW ? 2 : 1;
        for(size_t K = I; K < I + Count; ++K)
        {
            if(C.CursorShape == 3 || C.CursorShape == 4) { C.Gpu[K].Flags |= GPU_CURSOR_UNDER; }
            else if(C.CursorShape == 5 || C.CursorShape == 6) { if(K == I) { C.Gpu[K].Flags |= GPU_CURSOR_BAR; } }
            else
            {
                u32 T = C.Gpu[K].Fg;
                C.Gpu[K].Fg = C.Gpu[K].Bg;
                C.Gpu[K].Bg = T;
                C.Gpu[K].Flags &= ~GPU_SEE_THROUGH;
            }
        }
    }

    gpu_consts Consts = {};
    Consts.CellW = (u32)C.F.CellW;
    Consts.CellH = (u32)C.F.CellH;
    Consts.GridW = (u32)C.GridW;
    Consts.GridH = (u32)C.GridH;
    Consts.MarginColor = PackHex(Theme.Bg);
    Consts.UnderlineY = (u32)C.F.UnderlineY;
    Consts.StrikeY = (u32)C.F.StrikeY;
    Consts.LineH = (u32)C.F.LineH;
    Consts.CursorThick = (u32)Max(2, C.F.CellW / 6);
    Consts.Opacity = (u32)((100 - C.Transparency) * 255 / 100);
    // --- the image `clod view` shows: fitted to its pane above the caption row, centred, never
    // larger than its own size at this DPI ---
    if(C.ImagePane >= 1 && C.ImagePane <= C.PaneCount && C.ImageW && C.ImageH)
    {
        const s32* P = C.Panes[C.ImagePane - 1];
        float AreaX = (float)(P[0] * C.F.CellW);
        float AreaY = (float)(P[1] * C.F.CellH);
        float AreaW = (float)(P[2] * C.F.CellW);
        float AreaH = (float)(Max(P[3] - 1, 1) * C.F.CellH);
        float Scale = fminf(fminf(AreaW / C.ImageW, AreaH / C.ImageH), (float)C.Dpi / 96.0f);
        float DW = C.ImageW * Scale;
        float DH = C.ImageH * Scale;
        Consts.ImageRect[0] = (u32)(AreaX + (AreaW - DW) / 2);
        Consts.ImageRect[1] = (u32)(AreaY + (AreaH - DH) / 2);
        Consts.ImageRect[2] = Consts.ImageRect[0] + (u32)Max(1, (s32)DW);
        Consts.ImageRect[3] = Consts.ImageRect[1] + (u32)Max(1, (s32)DH);
    }
    bool32 Drawn = RendererDraw(C.R, C.Gpu, Consts);
    LatencyMark("client_present");
    if(!Drawn)
    {
        // --- device lost: start the GPU side over (ApplyFont makes new text formats); the next
        // tick redraws everything ---
        RendererRelease(C.R);
        if(!RendererCreate(C.R, C.Window)) { C.Quit = true; return; }
        ApplyFont(C);
        RendererSetImage(C.R, C.ImagePixels.data(), C.ImageW, C.ImageH);
    }
}

// Keys that don't arrive as WM_CHAR text, as the VT sequences Windows Terminal sends. Empty = not
// special: let TranslateMessage turn it into characters.
static std::string SpecialKey(WPARAM Vk, bool32 Shift, bool32 Ctrl, bool32 Alt)
{
    s32 Mod = 1 + (Shift ? 1 : 0) + (Alt ? 2 : 0) + (Ctrl ? 4 : 0);
    char Buf[32] = {};
    char Final = 0;
    s32 Tilde = 0;
    switch(Vk)
    {
        case VK_UP: Final = 'A'; break;
        case VK_DOWN: Final = 'B'; break;
        case VK_RIGHT: Final = 'C'; break;
        case VK_LEFT: Final = 'D'; break;
        case VK_HOME: Final = 'H'; break;
        case VK_END: Final = 'F'; break;
        case VK_F1: Final = 'P'; break;
        case VK_F2: Final = 'Q'; break;
        case VK_F3: Final = 'R'; break;
        case VK_F4: if(Alt) { return std::string(); } Final = 'S'; break; // Alt+F4 closes the window
        case VK_INSERT: Tilde = 2; break;
        case VK_DELETE: Tilde = 3; break;
        case VK_PRIOR: Tilde = 5; break;
        case VK_NEXT: Tilde = 6; break;
        case VK_F5: Tilde = 15; break;
        case VK_F6: Tilde = 17; break;
        case VK_F7: Tilde = 18; break;
        case VK_F8: Tilde = 19; break;
        case VK_F9: Tilde = 20; break;
        case VK_F10: Tilde = 21; break;
        case VK_F11: Tilde = 23; break;
        case VK_F12: Tilde = 24; break;
        case VK_TAB: return Shift ? std::string("\x1b[Z") : std::string();
        case VK_RETURN: return Shift ? std::string("\x1b\r") : std::string(); // Shift+Enter: a newline in claude's prompt
        case VK_BACK: return Ctrl ? std::string("\x08") : (Alt ? std::string("\x1b\x7f") : std::string("\x7f"));
        case VK_ESCAPE: return std::string("\x1b");
        case VK_SPACE: return Ctrl ? std::string(1, '\0') : std::string();
        default: return std::string();
    }
    if(Final >= 'P' && Final <= 'S') { snprintf(Buf, sizeof(Buf), Mod > 1 ? "\x1b[1;%d%c" : "\x1bO%c", Mod > 1 ? Mod : Final, Final); }
    else if(Final) { snprintf(Buf, sizeof(Buf), Mod > 1 ? "\x1b[1;%d%c" : "\x1b[%c", Mod > 1 ? Mod : Final, Final); }
    else { snprintf(Buf, sizeof(Buf), Mod > 1 ? "\x1b[%d;%d~" : "\x1b[%d~", Tilde, Mod); }
    return Buf;
}

// Mouse event as an SGR report, if the server wants this kind of event right now.
static void MouseReport(clodw& C, s32 Button, LPARAM Pos, bool32 Release)
{
    if(!C.MouseMode || C.F.CellW <= 0) { return; }
    s32 X = Clamp(GET_X_LPARAM(Pos) / C.F.CellW + 1, 1, Max(C.GridW, 1));
    s32 Y = Clamp(GET_Y_LPARAM(Pos) / C.F.CellH + 1, 1, Max(C.GridH, 1));
    bool32 Motion = (Button & 32) != 0;
    if(Motion && X == C.MouseX && Y == C.MouseY) { return; }
    if(GetKeyState(VK_SHIFT) < 0) { Button |= 4; }
    if(GetKeyState(VK_MENU) < 0) { Button |= 8; }
    if(GetKeyState(VK_CONTROL) < 0) { Button |= 16; }
    char Buf[48];
    snprintf(Buf, sizeof(Buf), "\x1b[<%d;%d;%d%c", Button, X, Y, Release ? 'm' : 'M');
    C.InBytes += Buf;
    C.MouseX = X;
    C.MouseY = Y;
}

static LRESULT CALLBACK ClodwWindowProc(HWND Window, UINT Msg, WPARAM W, LPARAM L)
{
    if(Msg == WM_NCCREATE) { SetWindowLongPtrW(Window, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)L)->lpCreateParams); }
    clodw* C = (clodw*)GetWindowLongPtrW(Window, GWLP_USERDATA);
    if(!C) { return DefWindowProcW(Window, Msg, W, L); }
    switch(Msg)
    {
        case WM_CLOSE: C->Quit = true; return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT Ps = {};
            BeginPaint(Window, &Ps);
            EndPaint(Window, &Ps);
            C->Dirty = true;
            return 0;
        }
        case WM_SIZE: C->Dirty = true; return 0;

        // --- move/resize: Windows runs its own loop, so tick from a timer meanwhile ---
        case WM_ENTERSIZEMOVE: SetTimer(Window, 1, 16, 0); return 0;
        case WM_EXITSIZEMOVE: KillTimer(Window, 1); return 0;
        case WM_TIMER: Tick(*C); return 0;

        case WM_DPICHANGED:
        {
            C->Dpi = HIWORD(W);
            ApplyFont(*C);
            RECT* Suggested = (RECT*)L;
            SetWindowPos(Window, 0, Suggested->left, Suggested->top, Suggested->right - Suggested->left,
                         Suggested->bottom - Suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        // --- text: WM_CHAR units to UTF-8; Alt+key arrives as WM_SYSCHAR and gets an ESC prefix ---
        case WM_CHAR:
            LatencyMark("key");
            [[fallthrough]];
        case WM_SYSCHAR:
        {
            wchar_t U = (wchar_t)W;
            if(Msg == WM_SYSCHAR && U == ' ') { break; } // Alt+Space: the window menu
            if(U >= 0xD800 && U <= 0xDBFF) { C->HighSurrogate = U; return 0; }
            u32 Cp = U;
            if(U >= 0xDC00 && U <= 0xDFFF && C->HighSurrogate) { Cp = 0x10000 + ((C->HighSurrogate - 0xD800) << 10) + (U - 0xDC00); }
            C->HighSurrogate = 0;
            if(Msg == WM_SYSCHAR) { C->InBytes += '\x1b'; }
            AppendUtf8(C->InBytes, Cp);
            return 0;
        }
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            // Alt combinations are input, not the menu bar; Alt+F4 and Alt+Space (window menu) keep their meaning.
            if((W == VK_F4 || W == VK_SPACE) && Msg == WM_SYSKEYDOWN) { break; }
            return 0;

        // --- Font... in the window menu: any installed font; applied now and saved to settings.txt ---
        // The window menu opening: "Be a good programmer" is ticked while the global CLAUDE.md has the
        // block (it may have been edited since).
        case WM_INITMENU:
        {
            bool32 On = ReadWholeFile(GlobalClaudeMd()).find(GoodStart) != std::string::npos;
            CheckMenuItem(GetSystemMenu(Window, FALSE), IDM_GOOD_PROGRAMMER, MF_BYCOMMAND | (On ? MF_CHECKED : MF_UNCHECKED));
        } break;
        case WM_SYSCOMMAND:
        {
            // Opacity > 85% etc.: the terminal's background lets the desktop through; saved to settings.txt.
            u32 Cmd = (u32)(W & 0xFFF0);
            if(Cmd >= IDM_OPACITY && Cmd < IDM_OPACITY + ArrayCount(OpacitySteps) * 16)
            {
                s32 Opacity = OpacitySteps[(Cmd - IDM_OPACITY) / 16];
                C->Transparency = 100 - Opacity;
                CheckOpacity(*C);
                WriteSetting("opacity", std::to_string(Opacity));
                C->Dirty = true;
                return 0;
            }
            // Be a good programmer: the server appends the user's programming principles to the system
            // prompt of each claude it starts from now on (and of `claude` typed in shells opened after).
            if(Cmd == IDM_GOOD_PROGRAMMER)
            {
                bool32 On = ReadWholeFile(GlobalClaudeMd()).find(GoodStart) == std::string::npos;
                if(!SetGoodProgrammer(On))
                {
                    std::wstring Text = L"Couldn't update " + GlobalClaudeMd();
                    MessageBoxW(Window, Text.c_str(), L"clod", MB_OK | MB_ICONWARNING);
                }
                return 0;
            }
            // Theme > Nord etc.: colors for the terminal and clod's bars; saved to settings.txt.
            if(Cmd >= IDM_THEME && Cmd < IDM_THEME + ArrayCount(Themes) * 16)
            {
                C->Theme = (Cmd - IDM_THEME) / 16;
                CheckTheme(*C);
                ApplyTitleBar(*C);
                WriteSetting("theme", Themes[C->Theme].Name);
                C->Dirty = true;
                return 0;
            }
            if(Cmd != IDM_FONT) { break; }
            LOGFONTW Lf = {};
            wcsncpy_s(Lf.lfFaceName, C->FontFamily.c_str(), _TRUNCATE);
            HDC Dc = GetDC(Window);
            Lf.lfHeight = -MulDiv((int)(C->FontPt * 10.0f + 0.5f), GetDeviceCaps(Dc, LOGPIXELSY), 720);
            ReleaseDC(Window, Dc);
            CHOOSEFONTW Cf = {};
            Cf.lStructSize = sizeof(Cf);
            Cf.hwndOwner = Window;
            Cf.lpLogFont = &Lf;
            Cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOSTYLESEL | CF_NOSCRIPTSEL;
            if(!ChooseFontW(&Cf)) { return 0; }
            C->FontFamily = Lf.lfFaceName;
            C->FontPt = Max(Cf.iPointSize, 10) / 10.0f;
            ApplyFont(*C);
            SaveFontSettings(C->FontFamily, C->FontPt);
            return 0;
        }

        // --- IME (Chinese, Japanese, Korean input): composition and candidates appear at the cursor,
        // in the window's font; Windows sends the finished text as WM_CHAR ---
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        {
            HIMC Imc = ImmGetContext(Window);
            if(Imc)
            {
                POINT At = {C->CursorX * C->F.CellW, C->CursorY * C->F.CellH};
                COMPOSITIONFORM Form = {CFS_POINT, At, {}};
                ImmSetCompositionWindow(Imc, &Form);
                CANDIDATEFORM Candidates = {0, CFS_CANDIDATEPOS, {At.x, At.y + C->F.CellH}, {}};
                ImmSetCandidateWindow(Imc, &Candidates);
                LOGFONTW Font = {};
                Font.lfHeight = -C->F.CellH;
                wcsncpy_s(Font.lfFaceName, C->FontFamily.c_str(), _TRUNCATE);
                ImmSetCompositionFontW(Imc, &Font);
                ImmReleaseContext(Window, Imc);
            }
            break;
        }

        case WM_SETFOCUS: C->InBytes += "\x1b[I"; C->Dirty = true; return 0;  // Dirty: the keep-active light
        case WM_KILLFOCUS: C->InBytes += "\x1b[O"; C->Dirty = true; return 0;

        // --- mouse, reported the way the server asked for (MouseMode) ---
        // --- right-click pastes (never reported to the program; Claude's fullscreen has no use for it) ---
        case WM_RBUTTONDOWN: PasteClipboard(*C); return 0;
        case WM_RBUTTONUP: return 0;

        case WM_LBUTTONDOWN: case WM_MBUTTONDOWN:
        {
            s32 Button = (Msg == WM_LBUTTONDOWN) ? 0 : (Msg == WM_MBUTTONDOWN ? 1 : 2);
            s32 Cx = GET_X_LPARAM(L) / Max(C->F.CellW, 1);
            s32 Cy = GET_Y_LPARAM(L) / Max(C->F.CellH, 1);
            if(C->MouseHeld || C->Selecting) { return 0; } // a second button during a drag
            if(C->HasSelection) { C->HasSelection = false; C->Dirty = true; }

            // --- Ctrl+click opens the http/https link under the pointer (not file://: that could run a
            // program); the scan stays inside the pane ---
            if(Button == 0 && (W & MK_CONTROL) && Cy >= 0 && Cy < C->GridH && Cx >= 0 && Cx < C->GridW)
            {
                const cell* Row = C->Grid.data() + (size_t)Cy * C->GridW;
                auto Stop = [](u32 Cp) { return Cp <= ' ' || (Cp < 0x80 && strchr("<>\"'`|", (int)Cp)); };
                s32 Lo = 0;
                s32 Hi = C->GridW - 1;
                for(s32 P = 0; P < C->PaneCount; ++P)
                {
                    const s32* R = C->Panes[P];
                    if(Cx >= R[0] && Cx < R[0] + R[2] && Cy >= R[1] && Cy < R[1] + R[3]) { Lo = R[0]; Hi = Min(R[0] + R[2] - 1, Hi); }
                }
                s32 X0 = Cx;
                s32 X1 = Cx;
                while(X0 > Lo && !Stop(Row[X0 - 1].Codepoint)) { X0 -= 1; }
                while(X1 < Hi && !Stop(Row[X1 + 1].Codepoint)) { X1 += 1; }
                std::string Link;
                for(s32 X = X0; X <= X1; ++X) { if(!(Row[X].Flags & CELL_WIDE_CONT) && !Stop(Row[X].Codepoint)) { AppendUtf8(Link, Row[X].Codepoint); } }
                while(!Link.empty() && strchr(".,;:!?)]}", Link.back())) { Link.pop_back(); }
                if(Link.compare(0, 7, "http://") == 0 || Link.compare(0, 8, "https://") == 0)
                {
                    std::wstring Wide(Link.size() + 1, 0);
                    Wide.resize(MultiByteToWideChar(CP_UTF8, 0, Link.data(), (int)Link.size(), &Wide[0], (int)Wide.size()));
                    ShellExecuteW(Window, L"open", Wide.c_str(), 0, 0, SW_SHOWNORMAL);
                    return 0;
                }
            }

            // --- selection: a plain drag in a pane whose program doesn't take the mouse, Shift+drag in
            // any pane; it stays inside the pane it starts in. A plain one still reports the press and
            // release (the server focuses the pane), Shift+drag over a program reports nothing ---
            bool32 Selects = false;
            bool32 Quiet = false;
            for(s32 P = 0; P < C->PaneCount && Button == 0; ++P)
            {
                const s32* R = C->Panes[P];
                if(Cx < R[0] || Cx >= R[0] + R[2] || Cy < R[1] || Cy >= R[1] + R[3]) { continue; }
                bool32 Taken = (C->PaneMouseTaken >> P) & 1;
                Selects = !Taken || (W & MK_SHIFT);
                Quiet = Taken;
                if(Selects)
                {
                    memcpy(C->SelPane, R, sizeof(C->SelPane));
                    C->Selecting = true;
                    C->SelAx = C->SelBx = Cx;
                    C->SelAy = C->SelBy = Cy;
                    SetCapture(Window);

                    // --- double-click selects a word, triple-click the pane's row ---
                    u64 NowMs = GetTickCount64();
                    bool32 Again = NowMs - C->ClickAtMs <= GetDoubleClickTime() && Cx == C->ClickX && Cy == C->ClickY;
                    C->Clicks = Again ? C->Clicks % 3 + 1 : 1;
                    C->ClickAtMs = NowMs;
                    C->ClickX = Cx;
                    C->ClickY = Cy;
                    if(C->Clicks == 2 && Cy < C->GridH)
                    {
                        const cell* Row = C->Grid.data() + (size_t)Cy * C->GridW;
                        auto Stop = [](const cell& Cl) { return !(Cl.Flags & CELL_WIDE_CONT) && (Cl.Codepoint <= ' ' || (Cl.Codepoint < 0x80 && strchr("\"'`()[]{}<>|,;", (int)Cl.Codepoint))); };
                        if(!Stop(Row[Cx]))
                        {
                            while(C->SelAx > R[0] && !Stop(Row[C->SelAx - 1])) { C->SelAx -= 1; }
                            while(C->SelBx + 1 < R[0] + R[2] && !Stop(Row[C->SelBx + 1])) { C->SelBx += 1; }
                            C->HasSelection = true;
                        }
                    }
                    if(C->Clicks == 3)
                    {
                        C->SelAx = R[0];
                        C->SelBx = R[0] + R[2] - 1;
                        C->HasSelection = true;
                    }
                    C->Dirty = true;
                }
            }
            if(Selects && Quiet) { return 0; }
            SetCapture(Window);
            C->MouseHeld = Button + 1;
            MouseReport(*C, Button, L, false);
            return 0;
        }
        case WM_LBUTTONUP: case WM_MBUTTONUP:
        {
            s32 Button = (Msg == WM_LBUTTONUP) ? 0 : 1;
            if(C->Selecting && Button == 0)
            {
                // A finished selection goes straight to the clipboard and stays highlighted.
                C->Selecting = false;
                if(C->HasSelection && !C->CopyOnSelectOff) { CopySelection(*C); }
            }
            if(C->MouseHeld == Button + 1) // only a release whose press was reported (not a link click)
            {
                C->MouseHeld = 0;
                MouseReport(*C, Button, L, true);
            }
            ReleaseCapture(); // last: it sends WM_CAPTURECHANGED, which must find nothing left to end
            return 0;
        }
        case WM_CAPTURECHANGED:
        {
            // Capture lost mid-drag (Alt+Tab, the Win key, a lock screen): end the selection, and let
            // the program see its button go up where the pointer last was.
            if((HWND)L == Window) { return 0; }
            if(C->Selecting)
            {
                C->Selecting = false;
                if(C->HasSelection && !C->CopyOnSelectOff) { CopySelection(*C); }
            }
            if(C->MouseHeld)
            {
                LPARAM At = MAKELPARAM(Max(C->MouseX - 1, 0) * C->F.CellW, Max(C->MouseY - 1, 0) * C->F.CellH);
                MouseReport(*C, C->MouseHeld - 1, At, true);
                C->MouseHeld = 0;
            }
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            if(C->Selecting)
            {
                // The moving end follows the pointer, held inside the selection's pane.
                s32 Cx = Clamp(GET_X_LPARAM(L) / Max(C->F.CellW, 1), C->SelPane[0], C->SelPane[0] + C->SelPane[2] - 1);
                s32 Cy = Clamp(GET_Y_LPARAM(L) / Max(C->F.CellH, 1), C->SelPane[1], C->SelPane[1] + C->SelPane[3] - 1);
                if(C->Clicks > 1 && Cx == C->ClickX && Cy == C->ClickY) { return 0; } // keep the word/row until the drag leaves its cell
                if(Cx != C->SelBx || Cy != C->SelBy)
                {
                    C->SelBx = Cx;
                    C->SelBy = Cy;
                    C->HasSelection = C->HasSelection || Cx != C->SelAx || Cy != C->SelAy; // a click alone selects nothing
                    C->Dirty = true;
                }
                return 0;
            }
            if(C->MouseHeld) { MouseReport(*C, (C->MouseHeld - 1) | 32, L, false); }
            else if(C->MouseMode == 1003) { MouseReport(*C, 3 | 32, L, false); }
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            POINT P = {GET_X_LPARAM(L), GET_Y_LPARAM(L)};
            ScreenToClient(Window, &P);
            C->WheelAccum += GET_WHEEL_DELTA_WPARAM(W);
            while(C->WheelAccum >= WHEEL_DELTA || C->WheelAccum <= -WHEEL_DELTA)
            {
                bool32 Up = C->WheelAccum > 0;
                C->WheelAccum += Up ? -WHEEL_DELTA : WHEEL_DELTA;
                if(GET_KEYSTATE_WPARAM(W) & MK_CONTROL) // Ctrl+wheel zooms, like Ctrl+= / Ctrl+-
                {
                    C->FontPt = fminf(fmaxf(C->FontPt + (Up ? 1.0f : -1.0f), 6.0f), 72.0f);
                    ApplyFont(*C);
                    SaveFontSettings(C->FontFamily, C->FontPt);
                    continue;
                }
                C->MouseX = 0; // a wheel report is never a duplicate
                MouseReport(*C, Up ? 64 : 65, MAKELPARAM(P.x, P.y), false);
            }
            return 0;
        }
    }
    return DefWindowProcW(Window, Msg, W, L);
}

static int WindowMain(s32 JumpToTab)
{
    static clodw C = {};
    RaiseForTyping(GetCurrentProcess());
    SeedUserSetup(); // a new install's default settings.txt (opacity 95) is there before it's read
    InstallFonts();  // Iosevka Clod, clod's default font, before the font is picked
    C.FontFamily = L"Iosevka Clod"; // bundled and installed by InstallFonts
    C.FontPt = DEFAULT_FONT_PT;
    C.KeepActiveOff = (ReadSetting("keep_active") == "off");
    C.CopyOnSelectOff = (ReadSetting("copy_on_select") == "off");
    s32 Opacity = atoi(ReadSetting("opacity").c_str());
    if(Opacity > 0) { C.Transparency = 100 - Clamp(Opacity, 20, 100); } // never so faint the window is lost
    std::string ThemeName = ReadSetting("theme");
    for(u32 I = 0; I < ArrayCount(Themes); ++I) { if(_stricmp(ThemeName.c_str(), Themes[I].Name) == 0) { C.Theme = I; } }

    // --- settings ---
    std::string Font = ReadSetting("font");
    if(!Font.empty())
    {
        std::wstring Wide(Font.size() + 1, 0);
        Wide.resize(MultiByteToWideChar(CP_UTF8, 0, Font.data(), (int)Font.size(), &Wide[0], (int)Wide.size()));
        C.FontFamily = Wide;
    }
    float Size = (float)atof(ReadSetting("font_size").c_str());
    if(Size > 0) { C.FontPt = Size; }

    // --- window, renderer and font; sized to 120x36 cells at the monitor's DPI ---
    HINSTANCE Instance = GetModuleHandleW(0);
    WNDCLASSW Class = {};
    Class.lpfnWndProc = ClodwWindowProc;
    Class.hInstance = Instance;
    Class.hIcon = LoadIconW(Instance, MAKEINTRESOURCEW(1));
    Class.hCursor = LoadCursorW(0, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    Class.lpszClassName = L"clodw";
    RegisterClassW(&Class);
    DWORD Style = WS_OVERLAPPEDWINDOW;
    C.Window = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, L"clodw", L"clod", Style, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, 0, 0, Instance, &C);
    if(!C.Window || !RendererCreate(C.R, C.Window)) { return 1; }
    HMENU WindowMenu = GetSystemMenu(C.Window, FALSE);
    AppendMenuW(WindowMenu, MF_SEPARATOR, 0, 0);
    AppendMenuW(WindowMenu, MF_STRING, IDM_FONT, L"Font...");
    C.OpacityMenu = CreatePopupMenu();
    for(u32 I = 0; I < ArrayCount(OpacitySteps); ++I)
    {
        wchar_t Label[16] = {};
        swprintf(Label, 16, L"%d%%", OpacitySteps[I]);
        AppendMenuW(C.OpacityMenu, MF_STRING, IDM_OPACITY + I * 16, Label);
    }
    AppendMenuW(WindowMenu, MF_POPUP, (UINT_PTR)C.OpacityMenu, L"Opacity");
    CheckOpacity(C);
    C.ThemeMenu = CreatePopupMenu();
    for(u32 I = 0; I < ArrayCount(Themes); ++I)
    {
        wchar_t Label[64] = {};
        MultiByteToWideChar(CP_UTF8, 0, Themes[I].Name, -1, Label, ArrayCount(Label));
        AppendMenuW(C.ThemeMenu, MF_STRING, IDM_THEME + I * 16, Label);
    }
    AppendMenuW(WindowMenu, MF_POPUP, (UINT_PTR)C.ThemeMenu, L"Theme");
    CheckTheme(C);
    ApplyTitleBar(C);
    AppendMenuW(WindowMenu, MF_STRING, IDM_GOOD_PROGRAMMER, L"Be a good programmer");
    C.Dpi = GetDpiForWindow(C.Window);
    ApplyFont(C);

    // --- where the window was last time ("left top right bottom maximized"), if that's still on a
    // monitor; placed twice because landing on a monitor of another DPI rescales the first try ---
    s32 Show = SW_SHOWNORMAL;
    WINDOWPLACEMENT Place = {sizeof(Place)};
    s32 Maximized = 0;
    FILE* PlaceFile = 0;
    _wfopen_s(&PlaceFile, WindowFilePath().c_str(), L"rb");
    bool32 Placed = PlaceFile && fscanf_s(PlaceFile, "%ld %ld %ld %ld %d", &Place.rcNormalPosition.left, &Place.rcNormalPosition.top,
                                          &Place.rcNormalPosition.right, &Place.rcNormalPosition.bottom, &Maximized) == 5;
    if(PlaceFile) { fclose(PlaceFile); }
    // Still usable: a good part of it (say its title bar) lies on a monitor's work area, not just a sliver.
    MONITORINFO Monitor = {sizeof(Monitor)};
    RECT Visible = {};
    Placed = Placed && GetMonitorInfoW(MonitorFromRect(&Place.rcNormalPosition, MONITOR_DEFAULTTONEAREST), &Monitor) &&
             IntersectRect(&Visible, &Monitor.rcWork, &Place.rcNormalPosition) && Visible.right - Visible.left >= 200 &&
             Visible.bottom - Visible.top >= 100;
    if(Placed)
    {
        Place.showCmd = SW_HIDE;
        SetWindowPlacement(C.Window, &Place);
        SetWindowPlacement(C.Window, &Place);
        Show = Maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    }
    else
    {
        // First run: 120x36 cells at the monitor's DPI.
        RECT Want = {0, 0, 120 * C.F.CellW, 36 * C.F.CellH};
        AdjustWindowRectExForDpi(&Want, Style, FALSE, 0, C.Dpi);
        SetWindowPos(C.Window, 0, 0, 0, Want.right - Want.left, Want.bottom - Want.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    ShowWindow(C.Window, Show);

    // --- connect (starting clod.exe --server from this folder if needed) and attach for cells ---
    wchar_t Self[MAX_PATH] = {};
    GetModuleFileNameW(0, Self, MAX_PATH);
    std::wstring ServerExe = Self;
    ServerExe = ServerExe.substr(0, ServerExe.find_last_of(L'\\') + 1) + L"clod.exe";
    std::string Error;
    HANDLE Pipe = ConnectToServer(ServerExe, Error);
    if(!Pipe)
    {
        MessageBoxA(C.Window, Error.c_str(), "clod", MB_OK | MB_ICONERROR);
        return 1;
    }
    static ipc_conn Server = {};
    IpcInit(Server, Pipe);
    C.Server = &Server;
    {
        RECT Client = {};
        GetClientRect(C.Window, &Client);
        msg_attach A = {};
        A.Version = CLOD_PROTOCOL_VERSION;
        A.Width = Max(1, (s32)Client.right / C.F.CellW);
        A.Height = Max(2, (s32)Client.bottom / C.F.CellH);
        A.JumpToTab = JumpToTab;
        A.ConsoleWindow = (u64)(uintptr_t)C.Window;
        A.Flags = ATTACH_CELLS;
        C.SentW = A.Width;
        C.SentH = A.Height;
        wchar_t Cwd[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, Cwd);
        char Cwd8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, Cwd, -1, Cwd8, sizeof(Cwd8), 0, 0);
        std::string Payload((const char*)&A, sizeof(A));
        Payload += Cwd8;
        IpcSendString(Server, MSG_ATTACH, Payload);
    }
    IpcPump(Server);

    // --- loop: wait for the server or the window, then tick ---
    while(!C.Quit)
    {
        HANDLE Waits[1] = {Server.ReadEvent};
        // Wakes for keep-active too, and often enough to blink a blinking cursor.
        DWORD Timeout = ((C.CursorShape & 1) && !C.CursorHidden) ? 100 : 30000;
        MsgWaitForMultipleObjectsEx(1, Waits, Timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG Msg = {};
        while(PeekMessageW(&Msg, 0, 0, 0, PM_REMOVE))
        {
            if(Msg.message == WM_QUIT) { C.Quit = true; break; }
            if(Msg.message == WM_KEYDOWN || Msg.message == WM_SYSKEYDOWN)
            {
                bool32 Shift = GetKeyState(VK_SHIFT) < 0;
                bool32 Ctrl = GetKeyState(VK_CONTROL) < 0;
                bool32 Alt = GetKeyState(VK_MENU) < 0;
                bool32 Modifier = Msg.wParam == VK_SHIFT || Msg.wParam == VK_CONTROL || Msg.wParam == VK_MENU;
                if(!Modifier)
                {
                    C.BlinkOff = false; // typing shows a blinking cursor at once
                    C.BlinkAtMs = GetTickCount64();
                }

                // --- copy: Ctrl+C with a selection (without one it stays ^C for the program), or
                // Ctrl+Shift+C. Rows are clipped to the selection's pane, trailing blanks dropped ---
                if(Msg.wParam == 'C' && Ctrl && !Alt && (Shift || C.HasSelection))
                {
                    if(C.HasSelection)
                    {
                        CopySelection(C);
                        C.HasSelection = false;
                        C.Dirty = true;
                    }
                    continue;
                }
                if(C.HasSelection && !Modifier) { C.HasSelection = false; C.Dirty = true; } // typing ends a selection

                // --- paste: Ctrl+Shift+V or Shift+Insert, as a bracketed paste with CR line ends ---
                if((Msg.wParam == 'V' && Ctrl && Shift) || (Msg.wParam == VK_INSERT && Shift && !Ctrl))
                {
                    PasteClipboard(C);
                    continue;
                }

                // --- zoom: Ctrl+= / Ctrl+- change the font size, Ctrl+0 goes back to the default; saved like Font... ---
                bool32 ZoomIn = Msg.wParam == VK_OEM_PLUS || Msg.wParam == VK_ADD;
                bool32 ZoomOut = Msg.wParam == VK_OEM_MINUS || Msg.wParam == VK_SUBTRACT;
                bool32 ZoomReset = Msg.wParam == '0' || Msg.wParam == VK_NUMPAD0;
                if(Ctrl && !Alt && !Shift && (ZoomIn || ZoomOut || ZoomReset))
                {
                    C.FontPt = ZoomReset ? DEFAULT_FONT_PT : fminf(fmaxf(C.FontPt + (ZoomIn ? 1.0f : -1.0f), 6.0f), 72.0f);
                    ApplyFont(C);
                    SaveFontSettings(C.FontFamily, C.FontPt);
                    continue;
                }
                // --- clod commands without the prefix (only a window can tell Ctrl+Shift+T from Ctrl+T):
                // Ctrl+Tab cycles tabs as Ctrl+PgDn/PgUp do; the rest go to the home screen (CSI 7777 ~,
                // which never toggles back out of it) and press its key there ---
                const char* Command = 0;
                if(Ctrl && !Alt && Msg.wParam == VK_TAB) { Command = Shift ? "\x1b[5;5~" : "\x1b[6;5~"; }
                else if(Ctrl && Shift && !Alt)
                {
                    switch(Msg.wParam)
                    {
                        case 'T': Command = "\x1b[7777~t"; break; // new shell, in this tab's folder
                        case 'N': Command = "\x1b[7777~n"; break; // new claude, in this tab's folder
                        case 'O': Command = "\x1b[7777~o"; break; // new claude in a folder you type
                        case 'S': Command = "\x1b[7777~"; break;  // home screen
                        case VK_SPACE: Command = "\x1b[7777~"; break;
                        case 'W': Command = "\x1b[7777~x"; break; // close this tab (asks y/n)
                    }
                }
                if(Command)
                {
                    C.InBytes += Command;
                    continue;
                }
                std::string Seq = SpecialKey(Msg.wParam, Shift, Ctrl, Alt);
                if(!Seq.empty())
                {
                    C.InBytes += Seq;
                    continue; // no TranslateMessage: no WM_CHAR for this key
                }
            }
            TranslateMessage(&Msg);
            DispatchMessageW(&Msg);
        }
        Tick(C);
    }

    IpcClose(Server);

    // --- remember where the window is (not for a throwaway --test window) ---
    WINDOWPLACEMENT Now = {sizeof(Now)};
    if(GetEnvironmentVariableW(L"CLOD_EPHEMERAL", 0, 0) == 0 && GetWindowPlacement(C.Window, &Now))
    {
        bool32 Zoomed = Now.showCmd == SW_SHOWMAXIMIZED || (Now.showCmd == SW_SHOWMINIMIZED && (Now.flags & WPF_RESTORETOMAXIMIZED));
        std::wstring Path = WindowFilePath();
        CreateDirectoryW(Path.substr(0, Path.find_last_of(L'\\')).c_str(), 0);
        FILE* File = 0;
        _wfopen_s(&File, Path.c_str(), L"wb");
        if(File)
        {
            fprintf(File, "%ld %ld %ld %ld %d\r\n", Now.rcNormalPosition.left, Now.rcNormalPosition.top, Now.rcNormalPosition.right, Now.rcNormalPosition.bottom, Zoomed ? 1 : 0);
            fclose(File);
        }
    }
    DestroyWindow(C.Window);
    // Another window took over, or the user detached: nothing to report. A lost server is news.
    bool32 News = C.ByeReason == "lost connection to the clod server" || C.ByeReason.find("different clod build") != std::string::npos;
    if(News) { MessageBoxA(0, C.ByeReason.c_str(), "clod", MB_OK | MB_ICONWARNING); }
    return 0;
}
