// RenderingSystem.cpp

#include "RenderingSystem.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <string>

// константные буферы в D3D12 выравниваются по 256 байт
inline UINT CalcCBSize(UINT byteSize)
{
    return (byteSize + 255) & ~255;
}

// Компилирует одну точку входа из Shaders.hlsl.
// target - профиль шейдера: vs_5_0, hs_5_0, ds_5_0, ps_5_0.
// Если компиляция упала, текст ошибки уходит в окно Output отладчика.
static ComPtr<ID3DBlob> CompileShader(const char* entryPoint, const char* target)
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> blob, errorBlob;
    HRESULT hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint, target, compileFlags, 0, &blob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        throw std::runtime_error(std::string("RenderingSystem: не удалось скомпилировать ") + entryPoint);
    }
    return blob;
}

void RenderingSystem::Init(ID3D12Device* device, UINT width, UINT height,
    ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT srvStartSlot,
    DXGI_FORMAT backBufferFormat, DXGI_FORMAT dsvFormat)
{
    mDevice = device;
    mWidth = width;
    mHeight = height;

    mGBuffer.Init(device, width, height, srvHeap, srvDescSize, srvStartSlot);

    CreateGeometryRootSignatureAndPSO(dsvFormat);
    CreateLightingRootSignatureAndPSO(backBufferFormat);

    // константный буфер для источников света, один блок на каждый свет
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

    // буфер в upload куче, держим его замапленным всё время работы программы
    if (FAILED(mLightCB->Map(0, nullptr, (void**)&mLightCBData)))
        throw std::runtime_error("RenderingSystem: Map LightCB failed");
}

// Root signature и PSO для geometry pass с тесселяцией.
// Главные отличия от обычного пайплайна:
//  в PSO кроме VS и PS заданы HS и DS
//  тип примитива - PATCH, а не TRIANGLE
//  текстуры и сэмплер видны всем стадиям, потому что displacement читается в DS
void RenderingSystem::CreateGeometryRootSignatureAndPSO(DXGI_FORMAT dsvFormat)
{
    // одна таблица на 3 SRV подряд: t0 diffuse, t1 normal map, t2 displacement map
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[3] = {};

    // b0 - константы объекта
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // b1 - константы кадра, они нужны и в HS (позиция камеры), и в DS (ViewProj, смещение)
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // таблица текстур материала, видна и DS, и PS
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // статический сэмплер s0: линейная фильтрация и повтор текстуры (нужен для тайлинга)
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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

    // все 4 программируемые стадии geometry pass
    ComPtr<ID3DBlob> vsBlob = CompileShader("GeometryVS", "vs_5_0");
    ComPtr<ID3DBlob> hsBlob = CompileShader("GeometryHS", "hs_5_0");
    ComPtr<ID3DBlob> dsBlob = CompileShader("GeometryDS", "ds_5_0");
    ComPtr<ID3DBlob> psBlob = CompileShader("GeometryPS", "ps_5_0");

    // формат вершины, должен совпадать со struct Vertex в main.cpp и VertexIn в шейдере
    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DISPNORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 56, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "DISPWEIGHT", 0, DXGI_FORMAT_R32_FLOAT,       0, 68, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mGeometryRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.HS = { hsBlob->GetBufferPointer(), hsBlob->GetBufferSize() };
    psoDesc.DS = { dsBlob->GetBufferPointer(), dsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout = { inputLayout, (UINT)(sizeof(inputLayout) / sizeof(inputLayout[0])) };

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // блендинг выключен для всех рендертаргетов G-буфера, пишем значения как есть
    for (UINT i = 0; i < GBuffer::kNumRenderTargets; ++i)
        psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;

    // при включённом HS входной примитив обязан быть патчем,
    // сколько в нём точек (3) задаётся уже при отрисовке через IASetPrimitiveTopology
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;

    // MRT: пишем сразу в 3 рендертаргета G-буфера
    psoDesc.NumRenderTargets = GBuffer::kNumRenderTargets;
    for (UINT i = 0; i < GBuffer::kNumRenderTargets; ++i)
        psoDesc.RTVFormats[i] = GBuffer::GetFormat(i);

    psoDesc.DSVFormat = dsvFormat;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mGeometryPSO))))
        throw std::runtime_error("RenderingSystem: CreateGraphicsPipelineState (geometry) failed");

    // тот же PSO, но рисует только рёбра треугольников и без отсечения задних граней
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mGeometryWireframePSO))))
        throw std::runtime_error("RenderingSystem: CreateGraphicsPipelineState (geometry wireframe) failed");
}

