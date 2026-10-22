// clodw's GPU side: one Direct3D 11 pixel shader draws the whole window from a grid of cells, each
// naming a tile in a glyph atlas texture plus its colors. Glyph tiles are drawn on demand with
// Direct2D/DirectWrite (clodw_glyphs.cpp). Device and swap chain set up like the game renderer
// (FLIP_DISCARD, 2 buffers, latency 1); BGRA support is added for Direct2D, and the swap chain goes
// through DirectComposition so the background can be see-through (settings "opacity").

#define ReleaseCom(P) do { if(P) { (P)->Release(); (P) = 0; } } while(0)

// One cell as the shader reads it. Colors are packed 0x00BBGGRR.
struct gpu_cell
{
    u32 Tile;  // atlas tile: x | y << 16; tile 0 is blank
    u32 Fg;
    u32 Bg;
    u32 Flags; // GPU_*
};

enum
{
    GPU_UNDERLINE = 1 << 0,
    GPU_STRIKE    = 1 << 1,
    GPU_COLOR     = 1 << 2, // the tile holds its own colors (emoji): draw them instead of tinting with Fg
    GPU_CURSOR_UNDER = 1 << 3, // underline cursor across this cell
    GPU_CURSOR_BAR   = 1 << 4, // bar cursor at this cell's left edge
    GPU_SEE_THROUGH  = 1 << 5, // the background is the terminal's default: drawn at the window's opacity
    GPU_DOT          = 1 << 6, // a round dot in Fg, drawn by the shader (no font draws it round in every cell shape)
};

// Matches cbuffer Consts in the shader.
struct alignas(16) gpu_consts
{
    u32 CellW;
    u32 CellH;
    u32 GridW;
    u32 GridH;
    u32 MarginColor;
    u32 UnderlineY;
    u32 StrikeY;
    u32 LineH;
    u32 CursorThick; // underline / bar cursor thickness in pixels
    u32 Opacity;     // 0..255, for GPU_SEE_THROUGH backgrounds and the margin
    u32 Pad[2];
};

#define ATLAS_SIZE 2048
#define GLYPH_MAX_CELLS 16 // widest glyph run: the Direct2D scratch surface is this many cells wide

struct renderer
{
    HWND Window;
    ID3D11Device* Device;
    ID3D11DeviceContext* Context;
    IDXGISwapChain1* SwapChain;
    IDCompositionDevice* Comp; // the swap chain is shown through DirectComposition, so it can be see-through
    IDCompositionTarget* CompTarget;
    IDCompositionVisual* CompVisual;
    ID3D11RenderTargetView* Target;
    s32 BackW;
    s32 BackH;

    ID3D11VertexShader* VertexShader;
    ID3D11PixelShader* PixelShader;
    ID3D11Buffer* Consts;
    ID3D11Buffer* Cells;
    ID3D11ShaderResourceView* CellsView;
    u32 CellCapacity;
    ID3D11Texture2D* Atlas;
    ID3D11ShaderResourceView* AtlasView;

    // Glyph drawing: Direct2D renders into Scratch, whose tiles are copied into the atlas.
    ID2D1Factory* D2D;
    IDWriteFactory2* DWrite;
    ID3D11Texture2D* Scratch;
    ID2D1RenderTarget* Draw2D;
    ID2D1SolidColorBrush* White;
};

static const char ShaderSource[] = R"(
struct Cell { uint Tile; uint Fg; uint Bg; uint Flags; };
cbuffer Consts : register(b0)
{
    uint CellW; uint CellH; uint GridW; uint GridH;
    uint MarginColor; uint UnderlineY; uint StrikeY; uint LineH;
    uint CursorThick; uint Opacity; uint Pad1; uint Pad2;
};
StructuredBuffer<Cell> Cells : register(t0);
Texture2D<float4> Atlas : register(t1);

float3 Unpack(uint C) { return float3(C & 255, (C >> 8) & 255, (C >> 16) & 255) / 255.0; }

// One triangle covering the window.
float4 VsMain(uint Id : SV_VertexID) : SV_Position
{
    float2 P = float2((Id << 1) & 2, Id & 2);
    return float4(P.x * 2 - 1, 1 - P.y * 2, 0, 1);
}

