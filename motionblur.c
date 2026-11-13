/*
 * CheesyProxxy motion blur  (original code, not taken from Latite)
 *
 * A DLL that CheesyProxxy injects into Minecraft Bedrock (Minecraft.Windows.exe).
 * It hooks IDXGISwapChain::Present / Present1 (vtable patch, no disassembler needed),
 * and blends the current frame with earlier frames on the GPU before it is shown.
 * Because it works at the swap chain, it also works while another client
 * (Oderso, Latite, ...) is loaded in the same game.
 *
 * Options (same ideas as Latite's Motion Blur module), read live from
 *   C:\Users\Public\CheesyProxxy\motionblur.cfg   (written by the CheesyProxxy menu)
 *     Intensity     1..8   how many earlier frames are blended in "Pixel average" mode
 *     Opacity       0..100 how strong the blur is
 *     Pixel average on/off: on = average of the last N frames, off = simple previous-frame blend
 *
 * Build (MinGW-w64):  x86_64-w64-mingw32-gcc -O2 -shared -o motionblur.dll motionblur.c -ld3d11 -ldxgi -luuid
 * Only Direct3D 11 swap chains are handled. If the game runs on D3D12 the DLL does nothing.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#define CFG_PATH L"C:\\Users\\Public\\CheesyProxxy\\motionblur.cfg"
#define CFG_MAGIC 0x4243504Du /* 'MPCB' */
#define MAX_HIST 8

typedef struct { DWORD magic, enabled, intensity, opacity, pixelAvg; } Cfg;
typedef struct { float opacity; int count; int mode; int pad; } CB;

typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (WINAPI *D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                       LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static PresentFn  g_origPresent;
static Present1Fn g_origPresent1;
static Cfg   g_cfg = { CFG_MAGIC, 0, 3, 60, 1 };
static DWORD g_lastCfgTick;
static BOOL  g_busy;

static ID3D11Device*            g_dev;
static ID3D11DeviceContext*     g_ctx;
static ID3D11VertexShader*      g_vs;
static ID3D11PixelShader*       g_ps;
static ID3D11Buffer*            g_cb;
static ID3D11Texture2D*         g_cur;
static ID3D11ShaderResourceView* g_curSrv;
static ID3D11Texture2D*         g_hist[MAX_HIST];
static ID3D11ShaderResourceView* g_histSrv[MAX_HIST];
static D3D11_TEXTURE2D_DESC     g_desc;
static int  g_head, g_filled, g_histN;

static const char g_hlsl[] =
"cbuffer C:register(b0){float opacity;int count;int mode;int pad;};\n"
"Texture2D cur:register(t0);\n"
"Texture2D hist[8]:register(t1);\n"
"float4 vs(uint id:SV_VertexID):SV_Position{\n"
"  float2 uv=float2((id<<1)&2,id&2);\n"
"  return float4(uv*float2(2,-2)+float2(-1,1),0,1);}\n"
"float4 ps(float4 pos:SV_Position):SV_Target{\n"
"  int3 p=int3(pos.xy,0);\n"
"  float4 c=cur.Load(p);\n"
"  if(count<=0) return c;\n"
"  if(mode==0) return float4(lerp(c.rgb,hist[0].Load(p).rgb,opacity),c.a);\n"
"  float3 sum=c.rgb;\n"
"  [unroll] for(int i=0;i<8;i++) if(i<count) sum+=hist[i].Load(p).rgb;\n"
"  return float4(lerp(c.rgb,sum/(count+1),opacity),c.a);}\n";

static void rel(void* pp) {
    IUnknown** p = (IUnknown**)pp;
    if (*p) { IUnknown_Release(*p); *p = NULL; }
}

static void free_targets(void) {
    rel(&g_curSrv); rel(&g_cur);
    for (int i = 0; i < MAX_HIST; i++) { rel(&g_histSrv[i]); rel(&g_hist[i]); }
    g_head = g_filled = g_histN = 0;
}

static void free_all(void) {
    free_targets();
    rel(&g_cb); rel(&g_ps); rel(&g_vs); rel(&g_ctx); rel(&g_dev);
}

static void load_cfg(void) {
    DWORD now = GetTickCount();
    if (now - g_lastCfgTick < 250) return;
    g_lastCfgTick = now;
    HANDLE h = CreateFileW(CFG_PATH, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    Cfg c; DWORD n = 0;
    if (ReadFile(h, &c, sizeof c, &n, NULL) && n == sizeof c && c.magic == CFG_MAGIC) {
        if (c.intensity < 1) c.intensity = 1;
        if (c.intensity > MAX_HIST) c.intensity = MAX_HIST;
        if (c.opacity > 100) c.opacity = 100;
        g_cfg = c;
    }
    CloseHandle(h);
}

static BOOL make_shaders(void) {
    HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!m) return FALSE;
    D3DCompileFn comp = (D3DCompileFn)GetProcAddress(m, "D3DCompile");
    if (!comp) return FALSE;
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    if (FAILED(comp(g_hlsl, sizeof g_hlsl - 1, "mb", NULL, NULL, "vs", "vs_5_0", 0, 0, &vb, &err))) {
        rel(&err); return FALSE;
    }
    if (FAILED(comp(g_hlsl, sizeof g_hlsl - 1, "mb", NULL, NULL, "ps", "ps_5_0", 0, 0, &pb, &err))) {
        rel(&err); rel(&vb); return FALSE;
    }
    HRESULT a = ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &g_vs);
    HRESULT b = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &g_ps);
    rel(&vb); rel(&pb);
    D3D11_BUFFER_DESC bd = { sizeof(CB), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    HRESULT c = ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb);
    return SUCCEEDED(a) && SUCCEEDED(b) && SUCCEEDED(c);
}

