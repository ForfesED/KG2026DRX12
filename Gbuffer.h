// =====================================================================================
// Gbuffer.h
// Класс, который хранит набор render target текстур (G-Buffer) для deferred rendering.
//
// У нас 3 цели (как на слайде 18 презентации):
//  SV_Target0 - Albedo (RGB) + специфический параметр в альфе
//  SV_Target1 - World Position (XYZ)
//  SV_Target2 - Normal (XYZ)
// Плюс отдельный depth-stencil буфер (он у App уже есть, переиспользуем).
// =====================================================================================

#pragma once

#include <wrl/client.h>
#include <d3d12.h>
#include <vector>

class GBuffer
{
public:
    static const UINT kNumRenderTargets = 3;

    // создаёт 3 текстуры-рендертаргета нужного размера + RTV для них +
    // SRV в переданную SRV-кучу, начиная со слота srvStartSlot (нужно 3 свободных слота подряд)
    void Init(ID3D12Device* device, UINT width, UINT height,
        ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT srvStartSlot);

    // переводит все рендертаргеты G-буфера в указанное состояние (для барьеров)
    void TransitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    // очищает все рендертаргеты G-буфера
    void Clear(ID3D12GraphicsCommandList* cmdList);

    // отдаёт CPU-хендлы RTV (для OMSetRenderTargets в opaque-проходе)
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtvHandle(UINT index) const;

    // отдаёт GPU-хендл начала SRV-таблицы G-буфера (для lighting-проходa, t-регистры)
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle() const { return mSrvGpuStart; }

    // форматы рендертаргетов - нужны при создании PSO geometry pass
    static DXGI_FORMAT GetFormat(UINT index);

    ID3D12Resource* GetResource(UINT index) const { return mResources[index].Get(); }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> mResources[kNumRenderTargets];
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    UINT mRtvDescSize = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE mSrvGpuStart = {};
};