// Terminal emulator for one child session. Pure state machine: bytes in, screen state out.
// Anything the child expects answered (cursor reports, device attributes) is queued in Reply;
// sequences meant for the outer terminal (OSC 52 clipboard) are queued in PassThrough.

enum vt_parse_state
{
    VT_GROUND = 0,
    VT_ESCAPE,
    VT_ESCAPE_INTER,
    VT_CSI,
    VT_OSC,
    VT_OSC_ESC,
    VT_STRING,     // DCS / SOS / PM / APC: swallowed until ST
    VT_STRING_ESC,
};

#define VT_MAX_PARAMS 16
#define VT_SCROLLBACK_MAX 10000
#define VT_OSC_MAX (4 << 20) // OSC 52 carries whole copies (base64)

struct vt_terminal
{
    s32 Cols;
    s32 Rows;
    std::vector<cell> Screens[2]; // [0] main, [1] alternate
    bool32 AltActive;
    std::deque<std::vector<cell>> Scrollback; // main screen only, oldest first, trailing blanks trimmed
    u64 ScrollbackPushed;                     // lines ever pushed; viewers use it to hold their scroll position

    s32 CursorX;
    s32 CursorY;
    bool32 PendingWrap;
    cell Pen;
    s32 ScrollTop;
    s32 ScrollBottomInset; // rows below the scroll region; 0 = region reaches the last row

    s32 SavedX;
    s32 SavedY;
    cell SavedPen;
    bool32 SavedOriginMode;

    bool32 CursorHidden;
    s32 CursorShape; // DECSCUSR: 0 default, 1/2 block, 3/4 underline, 5/6 bar (odd = blinking)
    bool32 AutowrapOff;
    bool32 OriginMode;
    bool32 InsertMode;
    bool32 AppCursorKeys;
    bool32 BracketedPaste;
    bool32 MouseSgr;
    bool32 FocusEvents;
    bool32 SyncOutput;
    s32 MouseMode; // 0, 1000, 1002 or 1003
    bool32 G0Graphics;
    bool32 G1Graphics;
    bool32 ShiftOut;

    s32 State;
    s32 Params[VT_MAX_PARAMS];
    s32 ParamCount;
    char Private;
    char Intermediate;
    char EscIntermediate;
    u32 Utf8Cp;
    s32 Utf8Need;
    std::string Osc;
    u32 LastPrinted;

    std::string Title;
    std::string Reply;
    std::string PassThrough;
    std::string Notify; // latest OSC 9 / OSC 777 notification text, cleared by the reader
    bool32 Bell;
};

static s32 VtBottom(const vt_terminal& T) { return T.Rows - 1 - T.ScrollBottomInset; }
static cell* VtRow(vt_terminal& T, s32 Y) { return T.Screens[T.AltActive ? 1 : 0].data() + (size_t)Y * T.Cols; }

// Erased cells keep the current background (BCE), like xterm.
static cell VtBlank(const vt_terminal& T)
{
    cell C = {};
    C.Bg = T.Pen.Bg;
    return C;
}

static void VtClearCells(vt_terminal& T, s32 Y, s32 X0, s32 X1)
{
    if(Y < 0 || Y >= T.Rows) { return; }
    X0 = Clamp(X0, 0, T.Cols);
    X1 = Clamp(X1, 0, T.Cols);
    cell* Row = VtRow(T, Y);
    cell B = VtBlank(T);
    for(s32 X = X0; X < X1; ++X) { Row[X] = B; }
}

static void VtPushScrollback(vt_terminal& T, const cell* Row, s32 Cols)
{
    s32 Len = Cols;
    cell Zero = {};
    while(Len > 0 && CellEqual(Row[Len - 1], Zero)) { --Len; }
    T.Scrollback.emplace_back(Row, Row + Len);
    T.ScrollbackPushed += 1;
    while(T.Scrollback.size() > VT_SCROLLBACK_MAX) { T.Scrollback.pop_front(); }
}

