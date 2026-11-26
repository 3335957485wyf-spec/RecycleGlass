#include "blur_gpu.h"

#include <d3d11.h>

/* d3dcompiler is loaded dynamically: the toolchain has no import library for
 * it, and d3dcompiler_47.dll ships with every Windows 10/11 install. */
typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void *,
    void *, LPCSTR, LPCSTR, UINT, UINT, void *, void *);
static PFN_D3DCompile pD3DCompile;
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- shaders ------------------------------------------------------- */

static const char SHADER_SRC[] =
"cbuffer CB : register(b0)\n"
"{\n"
"    int radius;\n"
"    int srcW, srcH;\n"
"    int dstW, dstH;\n"
"};\n"
"Texture2D srcT : register(t0);\n"
"struct VO { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
"VO VSMain(uint id : SV_VertexID)\n"
"{\n"
"    VO o;\n"
"    float2 uv = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
"    o.uv = uv;\n"
"    return o;\n"
"}\n"
"/* byte-exact vs the CPU path: integer rounding rules per pass */\n"
"float4 PSDown(VO i) : SV_TARGET\n"
"{\n"
"    int2 d = int2(i.uv * float2(dstW, dstH));\n"
"    int2 s = d * 2;\n"
"    int mx = srcW - 1, my = srcH - 1;\n"
"    float4 acc = 0;\n"
"    acc += round(srcT.Load(int3(min(s.x,     mx), min(s.y,     my), 0)) * 255.0);\n"
"    acc += round(srcT.Load(int3(min(s.x + 1, mx), min(s.y,     my), 0)) * 255.0);\n"
"    acc += round(srcT.Load(int3(min(s.x,     mx), min(s.y + 1, my), 0)) * 255.0);\n"
"    acc += round(srcT.Load(int3(min(s.x + 1, mx), min(s.y + 1, my), 0)) * 255.0);\n"
"    return floor((acc + 2.0) * 0.25) / 255.0;\n"
"}\n"
"float4 PSBoxH(VO i) : SV_TARGET\n"
"{\n"
"    int2 d = int2(i.uv * float2(dstW, dstH));\n"
"    int a = max(d.x - radius, 0);\n"
"    int b = min(d.x + radius, srcW - 1);\n"
"    float4 acc = 0;\n"
"    for (int k = a; k <= b; k++)\n"
"        acc += round(srcT.Load(int3(k, d.y, 0)) * 255.0);\n"
"    return floor(acc / (b - a + 1)) / 255.0;\n"
"}\n"
"float4 PSBoxV(VO i) : SV_TARGET\n"
"{\n"
"    int2 d = int2(i.uv * float2(dstW, dstH));\n"
"    int a = max(d.y - radius, 0);\n"
"    int b = min(d.y + radius, srcH - 1);\n"
"    float4 acc = 0;\n"
"    for (int k = a; k <= b; k++)\n"
"        acc += round(srcT.Load(int3(d.x, k, 0)) * 255.0);\n"
"    return floor(acc / (b - a + 1)) / 255.0;\n"
"}\n"
"float4 PSUp(VO i) : SV_TARGET\n"
"{\n"
"    int2 d = int2(i.uv * float2(dstW, dstH));\n"
"    int ix = d.x / 2, iy = d.y / 2;\n"
"    int fx = (d.x & 1) * 128, fy = (d.y & 1) * 128;\n"
"    int ix1 = min(ix + 1, srcW - 1), iy1 = min(iy + 1, srcH - 1);\n"
"    float4 a = round(srcT.Load(int3(ix,  iy,  0)) * 255.0);\n"
"    float4 b = round(srcT.Load(int3(ix1, iy,  0)) * 255.0);\n"
"    float4 c = round(srcT.Load(int3(ix,  iy1, 0)) * 255.0);\n"
"    float4 e = round(srcT.Load(int3(ix1, iy1, 0)) * 255.0);\n"
"    float4 top = a * (256 - fx) + b * fx;\n"
"    float4 bot = c * (256 - fx) + e * fx;\n"
"    return floor((top * (256 - fy) + bot * fy) / 65536.0) / 255.0;\n"
"}\n";

