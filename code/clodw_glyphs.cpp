// clodw's glyphs: the font, the cell size it implies, and the atlas of drawn glyph tiles. A glyph is
// drawn once, on first use, by Direct2D/DirectWrite (with DirectWrite's font fallback for scripts and
// emoji the font lacks) and then looked up by its text and style.

enum
{
    STYLE_BOLD   = 1,
    STYLE_ITALIC = 2,
};

struct font_state
{
    IDWriteTextFormat* Formats[4]; // by STYLE_* bits
    IDWriteTextFormat* Emoji;      // color clusters: the emoji font first, so FE0F gets its color form
    s32 CellW;
    s32 CellH;
    s32 Baseline;
    s32 UnderlineY;
    s32 StrikeY;
    s32 LineH;   // underline / strike thickness
};

#define GLYPH_MAX_CPS 16 // a ZWJ family is up to 7; flags with tags run longer

struct glyph_entry
{
    u32 Cps[GLYPH_MAX_CPS];
    u32 Count;   // 0 = empty slot
    u32 Style;
    u32 Cells;   // tiles wide (a cluster spans every cell ConPTY gave its parts)
    u32 Tile;    // first tile: x | y << 16
    bool32 Color;
    bool32 Overflow; // wider than its cells, so drawn shrunk
};

struct glyph_cache
{
    std::vector<glyph_entry> Table; // open addressing, power of two
    u32 Used;
    u32 TilesX;
    u32 TilesY;
    u32 NextX;
    u32 NextY;
};

// Empties the cache (new font, or the atlas is full); glyphs get drawn again as they're needed.
static void GlyphCacheReset(glyph_cache& G)
{
    G.Table.assign(G.Table.size(), glyph_entry{});
    G.Used = 0;
    G.NextX = 1; // tile 0 stays blank
    G.NextY = 0;
}

