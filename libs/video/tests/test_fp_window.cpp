// Read actual rasterized WPOS values, including offset viewports and non-unit W.
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
static void checked(HRESULT hr){check(SUCCEEDED(hr),"D3D failure");}
static void word(u8* p,u32 w){w=(w<<16)|(w>>16);for(int i=0;i<4;++i)p[i]=(u8)(w>>(24-8*i));}
static void mov(u8* p,u32 reg,u32 input,bool end){
    word(p,(1u<<24)|(reg<<1)|(15u<<9)|(input<<13)|(u32)end);
    word(p+4,1u|(1u<<11)|(2u<<13)|(3u<<15)|(7u<<18)|(1u<<23)|(2u<<25)|(3u<<27));word(p+8,0);word(p+12,0);
}
static ComPtr<ID3DBlob> compile(const std::string& s,const char* profile){
    ComPtr<ID3DBlob> code,error;HRESULT hr=D3DCompile(s.data(),s.size(),"WPOS regression",nullptr,nullptr,"main",profile,D3DCOMPILE_IEEE_STRICTNESS,0,&code,&error);
    if(error)std::cerr.write((char*)error->GetBufferPointer(),error->GetBufferSize());checked(hr);return code;
}
int main()try{
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;
    checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx));
    const char* vs="struct VOut{float4 pos:SV_POSITION;float4 col0:COLOR0;float4 col1:COLOR1;float4 fog:FOG;float4 tc0:TEXCOORD0;float4 tc1:TEXCOORD1;float4 tc2:TEXCOORD2;float4 tc3:TEXCOORD3;float4 tc4:TEXCOORD4;float4 tc5:TEXCOORD5;float4 tc6:TEXCOORD6;float4 tc7:TEXCOORD7;};VOut main(uint id:SV_VertexID){VOut o=(VOut)0;o.pos=float4(id==2?6:-2,id==1?6:-2,.5,2);o.col0=float4(1,0,0,1);return o;}";
    auto vc=compile(vs,"vs_5_0");ComPtr<ID3D11VertexShader> vertex;checked(dev->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vertex));ctx->VSSetShader(vertex.Get(),nullptr,0);
    D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=8;td.ArraySize=td.MipLevels=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color[2];ComPtr<ID3D11RenderTargetView> rtv[2];for(int i=0;i<2;++i){checked(dev->CreateTexture2D(&td,nullptr,&color[i]));checked(dev->CreateRenderTargetView(color[i].Get(),nullptr,&rtv[i]));}
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> readback;checked(dev->CreateTexture2D(&td,nullptr,&readback));
    D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;ComPtr<ID3D11RasterizerState> raster;checked(dev->CreateRasterizerState(&rd,&raster));ctx->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport={1,2,4,4,0,1};ctx->RSSetViewports(1,&viewport);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    float alpha_payload[4]={.25f,0,0,0};D3D11_BUFFER_DESC bd={};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA init={};init.pSysMem=alpha_payload;
    ComPtr<ID3D11Buffer> alpha_cb;checked(dev->CreateBuffer(&bd,&init,&alpha_cb));ID3D11Buffer* cb=alpha_cb.Get();ctx->PSSetConstantBuffers(1,1,&cb);
    unsigned pixels=0;
    for(u32 reciprocal:{0u,1u})for(u32 origin:{0u,1u})for(u32 center:{0u,1u})for(u32 height:{0u,8u,576u})for(u32 buffered:{0u,1u})for(u32 alpha:{0u,1u})for(u32 mrt:{0u,1u}){
        u8 p[32]={};mov(p,0,0,!mrt);if(mrt)mov(p+16,2,1,true);
        char hlsl[32768];check(rsx_fp_decompile_shadow_ex(p,mrt?32:16,0x40,0,0,buffered,hlsl,sizeof(hlsl),nullptr)>0,"decompile");
        const u32 window=height|(origin<<12)|(center<<16);
        check(rsx_fp_apply_window_position_ex(hlsl,sizeof(hlsl),window,reciprocal)==(origin||center||reciprocal?1:0),"WPOS patch");
        if(alpha)check((buffered?rsx_fp_apply_alpha_test_buffered(hlsl,sizeof(hlsl),0x206):rsx_fp_apply_alpha_test(hlsl,sizeof(hlsl),0x206,.25f))==1,"alpha patch");
        auto pc=compile(hlsl,"ps_5_0");ComPtr<ID3D11PixelShader> pixel;checked(dev->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&pixel));ctx->PSSetShader(pixel.Get(),nullptr,0);
        ID3D11RenderTargetView* targets[]={rtv[0].Get(),rtv[1].Get()};ctx->OMSetRenderTargets(mrt?2:1,targets,nullptr);ctx->Draw(3,0);ctx->CopyResource(readback.Get(),color[0].Get());
        D3D11_MAPPED_SUBRESOURCE mapped={};checked(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        bool pass=true;for(u32 y=2;y<6;++y)for(u32 x=1;x<5;++x){
            const float* actual=(const float*)((const char*)mapped.pData+y*mapped.RowPitch)+x*4;
            const float bias=center?.5f:0.f;
            // SV_POSITION.w is interpolated clip W on Direct3D, not GLSL's
            // reciprocal W. Test both preserving host W and converting to RSX W.
            const float expected[]={x+.5f-bias,(origin?height-(y+.5f):y+.5f)-bias,.25f,reciprocal?.5f:2.f};
            for(unsigned c=0;c<4;++c)if(std::abs(actual[c]-expected[c])>=.000001f){
                if(pass)std::cerr<<"window="<<std::hex<<window<<std::dec<<" buffered="<<buffered<<" alpha="<<alpha<<" mrt="<<mrt<<" xy="<<x<<","<<y<<" lane="<<c<<" actual="<<actual[c]<<" expected="<<expected[c]<<"\n";
                pass=false;
            }
            ++pixels;
        }
        ctx->Unmap(readback.Get(),0);check(pass,"GPU WPOS coordinates/origin/center/Z/W mismatch");
    }
    u8 p[16]={};mov(p,0,1,true);char hlsl[32768],original[32768];
    check(rsx_fp_decompile(p,16,0x40,hlsl,sizeof(hlsl))>0,"no-WPOS decompile");strcpy(original,hlsl);
    check(rsx_fp_apply_window_position(hlsl,sizeof(hlsl),0x1240)==0&&!strcmp(hlsl,original),"unused WPOS changed");
    check(rsx_fp_apply_window_position(hlsl,sizeof(hlsl),0x2000)<0,"invalid origin accepted");
    check(rsx_fp_apply_window_position(hlsl,sizeof(hlsl),0x20000)<0,"invalid center accepted");
    mov(p,0,0,true);check(rsx_fp_decompile(p,16,0x40,hlsl,sizeof(hlsl))>0,"bounded decompile");strcpy(original,hlsl);
    check(rsx_fp_apply_window_position(hlsl,(u32)strlen(hlsl)+1,0x1240)<0&&!strcmp(hlsl,original),"overflow changed source");
    char unterminated[16];memset(unterminated,'x',sizeof(unterminated));check(rsx_fp_apply_window_position(unterminated,sizeof(unterminated),0x1240)<0,"unterminated source accepted");
    std::cout<<"PASS: "<<pixels<<" WPOS pixel readbacks (top/bottom, half/integer, explicit window height, offset viewport, preserved Z, host/reciprocal W, buffered/inline, alpha, MRT); bounds and unchanged unused input\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