static void VtScrollUp(vt_terminal& T, s32 Top, s32 Bottom, s32 N)
{
    N = Min(N, Bottom - Top + 1);
    if(N <= 0) { return; }
    if(!T.AltActive && Top == 0)
    {
        for(s32 I = 0; I < N; ++I) { VtPushScrollback(T, VtRow(T, I), T.Cols); }
    }
    cell* S = VtRow(T, 0);
    memmove(S + (size_t)Top * T.Cols, S + (size_t)(Top + N) * T.Cols, (size_t)(Bottom - Top + 1 - N) * T.Cols * sizeof(cell));
    for(s32 Y = Bottom - N + 1; Y <= Bottom; ++Y) { VtClearCells(T, Y, 0, T.Cols); }
}

static void VtScrollDown(vt_terminal& T, s32 Top, s32 Bottom, s32 N)
{
    N = Min(N, Bottom - Top + 1);
    if(N <= 0) { return; }
    cell* S = VtRow(T, 0);
    memmove(S + (size_t)(Top + N) * T.Cols, S + (size_t)Top * T.Cols, (size_t)(Bottom - Top + 1 - N) * T.Cols * sizeof(cell));
    for(s32 Y = Top; Y < Top + N; ++Y) { VtClearCells(T, Y, 0, T.Cols); }
}

static void VtLineFeed(vt_terminal& T)
{
    if(T.CursorY == VtBottom(T)) { VtScrollUp(T, T.ScrollTop, VtBottom(T), 1); }
    else if(T.CursorY < T.Rows - 1) { T.CursorY += 1; }
    T.PendingWrap = false;
}

static void VtReverseIndex(vt_terminal& T)
{
    if(T.CursorY == T.ScrollTop) { VtScrollDown(T, T.ScrollTop, VtBottom(T), 1); }
    else if(T.CursorY > 0) { T.CursorY -= 1; }
    T.PendingWrap = false;
}

static void VtMoveTo(vt_terminal& T, s32 X, s32 Y)
{
    s32 MinY = T.OriginMode ? T.ScrollTop : 0;
    s32 MaxY = T.OriginMode ? VtBottom(T) : T.Rows - 1;
    T.CursorX = Clamp(X, 0, T.Cols - 1);
    T.CursorY = Clamp(Y, MinY, MaxY);
    T.PendingWrap = false;
}

static void VtSaveCursor(vt_terminal& T)
{
    T.SavedX = T.CursorX;
    T.SavedY = T.CursorY;
    T.SavedPen = T.Pen;
    T.SavedOriginMode = T.OriginMode;
}

static void VtRestoreCursor(vt_terminal& T)
{
    T.Pen = T.SavedPen;
    T.OriginMode = T.SavedOriginMode;
    T.CursorX = Clamp(T.SavedX, 0, T.Cols - 1);
    T.CursorY = Clamp(T.SavedY, 0, T.Rows - 1);
    T.PendingWrap = false;
}

static void VtInit(vt_terminal& T, s32 Cols, s32 Rows)
{
    T = {};
    T.Cols = Max(Cols, 1);
    T.Rows = Max(Rows, 1);
    T.Screens[0].assign((size_t)T.Cols * T.Rows, cell{});
    T.Screens[1].assign((size_t)T.Cols * T.Rows, cell{});
}

static void VtResize(vt_terminal& T, s32 Cols, s32 Rows)
{
    Cols = Max(Cols, 1);
    Rows = Max(Rows, 1);
    if(Cols == T.Cols && Rows == T.Rows) { return; }

    for(s32 S = 0; S < 2; ++S)
    {
        std::vector<cell>& Old = T.Screens[S];
        bool32 IsActive = (S == (T.AltActive ? 1 : 0));
        s32 CursorY = IsActive ? T.CursorY : 0;

        // --- shrinking height: lines above the cursor fall off the top (into scrollback for main) ---
        s32 Shift = 0;
        if(CursorY >= Rows) { Shift = CursorY - Rows + 1; }
        if(S == 0)
        {
            for(s32 Y = 0; Y < Shift; ++Y) { VtPushScrollback(T, Old.data() + (size_t)Y * T.Cols, T.Cols); }
        }

        std::vector<cell> New((size_t)Cols * Rows, cell{});
        s32 CopyCols = Min(Cols, T.Cols);
        for(s32 Y = 0; Y < Rows && Y + Shift < T.Rows; ++Y)
        {
            cell* Dst = New.data() + (size_t)Y * Cols;
            memcpy(Dst, Old.data() + (size_t)(Y + Shift) * T.Cols, CopyCols * sizeof(cell));
            if(CopyCols > 0 && (Dst[CopyCols - 1].Flags & CELL_WIDE)) { Dst[CopyCols - 1] = cell{}; }
        }
        Old.swap(New);
        if(IsActive) { T.CursorY -= Shift; }
    }

    T.Cols = Cols;
    T.Rows = Rows;
    T.CursorX = Clamp(T.CursorX, 0, Cols - 1);
    T.CursorY = Clamp(T.CursorY, 0, Rows - 1);
    T.SavedX = Clamp(T.SavedX, 0, Cols - 1);
    T.SavedY = Clamp(T.SavedY, 0, Rows - 1);
    T.ScrollTop = 0;
    T.ScrollBottomInset = 0;
    T.PendingWrap = false;
}

