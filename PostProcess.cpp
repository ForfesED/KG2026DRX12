// PostProcess.cpp

#include "PostProcess.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

// Компилирует одну точку входа из PostProcess.hlsl.
// Если компиляция упала, текст ошибки уходит в окно Output отладчика
static ComPtr<ID3DBlob> CompilePostShader(const char* entryPoint, const char* target)
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> blob, errorBlob;
    HRESULT hr = D3DCompileFromFile(L"PostProcess.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint, target, compileFlags, 0, &blob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        throw std::runtime_error(std::string("PostProcess: не удалось скомпилировать ") + entryPoint);
    }
    return blob;
}

void PostProcess::Init(ID3D12Device* device, UINT width, UINT height,
    ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT srvStartSlot,
    DXGI_FORMAT backBufferFormat)
{
    mDevice = device;
    mSrvHeap = srvHeap;
    mSrvDescSize = srvDescSize;
    mSrvStartSlot = srvStartSlot;

    // bloom считается в половинном разрешении: в 4 раза меньше пикселей, а свечение всё равно размытое
    UINT halfW = width / 2;
    UINT halfH = height / 2;

    mFullViewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    mFullScissor = { 0, 0, (LONG)width, (LONG)height };
    mHalfViewport = { 0.0f, 0.0f, (float)halfW, (float)halfH, 0.0f, 1.0f };
    mHalfScissor = { 0, 0, (LONG)halfW, (LONG)halfH };

    // своя RTV куча на 3 рендертаргета
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kTargetCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(mDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap))))
        throw std::runtime_error("PostProcess: CreateDescriptorHeap RTV failed");
    mRtvDescSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    CreateTarget(kScene, width, height);
    CreateTarget(kBloomA, halfW, halfH);
    CreateTarget(kBloomB, halfW, halfH);

    CreateRootSignature();
    CreatePSOs(backBufferFormat);
}

void PostProcess::CreateTarget(UINT index, UINT width, UINT height)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = kSceneFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // цвет очистки, с которым очистка работает быстрее всего (чёрный)
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = kSceneFormat;
    clear.Color[0] = 0.0f;
    clear.Color[1] = 0.0f;
    clear.Color[2] = 0.0f;
    clear.Color[3] = 1.0f;

    // начальное состояние "читается шейдером", как у G-буфера: рисовать в неё будем только после барьера
    if (FAILED(mDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&mTargets[index]))))
        throw std::runtime_error("PostProcess: CreateCommittedResource failed");

    // RTV - чтобы в текстуру можно было рисовать
    mDevice->CreateRenderTargetView(mTargets[index].Get(), nullptr, GetRtv(index));

    // SRV - чтобы текстуру можно было читать в шейдере. Лежат подряд, поэтому одна таблица t0..t2 видит все три
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = kSceneFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += (SIZE_T)(mSrvStartSlot + index) * mSrvDescSize;
    mDevice->CreateShaderResourceView(mTargets[index].Get(), &srvDesc, srvHandle);
}

void PostProcess::CreateRootSignature()
{
    // таблица t0..t2: сцена и две текстуры bloom
    D3D12_DESCRIPTOR_RANGE postRange = {};
    postRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    postRange.NumDescriptors = kSrvCount;
    postRange.BaseShaderRegister = 0;
    postRange.RegisterSpace = 0;
    postRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // таблица t3..t5: G-буфер. Лежит в другом месте кучи, поэтому отдельная таблица
    D3D12_DESCRIPTOR_RANGE gbufferRange = {};
    gbufferRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    gbufferRange.NumDescriptors = 3;
    gbufferRange.BaseShaderRegister = 3;
    gbufferRange.RegisterSpace = 0;
    gbufferRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[3] = {};

    // 0 - 4 числа прямо в root signature, без константного буфера (в шейдере это cbuffer b0)
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &postRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &gbufferRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s0 - линейная фильтрация, clamp: при выборке за краем экрана берётся крайний пиксель
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE; // input layout не нужен, вершины строятся в VS

    ComPtr<ID3DBlob> serialized, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        throw std::runtime_error("PostProcess: D3D12SerializeRootSignature failed");
    }
    if (FAILED(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&mRootSignature))))
        throw std::runtime_error("PostProcess: CreateRootSignature failed");
}

