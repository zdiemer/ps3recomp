// Prove exported r1.z participates in real pixel depth tests and writes.
#include "../rsx_fp_decompiler.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static void checked(HRESULT h){if(FAILED(h))throw std::runtime_error("D3D failure: "+std::to_string((unsigned)h));}
static void word(u8* p,u32 w){w=(w<<16)|(w>>16);for(int i=0;i<4;++i)p[i]=(u8)(w>>(24-8*i));}
static void mov(u8* p,u32 reg,u32 half,u32 mask,u32 input,bool end){
    word(p,(1u<<24)|(reg<<1)|(half<<7)|(mask<<9)|(input<<13)|(u32)end);
    word(p+4,1u|(1u<<11)|(2u<<13)|(3u<<15)|(7u<<18)|(1u<<23)|(2u<<25)|(3u<<27));word(p+8,0);word(p+12,0);
}
static ComPtr<ID3DBlob> compile(const std::string& s,const char* profile){
    ComPtr<ID3DBlob> code,error;HRESULT hr=D3DCompile(s.data(),s.size(),"depth export regression",nullptr,nullptr,"main",profile,D3DCOMPILE_IEEE_STRICTNESS,0,&code,&error);
    if(error)std::cerr.write((char*)error->GetBufferPointer(),error->GetBufferSize());checked(hr);return code;
}
int main()try{
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;
    checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx));
    // Match the decompiler's complete stage signature and register order.
    const char* vs="cbuffer Probe:register(b0){float4 data;}struct VOut{float4 pos:SV_POSITION;float4 col0:COLOR0;float4 col1:COLOR1;float4 fog:FOG;float4 tc0:TEXCOORD0;float4 tc1:TEXCOORD1;float4 tc2:TEXCOORD2;float4 tc3:TEXCOORD3;float4 tc4:TEXCOORD4;float4 tc5:TEXCOORD5;float4 tc6:TEXCOORD6;float4 tc7:TEXCOORD7;};VOut main(uint id:SV_VertexID){VOut o=(VOut)0;o.pos=float4(id==2?3:-1,id==1?3:-1,.5,1);o.col0=float4(1,0,0,.25);o.col1=float4(0,1,.9,1);o.tc0=data;return o;}";
    auto vc=compile(vs,"vs_5_0");ComPtr<ID3D11VertexShader> vertex;checked(dev->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vertex));ctx->VSSetShader(vertex.Get(),nullptr,0);
    D3D11_BUFFER_DESC bd={};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;ComPtr<ID3D11Buffer> cb;checked(dev->CreateBuffer(&bd,nullptr,&cb));ID3D11Buffer* c=cb.Get();ctx->VSSetConstantBuffers(0,1,&c);
    const float alpha_data[8]={0,0,0,0,.5f,0,0,0};bd.ByteWidth=sizeof(alpha_data);D3D11_SUBRESOURCE_DATA alpha_init={};alpha_init.pSysMem=alpha_data;
    ComPtr<ID3D11Buffer> alpha_cb;checked(dev->CreateBuffer(&bd,&alpha_init,&alpha_cb));c=alpha_cb.Get();ctx->PSSetConstantBuffers(1,1,&c);
    D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=4;td.ArraySize=td.MipLevels=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;checked(dev->CreateTexture2D(&td,nullptr,&depth));D3D11_DEPTH_STENCIL_VIEW_DESC dd={};dd.Format=DXGI_FORMAT_D32_FLOAT;dd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;checked(dev->CreateDepthStencilView(depth.Get(),&dd,&dsv));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> depth_read;checked(dev->CreateTexture2D(&td,nullptr,&depth_read));
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color[2];ComPtr<ID3D11RenderTargetView> rtv[2];for(int i=0;i<2;++i){checked(dev->CreateTexture2D(&td,nullptr,&color[i]));checked(dev->CreateRenderTargetView(color[i].Get(),nullptr,&rtv[i]));}
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> color_read;checked(dev->CreateTexture2D(&td,nullptr,&color_read));
    D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;checked(dev->CreateRasterizerState(&rd,&raster));ctx->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport={0,0,4,4,0,1};ctx->RSSetViewports(1,&viewport);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    unsigned executions=0;
    for(u32 exports:{0u,2u,4u,8u,14u})for(u32 half:{0u,1u})for(u32 mrt:{0u,1u})for(u32 buffered:{0u,1u})for(u32 alpha:{0u,1u}){
        u8 program[64]={};unsigned bytes=0;
        mov(program+bytes,0,half,15,1,false);bytes+=16;
        mov(program+bytes,1,1,4,2,false);bytes+=16; // Poison h1.z; depth always comes from fp32 r1.z.
        if(mrt){mov(program+bytes,half?4:2,half,15,2,false);bytes+=16;}
        mov(program+bytes,1,0,4,4,true);bytes+=16;
        char hlsl[32768];const u32 ctrl=(half?0u:0x40u)|exports;
        check(rsx_fp_decompile_shadow_ex(program,bytes,ctrl,0,0,buffered,hlsl,sizeof(hlsl),nullptr)>0,"depth decompile");
        check((strstr(hlsl,"SV_Depth")!=nullptr)==(exports!=0),"depth declaration");
        if(alpha)check((buffered?rsx_fp_apply_alpha_test_buffered(hlsl,sizeof(hlsl),0x204):rsx_fp_apply_alpha_test(hlsl,sizeof(hlsl),0x204,.5f))==1,"alpha patch");
        auto pc=compile(hlsl,"ps_5_0");ComPtr<ID3D11PixelShader> pixel;checked(dev->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&pixel));ctx->PSSetShader(pixel.Get(),nullptr,0);
        ID3D11RenderTargetView* targets[]={rtv[0].Get(),rtv[1].Get()};ctx->OMSetRenderTargets(mrt?2:1,targets,dsv.Get());
        for(float value:{.25f,.75f})for(u32 greater:{0u,1u})for(u32 write:{0u,1u}){
            const float payload[]={0,0,value,1};ctx->UpdateSubresource(cb.Get(),0,nullptr,payload,0,0);
            const float initial=greater?.4f:.6f,black[4]={0,0,0,0};ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,initial,0);
            ctx->ClearRenderTargetView(rtv[0].Get(),black);ctx->ClearRenderTargetView(rtv[1].Get(),black);
            D3D11_DEPTH_STENCIL_DESC sd={};sd.DepthEnable=TRUE;sd.DepthWriteMask=write?D3D11_DEPTH_WRITE_MASK_ALL:D3D11_DEPTH_WRITE_MASK_ZERO;sd.DepthFunc=greater?D3D11_COMPARISON_GREATER:D3D11_COMPARISON_LESS;
            ComPtr<ID3D11DepthStencilState> state;checked(dev->CreateDepthStencilState(&sd,&state));ctx->OMSetDepthStencilState(state.Get(),0);ctx->Draw(3,0);
            const float final_depth=exports?value:.5f;const bool passes=!alpha&&(greater?final_depth>initial:final_depth<initial);
            ctx->CopyResource(depth_read.Get(),depth.Get());D3D11_MAPPED_SUBRESOURCE m={};checked(ctx->Map(depth_read.Get(),0,D3D11_MAP_READ,0,&m));
            const float actual=((const float*)((const char*)m.pData+2*m.RowPitch))[2],expected=passes&&write?final_depth:initial;
            ctx->Unmap(depth_read.Get(),0);if(std::abs(actual-expected)>.000001f)std::cerr<<"ctrl="<<ctrl<<" mrt="<<mrt<<" buffered="<<buffered<<" alpha="<<alpha<<" depth="<<value<<" actual="<<actual<<" expected="<<expected<<"\n"<<hlsl;check(std::abs(actual-expected)<.000001f,"exported depth readback");
            for(u32 target=0;target<=mrt;++target){ctx->CopyResource(color_read.Get(),color[target].Get());checked(ctx->Map(color_read.Get(),0,D3D11_MAP_READ,0,&m));const float* p=(const float*)((const char*)m.pData+2*m.RowPitch)+8;const bool color_ok=p[target]==(passes?1.f:0.f);ctx->Unmap(color_read.Get(),0);check(color_ok,"depth/alpha color visibility");}
            ++executions;
        }
    }
    std::cout<<"PASS: "<<executions<<" pixel depth GPU readbacks (r1.z independent of raster Z and color file, depth enable bits, MRT, alpha discard, buffered/inline, LESS/GREATER, write mask)\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