// Picks the font and derives the cell size from its metrics at SizePx. Name is a face name as Windows'
// font dialog gives it (e.g. "Iosevka Extended", which DirectWrite sees as family "Iosevka" with an
// expanded stretch) or a DirectWrite family; unknown names fall back to Consolas. The atlas is laid
// out in cells, so the cache starts over.
static void FontSetup(font_state& F, glyph_cache& G, renderer& R, const std::wstring& FaceName, float SizePx)
{
    for(IDWriteTextFormat*& Format : F.Formats) { ReleaseCom(Format); }
    ReleaseCom(F.Emoji);

    // --- the regular face: through the GDI name first, then as a DirectWrite family ---
    IDWriteFont* Font = 0;
    IDWriteGdiInterop* Gdi = 0;
    R.DWrite->GetGdiInterop(&Gdi);
    LOGFONTW Lf = {};
    wcsncpy_s(Lf.lfFaceName, FaceName.c_str(), _TRUNCATE);
    Lf.lfWeight = FW_NORMAL;
    Lf.lfCharSet = DEFAULT_CHARSET;
    if(FAILED(Gdi->CreateFontFromLOGFONT(&Lf, &Font))) { Font = 0; }
    ReleaseCom(Gdi);
    if(!Font)
    {
        IDWriteFontCollection* System = 0;
        R.DWrite->GetSystemFontCollection(&System, FALSE);
        UINT32 Index = 0;
        BOOL Exists = FALSE;
        System->FindFamilyName(FaceName.c_str(), &Index, &Exists);
        if(!Exists) { System->FindFamilyName(L"Consolas", &Index, &Exists); }
        IDWriteFontFamily* Family = 0;
        System->GetFontFamily(Index, &Family);
        Family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &Font);
        ReleaseCom(Family);
        ReleaseCom(System);
    }
    DWRITE_FONT_STRETCH Stretch = Font->GetStretch();
    std::wstring Name;
    {
        IDWriteFontFamily* Family = 0;
        IDWriteLocalizedStrings* Names = 0;
        Font->GetFontFamily(&Family);
        Family->GetFamilyNames(&Names);
        UINT32 Index = 0;
        BOOL Exists = FALSE;
        Names->FindLocaleName(L"en-us", &Index, &Exists);
        UINT32 Len = 0;
        Names->GetStringLength(Exists ? Index : 0, &Len);
        Name.resize(Len + 1);
        Names->GetString(Exists ? Index : 0, &Name[0], Len + 1);
        Name.resize(Len);
        ReleaseCom(Names);
        ReleaseCom(Family);
    }

    // --- metrics from the regular face ---
    IDWriteFontFace* Face = 0;
    Font->CreateFontFace(&Face);
    DWRITE_FONT_METRICS M = {};
    Face->GetMetrics(&M);
    UINT32 Zero = '0';
    UINT16 Glyph = 0;
    Face->GetGlyphIndices(&Zero, 1, &Glyph);
    DWRITE_GLYPH_METRICS GM = {};
    Face->GetDesignGlyphMetrics(&Glyph, 1, &GM, FALSE);
    float Scale = SizePx / (float)M.designUnitsPerEm;
    float Ascent = M.ascent * Scale;
    float Descent = M.descent * Scale;
    float Gap = M.lineGap * Scale;
    F.CellW = Max(1, (s32)ceilf(GM.advanceWidth * Scale - 0.05f));
    F.CellH = Max(1, (s32)ceilf(Ascent + Descent + Gap));
    F.Baseline = (s32)roundf(Ascent + Gap / 2);
    F.LineH = Max(1, (s32)roundf(M.underlineThickness * Scale));
    F.UnderlineY = Min(F.CellH - F.LineH, F.Baseline - (s32)roundf(M.underlinePosition * Scale));
    F.StrikeY = F.Baseline - (s32)roundf(M.strikethroughPosition * Scale);
    ReleaseCom(Face);
    ReleaseCom(Font);

    // --- one text format per style, all on the same baseline so fallback fonts line up ---
    for(u32 Style = 0; Style < 4; ++Style)
    {
        R.DWrite->CreateTextFormat(Name.c_str(), 0, (Style & STYLE_BOLD) ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                   (Style & STYLE_ITALIC) ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                   Stretch, SizePx, L"", &F.Formats[Style]);
        F.Formats[Style]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        F.Formats[Style]->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, (float)F.CellH, (float)F.Baseline);
    }

    R.DWrite->CreateTextFormat(L"Segoe UI Emoji", 0, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                               SizePx, L"", &F.Emoji);
    F.Emoji->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    F.Emoji->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, (float)F.CellH, (float)F.Baseline);

    RendererSetCellSize(R, F.CellW, F.CellH);
    G.Table.resize(4096);
    G.TilesX = ATLAS_SIZE / F.CellW;
    G.TilesY = ATLAS_SIZE / F.CellH;
    GlyphCacheReset(G);
}

// Emoji draw in their own colors; everything else is a coverage mask tinted with the cell color.
static bool32 IsColorGlyph(const u32* Cps, u32 Count)
{
    for(u32 I = 0; I < Count; ++I) { if(Cps[I] == 0xFE0F) { return true; } }
    u32 Cp = Cps[0];
    // The emoji blocks only: higher planes hold CJK and private-use icons (Nerd Font) the user's font draws.
    return (Cp >= 0x1F000 && Cp <= 0x1FAFF) || (Cp >= 0x2300 && Cp <= 0x2BFF && CharWidth(Cp) == 2);
}