void PostProcess::CreatePSOs(DXGI_FORMAT backBufferFormat)
{
    ComPtr<ID3DBlob> vsBlob = CompilePostShader("PostVS", "vs_5_0");
    ComPtr<ID3DBlob> brightBlob = CompilePostShader("BrightPS", "ps_5_0");
    ComPtr<ID3DBlob> blurHBlob = CompilePostShader("BlurHPS", "ps_5_0");
    ComPtr<ID3DBlob> blurVBlob = CompilePostShader("BlurVPS", "ps_5_0");
    ComPtr<ID3DBlob> finalBlob = CompilePostShader("FinalPS", "ps_5_0");

    // общие настройки для всех 4 PSO, отличаются только пиксельный шейдер и формат рендертаргета
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.InputLayout = { nullptr, 0 }; // вершинного буфера нет

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // в strip треугольники идут с разным обходом, не отсекаем
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // смешивания нет: каждый проход просто перезаписывает пиксели
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // глубина не нужна
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    psoDesc.SampleDesc.Count = 1;

    // три прохода bloom рисуют в текстуры bloom
    psoDesc.RTVFormats[0] = kSceneFormat;

    psoDesc.PS = { brightBlob->GetBufferPointer(), brightBlob->GetBufferSize() };
    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mBrightPSO))))
        throw std::runtime_error("PostProcess: CreateGraphicsPipelineState (bright) failed");

    psoDesc.PS = { blurHBlob->GetBufferPointer(), blurHBlob->GetBufferSize() };
    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mBlurHPSO))))
        throw std::runtime_error("PostProcess: CreateGraphicsPipelineState (blur H) failed");

    psoDesc.PS = { blurVBlob->GetBufferPointer(), blurVBlob->GetBufferSize() };
    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mBlurVPSO))))
        throw std::runtime_error("PostProcess: CreateGraphicsPipelineState (blur V) failed");

    // итоговый проход рисует в back buffer
    psoDesc.RTVFormats[0] = backBufferFormat;
    psoDesc.PS = { finalBlob->GetBufferPointer(), finalBlob->GetBufferSize() };
    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mFinalPSO))))
        throw std::runtime_error("PostProcess: CreateGraphicsPipelineState (final) failed");
}

D3D12_CPU_DESCRIPTOR_HANDLE PostProcess::GetRtv(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)index * mRtvDescSize;
    return handle;
}

void PostProcess::Transition(ID3D12GraphicsCommandList* cmdList, UINT index,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = mTargets[index].Get();
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

void PostProcess::DrawQuad(ID3D12GraphicsCommandList* cmdList, ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
    cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmdList->SetPipelineState(pso);
    // 4 вершины, 1 экземпляр. Вершинного буфера нет, PostVS строит углы экрана по номеру вершины
    cmdList->DrawInstanced(4, 1, 0, 0);
}

void PostProcess::BeginScene(ID3D12GraphicsCommandList* cmdList)
{
    Transition(cmdList, kScene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // lighting pass складывает свет от всех источников, поэтому начинаем с чёрного
    const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    cmdList->ClearRenderTargetView(GetRtv(kScene), black, 0, nullptr);
}

void PostProcess::Render(ID3D12GraphicsCommandList* cmdList,
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
    D3D12_GPU_DESCRIPTOR_HANDLE gbufferSrv,
    bool bloomEnabled, bool chromaEnabled, UINT viewMode)
{
    // сцена нарисована, дальше её только читаем
    Transition(cmdList, kScene, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // root signature и все входы одни на все проходы, выставляем один раз
    cmdList->SetGraphicsRootSignature(mRootSignature.Get());

    UINT constants[4] = { bloomEnabled ? 1u : 0u, chromaEnabled ? 1u : 0u, viewMode, 0u };
    cmdList->SetGraphicsRoot32BitConstants(0, 4, constants, 0);

    D3D12_GPU_DESCRIPTOR_HANDLE postSrv = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
    postSrv.ptr += (SIZE_T)mSrvStartSlot * mSrvDescSize;
    cmdList->SetGraphicsRootDescriptorTable(1, postSrv);
    cmdList->SetGraphicsRootDescriptorTable(2, gbufferSrv);

    // 4 вершины подряд -> 2 треугольника, прямоугольник на весь экран
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // Bloom считаем, если он включён или если смотрим на свечение отдельно (режим 4)
    if (bloomEnabled || viewMode == 4)
    {
        // текстуры bloom вдвое меньше экрана
        cmdList->RSSetViewports(1, &mHalfViewport);
        cmdList->RSSetScissorRects(1, &mHalfScissor);

        // 1) яркие пиксели сцены -> A
        Transition(cmdList, kBloomA, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        DrawQuad(cmdList, mBrightPSO.Get(), GetRtv(kBloomA));
        Transition(cmdList, kBloomA, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // 2) размытие. Читать и писать одну текстуру одновременно нельзя, поэтому
        // горизонталь идёт из A в B, вертикаль из B обратно в A. Повторяем несколько раз для более широкого свечения
        for (UINT i = 0; i < kBlurPasses; ++i)
        {
            Transition(cmdList, kBloomB, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            DrawQuad(cmdList, mBlurHPSO.Get(), GetRtv(kBloomB));
            Transition(cmdList, kBloomB, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            Transition(cmdList, kBloomA, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            DrawQuad(cmdList, mBlurVPSO.Get(), GetRtv(kBloomA));
            Transition(cmdList, kBloomA, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // обратно на полный экран
        cmdList->RSSetViewports(1, &mFullViewport);
        cmdList->RSSetScissorRects(1, &mFullScissor);
    }

    // 3) итог: сцена + свечение + аберрация (или режим просмотра G-буфера) -> back buffer
    DrawQuad(cmdList, mFinalPSO.Get(), backBufferRtv);
}