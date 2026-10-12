// Execute decompiled TEX/TXP shadow programs on a real WARP pixel pipeline.
// A 2x2 depth pattern distinguishes compare-then-filter from filter-then-compare.
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
static void check(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
static void checked(HRESULT h) { if(FAILED(h)) throw std::runtime_error("D3D failure: "+std::to_string((unsigned)h)); }
static void word(u8* p,u32 w) { w=(w<<16)|(w>>16);for(int i=0;i<4;++i)p[i]=(u8)(w>>(24-8*i)); }
static void instruction(u8* p,u32 op,u32 unit,bool end=true) {
    word(p,(op<<24)|(unit<<17)|(4u<<13)|(15u<<9)|(u32)end);
    word(p+4,1u|(1u<<11)|(2u<<13)|(3u<<15)|(7u<<18)|(1u<<23)|(2u<<25)|(3u<<27));
    word(p+8,0);word(p+12,0);
}
static ComPtr<ID3DBlob> compile(const std::string& source,const char* target) {
    ComPtr<ID3DBlob> code,error;
    HRESULT hr=D3DCompile(source.data(),source.size(),"shadow regression",nullptr,nullptr,"main",target,D3DCOMPILE_IEEE_STRICTNESS,0,&code,&error);
    if(error)std::cerr.write((char*)error->GetBufferPointer(),error->GetBufferSize());checked(hr);return code;
}
int main() try {
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;
    checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx));
    const char* vs="float4 main(uint id:SV_VertexID):SV_POSITION{return float4(id==2?3:-1,id==1?3:-1,0,1);}";
    auto vc=compile(vs,"vs_5_0");ComPtr<ID3D11VertexShader> vertex;
    checked(dev->CreateVertexShader(vc->GetBufferPointer(),vc->GetBufferSize(),nullptr,&vertex));
    const float depths[]={.25f,.5f,.75f,1.f};
    D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=2;td.ArraySize=td.MipLevels=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init={};init.pSysMem=depths;init.SysMemPitch=8;
    ComPtr<ID3D11Texture2D> depth;checked(dev->CreateTexture2D(&td,&init,&depth));
    ComPtr<ID3D11ShaderResourceView> srv;checked(dev->CreateShaderResourceView(depth.Get(),nullptr,&srv));
    td.Width=td.Height=4;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color;checked(dev->CreateTexture2D(&td,nullptr,&color));
    ComPtr<ID3D11RenderTargetView> rtv;checked(dev->CreateRenderTargetView(color.Get(),nullptr,&rtv));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;checked(dev->CreateTexture2D(&td,nullptr,&readback));
    D3D11_BUFFER_DESC bd={};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> cb;checked(dev->CreateBuffer(&bd,nullptr,&cb));
    D3D11_VIEWPORT vp={0,0,4,4,0,1};ctx->RSSetViewports(1,&vp);
    D3D11_RASTERIZER_DESC rd={};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;checked(dev->CreateRasterizerState(&rd,&raster));ctx->RSSetState(raster.Get());
    ctx->VSSetShader(vertex.Get(),nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11RenderTargetView* rt=rtv.Get();ctx->OMSetRenderTargets(1,&rt,nullptr);
    ID3D11ShaderResourceView* view=srv.Get();ctx->PSSetShaderResources(0,1,&view);
    ID3D11Buffer* constant=cb.Get();ctx->PSSetConstantBuffers(0,1,&constant);
    unsigned executions=0;
    for(unsigned op:{0x17u,0x18u}) // TEX, TXP
    for(unsigned buffered:{0u,1u})
    for(unsigned unnorm:{0u,1u}) {
        u8 program[16]={};instruction(program,op,0);
        char hlsl[32768];u32 constants=999;
        check(rsx_fp_decompile_shadow_ex(program,16,0x40,0,1,buffered,hlsl,sizeof(hlsl),&constants)==1,"shadow decompile");
        check(constants==0,"constant count");
        if(unnorm) {u32 dims[16][2]={};dims[0][0]=dims[0][1]=2;
            check(rsx_fp_apply_unnorm_scale(hlsl,sizeof(hlsl),1,dims,0)==1,"unnormalized patch");}
        std::string source=hlsl;const auto entry=source.find("float4 main(");check(entry!=std::string::npos,"entry");
        source.replace(entry,12,"float4 run_fp(");
        source+="\ncbuffer Probe:register(b0){float4 coord;}\nfloat4 main(float4 pos:SV_POSITION):SV_TARGET{PSInput p=(PSInput)0;p.position=pos;p.tc0=coord;return run_fp(p);}\n";
        auto pc=compile(source,"ps_5_0");ComPtr<ID3D11PixelShader> pixel;
        checked(dev->CreatePixelShader(pc->GetBufferPointer(),pc->GetBufferSize(),nullptr,&pixel));ctx->PSSetShader(pixel.Get(),nullptr,0);
        for(unsigned linear:{0u,1u})
        for(unsigned func=1;func<=8;++func)
        for(float reference:{.125f,.25f,.625f,1.125f}) {
            D3D11_SAMPLER_DESC sd={};sd.Filter=linear?D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT:D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
            sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.ComparisonFunc=(D3D11_COMPARISON_FUNC)func;sd.MaxLOD=0;
            ComPtr<ID3D11SamplerState> sampler;checked(dev->CreateSamplerState(&sd,&sampler));ID3D11SamplerState* s=sampler.Get();ctx->PSSetSamplers(0,1,&s);
            const float project=op==0x18?2.f:1.f,uv=linear?.5f:.25f;
            const float coords[]={uv*project*(unnorm?2.f:1.f),uv*project*(unnorm?2.f:1.f),reference*project,project};
            ctx->UpdateSubresource(cb.Get(),0,nullptr,coords,0,0);ctx->Draw(3,0);ctx->CopyResource(readback.Get(),color.Get());
            D3D11_MAPPED_SUBRESOURCE m={};checked(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&m));
            float expected=0;for(unsigned i=0;i<(linear?4u:1u);++i) {
                const float d=depths[i];bool pass=false;
                switch(func){case 1:break;case 2:pass=reference<d;break;case 3:pass=reference==d;break;case 4:pass=reference<=d;break;case 5:pass=reference>d;break;case 6:pass=reference!=d;break;case 7:pass=reference>=d;break;case 8:pass=true;break;}
                expected+=(pass?1.f:0.f)/(linear?4.f:1.f);
            }
            const float* actual=(const float*)((const char*)m.pData+2*m.RowPitch)+8;
            bool pass=true;for(unsigned lane=0;lane<4;++lane)pass&=std::abs(actual[lane]-expected)<.00001f;
            if(!pass)std::cerr<<"op="<<op<<" buffered="<<buffered<<" unnorm="<<unnorm<<" linear="<<linear<<" func="<<func<<" ref="<<reference<<" actual="<<actual[0]<<" expected="<<expected<<"\n";
            ctx->Unmap(readback.Get(),0);check(pass,"GPU comparison mismatch");++executions;
        }
    }
    // Shader variants must retain ordinary/cube sampling alongside shadows.
    for(unsigned cube:{0u,2u}) {
        u8 program[32]={};instruction(program,0x17,0,false);instruction(program+16,0x17,1);
        char hlsl[32768];check(rsx_fp_decompile_shadow_ex(program,32,0x40,cube,1,0,hlsl,sizeof(hlsl),nullptr)==2,"mixed decompile");compile(hlsl,"ps_5_0");
    }
    u8 p[16]={};instruction(p,0x17,0);char legacy[32768],modern[32768];
    check(rsx_fp_decompile(p,16,0x40,legacy,sizeof(legacy))==1,"legacy");
    check(rsx_fp_decompile_shadow_ex(p,16,0x40,0,0,0,modern,sizeof(modern),nullptr)==1&&!strcmp(legacy,modern),"legacy output changed");
    check(rsx_fp_decompile_shadow_ex(p,16,0x40,1,1,0,modern,sizeof(modern),nullptr)<0,"cube shadow accepted");
    struct{char output[128];char guard[16];} bounded;memset(&bounded,0x5A,sizeof(bounded));
    check(rsx_fp_decompile_shadow_ex(p,16,0x40,0,1,0,bounded.output,sizeof(bounded.output),nullptr)<0,"short output accepted");
    for(char c:bounded.guard)check(c==0x5A,"output overrun");
    for(unsigned op:{0x31u,0x2Fu}) {instruction(p,op,0);check(rsx_fp_decompile_shadow_ex(p,16,0x40,0,1,0,modern,sizeof(modern),nullptr)<0,"unsupported shadow opcode accepted");}
    std::cout<<"PASS: "<<executions<<" shadow GPU readbacks (TEX/TXP, eight comparisons, point/PCF, projected reference, normalized/texel UVs, buffered/inline); mixed shaders and output bounds\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
