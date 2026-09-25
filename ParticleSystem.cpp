// ParticleSystem.cpp

#include "ParticleSystem.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <cmath>

using Microsoft::WRL::ComPtr;

// Компилирует одну точку входа из Particles.hlsl
static ComPtr<ID3DBlob> CompileParticleShader(const char* entryPoint, const char* target)
{
    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> blob, errorBlob;
    HRESULT hr = D3DCompileFromFile(L"Particles.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint, target, compileFlags, 0, &blob, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        throw std::runtime_error(std::string("ParticleSystem: не удалось скомпилировать ") + entryPoint);
    }
    return blob;
}

// сериализует и создаёт root signature, при ошибке выводит текст в Output
static ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc, const char* name)
{
    ComPtr<ID3DBlob> serialized, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errorBlob);
    if (FAILED(hr))
    {
        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        throw std::runtime_error(std::string("ParticleSystem: D3D12SerializeRootSignature failed: ") + name);
    }

    ComPtr<ID3D12RootSignature> rootSig;
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&rootSig))))
        throw std::runtime_error(std::string("ParticleSystem: CreateRootSignature failed: ") + name);
    return rootSig;
}

void ParticleSystem::Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = res;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> ParticleSystem::CreateDefaultBuffer(UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    ComPtr<ID3D12Resource> buffer;
    if (FAILED(mDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer))))
        throw std::runtime_error("ParticleSystem: CreateCommittedResource (default buffer) failed");
    return buffer;
}

// Создаёт upload-буфер, копирует в него данные и командует GPU перенести их в dst.
// dst в этот момент должен быть в состоянии COPY_DEST
void ParticleSystem::UploadToBuffer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* dst, const void* data, UINT64 size)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> upload;
    if (FAILED(mDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        throw std::runtime_error("ParticleSystem: CreateCommittedResource (upload) failed");

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    memcpy(mapped, data, (size_t)size);
    upload->Unmap(0, nullptr);

    cmdList->CopyBufferRegion(dst, 0, upload.Get(), 0, size);

    // upload-буфер должен дожить до выполнения копирования на GPU
    mUploadBuffers.push_back(upload);
}

void ParticleSystem::Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT uavStartSlot,
    UINT maxParticles, const DXGI_FORMAT* gbufferFormats, UINT gbufferCount, DXGI_FORMAT dsvFormat)
{
    mDevice = device;
    // размер пула кратен 256 - столько потоков в одной группе SimulateCS
    mMaxParticles = (maxParticles + 255) / 256 * 256;

    CreateBuffers(cmdList);
    CreateViews(srvHeap, srvDescSize, uavStartSlot);
    CreateComputePipeline();
    CreateRenderPipeline(gbufferFormats, gbufferCount, dsvFormat);
}