static void VtPrint(vt_terminal& T, u32 Cp)
{
    bool32 Graphics = T.ShiftOut ? T.G1Graphics : T.G0Graphics;
    if(Graphics && Cp >= 0x5F && Cp <= 0x7E)
    {
        static const u16 DecGraphics[] = {
            0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518,
            0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
            0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
        };
        Cp = DecGraphics[Cp - 0x5F];
    }

    s32 W = CharWidth(Cp);
    if(W == 0) { return; } // conhost's table has no zero-width characters; kept for safety
    if(W == 2 && T.Cols < 2) { return; }

    if(T.PendingWrap && !T.AutowrapOff)
    {
        T.CursorX = 0;
        VtLineFeed(T);
    }
    T.PendingWrap = false;

    if(W == 2 && T.CursorX == T.Cols - 1)
    {
        if(T.AutowrapOff) { return; }
        VtClearCells(T, T.CursorY, T.CursorX, T.Cols);
        T.CursorX = 0;
        VtLineFeed(T);
    }

    cell* Row = VtRow(T, T.CursorY);
    s32 X = T.CursorX;
    if(T.InsertMode && T.Cols - X - W > 0)
    {
        memmove(Row + X + W, Row + X, (size_t)(T.Cols - X - W) * sizeof(cell));
    }

    // --- don't leave half of a wide glyph behind ---
    if((Row[X].Flags & CELL_WIDE_CONT) && X > 0) { Row[X - 1] = VtBlank(T); }
    s32 End = X + W - 1;
    if((Row[End].Flags & CELL_WIDE) && End + 1 < T.Cols) { Row[End + 1] = VtBlank(T); }

    cell C = T.Pen;
    C.Codepoint = Cp;
    C.Flags = (W == 2) ? CELL_WIDE : 0;
    Row[X] = C;
    if(W == 2)
    {
        cell D = T.Pen;
        D.Codepoint = 0;
        D.Flags = CELL_WIDE_CONT;
        Row[X + 1] = D;
    }
    T.LastPrinted = Cp;

    if(X + W >= T.Cols)
    {
        T.CursorX = T.Cols - 1;
        T.PendingWrap = !T.AutowrapOff;
    }
    else
    {
        T.CursorX = X + W;
    }
}

static s32 VtParam(const vt_terminal& T, s32 I, s32 Default)
{
    if(I >= T.ParamCount || T.Params[I] == 0) { return Default; }
    return T.Params[I];
}

