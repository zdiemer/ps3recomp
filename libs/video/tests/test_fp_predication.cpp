// Execute decompiled NV40 predicated MOVs, including inactive nonfinite inputs.
#include "../rsx_fp_decompiler.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
static void checked(HRESULT h) { if (FAILED(h)) throw std::runtime_error("D3D failure: " + std::to_string((unsigned)h)); }
static void word(u8* p, u32 w) { w=(w<<16)|(w>>16);for(int i=0;i<4;++i)p[i]=(u8)(w>>(24-8*i)); }
int main() try {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    checked(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    const u32 swz=(1u<<11)|(2u<<13)|(3u<<15), cswz=(1u<<23)|(2u<<25)|(3u<<27), always=(7u<<18)|cswz;
    const float nan=std::numeric_limits<float>::quiet_NaN(), inf=std::numeric_limits<float>::infinity();
    const float payload[]={.25f,nan,.75f,nan, -1,1,0,1, nan,3,inf,5};
    const float expected[]={.25f,3,.75f,5};
    for (unsigned flags : {0u, (unsigned)D3DCOMPILE_IEEE_STRICTNESS})
    for (unsigned half=0;half<2;++half) {
        u8 program[48]={}; const u32 out=(0xFu<<9)|(half<<7);
        word(program, (1u<<24)|out|(1u<<13)); word(program+4, 1|swz|always);
        word(program+16,(1u<<24)|(1u<<30)|(1u<<8)|(0xFu<<9)|(2u<<13));word(program+20,1|swz|always);
        word(program+32,(1u<<24)|out|(4u<<13)|1);word(program+36,1|swz|(1u<<20)|cswz);
        char hlsl[16384]; if(rsx_fp_decompile(program,sizeof program,half?0:0x40,hlsl,sizeof hlsl)!=3)throw std::runtime_error("decompile");
        std::string source=hlsl;const auto entry=source.find("float4 main(");if(entry==std::string::npos)throw std::runtime_error("entry");
        source.replace(entry,12,"float4 run_fp(");
        source+="\ncbuffer Probe:register(b0){float4 initial;float4 conditions;float4 incoming;}\nRWStructuredBuffer<float4> result:register(u0);\n[numthreads(1,1,1)]void main(){PSInput p=(PSInput)0;p.col0=initial;p.col1=conditions;p.tc0=incoming;result[0]=run_fp(p);}\n";
        ComPtr<ID3DBlob> code,error;HRESULT hr=D3DCompile(source.data(),source.size(),"predication",nullptr,nullptr,"main","cs_5_0",flags,0,&code,&error);
        if(error)std::cerr.write((char*)error->GetBufferPointer(),error->GetBufferSize());checked(hr);
        ComPtr<ID3D11ComputeShader> shader;checked(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
        D3D11_BUFFER_DESC d={};d.ByteWidth=sizeof payload;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA init={};init.pSysMem=payload;ComPtr<ID3D11Buffer> cb;checked(device->CreateBuffer(&d,&init,&cb));
        d.ByteWidth=16;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.StructureByteStride=16;
        ComPtr<ID3D11Buffer> output;checked(device->CreateBuffer(&d,nullptr,&output));
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud={};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=1;
        ComPtr<ID3D11UnorderedAccessView> uav;checked(device->CreateUnorderedAccessView(output.Get(),&ud,&uav));
        ID3D11Buffer* cbs[]={cb.Get()};ID3D11UnorderedAccessView* uavs[]={uav.Get()};context->CSSetShader(shader.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,cbs);context->CSSetUnorderedAccessViews(0,1,uavs,nullptr);context->Dispatch(1,1,1);
        d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.MiscFlags=0;d.StructureByteStride=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> readback;checked(device->CreateBuffer(&d,nullptr,&readback));context->CopyResource(readback.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE m={};checked(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&m));
        bool passed=true;for(int i=0;i<4;++i){const float v=((float*)m.pData)[i];if(v!=expected[i]){std::cerr<<"half="<<half<<" lane="<<i<<" actual="<<v<<" expected="<<expected[i]<<"\n";passed=false;}}
        context->Unmap(readback.Get(),0);if(!passed)return 1;
    }
    std::cout<<"PASS: fp32/fp16 predication preserves inactive values and replaces active NaNs\n";return 0;
} catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