// Root signature и PSO для lighting pass.
//  b0 - LightPassConstants (камера и один источник света)
//  t0..t2 - G-buffer (Albedo, WorldPos, Normal)
// Рисуется full-screen треугольник с additive blending
void RenderingSystem::CreateLightingRootSignatureAndPSO(DXGI_FORMAT backBufferFormat)
{
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = GBuffer::kNumRenderTargets;
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // G-buffer читается через Load, сэмплер оставлен на случай, если понадобится Sample
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
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
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // input layout не нужен, треугольник строится в VS

    ComPtr<ID3DBlob> serialized, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        throw std::runtime_error("RenderingSystem: D3D12SerializeRootSignature (lighting) failed");
    }
    if (FAILED(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mLightingRootSignature))))
        throw std::runtime_error("RenderingSystem: CreateRootSignature (lighting) failed");

    ComPtr<ID3DBlob> vsBlob = CompileShader("LightingVS", "vs_5_0");
    ComPtr<ID3DBlob> psBlob = CompileShader("LightingPS", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mLightingRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout = { nullptr, 0 }; // вершин нет, всё генерируется в VS по SV_VertexID

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // additive blending: результат каждого источника света прибавляется к тому, что уже в бэк-буфере
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

    // буфер глубины в этом проходе не нужен совсем
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = backBufferFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mLightingPSO))))
        throw std::runtime_error("RenderingSystem: CreateGraphicsPipelineState (lighting) failed");
}

// Начало geometry pass: G-buffer становится рендертаргетом и очищается
void RenderingSystem::BeginGeometryPass(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle)
{
    // G-buffer из состояния "читается шейдером" переводим в "render target"
    mGBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[GBuffer::kNumRenderTargets];
    for (UINT i = 0; i < GBuffer::kNumRenderTargets; ++i)
        rtvHandles[i] = mGBuffer.GetRtvHandle(i);

    mGBuffer.Clear(cmdList);
    cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    cmdList->OMSetRenderTargets(GBuffer::kNumRenderTargets, rtvHandles, FALSE, &dsvHandle);
    cmdList->SetPipelineState(mWireframe ? mGeometryWireframePSO.Get() : mGeometryPSO.Get());
    cmdList->SetGraphicsRootSignature(mGeometryRootSignature.Get());

    // каждые 3 индекса из индексного буфера - один треугольный патч для hull shader
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
}

// Конец geometry pass: G-buffer снова доступен для чтения в lighting pass
void RenderingSystem::EndGeometryPass(ID3D12GraphicsCommandList* cmdList)
{
    mGBuffer.TransitionTo(cmdList, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// Lighting pass: для каждого источника света рисуем full-screen треугольник,
// блендинг складывает вклад всех источников в бэк-буфере
void RenderingSystem::RenderLights(ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    const std::vector<Light>& lights,
    const XMFLOAT4X4& viewProj, const XMFLOAT3& cameraPos)
{
    // depth buffer не привязываем, в этом проходе он не используется
    cmdList->OMSetRenderTargets(1, &backBufferRtv, FALSE, nullptr);
    cmdList->SetPipelineState(mLightingPSO.Get());
    cmdList->SetGraphicsRootSignature(mLightingRootSignature.Get());

    // таблица SRV G-буфера одна и та же для всех источников света
    cmdList->SetGraphicsRootDescriptorTable(1, mGBuffer.GetSrvGpuHandle());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (size_t i = 0; i < lights.size() && i < mMaxLights; ++i)
    {
        LightPassConstants lpc = {};
        lpc.ViewProj = viewProj;
        lpc.InvViewProj = viewProj; // не инвертируется, в шейдере не используется
        lpc.CameraPos = cameraPos;
        lpc.LightData = lights[i];

        // у каждого света свой блок в буфере, иначе следующий memcpy перетёр бы данные до выполнения на GPU
        memcpy(mLightCBData + i * mLightCBElementSize, &lpc, sizeof(lpc));

        D3D12_GPU_VIRTUAL_ADDRESS cbAddress = mLightCB->GetGPUVirtualAddress() + i * mLightCBElementSize;
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddress);

        // 3 вершины без вершинного буфера
        cmdList->DrawInstanced(3, 1, 0, 0);
    }
}