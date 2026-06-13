// =====================================================================================
// Gbuffer.cpp
// =====================================================================================

#include "Gbuffer.h"
#include <stdexcept>

DXGI_FORMAT GBuffer::GetFormat(UINT index)
{
    switch (index)
    {
    case 0: return DXGI_FORMAT_R8G8B8A8_UNORM;    // Albedo + Spec
    case 1: return DXGI_FORMAT_R32G32B32A32_FLOAT; // World Position
    case 2: return DXGI_FORMAT_R32G32B32A32_FLOAT; // Normal
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

void GBuffer::Init(ID3D12Device* device, UINT width, UINT height,
    ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT srvStartSlot)
{
    // -------- создаём собственную RTV-кучу на kNumRenderTargets дескрипторов --------
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = kNumRenderTargets;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mRtvHeap))))
        throw std::runtime_error("GBuffer: CreateDescriptorHeap RTV failed");

    mRtvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();

    // запоминаем начало диапазона SRV в общей куче - оно нам понадобится в lighting-проходе
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuStartHeap = srvHeap->GetGPUDescriptorHandleForHeapStart();
    mSrvGpuStart = srvGpuStartHeap;
    mSrvGpuStart.ptr += (SIZE_T)srvStartSlot * srvDescSize;

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpuHandle.ptr += (SIZE_T)srvStartSlot * srvDescSize;

    for (UINT i = 0; i < kNumRenderTargets; ++i)
    {
        DXGI_FORMAT format = GetFormat(i);

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = format;
        clear.Color[0] = 0.0f;
        clear.Color[1] = 0.0f;
        clear.Color[2] = 0.0f;
        clear.Color[3] = 0.0f;

        if (FAILED(device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
            IID_PPV_ARGS(&mResources[i]))))
        {
            throw std::runtime_error("GBuffer: CreateCommittedResource failed");
        }

        // RTV
        device->CreateRenderTargetView(mResources[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += mRtvDescSize;

        // SRV (для чтения в lighting-проходе)
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(mResources[i].Get(), &srvDesc, srvCpuHandle);
        srvCpuHandle.ptr += srvDescSize;
    }
}

void GBuffer::TransitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barriers[kNumRenderTargets] = {};
    for (UINT i = 0; i < kNumRenderTargets; ++i)
    {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = mResources[i].Get();
        barriers[i].Transition.StateBefore = before;
        barriers[i].Transition.StateAfter = after;
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmdList->ResourceBarrier(kNumRenderTargets, barriers);
}

void GBuffer::Clear(ID3D12GraphicsCommandList* cmdList)
{
    const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (UINT i = 0; i < kNumRenderTargets; ++i)
    {
        cmdList->ClearRenderTargetView(GetRtvHandle(i), zero, 0, nullptr);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE GBuffer::GetRtvHandle(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)index * mRtvDescSize;
    return handle;
}