static BOOL make_targets(ID3D11Texture2D* back, int n) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(back, &d);
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
    d.MipLevels = 1; d.ArraySize = 1;
    if (d.SampleDesc.Count != 1) return FALSE;
    g_desc = d;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_cur))) return FALSE;
    if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)g_cur, NULL, &g_curSrv))) return FALSE;
    for (int i = 0; i < n; i++) {
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &g_hist[i]))) return FALSE;
        if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource*)g_hist[i], NULL, &g_histSrv[i]))) return FALSE;
    }
    g_histN = n; g_head = g_filled = 0;
    return TRUE;
}

static void do_blur(IDXGISwapChain* sc) {
    ID3D11Device* dev = NULL;
    if (FAILED(IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void**)&dev))) return; /* not D3D11 */
    if (dev != g_dev) {
        free_all();
        g_dev = dev;
        ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
        if (!make_shaders()) { free_all(); return; }
    } else {
        ID3D11Device_Release(dev);
    }

    ID3D11Texture2D* back = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void**)&back))) return;
    D3D11_TEXTURE2D_DESC bd;
    ID3D11Texture2D_GetDesc(back, &bd);

    int mode = g_cfg.pixelAvg ? 1 : 0;
    int n = mode ? (int)g_cfg.intensity : 1;
    if (!g_cur || bd.Width != g_desc.Width || bd.Height != g_desc.Height ||
        bd.Format != g_desc.Format || n != g_histN) {
        free_targets();
        if (!make_targets(back, n)) { free_targets(); rel(&back); return; }
    }

    /* snapshot of the finished frame */
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource*)g_cur, (ID3D11Resource*)back);

    ID3D11RenderTargetView* rtv = NULL;
    if (SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource*)back, NULL, &rtv)) && g_filled > 0) {
        /* save the few bits of pipeline state we touch */
        ID3D11RenderTargetView* oRtv[8] = {0}; ID3D11DepthStencilView* oDsv = NULL;
        D3D11_VIEWPORT oVp[16]; UINT nVp = 16;
        ID3D11VertexShader* oVs = NULL; ID3D11PixelShader* oPs = NULL;
        ID3D11ShaderResourceView* oSrv[9] = {0};
        ID3D11Buffer* oCb = NULL; ID3D11InputLayout* oIl = NULL;
        ID3D11BlendState* oBs = NULL; FLOAT oBf[4]; UINT oSm;
        ID3D11DepthStencilState* oDs = NULL; UINT oSr;
        ID3D11RasterizerState* oRs = NULL; D3D11_PRIMITIVE_TOPOLOGY oTop;
        ID3D11DeviceContext* c = g_ctx;
        ID3D11DeviceContext_OMGetRenderTargets(c, 8, oRtv, &oDsv);
        ID3D11DeviceContext_RSGetViewports(c, &nVp, oVp);
        ID3D11DeviceContext_VSGetShader(c, &oVs, NULL, NULL);
        ID3D11DeviceContext_PSGetShader(c, &oPs, NULL, NULL);
        ID3D11DeviceContext_PSGetShaderResources(c, 0, 9, oSrv);
        ID3D11DeviceContext_PSGetConstantBuffers(c, 0, 1, &oCb);
        ID3D11DeviceContext_IAGetInputLayout(c, &oIl);
        ID3D11DeviceContext_IAGetPrimitiveTopology(c, &oTop);
        ID3D11DeviceContext_OMGetBlendState(c, &oBs, oBf, &oSm);
        ID3D11DeviceContext_OMGetDepthStencilState(c, &oDs, &oSr);
        ID3D11DeviceContext_RSGetState(c, &oRs);

        CB cb = { g_cfg.opacity / 100.0f, g_filled < n ? g_filled : n, mode, 0 };
        if (mode == 0) cb.count = 1;
        ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource*)g_cb, 0, NULL, &cb, 0, 0);

        ID3D11ShaderResourceView* srv[9] = {0};
        srv[0] = g_curSrv;
        /* newest history first (not required for averaging, kept simple) */
        for (int i = 0; i < g_histN; i++) srv[1 + i] = g_histSrv[i];

        D3D11_VIEWPORT vp = { 0, 0, (FLOAT)bd.Width, (FLOAT)bd.Height, 0, 1 };
        ID3D11DeviceContext_OMSetRenderTargets(c, 1, &rtv, NULL);
        ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, 0xFFFFFFFF);
        ID3D11DeviceContext_OMSetDepthStencilState(c, NULL, 0);
        ID3D11DeviceContext_RSSetState(c, NULL);
        ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
        ID3D11DeviceContext_IASetInputLayout(c, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(c, g_vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(c, g_ps, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &g_cb);
        ID3D11DeviceContext_PSSetShaderResources(c, 0, 9, srv);
        ID3D11DeviceContext_Draw(c, 3, 0);

        /* restore */
        ID3D11DeviceContext_PSSetShaderResources(c, 0, 9, oSrv);
        ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &oCb);
        ID3D11DeviceContext_PSSetShader(c, oPs, NULL, 0);
        ID3D11DeviceContext_VSSetShader(c, oVs, NULL, 0);
        ID3D11DeviceContext_IASetInputLayout(c, oIl);
        ID3D11DeviceContext_IASetPrimitiveTopology(c, oTop);
        ID3D11DeviceContext_OMSetBlendState(c, oBs, oBf, oSm);
        ID3D11DeviceContext_OMSetDepthStencilState(c, oDs, oSr);
        ID3D11DeviceContext_RSSetState(c, oRs);
        ID3D11DeviceContext_RSSetViewports(c, nVp, oVp);
        ID3D11DeviceContext_OMSetRenderTargets(c, 8, oRtv, oDsv);
        for (int i = 0; i < 9; i++) rel(&oSrv[i]);
        for (int i = 0; i < 8; i++) rel(&oRtv[i]);
        rel(&oDsv); rel(&oVs); rel(&oPs); rel(&oCb); rel(&oIl); rel(&oBs); rel(&oDs); rel(&oRs);
    }
    rel(&rtv);

    /* remember a frame for next time: mode 1 = raw frames, mode 0 = our blended output */
    ID3D11Texture2D* src = (mode == 0) ? back : g_cur;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource*)g_hist[g_head], (ID3D11Resource*)src);
    g_head = (g_head + 1) % g_histN;
    if (g_filled < g_histN) g_filled++;
    rel(&back);
}

