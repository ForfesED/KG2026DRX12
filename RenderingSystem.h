// =====================================================================================
// RenderingSystem.h
// Класс, который отвечает за весь deferred rendering:
//  - geometry pass (заполнение G-буфера)
//  - lighting pass (накопление освещения от источников света через full-screen quad)
//
// Источники света трёх типов (как требуется в домашке): Directional, Point, Spot.
// =====================================================================================

#pragma once

#include <wrl/client.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>
#include "Gbuffer.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Типы источников света - должны совпадать со значениями LightType в шейдере
enum class LightType : int
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

// Описание одного источника света. Раскладка должна совпадать со
// структурой Light в Shaders.hlsl (см. ниже).
struct Light
{
    XMFLOAT3 Position = { 0, 0, 0 };
    float    Range = 50.0f;

    XMFLOAT3 Direction = { 0, -1, 0 };
    float    SpotPower = 16.0f; // насколько узкий конус у spot-света

    XMFLOAT3 Color = { 1, 1, 1 };
    float    Intensity = 1.0f;

    int      Type = (int)LightType::Point;
    XMFLOAT3 _pad = {};
};

// Константы для lighting pass (одна на каждый источник света)
struct LightPassConstants
{
    XMFLOAT4X4 ViewProj;     // для восстановления / на будущее
    XMFLOAT4X4 InvViewProj;  // пока не используем (мировые координаты уже лежат в G-буфере)
    XMFLOAT3   CameraPos;
    float      Pad0;
    Light      LightData;
};

class RenderingSystem
{
public:
    // создаёт GBuffer, root signature и PSO для geometry/lighting проходов
    void Init(ID3D12Device* device, UINT width, UINT height,
        ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT srvStartSlot,
        DXGI_FORMAT backBufferFormat, DXGI_FORMAT dsvFormat);

    // -------- Geometry pass --------
    // переключает render targets на G-buffer, очищает их, ставит PSO geometry pass
    void BeginGeometryPass(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);
    void EndGeometryPass(ID3D12GraphicsCommandList* cmdList);

    ID3D12RootSignature* GetGeometryRootSignature() const { return mGeometryRootSignature.Get(); }
    ID3D12PipelineState* GetGeometryPSO() const { return mGeometryPSO.Get(); }

    // -------- Lighting pass --------
    // рисует back buffer = накопление освещения от всех источников света
    // через full-screen quad с additive blending (слайды 11, 13, 21, 22)
    void RenderLights(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        const std::vector<Light>& lights,
        const XMFLOAT4X4& viewProj, const XMFLOAT3& cameraPos);

    GBuffer& GetGBuffer() { return mGBuffer; }

private:
    void CreateGeometryRootSignatureAndPSO(DXGI_FORMAT backBufferFormat, DXGI_FORMAT dsvFormat);
    void CreateLightingRootSignatureAndPSO(DXGI_FORMAT backBufferFormat);

private:
    ID3D12Device* mDevice = nullptr;
    UINT mWidth = 0, mHeight = 0;

    GBuffer mGBuffer;

    // geometry pass - переиспользует ту же root signature, что и раньше (b0/b1/t0),
    // но PSO теперь пишет в 3 рендертаргета (MRT)
    ComPtr<ID3D12RootSignature> mGeometryRootSignature;
    ComPtr<ID3D12PipelineState> mGeometryPSO;

    // lighting pass - своя root signature: b0 (константы света/камеры) + t0..t2 (G-buffer SRV)
    ComPtr<ID3D12RootSignature> mLightingRootSignature;
    ComPtr<ID3D12PipelineState> mLightingPSO;       // point/spot - additive
    ComPtr<ID3D12PipelineState> mLightingPSOAmbientDir; // directional/ambient - тоже additive, но отдельный PSO под VS без позиции

    ComPtr<ID3D12Resource> mLightCB;
    BYTE* mLightCBData = nullptr;
    UINT mLightCBElementSize = 0;
    UINT mMaxLights = 32;
};