// The first of Cells tiles for this text in this style, drawing it into the atlas on first use.
// Overflow reports a glyph wider than its cells (drawn shrunk). Returns false when the atlas is full
// (reset the cache and rebuild the frame).
static bool32 GlyphLookup(glyph_cache& G, font_state& F, renderer& R, const u32* Cps, u32 Count, u32 Style, u32 Cells,
                          u32* Tile, bool32* Color, bool32* Overflow)
{
    Count = Min((s32)Count, GLYPH_MAX_CPS);
    Cells = Clamp((s32)Cells, 1, GLYPH_MAX_CELLS);
    u32 Hash = 2166136261u ^ (Style * 31 + Cells);
    for(u32 I = 0; I < Count; ++I) { Hash = (Hash ^ Cps[I]) * 16777619u; }
    u32 Mask = (u32)G.Table.size() - 1;
    u32 Slot = Hash & Mask;
    for(;;)
    {
        glyph_entry& E = G.Table[Slot];
        if(!E.Count) { break; }
        if(E.Count == Count && E.Style == Style && E.Cells == Cells && memcmp(E.Cps, Cps, Count * sizeof(u32)) == 0)
        {
            *Tile = E.Tile;
            *Color = E.Color;
            *Overflow = E.Overflow;
            return true;
        }
        Slot = (Slot + 1) & Mask;
    }

    // --- a free run of tiles in the atlas; the table stays at most half full ---
    if(G.NextX + Cells > G.TilesX) { G.NextX = 0; G.NextY += 1; }
    if(G.NextY >= G.TilesY || G.Used * 2 >= G.Table.size()) { return false; }
    u32 X = G.NextX;
    u32 Y = G.NextY;
    G.NextX += Cells;

    // --- draw it with Direct2D, white on transparent, and copy its tiles into the atlas ---
    // A lone symbol asks for text presentation (U+FE0E), or fallback picks a color emoji. A glyph
    // wider than its cells (often from a fallback font) is scaled to fit.
    bool32 IsColor = IsColorGlyph(Cps, Count);
    std::wstring Text;
    for(u32 I = 0; I < Count; ++I)
    {
        u32 Cp = Cps[I];
        if(Cp >= 0x10000) { Text += (wchar_t)(0xD800 + ((Cp - 0x10000) >> 10)); Text += (wchar_t)(0xDC00 + ((Cp - 0x10000) & 0x3FF)); }
        else { Text += (wchar_t)Cp; }
    }
    if(!IsColor && Count == 1 && Cps[0] >= 0x2000 && Cps[0] < 0x3300 && CharWidth(Cps[0]) == 1) { Text += (wchar_t)0xFE0E; }
    float BoxW = (float)(Cells * F.CellW);
    IDWriteTextLayout* Layout = 0;
    R.DWrite->CreateTextLayout(Text.data(), (UINT32)Text.size(), IsColor ? F.Emoji : F.Formats[Style & 3], 10000.0f, (float)F.CellH, &Layout);
    DWRITE_TEXT_METRICS TM = {};
    Layout->GetMetrics(&TM);
    float Fit = (TM.widthIncludingTrailingWhitespace > BoxW + 0.5f) ? BoxW / TM.widthIncludingTrailingWhitespace : 1.0f;
    R.Draw2D->BeginDraw();
    R.Draw2D->Clear(D2D1::ColorF(0, 0, 0, 0));
    // Squeeze sideways only: a too-wide glyph keeps its height and stays on the line. Emoji keep
    // their proportions.
    R.Draw2D->SetTransform(D2D1::Matrix3x2F::Scale(Fit, IsColor ? Fit : 1.0f, D2D1::Point2F(0.0f, (float)F.Baseline)));
    R.Draw2D->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), Layout, R.White,
                             IsColor ? D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT : D2D1_DRAW_TEXT_OPTIONS_NONE);
    R.Draw2D->SetTransform(D2D1::Matrix3x2F::Identity());
    R.Draw2D->EndDraw();
    ReleaseCom(Layout);
    D3D11_BOX Src = {0, 0, 0, Cells * (UINT)F.CellW, (UINT)F.CellH, 1};
    R.Context->CopySubresourceRegion(R.Atlas, 0, X * F.CellW, Y * F.CellH, 0, R.Scratch, 0, &Src);

    glyph_entry& E = G.Table[Slot];
    memcpy(E.Cps, Cps, Count * sizeof(u32));
    E.Count = Count;
    E.Style = Style;
    E.Cells = Cells;
    E.Tile = X | (Y << 16);
    E.Color = IsColor;
    E.Overflow = Fit < 1.0f;
    G.Used += 1;
    *Tile = E.Tile;
    *Color = E.Color;
    *Overflow = E.Overflow;
    return true;
}