/* Must stay byte-identical to the HLSL cbuffer above.  The old declaration
 * carried an unused "int3 padv" tail, which HLSL packs at offset 32 (vectors
 * are 16-byte aligned) while this struct put it at 20 - so the CPU struct was
 * 32 bytes for a 48-byte shader buffer.  Nothing read padv, which is the only
 * reason it never showed; dropping it from both sides makes the sizes agree
 * (5 ints, rounded up to 32 by HLSL) and removes the trap. */
typedef struct {
    int radius;
    int srcW, srcH;
    int dstW, dstH;
} BlurCB;

/* ---- device state --------------------------------------------------- */

static ID3D11Device        *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11VertexShader  *g_vs;
static ID3D11PixelShader   *g_ps_down, *g_ps_bh, *g_ps_bv, *g_ps_up;
static ID3D11SamplerState  *g_sampler;
static ID3D11Buffer        *g_cb;
static CRITICAL_SECTION     g_lk;
static INIT_ONCE            g_once = INIT_ONCE_STATIC_INIT;

/* full-res src/out + half-res ping-pong + readback staging */
static ID3D11Texture2D          *g_t_src, *g_t_ha, *g_t_hb, *g_t_out, *g_t_stg;
static ID3D11ShaderResourceView *g_srv_src, *g_srv_ha, *g_srv_hb, *g_srv_out;
static ID3D11RenderTargetView   *g_rtv_ha, *g_rtv_hb, *g_rtv_out;
static int g_cw, g_ch;          /* cached full-res size */

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = pD3DCompile(SHADER_SRC, sizeof(SHADER_SRC) - 1, "blur_gpu",
                             NULL, NULL, entry, target,
                             3 /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */,
                             0, &blob, &err);
    if (FAILED(hr))
    {
        if (err)
            err->lpVtbl->Release(err);
        return NULL;
    }
    if (err)
        err->lpVtbl->Release(err);
    return blob;
}

static void cache_free(void)
{
    if (g_rtv_ha)  { g_rtv_ha->lpVtbl->Release(g_rtv_ha);  g_rtv_ha = NULL; }
    if (g_rtv_hb)  { g_rtv_hb->lpVtbl->Release(g_rtv_hb);  g_rtv_hb = NULL; }
    if (g_rtv_out) { g_rtv_out->lpVtbl->Release(g_rtv_out); g_rtv_out = NULL; }
    if (g_srv_src) { g_srv_src->lpVtbl->Release(g_srv_src); g_srv_src = NULL; }
    if (g_srv_ha)  { g_srv_ha->lpVtbl->Release(g_srv_ha);  g_srv_ha = NULL; }
    if (g_srv_hb)  { g_srv_hb->lpVtbl->Release(g_srv_hb);  g_srv_hb = NULL; }
    if (g_srv_out) { g_srv_out->lpVtbl->Release(g_srv_out); g_srv_out = NULL; }
    if (g_t_src)   { g_t_src->lpVtbl->Release(g_t_src);   g_t_src = NULL; }
    if (g_t_ha)    { g_t_ha->lpVtbl->Release(g_t_ha);    g_t_ha = NULL; }
    if (g_t_hb)    { g_t_hb->lpVtbl->Release(g_t_hb);    g_t_hb = NULL; }
    if (g_t_out)   { g_t_out->lpVtbl->Release(g_t_out);   g_t_out = NULL; }
    if (g_t_stg)   { g_t_stg->lpVtbl->Release(g_t_stg);   g_t_stg = NULL; }
    g_cw = g_ch = 0;
}

static BOOL make_tex(int w, int h, UINT bind, UINT cpu,
                     ID3D11Texture2D **tex,
                     ID3D11ShaderResourceView **srv,
                     ID3D11RenderTargetView **rtv)
{
    D3D11_TEXTURE2D_DESC d;
    HRESULT hr;

    ZeroMemory(&d, sizeof(d));
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = cpu ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    d.BindFlags = cpu ? 0 : bind;
    d.CPUAccessFlags = cpu;
    hr = g_dev->lpVtbl->CreateTexture2D(g_dev, &d, NULL, tex);
    if (FAILED(hr))
        return FALSE;
    if (srv)
    {
        hr = g_dev->lpVtbl->CreateShaderResourceView(g_dev,
                (ID3D11Resource *)*tex, NULL, srv);
        if (FAILED(hr))
            return FALSE;
    }
    if (rtv)
    {
        hr = g_dev->lpVtbl->CreateRenderTargetView(g_dev,
                (ID3D11Resource *)*tex, NULL, rtv);
        if (FAILED(hr))
            return FALSE;
    }
    return TRUE;
}

