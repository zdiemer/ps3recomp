// Execute the shared raw-geometry decoder on a GPU and compare actual readback
// with the existing CPU fetcher: CS/O3 plus the real wrapped VS/flags0 through
// stream output. Run WARP for deterministic CI or hardware for the driver used
// by gameplay. This does not measure full-game vertex shader or hardware cost.
// No game data is required.
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

    // Compile the same wrapper and flags used by production vertex shaders.
    // A passthrough geometry shader makes every decoded VS component available
    // to stream output without rasterization or interpolation changing it.
    std::string fields="float4 pos:SV_Position;", inputs;
    for(unsigned attr=0;attr<8;++attr){
        fields+="float4 a"+std::to_string(attr)+":TEXCOORD"+std::to_string(attr)+";";
        inputs+="float4 a"+std::to_string(attr)+":ATTR"+std::to_string(attr)+";";
    }
    const std::string output_type="struct VSOutput{"+fields+"};";
    std::string vertex_source="struct VSInput{"+inputs+"};"+output_type+
        "VSOutput main(VSInput input) {VSOutput o;o.pos=float4(0,0,0,1);";
    for(unsigned attr=0;attr<8;++attr)
        vertex_source+="o.a"+std::to_string(attr)+"=input.a"+std::to_string(attr)+";";
    vertex_source+="return o;}";
    std::vector<char> wrapped(16384);
    memcpy(wrapped.data(),vertex_source.c_str(),vertex_source.size()+1);
    require(rsx_vertex_pull_wrap(wrapped.data(),(u32)wrapped.size(),255),"wrap executed vertex entry");
    ComPtr<ID3DBlob> vertex_code,geometry_code;
    error.Reset();
    hr=D3DCompile(wrapped.data(),strlen(wrapped.data()),"pull-vs-execute",nullptr,nullptr,
        "main","vs_5_0",0,0,&vertex_code,&error);
    if(error)std::cerr.write((char*)error->GetBufferPointer(),error->GetBufferSize());checked(hr);
    ComPtr<ID3D11VertexShader> vertex_shader;
    checked(device->CreateVertexShader(vertex_code->GetBufferPointer(),vertex_code->GetBufferSize(),nullptr,&vertex_shader));
    const std::string geometry_source=output_type+
        "[maxvertexcount(1)]void main(point VSOutput input[1],inout PointStream<VSOutput> output){output.Append(input[0]);}";
    error.Reset();
    hr=D3DCompile(geometry_source.data(),geometry_source.size(),"pull-vs-stream",nullptr,nullptr,
        "main","gs_5_0",0,0,&geometry_code,&error);
    if(error)std::cerr.write((char*)error->GetBufferPointer(),error->GetBufferSize());checked(hr);
    D3D11_SO_DECLARATION_ENTRY declaration[8]={};
    for(unsigned attr=0;attr<8;++attr){
        declaration[attr].SemanticName="TEXCOORD";
        declaration[attr].SemanticIndex=attr;
        declaration[attr].ComponentCount=4;
    }
    const UINT stream_stride=8*16;
    ComPtr<ID3D11GeometryShader> geometry_shader;
    checked(device->CreateGeometryShaderWithStreamOutput(geometry_code->GetBufferPointer(),
        geometry_code->GetBufferSize(),declaration,8,&stream_stride,1,
        D3D11_SO_NO_RASTERIZED_STREAM,nullptr,&geometry_shader));
    d={};d.ByteWidth=128*stream_stride;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> vertex_output;checked(device->CreateBuffer(&d,nullptr,&vertex_output));
    rsx_vertex_pull_payload packet={};unsigned comparisons=0,vertex_comparisons=0;
    for(unsigned size=1;size<=4;++size)for(unsigned alignment=0;alignment<4;++alignment)
    for(unsigned variant=0;variant<4;++variant){
        std::vector<u8> bytes(128*160+32);u32 random=0x31415926u+size+alignment+variant;
        for(auto& b:bytes){random^=random<<13;random^=random>>17;random^=random<<5;b=(u8)random;}
        // Put edge values in every component, exercising each scalar store
        // and the i<n guard for all sizes. Preserve the CPU's half-subnormal
        // policy; NaN payload identity is deliberately not required below.
        const u16 halves[]={0,0x8000,1,0x8001,0x3ff,0x83ff,0x400,0x8400,
            0x7bff,0xfbff,0x3c00,0xbc00,0x7c00,0xfc00,0x7e01,0xfe01};
        const u32 floats[]={0,0x80000000u,0x00800000u,0x7f7fffffu,
            0x7f800000u,0xff800000u,0x7fc00001u,0xffc00001u};
        const u16 shorts[]={0x8000,0x8001,0xffff,0,1,0x7ffe,0x7fff,0x1234};
        const u8 octets[]={0,1,127,128,254,255,17,239};
        for(u32 i=0;i<16;++i)for(u32 component=0;component<4;++component){
            const u32 word=floats[(i+component)%8];
            for(u32 b=0;b<4;++b)bytes[i*160+18+alignment+component*4+b]=(u8)(word>>(24-b*8));
            const u16 half=halves[(i+component)%16],integer=shorts[(i+component)%8];
            bytes[i*160+36+alignment+component*2]=(u8)(half>>8);
            bytes[i*160+37+alignment+component*2]=(u8)half;
            for(u32 attr: {0u,4u}){
                bytes[i*160+attr*18+alignment+component*2]=(u8)(integer>>8);
                bytes[i*160+attr*18+alignment+component*2+1]=(u8)integer;
            }
            for(u32 attr: {3u,6u})bytes[i*160+attr*18+alignment+component]=octets[(i+component)%8];
        }
        rsx_vertex_fetch_plan plan={};rsx_vertex_layout_plan_init(&plan.layout,255);
        rsx_vertex_ref refs[128];
        const u32 first=variant==1?37:0;
        // The occurrence path must decode repeated source vertices identically,
        // including a nonzero base and wrapped effective index arithmetic.
        for(u32 i=0;i<128;++i){const u32 index=first+(variant==3?(i%17)*7:(i*37)%128);refs[i].base_index=variant?0xFFFF0u:0;refs[i].vertex_id=(index-refs[i].base_index)&0xFFFFFu;}
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
        const auto compare=[&](const float* values,const char* stage,unsigned& count){
            bool pass=true;
            for(u32 i=0;i<128;++i){float expected[32];require(rsx_vertex_fetch_one(&plan,&refs[i],(u8*)expected)!=0,"CPU reference fetch");
                for(u32 lane=0;lane<32;++lane){float actual=values[i*32+lane],value=expected[lane];
                    const u32 type=plan.attr[lane/4].desc.type;
                    const bool normalized=type==1 || type==4 || type==6;
                    const bool equal=!memcmp(&actual,&value,4) || (std::isnan(actual)&&std::isnan(value)) ||
                        (normalized&&std::isfinite(actual)&&std::isfinite(value)&&std::abs(actual-value)<=std::max(1.f,std::abs(value))*0.000001f);
                    if(!equal&&pass)std::cerr<<stage<<" size="<<size<<" alignment="<<alignment<<" variant="<<variant<<" vertex="<<i<<" lane="<<lane<<" actual="<<actual<<" expected="<<value<<"\n";
                    pass&=equal;++count;
                }}
            return pass;
        };
        bool pass=compare((const float*)mapped.pData,"CS/O3",comparisons);
        context->Unmap(readback.Get(),0);require(pass,"CS raw vertex decode mismatch");

        ID3D11Buffer* stream_targets[]={vertex_output.Get()};const UINT stream_offset=0;
        context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        context->VSSetShader(vertex_shader.Get(),nullptr,0);context->VSSetShaderResources(20,1,srvs);
        context->GSSetShader(geometry_shader.Get(),nullptr,0);
        context->SOSetTargets(1,stream_targets,&stream_offset);context->Draw(128,0);
        context->SOSetTargets(0,nullptr,nullptr);context->GSSetShader(nullptr,nullptr,0);
        context->CopyResource(readback.Get(),vertex_output.Get());
        checked(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        pass=compare((const float*)mapped.pData,"wrapped VS/flags0",vertex_comparisons);
        context->Unmap(readback.Get(),0);require(pass,"wrapped VS raw vertex decode mismatch");
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
    free(packet.data);std::cout<<"PASS: "<<comparisons<<" CS/O3 lanes and "<<vertex_comparisons<<" wrapped VS/flags0 stream-output lanes, formats 1..7, sizes 1..4, byte alignment, index wrap, defaults, snapshots, fallbacks and VS compilation\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