static void on_present(IDXGISwapChain* sc) {
    if (g_busy) return;
    g_busy = TRUE;
    load_cfg();
    if (g_cfg.enabled && g_cfg.opacity > 0) do_blur(sc);
    else if (g_cur) free_targets();
    g_busy = FALSE;
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    on_present(sc);
    return g_origPresent(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    on_present((IDXGISwapChain*)sc);
    return g_origPresent1(sc, sync, flags, p);
}

static BOOL patch(void** slot, void* hook, void** orig) {
    DWORD old;
    if (*slot == hook) return TRUE;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return FALSE;
    *orig = *slot; *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return TRUE;
}

static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    HWND w = CreateWindowExW(0, L"STATIC", L"cp", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, NULL, NULL);
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = 8; sd.BufferDesc.Height = 8;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = w;
    sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = NULL; ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL;
    if (FAILED(D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
            D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx))) {
        if (w) DestroyWindow(w);
        return 0;
    }
    void** vt = *(void***)sc;
    patch(&vt[8], (void*)hkPresent, (void**)&g_origPresent);          /* IDXGISwapChain::Present  */
    IDXGISwapChain1* sc1 = NULL;
    if (SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain1, (void**)&sc1))) {
        void** vt1 = *(void***)sc1;
        patch(&vt1[22], (void*)hkPresent1, (void**)&g_origPresent1);  /* IDXGISwapChain1::Present1 */
        IDXGISwapChain1_Release(sc1);
    }
    /* the dummy objects can go; the patched vtable lives in dxgi.dll and is shared by the game's swap chain */
    IDXGISwapChain_Release(sc); ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev);
    if (w) DestroyWindow(w);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID res) {
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        WCHAR path[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
        BOOL isMc = FALSE;
        if (n) {
            const WCHAR* base = path; for (DWORD i = 0; i < n; i++) if (path[i] == L'\\') base = path + i + 1;
            isMc = (lstrcmpiW(base, L"Minecraft.Windows.exe") == 0); /* only ever active inside Minecraft */
        }
        if (!isMc) return FALSE;
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
