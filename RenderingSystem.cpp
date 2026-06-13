// =====================================================================================
// RenderingSystem.cpp
// =====================================================================================

#include "RenderingSystem.h"
#include <d3dcompiler.h>
#include <stdexcept>

inline UINT CalcCBSize(UINT byteSize)
{
    return (byteSize + 255) & ~255;
}

void RenderingSystem::Init(ID3D12Device* device, UINT width, UINT height,
    ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT srvStartSlot,
    DXGI_FORMAT backBufferFormat, DXGI_FORMAT dsvFormat)
{
    mDevice = device;
    mWidth = width;
    mHeight = height;

    mGBuffer.Init(device, width, height, srvHeap, srvDescSize, srvStartSlot);

    CreateGeometryRootSignatureAndPSO(backBufferFormat, dsvFormat);
    CreateLightingRootSignatureAndPSO(backBufferFormat);

    // константный буфер для источников света - один блок на каждый свет
    mLightCBElementSize = CalcCBSize(sizeof(LightPassConstants));
    UINT totalSize = mLightCBElementSize * mMaxLights;

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = totalSize;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mLightCB))))
        throw std::runtime_error("RenderingSystem: CreateCommittedResource LightCB failed");

    if (FAILED(mLightCB->Map(0, nullptr, (void**)&mLightCBData)))
        throw std::runtime_error("RenderingSystem: Map LightCB failed");
}

// =====================================================================================
// Geometry pass root signature и PSO
// Та же самая раскладка (b0 объект, b1 камера, t0 текстура), но PSO пишет в 3 MRT
// =====================================================================================
void RenderingSystem::CreateGeometryRootSignatureAndPSO(DXGI_FORMAT backBufferFormat, DXGI_FORMAT dsvFormat)
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0; // t0

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0; // b0
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1; // b1
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0; // s0
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        throw std::runtime_error("RenderingSystem: D3D12SerializeRootSignature (geometry) failed");
    }
    if (FAILED(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mGeometryRootSignature))))
        throw std::runtime_error("RenderingSystem: CreateRootSignature (geometry) failed");

    // ------ компилируем шейдеры geometry pass ------
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob2;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, nullptr, "GeometryVS", "vs_5_0", compileFlags, 0, &vsBlob, &errorBlob2);
    if (FAILED(hr))
    {
        if (errorBlob2) OutputDebugStringA((char*)errorBlob2->GetBufferPointer());
        throw std::runtime_error("RenderingSystem: Compile GeometryVS failed");
    }

    hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, nullptr, "GeometryPS", "ps_5_0", compileFlags, 0, &psBlob, &errorBlob2);
    if (FAILED(hr))
    {
        if (errorBlob2) OutputDebugStringA((char*)errorBlob2->GetBufferPointer());
        throw std::runtime_error("RenderingSystem: Compile GeometryPS failed");
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mGeometryRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout = { inputLayout, (UINT)(sizeof(inputLayout) / sizeof(inputLayout[0])) };

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // блендинг отключен для всех 3 рендертаргетов (G-Buffer пишется как обычно)
    for (UINT i = 0; i < GBuffer::kNumRenderTargets; ++i)
        psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // -------- MRT: 3 рендертаргета G-буфера (слайд 18) --------
    psoDesc.NumRenderTargets = GBuffer::kNumRenderTargets;
    for (UINT i = 0; i < GBuffer::kNumRenderTargets; ++i)
        psoDesc.RTVFormats[i] = GBuffer::GetFormat(i);

    psoDesc.DSVFormat = dsvFormat;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mGeometryPSO))))
        throw std::runtime_error("RenderingSystem: CreateGraphicsPipelineState (geometry) failed");
}