static void VtSgr(vt_terminal& T)
{
    if(T.ParamCount == 0) { T.Pen = cell{}; return; }
    for(s32 I = 0; I < T.ParamCount; ++I)
    {
        s32 P = T.Params[I];
        switch(P)
        {
            case 0: T.Pen = cell{}; break;
            case 1: T.Pen.Attr |= ATTR_BOLD; break;
            case 2: T.Pen.Attr |= ATTR_DIM; break;
            case 3: T.Pen.Attr |= ATTR_ITALIC; break;
            case 4: case 21: T.Pen.Attr |= ATTR_UNDERLINE; break;
            case 5: case 6: T.Pen.Attr |= ATTR_BLINK; break;
            case 7: T.Pen.Attr |= ATTR_INVERSE; break;
            case 8: T.Pen.Attr |= ATTR_HIDDEN; break;
            case 9: T.Pen.Attr |= ATTR_STRIKE; break;
            case 22: T.Pen.Attr &= ~(ATTR_BOLD | ATTR_DIM); break;
            case 23: T.Pen.Attr &= ~ATTR_ITALIC; break;
            case 24: T.Pen.Attr &= ~ATTR_UNDERLINE; break;
            case 25: T.Pen.Attr &= ~ATTR_BLINK; break;
            case 27: T.Pen.Attr &= ~ATTR_INVERSE; break;
            case 28: T.Pen.Attr &= ~ATTR_HIDDEN; break;
            case 29: T.Pen.Attr &= ~ATTR_STRIKE; break;
            case 39: T.Pen.Fg = 0; break;
            case 49: T.Pen.Bg = 0; break;
            case 38: case 48: case 58:
            {
                u32 Color = 0;
                if(I + 2 < T.ParamCount && T.Params[I + 1] == 5)
                {
                    Color = Palette(T.Params[I + 2]);
                    I += 2;
                }
                else if(I + 4 < T.ParamCount && T.Params[I + 1] == 2)
                {
                    Color = Rgb(T.Params[I + 2], T.Params[I + 3], T.Params[I + 4]);
                    I += 4;
                }
                else
                {
                    I = T.ParamCount;
                    break;
                }
                if(P == 38) { T.Pen.Fg = Color; }
                if(P == 48) { T.Pen.Bg = Color; }
            } break;
            default:
            {
                if(P >= 30 && P <= 37) { T.Pen.Fg = Palette(P - 30); }
                else if(P >= 40 && P <= 47) { T.Pen.Bg = Palette(P - 40); }
                else if(P >= 90 && P <= 97) { T.Pen.Fg = Palette(P - 90 + 8); }
                else if(P >= 100 && P <= 107) { T.Pen.Bg = Palette(P - 100 + 8); }
            } break;
        }
    }
}

// Returns a pointer to the mode flag for DECSET/DECRST, or null if unsupported.
static bool32* VtModeFlag(vt_terminal& T, s32 Mode)
{
    switch(Mode)
    {
        case 1: return &T.AppCursorKeys;
        case 6: return &T.OriginMode;
        case 1004: return &T.FocusEvents;
        case 1006: return &T.MouseSgr;
        case 2004: return &T.BracketedPaste;
        case 2026: return &T.SyncOutput;
    }
    return 0;
}

static void VtSetPrivateMode(vt_terminal& T, s32 Mode, bool32 On)
{
    switch(Mode)
    {
        case 7: T.AutowrapOff = !On; break;
        case 25: T.CursorHidden = !On; break;
        case 1000: case 1002: case 1003:
        {
            if(On) { T.MouseMode = Mode; }
            else if(T.MouseMode == Mode) { T.MouseMode = 0; }
        } break;
        case 47: case 1047: case 1049:
        {
            if(On == (bool32)(T.AltActive != 0)) { break; }
            if(On)
            {
                if(Mode == 1049) { VtSaveCursor(T); }
                T.AltActive = true;
                std::fill(T.Screens[1].begin(), T.Screens[1].end(), cell{});
            }
            else
            {
                T.AltActive = false;
                if(Mode == 1049) { VtRestoreCursor(T); }
            }
        } break;
        default:
        {
            bool32* Flag = VtModeFlag(T, Mode);
            if(Flag) { *Flag = On; }
            if(Mode == 6) { VtMoveTo(T, 0, T.OriginMode ? T.ScrollTop : 0); }
        } break;
    }
}

static s32 VtReportMode(vt_terminal& T, s32 Mode)
{
    bool32 On = false;
    switch(Mode)
    {
        case 7: On = !T.AutowrapOff; break;
        case 25: On = !T.CursorHidden; break;
        case 1000: case 1002: case 1003: On = (T.MouseMode == Mode); break;
        case 47: case 1047: case 1049: On = T.AltActive; break;
        default:
        {
            bool32* Flag = VtModeFlag(T, Mode);
            if(!Flag) { return 0; }
            On = *Flag;
        } break;
    }
    return On ? 1 : 2;
}