float4 PsMain(float4 Pos : SV_Position) : SV_Target
{
    uint2 P = uint2(Pos.xy);
    uint2 C = uint2(P.x / CellW, P.y / CellH);
    float See = Opacity / 255.0;
    if(C.x >= GridW || C.y >= GridH) { return float4(Unpack(MarginColor) * See, See); }
    uint2 In = P - C * uint2(CellW, CellH);
    Cell K = Cells[C.y * GridW + C.x];
    uint2 Tile = uint2(K.Tile & 0xFFFF, K.Tile >> 16);
    float4 T = K.Tile ? Atlas.Load(int3(Tile * uint2(CellW, CellH) + In, 0)) : float4(0, 0, 0, 0); // premultiplied; tile 0 = blank
    float3 Fg = Unpack(K.Fg);
    float3 Bg = Unpack(K.Bg);
    float3 Ink = (K.Flags & 4) ? T.rgb : Fg * T.a;
    float BgA = (K.Flags & 32) ? See : 1;
    float3 Out = Ink + Bg * BgA * (1 - T.a); // premultiplied, like the swap chain
    float OutA = T.a + BgA * (1 - T.a);
    if(K.Flags & 64)
    {
        float2 D = float2(In) + 0.5 - float2(CellW, CellH) * 0.5;
        float Cov = saturate(min(CellW, CellH) * 0.32 - length(D) + 0.5); // antialiased edge
        Out = Fg * Cov + Bg * BgA * (1 - Cov);
        OutA = Cov + BgA * (1 - Cov);
    }
    bool Under = (K.Flags & 1) && In.y >= UnderlineY && In.y < UnderlineY + LineH;
    bool Strike = (K.Flags & 2) && In.y >= StrikeY && In.y < StrikeY + LineH;
    bool CursorUnder = (K.Flags & 8) && In.y + CursorThick >= CellH;
    bool CursorBar = (K.Flags & 16) && In.x < CursorThick;
    if(Under || Strike || CursorUnder || CursorBar) { Out = Fg; OutA = 1; }
    return float4(Out, OutA);
}
)";

static bool32 RendererFail(HRESULT Hr, const char* What)
{
    char Buf[256];
    snprintf(Buf, sizeof(Buf), "clodw: %s failed (0x%08X)", What, (u32)Hr);
    MessageBoxA(0, Buf, "clodw", MB_OK | MB_ICONERROR);
    return false;
}