void ParticleSystem::CreateBuffers(ID3D12GraphicsCommandList* cmdList)
{
    const D3D12_RESOURCE_FLAGS uavFlag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // Particle Pool: сначала все ячейки пустые (Alive = 0), заливаем нулями
    UINT64 poolSize = (UINT64)mMaxParticles * sizeof(GpuParticle);
    mParticlePool = CreateDefaultBuffer(poolSize, uavFlag, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<GpuParticle> emptyParticles(mMaxParticles);
    memset(emptyParticles.data(), 0, (size_t)poolSize);
    UploadToBuffer(cmdList, mParticlePool.Get(), emptyParticles.data(), poolSize);
    Transition(cmdList, mParticlePool.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Dead List: сначала свободны все ячейки, в списке номера 0, 1, 2, ..., max-1
    UINT64 listSize = (UINT64)mMaxParticles * sizeof(UINT);
    mDeadList = CreateDefaultBuffer(listSize, uavFlag, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<UINT> allIndices(mMaxParticles);
    for (UINT i = 0; i < mMaxParticles; ++i)
        allIndices[i] = i;
    UploadToBuffer(cmdList, mDeadList.Get(), allIndices.data(), listSize);
    Transition(cmdList, mDeadList.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Счётчик Dead List: в списке max элементов
    mDeadListCounter = CreateDefaultBuffer(sizeof(UINT), uavFlag, D3D12_RESOURCE_STATE_COPY_DEST);
    UINT deadCount = mMaxParticles;
    UploadToBuffer(cmdList, mDeadListCounter.Get(), &deadCount, sizeof(UINT));
    Transition(cmdList, mDeadListCounter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Alive List и его счётчик. Счётчик обнуляется каждый кадр перед SimulateCS
    mAliveList = CreateDefaultBuffer(listSize, uavFlag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    mAliveListCounter = CreateDefaultBuffer(sizeof(UINT), uavFlag, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Копия счётчика Dead List для EmitCS. Корневой CBV требует 256 байт и адрес, выровненный по 256
    mDeadCountCB = CreateDefaultBuffer(256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // Аргументы ExecuteIndirect. Число вершин будет каждый кадр копироваться из счётчика Alive List,
    // остальное постоянно: один инстанс, начинаем с нулевой вершины и нулевого инстанса
    mIndirectArgs = CreateDefaultBuffer(sizeof(D3D12_DRAW_ARGUMENTS), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_DRAW_ARGUMENTS args = {};
    args.VertexCountPerInstance = 0;
    args.InstanceCount = 1;
    args.StartVertexLocation = 0;
    args.StartInstanceLocation = 0;
    UploadToBuffer(cmdList, mIndirectArgs.Get(), &args, sizeof(args));
    Transition(cmdList, mIndirectArgs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // upload-буферы, которые живут всё время: ноль для сброса счётчика и две группы констант
    D3D12_HEAP_PROPERTIES uploadProps = {};
    uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    desc.Width = sizeof(UINT);
    if (FAILED(mDevice->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mZeroUpload))))
        throw std::runtime_error("ParticleSystem: CreateCommittedResource (zero) failed");
    void* zeroData = nullptr;
    mZeroUpload->Map(0, nullptr, &zeroData);
    *(UINT*)zeroData = 0;
    mZeroUpload->Unmap(0, nullptr);

    desc.Width = (sizeof(ParticleSimConstants) + 255) & ~255;
    if (FAILED(mDevice->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mSimCB))))
        throw std::runtime_error("ParticleSystem: CreateCommittedResource (sim CB) failed");
    mSimCB->Map(0, nullptr, (void**)&mSimCBData);

    desc.Width = (sizeof(ParticleRenderConstants) + 255) & ~255;
    if (FAILED(mDevice->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mRenderCB))))
        throw std::runtime_error("ParticleSystem: CreateCommittedResource (render CB) failed");
    mRenderCB->Map(0, nullptr, (void**)&mRenderCBData);

    // readback-буфер: GPU копирует сюда число живых частиц, CPU читает его после конца кадра
    D3D12_HEAP_PROPERTIES readbackProps = {};
    readbackProps.Type = D3D12_HEAP_TYPE_READBACK;
    desc.Width = sizeof(UINT);
    if (FAILED(mDevice->CreateCommittedResource(&readbackProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&mAliveCountReadback))))
        throw std::runtime_error("ParticleSystem: CreateCommittedResource (readback) failed");
    mAliveCountReadback->Map(0, nullptr, (void**)&mAliveCountReadbackData);
    *mAliveCountReadbackData = 0;
}

// UAV для compute шейдеров, 3 подряд в общей куче:
//  u0 - Particle Pool
//  u1 - Dead List со счётчиком (Consume в EmitCS, Append в SimulateCS)
//  u2 - Alive List со счётчиком (Append в SimulateCS)
// Append и Consume работают только через UAV со счётчиком: счётчик хранит, сколько элементов в списке,
// Append атомарно увеличивает его и пишет в конец, Consume атомарно уменьшает и читает с конца
void ParticleSystem::CreateViews(ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT uavStartSlot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)uavStartSlot * srvDescSize;

    mUavTableGpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
    mUavTableGpu.ptr += (SIZE_T)uavStartSlot * srvDescSize;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = mMaxParticles;
    uavDesc.Buffer.CounterOffsetInBytes = 0;

    // u0: пул, без счётчика
    uavDesc.Buffer.StructureByteStride = sizeof(GpuParticle);
    mDevice->CreateUnorderedAccessView(mParticlePool.Get(), nullptr, &uavDesc, cpu);
    cpu.ptr += srvDescSize;

    // u1: Dead List, счётчик лежит в отдельном буфере mDeadListCounter
    uavDesc.Buffer.StructureByteStride = sizeof(UINT);
    mDevice->CreateUnorderedAccessView(mDeadList.Get(), mDeadListCounter.Get(), &uavDesc, cpu);
    cpu.ptr += srvDescSize;

    // u2: Alive List со своим счётчиком
    mDevice->CreateUnorderedAccessView(mAliveList.Get(), mAliveListCounter.Get(), &uavDesc, cpu);
}

// Compute root signature, общая для EmitCS и SimulateCS:
//  0 - b0, параметры симуляции
//  1 - b1, число свободных ячеек (копия счётчика Dead List)
//  2 - таблица u0..u2. Append/Consume буферы можно привязать только таблицей:
//      у корневого UAV-дескриптора нет счётчика
void ParticleSystem::CreateComputePipeline()
{
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 3;
    uavRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // у compute шейдера своей видимости нет, только ALL

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &uavRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    mComputeRootSignature = CreateRootSignature(mDevice, rsDesc, "compute");

    // у compute PSO только root signature и один шейдер
    ComPtr<ID3DBlob> emitBlob = CompileParticleShader("EmitCS", "cs_5_0");
    ComPtr<ID3DBlob> simulateBlob = CompileParticleShader("SimulateCS", "cs_5_0");

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mComputeRootSignature.Get();

    psoDesc.CS = { emitBlob->GetBufferPointer(), emitBlob->GetBufferSize() };
    if (FAILED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mEmitPSO))))
        throw std::runtime_error("ParticleSystem: CreateComputePipelineState (emit) failed");

    psoDesc.CS = { simulateBlob->GetBufferPointer(), simulateBlob->GetBufferSize() };
    if (FAILED(mDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mSimulatePSO))))
        throw std::runtime_error("ParticleSystem: CreateComputePipelineState (simulate) failed");
}