static void VtCsiDispatch(vt_terminal& T, char Final)
{
    char Buf[96];
    s32 P0 = VtParam(T, 0, 1);

    if(T.Private == '?')
    {
        switch(Final)
        {
            case 'h': for(s32 I = 0; I < T.ParamCount; ++I) { VtSetPrivateMode(T, T.Params[I], true); } break;
            case 'l': for(s32 I = 0; I < T.ParamCount; ++I) { VtSetPrivateMode(T, T.Params[I], false); } break;
            case 'p':
            {
                if(T.Intermediate == '$')
                {
                    snprintf(Buf, sizeof(Buf), "\x1b[?%d;%d$y", T.Params[0], VtReportMode(T, T.Params[0]));
                    T.Reply += Buf;
                }
            } break;
            case 'n':
            {
                if(T.Params[0] == 6)
                {
                    s32 Y = T.CursorY - (T.OriginMode ? T.ScrollTop : 0);
                    snprintf(Buf, sizeof(Buf), "\x1b[?%d;%dR", Y + 1, T.CursorX + 1);
                    T.Reply += Buf;
                }
            } break;
            case 'J': case 'K': T.Private = 0; VtCsiDispatch(T, Final); break;
        }
        return;
    }
    if(T.Private == '>')
    {
        if(Final == 'c') { T.Reply += "\x1b[>0;10;1c"; }
        return;
    }
    if(T.Private) { return; }

    if(T.Intermediate)
    {
        if(T.Intermediate == '!' && Final == 'p') // DECSTR soft reset
        {
            T.Pen = cell{};
            T.CursorHidden = false;
            T.AutowrapOff = false;
            T.OriginMode = false;
            T.InsertMode = false;
            T.AppCursorKeys = false;
            T.ScrollTop = 0;
            T.ScrollBottomInset = 0;
        }
        if(T.Intermediate == ' ' && Final == 'q') { T.CursorShape = Clamp(T.ParamCount ? T.Params[0] : 0, 0, 6); } // DECSCUSR
        return;
    }

    switch(Final)
    {
        case '@':
        {
            cell* Row = VtRow(T, T.CursorY);
            s32 N = Min(P0, T.Cols - T.CursorX);
            memmove(Row + T.CursorX + N, Row + T.CursorX, (size_t)(T.Cols - T.CursorX - N) * sizeof(cell));
            VtClearCells(T, T.CursorY, T.CursorX, T.CursorX + N);
            T.PendingWrap = false;
        } break;
        case 'A': VtMoveTo(T, T.CursorX, Max(T.CursorY - P0, T.CursorY >= T.ScrollTop ? T.ScrollTop : 0)); break;
        case 'B': case 'e': VtMoveTo(T, T.CursorX, Min(T.CursorY + P0, T.CursorY <= VtBottom(T) ? VtBottom(T) : T.Rows - 1)); break;
        case 'C': case 'a': VtMoveTo(T, T.CursorX + P0, T.CursorY); break;
        case 'D': VtMoveTo(T, T.CursorX - P0, T.CursorY); break;
        case 'E': VtMoveTo(T, 0, T.CursorY + P0); break;
        case 'F': VtMoveTo(T, 0, T.CursorY - P0); break;
        case 'G': case '`': VtMoveTo(T, P0 - 1, T.CursorY); break;
        case 'H': case 'f':
        {
            s32 Base = T.OriginMode ? T.ScrollTop : 0;
            VtMoveTo(T, VtParam(T, 1, 1) - 1, Base + P0 - 1);
        } break;
        case 'd':
        {
            s32 Base = T.OriginMode ? T.ScrollTop : 0;
            VtMoveTo(T, T.CursorX, Base + P0 - 1);
        } break;
        case 'I':
        {
            for(s32 I = 0; I < P0; ++I) { T.CursorX = Min((T.CursorX / 8 + 1) * 8, T.Cols - 1); }
            T.PendingWrap = false;
        } break;
        case 'Z':
        {
            for(s32 I = 0; I < P0; ++I) { T.CursorX = Max(((T.CursorX + 7) / 8 - 1) * 8, 0); }
            T.PendingWrap = false;
        } break;
        case 'J':
        {
            s32 Mode = VtParam(T, 0, 0);
            if(Mode == 0)
            {
                VtClearCells(T, T.CursorY, T.CursorX, T.Cols);
                for(s32 Y = T.CursorY + 1; Y < T.Rows; ++Y) { VtClearCells(T, Y, 0, T.Cols); }
            }
            else if(Mode == 1)
            {
                for(s32 Y = 0; Y < T.CursorY; ++Y) { VtClearCells(T, Y, 0, T.Cols); }
                VtClearCells(T, T.CursorY, 0, T.CursorX + 1);
            }
            else if(Mode == 2)
            {
                for(s32 Y = 0; Y < T.Rows; ++Y) { VtClearCells(T, Y, 0, T.Cols); }
            }
            else if(Mode == 3)
            {
                T.Scrollback.clear();
            }
        } break;
        case 'K':
        {
            s32 Mode = VtParam(T, 0, 0);
            if(Mode == 0) { VtClearCells(T, T.CursorY, T.CursorX, T.Cols); }
            else if(Mode == 1) { VtClearCells(T, T.CursorY, 0, T.CursorX + 1); }
            else if(Mode == 2) { VtClearCells(T, T.CursorY, 0, T.Cols); }
        } break;
        case 'L':
        {
            if(T.CursorY >= T.ScrollTop && T.CursorY <= VtBottom(T)) { VtScrollDown(T, T.CursorY, VtBottom(T), P0); }
            T.CursorX = 0;
            T.PendingWrap = false;
        } break;
        case 'M':
        {
            if(T.CursorY >= T.ScrollTop && T.CursorY <= VtBottom(T))
            {
                // DL never feeds scrollback, even at the top of the screen, so no VtScrollUp
                cell* S = VtRow(T, 0);
                s32 Bottom = VtBottom(T);
                s32 N = Min(P0, Bottom - T.CursorY + 1);
                memmove(S + (size_t)T.CursorY * T.Cols, S + (size_t)(T.CursorY + N) * T.Cols, (size_t)(Bottom - T.CursorY + 1 - N) * T.Cols * sizeof(cell));
                for(s32 Y = Bottom - N + 1; Y <= Bottom; ++Y) { VtClearCells(T, Y, 0, T.Cols); }
            }
            T.CursorX = 0;
            T.PendingWrap = false;
        } break;
        case 'P':
        {
            cell* Row = VtRow(T, T.CursorY);
            s32 N = Min(P0, T.Cols - T.CursorX);
            memmove(Row + T.CursorX, Row + T.CursorX + N, (size_t)(T.Cols - T.CursorX - N) * sizeof(cell));
            VtClearCells(T, T.CursorY, T.Cols - N, T.Cols);
            T.PendingWrap = false;
        } break;
        case 'S': VtScrollUp(T, T.ScrollTop, VtBottom(T), P0); break;
        case 'T': VtScrollDown(T, T.ScrollTop, VtBottom(T), P0); break;
        case 'X': VtClearCells(T, T.CursorY, T.CursorX, T.CursorX + P0); T.PendingWrap = false; break;
        case 'b':
        {
            if(T.LastPrinted) { for(s32 I = 0; I < Min(P0, 4096); ++I) { VtPrint(T, T.LastPrinted); } }
        } break;
        case 'c': T.Reply += "\x1b[?62;22c"; break;
        case 'h': case 'l':
        {
            for(s32 I = 0; I < T.ParamCount; ++I)
            {
                if(T.Params[I] == 4) { T.InsertMode = (Final == 'h'); }
            }
        } break;
        case 'm': VtSgr(T); break;
        case 'n':
        {
            s32 Mode = VtParam(T, 0, 0);
            if(Mode == 5) { T.Reply += "\x1b[0n"; }
            else if(Mode == 6)
            {
                s32 Y = T.CursorY - (T.OriginMode ? T.ScrollTop : 0);
                snprintf(Buf, sizeof(Buf), "\x1b[%d;%dR", Y + 1, T.CursorX + 1);
                T.Reply += Buf;
            }
        } break;
        case 'r':
        {
            s32 Top = VtParam(T, 0, 1) - 1;
            s32 Bottom = VtParam(T, 1, T.Rows) - 1;
            Bottom = Min(Bottom, T.Rows - 1);
            if(Top < Bottom)
            {
                T.ScrollTop = Top;
                T.ScrollBottomInset = T.Rows - 1 - Bottom;
                VtMoveTo(T, 0, T.OriginMode ? T.ScrollTop : 0);
            }
        } break;
        case 's': VtSaveCursor(T); break;
        case 'u': VtRestoreCursor(T); break;
        case 't':
        {
            s32 Op = VtParam(T, 0, 0);
            if(Op == 18)
            {
                snprintf(Buf, sizeof(Buf), "\x1b[8;%d;%dt", T.Rows, T.Cols);
                T.Reply += Buf;
            }
        } break;
    }
}

