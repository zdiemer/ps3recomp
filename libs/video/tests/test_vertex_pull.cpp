// Execute the shared raw-geometry decoder on a GPU and compare actual readback
// with the existing CPU fetcher. Run WARP for deterministic CI or hardware for
// the driver used by gameplay. No game data is required.
#define NOMINMAX
#include <d3d11.h>
#include <d3dcompiler.h>
#include "../rsx_vertex_pull.h"
#include "../rsx_vertex_formats.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static void checked(HRESULT hr){if(FAILED(hr))throw std::runtime_error("D3D failure "+std::to_string((unsigned)hr));}
int main(int argc,char**) try {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    checked(D3D11CreateDevice(nullptr,argc>1?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,
        nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    std::string source=rsx_vertex_pull_hlsl;
    source+="RWStructuredBuffer<float4> result:register(u0);[numthreads(64,1,1)]void main(uint3 id:SV_DispatchThreadID){if(id.x<128)for(uint a=0;a<8;++a)result[id.x*8+a]=rsx_pull(id.x,a);}";
    ComPtr<ID3DBlob> code,error;
    HRESULT hr=D3DCompile(source.data(),source.size(),"vertex-pull",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
    if(error)std::cerr.write((char*)error->GetBufferPointer(),error->GetBufferSize());checked(hr);
    ComPtr<ID3D11ComputeShader> shader;checked(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    D3D11_BUFFER_DESC d={};d.ByteWidth=128*8*16;d.Usage=D3D11_USAGE_DEFAULT;
    d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=16;
    ComPtr<ID3D11Buffer> output;checked(device->CreateBuffer(&d,nullptr,&output));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud={};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=128*8;
    ComPtr<ID3D11UnorderedAccessView> uav;checked(device->CreateUnorderedAccessView(output.Get(),&ud,&uav));
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.MiscFlags=0;d.StructureByteStride=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> readback;checked(device->CreateBuffer(&d,nullptr,&readback));
    rsx_vertex_pull_payload packet={};unsigned comparisons=0;
    for(unsigned size=1;size<=4;++size)for(unsigned alignment=0;alignment<4;++alignment)
    for(unsigned variant=0;variant<3;++variant){
        std::vector<u8> bytes(128*160+32);u32 random=0x31415926u+size+alignment+variant;
        for(auto& b:bytes){random^=random<<13;random^=random>>17;random^=random<<5;b=(u8)random;}
        // Include half zeros, subnormals, infinities and NaNs in both signs.
        const u16 halves[]={0,0x8000,1,0x3ff,0x7c00,0xfc00,0x7e01,0xfe01};
        for(u32 i=0;i<8;++i){bytes[i*160+36+alignment]=(u8)(halves[i]>>8);bytes[i*160+37+alignment]=(u8)halves[i];}
        rsx_vertex_fetch_plan plan={};rsx_vertex_layout_plan_init(&plan.layout,255);
        rsx_vertex_ref refs[128];
        const u32 first=variant==1?37:0;
        for(u32 i=0;i<128;++i){const u32 index=first+(i*37)%128;refs[i].base_index=variant?0xFFFF0u:0;refs[i].vertex_id=(index-refs[i].base_index)&0xFFFFFu;}
        for(u32 attr=0;attr<8;++attr){
            auto& f=plan.attr[attr];f.default_value[0]=.25f;f.default_value[1]=-2;f.default_value[2]=3;f.default_value[3]=1;
            if(attr==7 || (variant==2 && attr==3))continue;
            f.desc.type=attr+1;f.desc.size=size;f.elem_size=rsx_vertex_attrib_size(f.desc.type,size);
            f.stride=variant==2?0:160;f.stable_base=bytes.data()+attr*18+alignment;f.stable_first=first;f.stable_last=first+127;
        }
        require(rsx_vertex_pull_pack(&packet,&plan,refs,128,1<<20),"pack eligible raw geometry");
        // Snapshot ownership: modifying the producer after upload must not
        // modify packet bytes. CPU reference remains the original producer.
        const auto copy=std::vector<u8>(packet.data,packet.data+packet.bytes);
        bytes[alignment]^=0xFF;require(!memcmp(copy.data(),packet.data,packet.bytes),"immutable snapshot");
        bytes[alignment]^=0xFF;
        d={};d.ByteWidth=packet.bytes;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SUBRESOURCE_DATA initial={};initial.pSysMem=packet.data;
        ComPtr<ID3D11Buffer> input;checked(device->CreateBuffer(&d,&initial,&input));
        D3D11_SHADER_RESOURCE_VIEW_DESC sd={};sd.Format=DXGI_FORMAT_R32_TYPELESS;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;sd.BufferEx.NumElements=packet.bytes/4;sd.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        ComPtr<ID3D11ShaderResourceView> srv;checked(device->CreateShaderResourceView(input.Get(),&sd,&srv));
        ID3D11ShaderResourceView* srvs[]={srv.Get()};ID3D11UnorderedAccessView* uavs[]={uav.Get()};
        context->CSSetShader(shader.Get(),nullptr,0);context->CSSetShaderResources(20,1,srvs);context->CSSetUnorderedAccessViews(0,1,uavs,nullptr);context->Dispatch(2,1,1);
        context->CopyResource(readback.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped={};checked(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        bool pass=true;
        for(u32 i=0;i<128;++i){float expected[32];require(rsx_vertex_fetch_one(&plan,&refs[i],(u8*)expected)!=0,"CPU reference fetch");
            for(u32 lane=0;lane<32;++lane){float actual=((float*)mapped.pData)[i*32+lane],value=expected[lane];
                const u32 type=plan.attr[lane/4].desc.type;
                const bool normalized=type==1 || type==4 || type==6;
                const bool equal=!memcmp(&actual,&value,4) || (std::isnan(actual)&&std::isnan(value)) ||
                    (normalized&&std::isfinite(actual)&&std::isfinite(value)&&std::abs(actual-value)<=std::max(1.f,std::abs(value))*0.000001f);
                if(!equal&&pass)std::cerr<<"size="<<size<<" alignment="<<alignment<<" variant="<<variant<<" vertex="<<i<<" lane="<<lane<<" actual="<<actual<<" expected="<<value<<"\n";
                pass&=equal;++comparisons;
            }}
        context->Unmap(readback.Get(),0);require(pass,"GPU raw vertex decode mismatch");
        auto unsupported=plan;unsupported.attr[0].desc.frequency=2;
        require(!rsx_vertex_pull_pack(&packet,&unsupported,refs,128,1<<20),"frequency fallback");
        unsupported=plan;unsupported.attr[0].stable_base=nullptr;
        require(!rsx_vertex_pull_pack(&packet,&unsupported,refs,128,1<<20),"unmapped fallback");
        unsupported=plan;unsupported.inline_data=bytes.data();
        require(!rsx_vertex_pull_pack(&packet,&unsupported,refs,128,1<<20),"inline fallback");
        require(!rsx_vertex_pull_pack(&packet,&plan,refs,128,100),"upload capacity fallback");
    }
    char vs[8192]="struct VSInput{float4 a0:ATTR0;float4 a3:ATTR3;};struct VSOutput{float4 pos:SV_Position;};VSOutput main(VSInput input) {VSOutput o;o.pos=input.a0+input.a3;return o;}";
    require(rsx_vertex_pull_wrap(vs,sizeof vs,9),"wrap real vertex entry");
    code.Reset();error.Reset();hr=D3DCompile(vs,strlen(vs),"pull-vs",nullptr,nullptr,"main","vs_5_0",0,0,&code,&error);
    if(error)std::cerr.write((char*)error->GetBufferPointer(),error->GetBufferSize());checked(hr);
    free(packet.data);std::cout<<"PASS: "<<comparisons<<" GPU vertex lanes, formats 1..7, sizes 1..4, byte alignment, index wrap, defaults, snapshots, fallbacks and VS compilation\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