// Render root signature и PSO:
//  0 - b0, камера
//  1 - t0, пул частиц (корневой SRV - структурный буфер можно привязать без таблицы)
//  2 - t1, Alive List
// Стадии VS -> GS -> PS, вход - список точек без вершинного буфера, выход - те же 3 рендертаргета G-буфера
void ParticleSystem::CreateRenderPipeline(const DXGI_FORMAT* gbufferFormats, UINT gbufferCount, DXGI_FORMAT dsvFormat)
{
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL; // константы нужны в GS (view, proj) и PS (InvView)

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS; // input layout не нужен
    mRenderRootSignature = CreateRootSignature(mDevice, rsDesc, "render");

    ComPtr<ID3DBlob> vsBlob = CompileParticleShader("ParticleVS", "vs_5_0");
    ComPtr<ID3DBlob> gsBlob = CompileParticleShader("ParticleGS", "gs_5_0");
    ComPtr<ID3DBlob> psBlob = CompileParticleShader("ParticlePS", "ps_5_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mRenderRootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.GS = { gsBlob->GetBufferPointer(), gsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.InputLayout = { nullptr, 0 }; // вершинного буфера нет, всё берётся по SV_VertexID

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; // билборд всегда повёрнут к камере, отсекать нечего
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    // частицы непрозрачные: без блендинга, с обычным тестом и записью глубины
    for (UINT i = 0; i < gbufferCount; ++i)
        psoDesc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT; // одна точка на частицу
    psoDesc.NumRenderTargets = gbufferCount;
    for (UINT i = 0; i < gbufferCount; ++i)
        psoDesc.RTVFormats[i] = gbufferFormats[i];
    psoDesc.DSVFormat = dsvFormat;
    psoDesc.SampleDesc.Count = 1;

    if (FAILED(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mRenderPSO))))
        throw std::runtime_error("ParticleSystem: CreateGraphicsPipelineState (render) failed");

    // Command signature для ExecuteIndirect: в буфере аргументов лежит ровно один обычный Draw
    // (D3D12_DRAW_ARGUMENTS). Root signature тут не нужна, потому что аргументы не меняют корневых параметров
    D3D12_INDIRECT_ARGUMENT_DESC argDesc = {};
    argDesc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

    D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
    sigDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
    sigDesc.NumArgumentDescs = 1;
    sigDesc.pArgumentDescs = &argDesc;

    if (FAILED(mDevice->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&mCommandSignature))))
        throw std::runtime_error("ParticleSystem: CreateCommandSignature failed");
}

void ParticleSystem::Update(float elapsedTime, const XMMATRIX& view, const XMMATRIX& proj)
{
    // Эмиссия: сколько частиц родить в этом кадре.
    // Средняя частота плюс случайное отклонение, умноженные на время кадра.
    // Дробная часть копится между кадрами, чтобы при высоком FPS частицы всё равно рождались
    float randomFactor = (float)rand() / (float)RAND_MAX * 2.0f - 1.0f;
    mAccumulatedTime += (mEmitRate + randomFactor * mEmitVariance) * elapsedTime;
    if (mAccumulatedTime < 0.0f)
        mAccumulatedTime = 0.0f;

    UINT emitCount = (UINT)mAccumulatedTime;
    mAccumulatedTime -= (float)emitCount;

    // за один кадр больше пула не родить
    if (emitCount > mMaxParticles)
        emitCount = mMaxParticles;

    mSimConstants.EmitterPos = mEmitterPos;
    mSimConstants.DeltaTime = elapsedTime;
    mSimConstants.Gravity = XMFLOAT3(0.0f, -500.0f, 0.0f);
    mSimConstants.EmitCount = emitCount;
    mSimConstants.Wind = XMFLOAT3(40.0f, 0.0f, 0.0f);
    mSimConstants.RandomSeed = ++mFrameIndex;
    mSimConstants.GroundHeight = 0.0f;
    mSimConstants.MaxParticles = mMaxParticles;
    memcpy(mSimCBData, &mSimConstants, sizeof(mSimConstants));

    ParticleRenderConstants rc = {};
    XMStoreFloat4x4(&rc.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&rc.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&rc.InvView, XMMatrixTranspose(XMMatrixInverse(nullptr, view)));
    rc.Curvature = 0.7f;
    memcpy(mRenderCBData, &rc, sizeof(rc));
}