static void VtOscDispatch(vt_terminal& T)
{
    size_t Semi = T.Osc.find(';');
    if(Semi == std::string::npos) { return; }
    s32 Code = atoi(T.Osc.c_str());
    const char* Arg = T.Osc.c_str() + Semi + 1;
    switch(Code)
    {
        case 0: case 2: T.Title = Arg; break;
        case 10: if(Arg[0] == '?') { T.Reply += "\x1b]10;rgb:cccc/cccc/cccc\x1b\\"; } break;
        case 11: if(Arg[0] == '?') { T.Reply += "\x1b]11;rgb:0c0c/0c0c/0c0c\x1b\\"; } break;
        case 52: T.PassThrough += "\x1b]" + T.Osc + "\x1b\\"; break;
        case 9:
        {
            // OSC 9;<text> is a desktop notification; 9;4;... is a progress report, not one.
            if(strncmp(Arg, "4;", 2) != 0) { T.Notify = Arg; }
        } break;
        case 777:
        {
            // OSC 777;notify;<title>;<body>
            if(strncmp(Arg, "notify;", 7) == 0) { T.Notify = Arg + 7; }
        } break;
    }
}

static void VtControl(vt_terminal& T, u8 B)
{
    switch(B)
    {
        case 0x07: T.Bell = true; break;
        case 0x08:
        {
            if(T.CursorX > 0) { T.CursorX -= 1; }
            T.PendingWrap = false;
        } break;
        case 0x09:
        {
            T.CursorX = Min((T.CursorX / 8 + 1) * 8, T.Cols - 1);
            T.PendingWrap = false;
        } break;
        case 0x0A: case 0x0B: case 0x0C: VtLineFeed(T); break;
        case 0x0D: T.CursorX = 0; T.PendingWrap = false; break;
        case 0x0E: T.ShiftOut = true; break;
        case 0x0F: T.ShiftOut = false; break;
        case 0x18: case 0x1A: T.State = VT_GROUND; break;
        case 0x1B:
        {
            T.State = VT_ESCAPE;
            T.EscIntermediate = 0;
        } break;
    }
}