static BOOL cache_ensure(int w, int h)
{
    int hw = (w + 1) / 2, hh = (h + 1) / 2;

    if (g_cw == w && g_ch == h)
        return TRUE;
    cache_free();
    if (!make_tex(w, h, D3D11_BIND_SHADER_RESOURCE, 0,
                  &g_t_src, &g_srv_src, NULL))
        goto fail;
    if (!make_tex(hw, hh, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                  0, &g_t_ha, &g_srv_ha, &g_rtv_ha))
        goto fail;
    if (!make_tex(hw, hh, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                  0, &g_t_hb, &g_srv_hb, &g_rtv_hb))
        goto fail;
    if (!make_tex(w, h, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                  0, &g_t_out, &g_srv_out, &g_rtv_out))
        goto fail;
    if (!make_tex(w, h, 0, D3D11_CPU_ACCESS_READ, &g_t_stg, NULL, NULL))
        goto fail;
    g_cw = w;
    g_ch = h;
    return TRUE;
fail:
    cache_free();
    return FALSE;
}

static void set_cb(int radius, int sw, int sh, int dw, int dh)
{
    BlurCB cb;
    ZeroMemory(&cb, sizeof(cb));
    cb.radius = radius;
    cb.srcW = sw;
    cb.srcH = sh;
    cb.dstW = dw;
    cb.dstH = dh;
    g_ctx->lpVtbl->UpdateSubresource(g_ctx, (ID3D11Resource *)g_cb, 0, NULL,
                                     &cb, 0, 0);
}

static void draw_ps(ID3D11PixelShader *ps, ID3D11ShaderResourceView *srv,
                    ID3D11RenderTargetView *rtv, int w, int h,
                    int radius, int sw, int sh)
{
    D3D11_VIEWPORT vp;
    ID3D11ShaderResourceView *srvs[1];

    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (FLOAT)w;
    vp.Height = (FLOAT)h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    set_cb(radius, sw, sh, w, h);
    g_ctx->lpVtbl->OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    g_ctx->lpVtbl->RSSetViewports(g_ctx, 1, &vp);
    srvs[0] = srv;
    g_ctx->lpVtbl->PSSetShaderResources(g_ctx, 0, 1, srvs);
    g_ctx->lpVtbl->PSSetShader(g_ctx, ps, NULL, 0);
    g_ctx->lpVtbl->Draw(g_ctx, 3, 0);

    srvs[0] = NULL;      /* unbind so the next pass may RT-bind the same tex */
    g_ctx->lpVtbl->PSSetShaderResources(g_ctx, 0, 1, srvs);
}