void ParticleSystem::Simulate(ID3D12GraphicsCommandList* cmdList)
{
    // 1. Сколько свободных ячеек в Dead List. Значение счётчика есть только на GPU,
    //    копируем его в константный буфер, из которого его прочитает EmitCS
    Transition(cmdList, mDeadListCounter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmdList, mDeadCountCB.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(mDeadCountCB.Get(), 0, mDeadListCounter.Get(), 0, sizeof(UINT));
    Transition(cmdList, mDeadListCounter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, mDeadCountCB.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

    // 2. Обнуляем счётчик Alive List: SimulateCS заполнит список заново
    Transition(cmdList, mAliveListCounter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(mAliveListCounter.Get(), 0, mZeroUpload.Get(), 0, sizeof(UINT));
    Transition(cmdList, mAliveListCounter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    cmdList->SetComputeRootSignature(mComputeRootSignature.Get());
    cmdList->SetComputeRootConstantBufferView(0, mSimCB->GetGPUVirtualAddress());
    cmdList->SetComputeRootConstantBufferView(1, mDeadCountCB->GetGPUVirtualAddress());
    cmdList->SetComputeRootDescriptorTable(2, mUavTableGpu);

    // 3. Emit: по потоку на новую частицу, группы по 256 потоков
    if (mSimConstants.EmitCount > 0)
    {
        cmdList->SetPipelineState(mEmitPSO.Get());
        cmdList->Dispatch((mSimConstants.EmitCount + 255) / 256, 1, 1);
    }

    // UAV-барьер: SimulateCS должен увидеть частицы, которые только что записал EmitCS
    D3D12_RESOURCE_BARRIER uavBarrier = {};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = nullptr; // nullptr - барьер для всех UAV сразу
    cmdList->ResourceBarrier(1, &uavBarrier);

    // 4. Simulate: по потоку на каждую ячейку пула
    cmdList->SetPipelineState(mSimulatePSO.Get());
    cmdList->Dispatch(mMaxParticles / 256, 1, 1);

    cmdList->ResourceBarrier(1, &uavBarrier);

    // 5. Число живых частиц из счётчика Alive List - это число вершин для ExecuteIndirect.
    //    Заодно копируем его в readback-буфер, чтобы показать в заголовке окна
    Transition(cmdList, mAliveListCounter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmdList, mIndirectArgs.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->CopyBufferRegion(mIndirectArgs.Get(), 0, mAliveListCounter.Get(), 0, sizeof(UINT));
    cmdList->CopyBufferRegion(mAliveCountReadback.Get(), 0, mAliveListCounter.Get(), 0, sizeof(UINT));
    Transition(cmdList, mAliveListCounter.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, mIndirectArgs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    // 6. Пул и Alive List дальше только читаются вершинным шейдером
    Transition(cmdList, mParticlePool.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(cmdList, mAliveList.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void ParticleSystem::Render(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->SetPipelineState(mRenderPSO.Get());
    cmdList->SetGraphicsRootSignature(mRenderRootSignature.Get());
    cmdList->SetGraphicsRootConstantBufferView(0, mRenderCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootShaderResourceView(1, mParticlePool->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootShaderResourceView(2, mAliveList->GetGPUVirtualAddress());

    // каждая вершина - одна частица, GS разворачивает её в билборд
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);

    // Число вершин берётся из буфера mIndirectArgs на GPU, CPU его не знает.
    // Это и есть DrawInstancedIndirect со слайда: VertexCount = число живых частиц, InstanceCount = 1
    cmdList->ExecuteIndirect(mCommandSignature.Get(), 1, mIndirectArgs.Get(), 0, nullptr, 0);

    // обратно в UAV для compute шейдеров следующего кадра
    Transition(cmdList, mParticlePool.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(cmdList, mAliveList.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}