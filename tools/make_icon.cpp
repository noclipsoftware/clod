// Draws clod's icon (near-black rounded block, white serif lambda) and writes it as a multi-size .ico.
// build.bat runs this and embeds the result in clod.exe, which is where Explorer, the taskbar,
// the console window and the tray all get the icon from.
//
//   make_icon <output.ico>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>

#include "../code/clod_base.h"

// Renders at Size x Size into straight-alpha BGRA, top row first. Drawn 4x larger and averaged
// down, since GDI's shapes have no anti-aliasing.
static std::vector<u32> DrawIcon(s32 Size)
{
    const s32 Scale = 4;
    s32 Big = Size * Scale;
    BITMAPINFO Bi = {};
    Bi.bmiHeader.biSize = sizeof(Bi.bmiHeader);
    Bi.bmiHeader.biWidth = Big;
    Bi.bmiHeader.biHeight = -Big;
    Bi.bmiHeader.biPlanes = 1;
    Bi.bmiHeader.biBitCount = 32;
    Bi.bmiHeader.biCompression = BI_RGB;
    u32* Pixels = 0;
    HDC Screen = GetDC(0);
    HBITMAP Bitmap = CreateDIBSection(Screen, &Bi, DIB_RGB_COLORS, (void**)&Pixels, 0, 0);
    HDC Dc = CreateCompatibleDC(Screen);
    ReleaseDC(0, Screen);
    HGDIOBJ OldBitmap = SelectObject(Dc, Bitmap);
    memset(Pixels, 0, (size_t)Big * Big * 4);

    // The design lives on a 1024 grid; D() maps it to this render size.
    auto D = [Big](s32 V) { return V * Big / 1024; };
    HGDIOBJ OldPen = SelectObject(Dc, GetStockObject(NULL_PEN));

    // --- near-black rounded block, edge to edge like other apps' taskbar icons ---
    HBRUSH Brush = CreateSolidBrush(RGB(0x14, 0x14, 0x14));
    HGDIOBJ OldBrush = SelectObject(Dc, Brush);
    RoundRect(Dc, D(0), D(0), D(1024) + 1, D(1024) + 1, D(326), D(326));
    SelectObject(Dc, OldBrush);
    DeleteObject(Brush);
    GdiFlush();
    // GDI leaves alpha at zero; every painted pixel is opaque (the block isn't pure black).
    for(s32 I = 0; I < Big * Big; ++I) { if(Pixels[I]) { Pixels[I] |= 0xFF000000; } }

    // --- white serif lambda (Georgia bold italic), centred on the block by its ink ---
    // GDI centres text by its line box, which sits an italic serif glyph well off-centre, so the
    // glyph is drawn into a coverage mask first and placed by where its pixels actually landed.
    BITMAPINFO MaskBi = Bi;
    u32* Mask = 0;
    HBITMAP MaskBitmap = CreateDIBSection(Dc, &MaskBi, DIB_RGB_COLORS, (void**)&Mask, 0, 0);
    HDC MaskDc = CreateCompatibleDC(Dc);
    HGDIOBJ OldMaskBitmap = SelectObject(MaskDc, MaskBitmap);
    memset(Mask, 0, (size_t)Big * Big * 4);
    HFONT Font = CreateFontW(-D(868), 0, 0, 0, FW_BOLD, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Georgia");
    HGDIOBJ OldFont = SelectObject(MaskDc, Font);
    SetBkMode(MaskDc, TRANSPARENT);
    SetTextColor(MaskDc, RGB(255, 255, 255));
    TextOutW(MaskDc, D(40), D(0), L"\x03BB", 1);
    GdiFlush();

    s32 MinX = Big, MinY = Big, MaxX = -1, MaxY = -1;
    for(s32 Y = 0; Y < Big; ++Y)
    {
        for(s32 X = 0; X < Big; ++X)
        {
            if(!(Mask[(size_t)Y * Big + X] & 0xFF)) { continue; }
            MinX = Min(MinX, X); MaxX = Max(MaxX, X);
            MinY = Min(MinY, Y); MaxY = Max(MaxY, Y);
        }
    }
    s32 CenterX = D(512);
    s32 CenterY = D(534); // a touch low: the hooked top makes the glyph look top-heavy
    s32 ShiftX = CenterX - (MinX + MaxX) / 2;
    s32 ShiftY = CenterY - (MinY + MaxY) / 2;
    const u32 Glyph[3] = {0xFF, 0xFF, 0xFF}; // B, G, R
    for(s32 Y = MinY; Y <= MaxY; ++Y)
    {
        for(s32 X = MinX; X <= MaxX; ++X)
        {
            u32 Coverage = Mask[(size_t)Y * Big + X] & 0xFF;
            s32 TX = X + ShiftX, TY = Y + ShiftY;
            if(!Coverage || TX < 0 || TY < 0 || TX >= Big || TY >= Big) { continue; }
            u32& P = Pixels[(size_t)TY * Big + TX];
            u32 B = ((P & 0xFF) * (255 - Coverage) + Glyph[0] * Coverage) / 255;
            u32 G = (((P >> 8) & 0xFF) * (255 - Coverage) + Glyph[1] * Coverage) / 255;
            u32 R = (((P >> 16) & 0xFF) * (255 - Coverage) + Glyph[2] * Coverage) / 255;
            P = 0xFF000000 | (R << 16) | (G << 8) | B;
        }
    }
    SelectObject(MaskDc, OldFont);
    SelectObject(MaskDc, OldMaskBitmap);
    DeleteObject(Font);
    DeleteObject(MaskBitmap);
    DeleteDC(MaskDc);

    // --- average each Scale x Scale block (premultiplied, so edges don't darken) ---
    std::vector<u32> Out((size_t)Size * Size);
    for(s32 Y = 0; Y < Size; ++Y)
    {
        for(s32 X = 0; X < Size; ++X)
        {
            u32 Sum[4] = {};
            for(s32 SY = 0; SY < Scale; ++SY)
            {
                for(s32 SX = 0; SX < Scale; ++SX)
                {
                    u32 P = Pixels[(size_t)(Y * Scale + SY) * Big + X * Scale + SX];
                    u32 A = P >> 24;
                    Sum[0] += (P & 0xFF) * A;
                    Sum[1] += ((P >> 8) & 0xFF) * A;
                    Sum[2] += ((P >> 16) & 0xFF) * A;
                    Sum[3] += A;
                }
            }
            u32 A = Sum[3] / (Scale * Scale);
            u32 B = Sum[3] ? Sum[0] / Sum[3] : 0;
            u32 G = Sum[3] ? Sum[1] / Sum[3] : 0;
            u32 Rd = Sum[3] ? Sum[2] / Sum[3] : 0;
            Out[(size_t)Y * Size + X] = (A << 24) | (Rd << 16) | (G << 8) | B;
        }
    }

    SelectObject(Dc, OldPen);
    SelectObject(Dc, OldBitmap);
    DeleteObject(Bitmap);
    DeleteDC(Dc);
    return Out;
}

static void Put16(std::string& S, u32 V) { S += (char)(V & 0xFF); S += (char)((V >> 8) & 0xFF); }
static void Put32(std::string& S, u32 V) { Put16(S, V & 0xFFFF); Put16(S, V >> 16); }

int wmain(int Argc, wchar_t** Argv)
{
    if(Argc < 2) { fprintf(stderr, "usage: make_icon <output.ico>\n"); return 1; }
    static const s32 Sizes[] = {16, 20, 24, 32, 40, 48, 64, 256};
    const u32 Count = ArrayCount(Sizes);

    // --- each image: BITMAPINFOHEADER (double height = colour + mask), BGRA bottom-up, empty AND mask ---
    std::vector<std::string> Images;
    for(u32 I = 0; I < Count; ++I)
    {
        s32 S = Sizes[I];
        std::vector<u32> Px = DrawIcon(S);
        std::string Img;
        Put32(Img, 40);
        Put32(Img, S);
        Put32(Img, S * 2);
        Put16(Img, 1);
        Put16(Img, 32);
        Put32(Img, 0);
        Put32(Img, 0);
        Put32(Img, 0);
        Put32(Img, 0);
        Put32(Img, 0);
        Put32(Img, 0);
        for(s32 Y = S - 1; Y >= 0; --Y)
        {
            for(s32 X = 0; X < S; ++X) { Put32(Img, Px[(size_t)Y * S + X]); }
        }
        s32 MaskRow = ((S + 31) / 32) * 4;
        Img.append((size_t)MaskRow * S, '\0'); // alpha decides transparency; the mask is unused
        Images.push_back(Img);
    }

    // --- ICONDIR, one ICONDIRENTRY per image, then the images ---
    std::string File;
    Put16(File, 0);
    Put16(File, 1);
    Put16(File, Count);
    u32 Offset = 6 + 16 * Count;
    for(u32 I = 0; I < Count; ++I)
    {
        s32 S = Sizes[I];
        File += (char)(S >= 256 ? 0 : S);
        File += (char)(S >= 256 ? 0 : S);
        File += (char)0;
        File += (char)0;
        Put16(File, 1);
        Put16(File, 32);
        Put32(File, (u32)Images[I].size());
        Put32(File, Offset);
        Offset += (u32)Images[I].size();
    }
    for(const std::string& Img : Images) { File += Img; }

    FILE* Out = 0;
    _wfopen_s(&Out, Argv[1], L"wb");
    if(!Out) { fprintf(stderr, "make_icon: can't write output\n"); return 1; }
    fwrite(File.data(), 1, File.size(), Out);
    fclose(Out);
    return 0;
}