static bool32 RendererCreate(renderer& R, HWND Window)
{
    R.Window = Window;
    HRESULT Hr = S_OK;

    // --- device and swap chain ---
    UINT Flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if CLODW_DEBUG
    Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL Levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    Hr = D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, Flags, Levels, ArrayCount(Levels), D3D11_SDK_VERSION, &R.Device, 0, &R.Context);
    if(FAILED(Hr)) { return RendererFail(Hr, "D3D11CreateDevice"); }

    IDXGIDevice1* DxgiDevice = 0;
    IDXGIAdapter* Adapter = 0;
    IDXGIFactory2* Factory = 0;
    R.Device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&DxgiDevice);
    DxgiDevice->SetMaximumFrameLatency(1);
    DxgiDevice->GetAdapter(&Adapter);
    Adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&Factory);
    // A composition swap chain with premultiplied alpha, so default backgrounds can let the desktop
    // show through (the window has WS_EX_NOREDIRECTIONBITMAP). Its size is explicit, unlike an HWND's.
    RECT Client = {};
    GetClientRect(Window, &Client);
    DXGI_SWAP_CHAIN_DESC1 Desc = {};
    Desc.Width = (UINT)Max(1, (s32)(Client.right - Client.left));
    Desc.Height = (UINT)Max(1, (s32)(Client.bottom - Client.top));
    Desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Desc.SampleDesc.Count = 1;
    Desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Desc.BufferCount = 2;
    Desc.Scaling = DXGI_SCALING_STRETCH;
    Desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    Desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    Hr = Factory->CreateSwapChainForComposition(R.Device, &Desc, 0, &R.SwapChain);
    ReleaseCom(Factory);
    ReleaseCom(Adapter);
    if(FAILED(Hr)) { ReleaseCom(DxgiDevice); return RendererFail(Hr, "CreateSwapChainForComposition"); }
    Hr = DCompositionCreateDevice(DxgiDevice, __uuidof(IDCompositionDevice), (void**)&R.Comp);
    if(SUCCEEDED(Hr)) { Hr = R.Comp->CreateTargetForHwnd(Window, TRUE, &R.CompTarget); }
    if(SUCCEEDED(Hr)) { Hr = R.Comp->CreateVisual(&R.CompVisual); }
    if(SUCCEEDED(Hr)) { Hr = R.CompVisual->SetContent(R.SwapChain); }
    if(SUCCEEDED(Hr)) { Hr = R.CompTarget->SetRoot(R.CompVisual); }
    if(SUCCEEDED(Hr)) { Hr = R.Comp->Commit(); }
    if(FAILED(Hr)) { ReleaseCom(DxgiDevice); return RendererFail(Hr, "DirectComposition"); }

    // --- shaders, compiled from the embedded source ---
    ID3DBlob* Code = 0;
    ID3DBlob* Errors = 0;
    Hr = D3DCompile(ShaderSource, sizeof(ShaderSource) - 1, "clodw", 0, 0, "VsMain", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
    if(FAILED(Hr)) { if(Errors) { MessageBoxA(0, (const char*)Errors->GetBufferPointer(), "clodw shader", MB_OK); } return false; }
    R.Device->CreateVertexShader(Code->GetBufferPointer(), Code->GetBufferSize(), 0, &R.VertexShader);
    ReleaseCom(Code);
    Hr = D3DCompile(ShaderSource, sizeof(ShaderSource) - 1, "clodw", 0, 0, "PsMain", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
    if(FAILED(Hr)) { if(Errors) { MessageBoxA(0, (const char*)Errors->GetBufferPointer(), "clodw shader", MB_OK); } return false; }
    R.Device->CreatePixelShader(Code->GetBufferPointer(), Code->GetBufferSize(), 0, &R.PixelShader);
    ReleaseCom(Code);

    D3D11_BUFFER_DESC CDesc = {};
    CDesc.ByteWidth = sizeof(gpu_consts);
    CDesc.Usage = D3D11_USAGE_DYNAMIC;
    CDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    R.Device->CreateBuffer(&CDesc, 0, &R.Consts);

    // --- glyph atlas: premultiplied BGRA tiles, filled by copies from the Direct2D scratch ---
    D3D11_TEXTURE2D_DESC TDesc = {};
    TDesc.Width = ATLAS_SIZE;
    TDesc.Height = ATLAS_SIZE;
    TDesc.MipLevels = 1;
    TDesc.ArraySize = 1;
    TDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    TDesc.SampleDesc.Count = 1;
    TDesc.Usage = D3D11_USAGE_DEFAULT;
    TDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Hr = R.Device->CreateTexture2D(&TDesc, 0, &R.Atlas);
    if(FAILED(Hr)) { ReleaseCom(DxgiDevice); return RendererFail(Hr, "atlas texture"); }
    R.Device->CreateShaderResourceView(R.Atlas, 0, &R.AtlasView);

    // --- Direct2D / DirectWrite ---
    Hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), 0, (void**)&R.D2D);
    if(FAILED(Hr)) { ReleaseCom(DxgiDevice); return RendererFail(Hr, "D2D1CreateFactory"); }
    Hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), (IUnknown**)&R.DWrite);
    if(FAILED(Hr)) { ReleaseCom(DxgiDevice); return RendererFail(Hr, "DWriteCreateFactory"); }
    ReleaseCom(DxgiDevice);
    return true;
}

// The Direct2D scratch surface glyphs are drawn into: wide enough for the longest cluster.
static void RendererSetCellSize(renderer& R, s32 CellW, s32 CellH)
{
    ReleaseCom(R.White);
    ReleaseCom(R.Draw2D);
    ReleaseCom(R.Scratch);
    D3D11_TEXTURE2D_DESC TDesc = {};
    TDesc.Width = (UINT)(CellW * GLYPH_MAX_CELLS);
    TDesc.Height = (UINT)CellH;
    TDesc.MipLevels = 1;
    TDesc.ArraySize = 1;
    TDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    TDesc.SampleDesc.Count = 1;
    TDesc.Usage = D3D11_USAGE_DEFAULT;
    TDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    R.Device->CreateTexture2D(&TDesc, 0, &R.Scratch);
    IDXGISurface* Surface = 0;
    R.Scratch->QueryInterface(__uuidof(IDXGISurface), (void**)&Surface);
    // 96 DPI: one Direct2D unit is one pixel; font sizes are given in pixels.
    D2D1_RENDER_TARGET_PROPERTIES Props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    R.D2D->CreateDxgiSurfaceRenderTarget(Surface, &Props, &R.Draw2D);
    ReleaseCom(Surface);
    R.Draw2D->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    R.Draw2D->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &R.White);
}