static BOOL CALLBACK gpu_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    ID3DBlob *vs_b, *psd_b, *psh_b, *psv_b, *psu_b;
    D3D11_SAMPLER_DESC sd;
    D3D11_BUFFER_DESC bd;
    HRESULT hr;
    static const D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0,
                                            D3D_FEATURE_LEVEL_10_0 };

    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lk);

    {
        HMODULE m = LoadLibraryW(L"d3dcompiler_47.dll");
        if (!m)
            m = LoadLibraryW(L"d3dcompiler_43.dll");
        if (m)
            pD3DCompile = (PFN_D3DCompile)GetProcAddress(m, "D3DCompile");
        if (!pD3DCompile)
            return TRUE;      /* no shader compiler; CPU path only */
    }

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           lv, 2, D3D11_SDK_VERSION,
                           &g_dev, NULL, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               lv, 2, D3D11_SDK_VERSION,
                               &g_dev, NULL, &g_ctx);
    if (FAILED(hr))
        return TRUE;          /* no GPU path; callers fall back */

    vs_b = compile("VSMain", "vs_5_0");
    psd_b = compile("PSDown", "ps_5_0");
    psh_b = compile("PSBoxH", "ps_5_0");
    psv_b = compile("PSBoxV", "ps_5_0");
    psu_b = compile("PSUp", "ps_5_0");
    if (!vs_b || !psd_b || !psh_b || !psv_b || !psu_b)
        goto fail;

    if (FAILED(g_dev->lpVtbl->CreateVertexShader(g_dev,
            vs_b->lpVtbl->GetBufferPointer(vs_b),
            vs_b->lpVtbl->GetBufferSize(vs_b), NULL, &g_vs)))
        goto fail;
    if (FAILED(g_dev->lpVtbl->CreatePixelShader(g_dev,
            psd_b->lpVtbl->GetBufferPointer(psd_b),
            psd_b->lpVtbl->GetBufferSize(psd_b), NULL, &g_ps_down)))
        goto fail;
    if (FAILED(g_dev->lpVtbl->CreatePixelShader(g_dev,
            psh_b->lpVtbl->GetBufferPointer(psh_b),
            psh_b->lpVtbl->GetBufferSize(psh_b), NULL, &g_ps_bh)))
        goto fail;
    if (FAILED(g_dev->lpVtbl->CreatePixelShader(g_dev,
            psv_b->lpVtbl->GetBufferPointer(psv_b),
            psv_b->lpVtbl->GetBufferSize(psv_b), NULL, &g_ps_bv)))
        goto fail;
    if (FAILED(g_dev->lpVtbl->CreatePixelShader(g_dev,
            psu_b->lpVtbl->GetBufferPointer(psu_b),
            psu_b->lpVtbl->GetBufferSize(psu_b), NULL, &g_ps_up)))
        goto fail;

    ZeroMemory(&sd, sizeof(sd));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(g_dev->lpVtbl->CreateSamplerState(g_dev, &sd, &g_sampler)))
        goto fail;

    ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth = sizeof(BlurCB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_dev->lpVtbl->CreateBuffer(g_dev, &bd, NULL, &g_cb)))
        goto fail;

    vs_b->lpVtbl->Release(vs_b);
    psd_b->lpVtbl->Release(psd_b);
    psh_b->lpVtbl->Release(psh_b);
    psv_b->lpVtbl->Release(psv_b);
    psu_b->lpVtbl->Release(psu_b);
    return TRUE;

fail:
    if (vs_b)  vs_b->lpVtbl->Release(vs_b);
    if (psd_b) psd_b->lpVtbl->Release(psd_b);
    if (psh_b) psh_b->lpVtbl->Release(psh_b);
    if (psv_b) psv_b->lpVtbl->Release(psv_b);
    if (psu_b) psu_b->lpVtbl->Release(psu_b);
    if (g_cb)      { g_cb->lpVtbl->Release(g_cb);      g_cb = NULL; }
    if (g_sampler) { g_sampler->lpVtbl->Release(g_sampler); g_sampler = NULL; }
    if (g_ps_up)   { g_ps_up->lpVtbl->Release(g_ps_up);   g_ps_up = NULL; }
    if (g_ps_bv)   { g_ps_bv->lpVtbl->Release(g_ps_bv);   g_ps_bv = NULL; }
    if (g_ps_bh)   { g_ps_bh->lpVtbl->Release(g_ps_bh);   g_ps_bh = NULL; }
    if (g_ps_down) { g_ps_down->lpVtbl->Release(g_ps_down); g_ps_down = NULL; }
    if (g_vs)      { g_vs->lpVtbl->Release(g_vs);      g_vs = NULL; }
    return TRUE;
}

BOOL gpu_blur_init(void)
{
    InitOnceExecuteOnce(&g_once, gpu_once, NULL, NULL);
    return g_dev != NULL && g_vs != NULL;
}