// =====================================================================================
// Lighting pass root signature и PSO
// b0 - LightPassConstants (камера + один источник света)
// t0..t2 - G-buffer (Albedo, WorldPos, Normal)
// Full-screen triangle/quad, additive blending (слайды 11, 13, 21, 22)
// =====================================================================================
void RenderingSystem::CreateLightingRootSignatureAndPSO(DXGI_FORMAT backBufferFormat)
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = GBuffer::kNumRenderTargets; // t0,t1,t2
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0; // b0
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; // G-buffer читаем через Load, но сэмплер на всякий случай
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // нет input layout - full screen triangle генерируется в VS

    ComPtr<ID3DBlob> serialized, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        throw std::runtime_error("RenderingSystem: D3D12SerializeRootSignature (lighting) failed");
    }
    if (FAILED(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mLightingRootSignature))))
        throw std::runtime_error("RenderingSystem: CreateRootSignature (lighting) failed");

    // ------ компилируем шейдеры lighting pass ------
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob2;
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, nullptr, "LightingVS", "vs_5_0", compileFlags, 0, &vsBlob, &errorBlob2);
    if (FAILED(hr))
    {
        if (errorBlob2) OutputDebugStringA((char*)errorBlob2->GetBufferPointer());
        throw std::runtime_error("RenderingSystem: Compile LightingVS failed");
    }

    hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, nullptr, "LightingPS", "ps_5_0", compileFlags, 0, &psBlob, &errorBlob2);
    if (FAILED(hr))
    {
        if (errorBlob2) OutputDebugStringA((char*)errorBlob2->GetBufferPointer());
        throw std::runtime_error("RenderingSystem: Compile LightingPS failed");
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mLightingRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout = { nullptr, 0 }; // вершин нет, всё генерится в VS по SV_VertexID

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // full-screen quad, культинг не нужен
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // -------- Additive blending - накопление освещения в бэк-буфере (слайды 11, 13, 21) --------
    D3D12_RENDER_TARGET_BLEND_DESC blend = {};
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ONE;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.BlendState.RenderTarget[0] = blend;

    // -------- depth test выключен на запись, отключаем буфер глубины целиком (слайд 11/15) --------
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = backBufferFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN; // depth не используется в lighting pass
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mLightingPSO))))
        throw std::runtime_error("RenderingSystem: CreateGraphicsPipelineState (lighting) failed");
}

// =====================================================================================
// Geometry pass - рендерим всю сцену в G-buffer (без освещения)
// =====================================================================================
void RenderingSystem::BeginGeometryPass(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    // G-buffer переводим из "читаемого шейдером" в "render target"
    mGBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[GBuffer::kNumRenderTargets];
    for (UINT i = 0; i < GBuffer::kNumRenderTargets; ++i)
        rtvHandles[i] = mGBuffer.GetRtvHandle(i);

    mGBuffer.Clear(cmdList);
    cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    cmdList->OMSetRenderTargets(GBuffer::kNumRenderTargets, rtvHandles, FALSE, &dsvHandle);
    cmdList->SetPipelineState(mGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSignature.Get());
}

void RenderingSystem::EndGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    // G-buffer переводим обратно в "читаемый шейдером" - для lighting pass
    mGBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// =====================================================================================
// Lighting pass - для каждого источника света рисуем full-screen triangle с additive
// blending, накапливая освещение в бэк-буфере (слайды 11, 13, 21, 22)
// =====================================================================================
void RenderingSystem::RenderLights(ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    const std::vector<Light>& lights,
    const XMFLOAT4X4& viewProj, const XMFLOAT3& cameraPos)
{
    // depth buffer не привязываем - запись в него отключена (слайд 11: "Запись в буфер глубины отключена")
    cmdList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSignature.Get());

    // G-Buffer SRV таблица (t0..t2) одинакова для всех источников света
    cmdList->SetGraphicsRootDescriptorTable(1, mGBuffer.GetSrvGpuHandle());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (size_t i = 0; i < lights.size() && i < mMaxLights; ++i)
    {
        LightPassConstants lpc = {};
        lpc.ViewProj = viewProj;
        lpc.InvViewProj = viewProj; // не инвертируем - в шейдере не используется, оставлено для расширения
        lpc.CameraPos = cameraPos;
        lpc.LightData = lights[i];

        memcpy(mLightCBData + i * mLightCBElementSize, &lpc, sizeof(lpc));

        D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mLightCB->GetGPUVirtualAddress() + i * mLightCBElementSize;
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddress);

        // full-screen triangle - 3 вершины, ничего не привязываем как vertex buffer (слайд 22)
        cmdList->DrawInstanced(3, 1, 0, 0);
    }
}