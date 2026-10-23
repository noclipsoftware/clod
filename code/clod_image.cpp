// Image files for `clod view`: decoded with Windows Imaging Component (PNG, JPEG, GIF, BMP, TIFF, ICO,
// JPEG XR, and WebP/HEIF where Windows has their codecs). The server decodes a small copy for the
// half-block preview a console client gets; clodw decodes one for the GPU.

// Path's first frame as premultiplied BGRA (0xAARRGGBB words), top row first, scaled down to fit
// MaxW x MaxH (never up). FullW/FullH get the image's own size. False when it can't be read.
static bool32 DecodeImage(const std::wstring& Path, u32 MaxW, u32 MaxH, std::vector<u32>& Pixels, u32* W, u32* H, u32* FullW, u32* FullH)
{
    Pixels.clear();
    *W = *H = *FullW = *FullH = 0;
    HRESULT Init = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* Factory = 0;
    IWICBitmapDecoder* Decoder = 0;
    IWICBitmapFrameDecode* Frame = 0;
    IWICBitmapScaler* Scaler = 0;
    IWICFormatConverter* Converter = 0;
    bool32 Ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Factory)));
    Ok = Ok && SUCCEEDED(Factory->CreateDecoderFromFilename(Path.c_str(), 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &Decoder));
    Ok = Ok && SUCCEEDED(Decoder->GetFrame(0, &Frame));
    UINT IW = 0, IH = 0;
    Ok = Ok && SUCCEEDED(Frame->GetSize(&IW, &IH)) && IW > 0 && IH > 0;
    if(Ok)
    {
        // --- fit: scale down, keeping the shape ---
        double Scale = 1.0;
        if(IW > MaxW) { Scale = (double)MaxW / IW; }
        if(IH * Scale > MaxH) { Scale = (double)MaxH / IH; }
        UINT SW = (UINT)Max(1, (s32)(IW * Scale + 0.5));
        UINT SH = (UINT)Max(1, (s32)(IH * Scale + 0.5));
        IWICBitmapSource* Source = Frame;
        if(SW != IW || SH != IH)
        {
            Ok = SUCCEEDED(Factory->CreateBitmapScaler(&Scaler)) && SUCCEEDED(Scaler->Initialize(Frame, SW, SH, WICBitmapInterpolationModeFant));
            Source = Scaler;
        }
        Ok = Ok && SUCCEEDED(Factory->CreateFormatConverter(&Converter)) &&
             SUCCEEDED(Converter->Initialize(Source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, 0, 0.0, WICBitmapPaletteTypeCustom));
        if(Ok)
        {
            Pixels.resize((size_t)SW * SH);
            Ok = SUCCEEDED(Converter->CopyPixels(0, SW * 4, (UINT)(Pixels.size() * 4), (BYTE*)Pixels.data()));
        }
        if(Ok)
        {
            *W = SW;
            *H = SH;
            *FullW = IW;
            *FullH = IH;
        }
    }
    if(Converter) { Converter->Release(); }
    if(Scaler) { Scaler->Release(); }
    if(Frame) { Frame->Release(); }
    if(Decoder) { Decoder->Release(); }
    if(Factory) { Factory->Release(); }
    if(SUCCEEDED(Init)) { CoUninitialize(); }
    if(!Ok) { Pixels.clear(); }
    return Ok;
}
