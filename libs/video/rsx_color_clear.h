/* D3D12 NV4097 color clears. ClearRenderTargetView always writes RGBA;
 * individual clear bits require a draw with a channel write mask. */
#ifndef PS3RECOMP_RSX_COLOR_CLEAR_H
#define PS3RECOMP_RSX_COLOR_CLEAR_H
#include <d3d12.h>
#include <d3dcompiler.h>
#include <limits.h>
#include <string.h>

typedef struct rsx_color_clear {
    ID3D12RootSignature* root;
    ID3DBlob* vs;
    ID3DBlob* ps;
    ID3D12PipelineState* pso[2][16];
} rsx_color_clear;

static D3D12_RECT rsx_clear_rect(unsigned width, unsigned height,
                               unsigned horizontal, unsigned vertical, int written)
{
    D3D12_RECT r = {0, 0, (LONG)width, (LONG)height};
    if (written) {
        r.left = (LONG)(horizontal & 0xFFFFu);
        r.top = (LONG)(vertical & 0xFFFFu);
        r.right = r.left + (LONG)(horizontal >> 16);
        r.bottom = r.top + (LONG)(vertical >> 16);
        if (r.left > (LONG)width) r.left = (LONG)width;
        if (r.top > (LONG)height) r.top = (LONG)height;
        if (r.right > (LONG)width) r.right = (LONG)width;
        if (r.bottom > (LONG)height) r.bottom = (LONG)height;
    }
    return r;
}

static void rsx_color_clear_release(rsx_color_clear* c)
{
    for (unsigned f = 0; f < 2; f++)
        for (unsigned m = 0; m < 16; m++)
            if (c->pso[f][m]) c->pso[f][m]->lpVtbl->Release(c->pso[f][m]);
    if (c->root) c->root->lpVtbl->Release(c->root);
    if (c->vs) c->vs->lpVtbl->Release(c->vs);
    if (c->ps) c->ps->lpVtbl->Release(c->ps);
    memset(c, 0, sizeof(*c));
}

static HRESULT rsx_color_clear_init(rsx_color_clear* c, ID3D12Device* dev)
{
    static const char vs[] =
        "float4 main(uint i:SV_VertexID):SV_Position {"
        " float2 p=float2((i<<1)&2,i&2); return float4(p*2-1,0,1); }";
    static const char ps[] =
        "cbuffer ClearColor:register(b0){float4 color;}"
        "float4 main():SV_Target{return color;}";
    HRESULT hr;
    if (!c->vs) {
        hr = D3DCompile(vs, sizeof(vs)-1, "rsx_clear_vs", NULL, NULL,
                        "main", "vs_5_0", 0, 0, &c->vs, NULL);
        if (FAILED(hr)) return hr;
    }
    if (!c->ps) {
        hr = D3DCompile(ps, sizeof(ps)-1, "rsx_clear_ps", NULL, NULL,
                        "main", "ps_5_0", 0, 0, &c->ps, NULL);
        if (FAILED(hr)) return hr;
    }
    if (!c->root) {
        D3D12_ROOT_PARAMETER param = {0};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.Num32BitValues = 4;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc = {0};
        desc.NumParameters = 1; desc.pParameters = &param;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ID3DBlob* blob = NULL;
        hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL);
        if (FAILED(hr)) return hr;
        hr = dev->lpVtbl->CreateRootSignature(dev, 0,
            blob->lpVtbl->GetBufferPointer(blob), blob->lpVtbl->GetBufferSize(blob),
            &IID_ID3D12RootSignature, (void**)&c->root);
        blob->lpVtbl->Release(blob);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

/* The target must be in RENDER_TARGET state. Caller restores draw state. */
static HRESULT rsx_color_clear_record(rsx_color_clear* c, ID3D12Device* dev,
    ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    DXGI_FORMAT format, unsigned width, unsigned height, const D3D12_RECT* rect,
    unsigned channels, const float color[4])
{
    channels &= 15u;
    if (!channels || rect->left >= rect->right || rect->top >= rect->bottom) return S_OK;
    if (channels == 15u) {
        list->lpVtbl->ClearRenderTargetView(list, rtv, color, 1, rect);
        return S_OK;
    }
    HRESULT hr = rsx_color_clear_init(c, dev);
    if (FAILED(hr)) return hr;
    unsigned f = format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!f && format != DXGI_FORMAT_R8G8B8A8_UNORM) return E_INVALIDARG;
    if (!c->pso[f][channels]) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p = {0};
        p.pRootSignature = c->root;
        p.VS.pShaderBytecode = c->vs->lpVtbl->GetBufferPointer(c->vs);
        p.VS.BytecodeLength = c->vs->lpVtbl->GetBufferSize(c->vs);
        p.PS.pShaderBytecode = c->ps->lpVtbl->GetBufferPointer(c->ps);
        p.PS.BytecodeLength = c->ps->lpVtbl->GetBufferSize(c->ps);
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = (UINT8)channels;
        p.SampleMask = UINT_MAX;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1; p.RTVFormats[0] = format; p.SampleDesc.Count = 1;
        hr = dev->lpVtbl->CreateGraphicsPipelineState(dev, &p,
            &IID_ID3D12PipelineState, (void**)&c->pso[f][channels]);
        if (FAILED(hr)) return hr;
    }
    D3D12_VIEWPORT viewport = {0, 0, (FLOAT)width, (FLOAT)height, 0, 1};
    list->lpVtbl->SetPipelineState(list, c->pso[f][channels]);
    list->lpVtbl->SetGraphicsRootSignature(list, c->root);
    list->lpVtbl->SetGraphicsRoot32BitConstants(list, 0, 4, color, 0);
    list->lpVtbl->OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    list->lpVtbl->RSSetViewports(list, 1, &viewport);
    list->lpVtbl->RSSetScissorRects(list, 1, rect);
    list->lpVtbl->IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->lpVtbl->DrawInstanced(list, 3, 1, 0, 0);
    return S_OK;
}
#endif