void gpu_blur_shutdown(void)
{
    if (!g_dev)
        return;
    EnterCriticalSection(&g_lk);
    cache_free();
    if (g_cb)      { g_cb->lpVtbl->Release(g_cb);      g_cb = NULL; }
    if (g_sampler) { g_sampler->lpVtbl->Release(g_sampler); g_sampler = NULL; }
    if (g_ps_up)   { g_ps_up->lpVtbl->Release(g_ps_up);   g_ps_up = NULL; }
    if (g_ps_bv)   { g_ps_bv->lpVtbl->Release(g_ps_bv);   g_ps_bv = NULL; }
    if (g_ps_bh)   { g_ps_bh->lpVtbl->Release(g_ps_bh);   g_ps_bh = NULL; }
    if (g_ps_down) { g_ps_down->lpVtbl->Release(g_ps_down); g_ps_down = NULL; }
    if (g_vs)      { g_vs->lpVtbl->Release(g_vs);      g_vs = NULL; }
    if (g_ctx)     { g_ctx->lpVtbl->Release(g_ctx);     g_ctx = NULL; }
    if (g_dev)     { g_dev->lpVtbl->Release(g_dev);     g_dev = NULL; }
    LeaveCriticalSection(&g_lk);
}

BOOL gpu_blur2x(const BYTE *src, BYTE *dst, int w, int h, float sigma)
{
    D3D11_BOX box;
    D3D11_MAPPED_SUBRESOURCE mr;
    ID3D11Buffer *cbs[1];
    float sig_h;
    int radius, lim, hw, hh, pass, row;
    HRESULT hr;

    if (!src || !dst || w < 4 || h < 4)
        return FALSE;
    if (!gpu_blur_init())
        return FALSE;

    sig_h = sigma * 0.5f;                       /* blur lives at half res */
    if (sig_h < 0.6f)
        return FALSE;                           /* caller: plain crop */
    radius = (int)lroundf((sqrtf(4.0f * sig_h * sig_h + 1.0f) - 1.0f) * 0.5f);
    if (radius < 1)
        radius = 1;
    hw = (w + 1) / 2;
    hh = (h + 1) / 2;
    lim = (hw < hh ? hw : hh) - 1;
    if (radius > lim)
        radius = lim;

    EnterCriticalSection(&g_lk);
    if (!cache_ensure(w, h))
    {
        LeaveCriticalSection(&g_lk);
        return FALSE;
    }

    /* upload the card rect */
    box.left = 0;
    box.top = 0;
    box.front = 0;
    box.right = w;
    box.bottom = h;
    box.back = 1;
    g_ctx->lpVtbl->UpdateSubresource(g_ctx, (ID3D11Resource *)g_t_src, 0, &box,
                                     src, (UINT)w * 4, 0);

    g_ctx->lpVtbl->VSSetShader(g_ctx, g_vs, NULL, 0);
    g_ctx->lpVtbl->PSSetSamplers(g_ctx, 0, 1, (ID3D11SamplerState *[1])
                                { g_sampler });
    cbs[0] = g_cb;
    g_ctx->lpVtbl->VSSetConstantBuffers(g_ctx, 0, 1, cbs);
    g_ctx->lpVtbl->PSSetConstantBuffers(g_ctx, 0, 1, cbs);
    g_ctx->lpVtbl->IASetInputLayout(g_ctx, NULL);
    g_ctx->lpVtbl->IASetPrimitiveTopology(g_ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    /* 2x2 box downsample */
    draw_ps(g_ps_down, g_srv_src, g_rtv_ha, hw, hh, 0, w, h);

    /* three separable box passes (matches blur_bgra) */
    for (pass = 0; pass < 3; pass++)
    {
        draw_ps(g_ps_bh, g_srv_ha, g_rtv_hb, hw, hh, radius, hw, hh);
        draw_ps(g_ps_bv, g_srv_hb, g_rtv_ha, hw, hh, radius, hw, hh);
    }

    /* bilinear upsample back to full res */
    draw_ps(g_ps_up, g_srv_ha, g_rtv_out, w, h, 0, hw, hh);

    g_ctx->lpVtbl->CopyResource(g_ctx, (ID3D11Resource *)g_t_stg,
                                (ID3D11Resource *)g_t_out);
    hr = g_ctx->lpVtbl->Map(g_ctx, (ID3D11Resource *)g_t_stg, 0,
                            D3D11_MAP_READ, 0, &mr);
    if (SUCCEEDED(hr))
    {
        for (row = 0; row < h; row++)
            memcpy(dst + (size_t)row * w * 4,
                   (const BYTE *)mr.pData + (size_t)row * mr.RowPitch,
                   (size_t)w * 4);
        g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_t_stg, 0);
    }
    LeaveCriticalSection(&g_lk);
    return SUCCEEDED(hr);
}