static void VtEscDispatch(vt_terminal& T, u8 B)
{
    if(T.EscIntermediate == '(' || T.EscIntermediate == ')')
    {
        bool32 Graphics = (B == '0');
        if(T.EscIntermediate == '(') { T.G0Graphics = Graphics; }
        else { T.G1Graphics = Graphics; }
        return;
    }
    if(T.EscIntermediate) { return; }

    switch(B)
    {
        case '7': VtSaveCursor(T); break;
        case '8': VtRestoreCursor(T); break;
        case 'D': VtLineFeed(T); break;
        case 'E': T.CursorX = 0; VtLineFeed(T); break;
        case 'M': VtReverseIndex(T); break;
        case 'c':
        {
            std::deque<std::vector<cell>> Keep;
            Keep.swap(T.Scrollback);
            u64 Pushed = T.ScrollbackPushed;
            std::string Reply = T.Reply;
            std::string Pass = T.PassThrough;
            std::string Notify = T.Notify;
            VtInit(T, T.Cols, T.Rows);
            T.Scrollback.swap(Keep);
            T.ScrollbackPushed = Pushed;
            T.Reply = Reply;
            T.PassThrough = Pass;
            T.Notify = Notify;
        } break;
    }
}

static void VtFeed(vt_terminal& T, const u8* Data, size_t Size)
{
    for(size_t I = 0; I < Size; ++I)
    {
        u8 B = Data[I];
        switch(T.State)
        {
            case VT_GROUND:
            {
                if(T.Utf8Need > 0)
                {
                    if((B & 0xC0) == 0x80)
                    {
                        T.Utf8Cp = (T.Utf8Cp << 6) | (B & 0x3F);
                        if(--T.Utf8Need == 0) { VtPrint(T, T.Utf8Cp); }
                        break;
                    }
                    T.Utf8Need = 0;
                    VtPrint(T, 0xFFFD);
                }
                if(B < 0x20 || B == 0x7F) { VtControl(T, B); }
                else if(B < 0x80) { VtPrint(T, B); }
                else if((B & 0xE0) == 0xC0) { T.Utf8Cp = B & 0x1F; T.Utf8Need = 1; }
                else if((B & 0xF0) == 0xE0) { T.Utf8Cp = B & 0x0F; T.Utf8Need = 2; }
                else if((B & 0xF8) == 0xF0) { T.Utf8Cp = B & 0x07; T.Utf8Need = 3; }
                else { VtPrint(T, 0xFFFD); }
            } break;

            case VT_ESCAPE:
            {
                T.State = VT_GROUND;
                if(B == '[')
                {
                    T.State = VT_CSI;
                    T.ParamCount = 0;
                    T.Private = 0;
                    T.Intermediate = 0;
                    memset(T.Params, 0, sizeof(T.Params));
                }
                else if(B == ']') { T.State = VT_OSC; T.Osc.clear(); }
                else if(B == 'P' || B == 'X' || B == '^' || B == '_') { T.State = VT_STRING; }
                else if(B >= 0x20 && B <= 0x2F) { T.EscIntermediate = (char)B; T.State = VT_ESCAPE_INTER; }
                else if(B < 0x20) { VtControl(T, B); }
                else { VtEscDispatch(T, B); }
            } break;

            case VT_ESCAPE_INTER:
            {
                if(B >= 0x20 && B <= 0x2F) { break; }
                T.State = VT_GROUND;
                if(B < 0x20) { VtControl(T, B); }
                else { VtEscDispatch(T, B); }
            } break;

            case VT_CSI:
            {
                if(B >= '0' && B <= '9')
                {
                    if(T.ParamCount == 0) { T.ParamCount = 1; }
                    s32& P = T.Params[T.ParamCount - 1];
                    if(P < 100000) { P = P * 10 + (B - '0'); }
                }
                else if(B == ';' || B == ':')
                {
                    if(T.ParamCount == 0) { T.ParamCount = 1; }
                    if(T.ParamCount < VT_MAX_PARAMS) { T.ParamCount += 1; }
                }
                else if(B >= 0x3C && B <= 0x3F) { T.Private = (char)B; }
                else if(B >= 0x20 && B <= 0x2F) { T.Intermediate = (char)B; }
                else if(B >= 0x40 && B <= 0x7E)
                {
                    T.State = VT_GROUND;
                    VtCsiDispatch(T, (char)B);
                }
                else if(B < 0x20) { VtControl(T, B); }
            } break;

            case VT_OSC:
            {
                if(B == 0x07) { T.State = VT_GROUND; VtOscDispatch(T); }
                else if(B == 0x1B) { T.State = VT_OSC_ESC; }
                else if(T.Osc.size() < VT_OSC_MAX) { T.Osc += (char)B; }
            } break;

            case VT_OSC_ESC:
            {
                VtOscDispatch(T);
                T.State = VT_GROUND;
                if(B != '\\')
                {
                    T.State = VT_ESCAPE;
                    T.EscIntermediate = 0;
                    --I;
                }
            } break;

            case VT_STRING:
            {
                if(B == 0x1B) { T.State = VT_STRING_ESC; }
            } break;

            case VT_STRING_ESC:
            {
                T.State = (B == '\\') ? VT_GROUND : VT_STRING;
            } break;
        }
    }
}