// Everything, for starting over after the GPU device is lost (driver update, TDR).
static void RendererRelease(renderer& R)
{
    if(R.Context) { R.Context->ClearState(); }
    ReleaseCom(R.White);
    ReleaseCom(R.Draw2D);
    ReleaseCom(R.Scratch);
    ReleaseCom(R.DWrite);
    ReleaseCom(R.D2D);
    ReleaseCom(R.AtlasView);
    ReleaseCom(R.Atlas);
    ReleaseCom(R.CellsView);
    ReleaseCom(R.Cells);
    ReleaseCom(R.Consts);
    ReleaseCom(R.PixelShader);
    ReleaseCom(R.VertexShader);
    ReleaseCom(R.Target);
    ReleaseCom(R.CompVisual);
    ReleaseCom(R.CompTarget);
    ReleaseCom(R.Comp);
    ReleaseCom(R.SwapChain);
    ReleaseCom(R.Context);
    ReleaseCom(R.Device);
    R = renderer{};
}

// Draws one frame. Cells is GridW x GridH; the swap chain follows the window's client size.
// Returns false when the device was lost: release and create the renderer again.
static bool32 RendererDraw(renderer& R, const std::vector<gpu_cell>& Cells, const gpu_consts& Consts)
{
    RECT Client = {};
    GetClientRect(R.Window, &Client);
    s32 W = Max(1, (s32)(Client.right - Client.left));
    s32 H = Max(1, (s32)(Client.bottom - Client.top));

    // --- resize: unbind, release, ResizeBuffers, recreate ---
    if(W != R.BackW || H != R.BackH || !R.Target)
    {
        R.Context->OMSetRenderTargets(0, 0, 0);
        ReleaseCom(R.Target);
        R.SwapChain->ResizeBuffers(0, (UINT)W, (UINT)H, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* Back = 0;
        R.SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&Back);
        R.Device->CreateRenderTargetView(Back, 0, &R.Target);
        ReleaseCom(Back);
        R.BackW = W;
        R.BackH = H;
    }

    // --- cells: a DEFAULT structured buffer, grown as needed, updated in place ---
    u32 Count = Max(1, (s32)Cells.size());
    if(Count > R.CellCapacity)
    {
        ReleaseCom(R.CellsView);
        ReleaseCom(R.Cells);
        R.CellCapacity = Count + Count / 2;
        D3D11_BUFFER_DESC BDesc = {};
        BDesc.ByteWidth = R.CellCapacity * sizeof(gpu_cell);
        BDesc.Usage = D3D11_USAGE_DEFAULT;
        BDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        BDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        BDesc.StructureByteStride = sizeof(gpu_cell);
        R.Device->CreateBuffer(&BDesc, 0, &R.Cells);
        D3D11_SHADER_RESOURCE_VIEW_DESC VDesc = {};
        VDesc.Format = DXGI_FORMAT_UNKNOWN;
        VDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        VDesc.Buffer.NumElements = R.CellCapacity;
        R.Device->CreateShaderResourceView(R.Cells, &VDesc, &R.CellsView);
    }
    if(!Cells.empty())
    {
        D3D11_BOX Box = {0, 0, 0, (UINT)(Cells.size() * sizeof(gpu_cell)), 1, 1};
        R.Context->UpdateSubresource(R.Cells, 0, &Box, Cells.data(), 0, 0);
    }
    D3D11_MAPPED_SUBRESOURCE Mapped = {};
    if(SUCCEEDED(R.Context->Map(R.Consts, 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped)))
    {
        memcpy(Mapped.pData, &Consts, sizeof(Consts));
        R.Context->Unmap(R.Consts, 0);
    }

    // --- one full-screen triangle ---
    D3D11_VIEWPORT View = {0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f};
    R.Context->RSSetViewports(1, &View);
    R.Context->OMSetRenderTargets(1, &R.Target, 0);
    R.Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    R.Context->IASetInputLayout(0);
    R.Context->VSSetShader(R.VertexShader, 0, 0);
    R.Context->PSSetShader(R.PixelShader, 0, 0);
    R.Context->PSSetConstantBuffers(0, 1, &R.Consts);
    ID3D11ShaderResourceView* Views[2] = {R.CellsView, R.AtlasView};
    R.Context->PSSetShaderResources(0, 2, Views);
    R.Context->Draw(3, 0);
    HRESULT Hr = R.SwapChain->Present(1, 0);
    return !(Hr == DXGI_ERROR_DEVICE_REMOVED || Hr == DXGI_ERROR_DEVICE_RESET);
}
