// RenderingSystem.h
// Класс, который отвечает за весь deferred rendering:
//  shadow pass   - сцена с точки зрения солнца в каскадную карту теней (VS -> GS, без PS)
//  geometry pass - заполнение G-буфера, геометрия идёт через тесселяцию (VS -> HS -> DS -> PS)
//  lighting pass - накопление освещения от источников света через full-screen треугольник
//
// Источники света трёх типов: Directional, Point, Spot.

#pragma once

#include <wrl/client.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>
#include "Gbuffer.h"
#include "ShadowMap.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Типы источников света, должны совпадать со значениями LIGHT_* в шейдере
enum class LightType : int
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

// Описание одного источника света. Раскладка должна совпадать со структурой Light в Shaders.hlsl
struct Light
{
    XMFLOAT3 Position = { 0, 0, 0 };
    float    Range = 50.0f;

    XMFLOAT3 Direction = { 0, -1, 0 };
    float    SpotPower = 16.0f; // чем больше, тем уже конус прожектора

    XMFLOAT3 Color = { 1, 1, 1 };
    float    Intensity = 1.0f;

    int      Type = (int)LightType::Point;
    XMFLOAT3 _pad = {};
};

// Константы для lighting pass, одна копия на каждый источник света
struct LightPassConstants
{
    XMFLOAT4X4 ViewProj;
    XMFLOAT4X4 InvViewProj;  // пока не используется, мировые координаты уже лежат в G-буфере
    XMFLOAT3   CameraPos;
    float      Pad0;
    Light      LightData;
};

class RenderingSystem
{
public:
    // создаёт GBuffer, каскадную карту теней, root signature и PSO для всех проходов.
    // gbufferSrvSlot - первый из 3 слотов SRV кучи под G-buffer, shadowSrvSlot - слот под карту теней
    void Init(ID3D12Device* device, UINT width, UINT height,
        ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT gbufferSrvSlot,
        UINT shadowSrvSlot, UINT shadowMapSize,
        DXGI_FORMAT backBufferFormat, DXGI_FORMAT dsvFormat);

    // Shadow pass.
    // Переключает вывод на карту теней, очищает её, ставит свой viewport и PSO.
    // Раскладка root signature:
    //  0 - b0, константы объекта (из них нужна только мировая матрица)
    //  1 - b1, константы каскадов (выставляются здесь же)
    // После EndShadowPass viewport нужно вернуть на размер окна
    void BeginShadowPass(ID3D12GraphicsCommandList* cmdList);
    void EndShadowPass(ID3D12GraphicsCommandList* cmdList);

    CascadedShadowMap& GetShadowMap() { return mShadowMap; }

    // Geometry pass.
    // Переключает render targets на G-buffer, очищает их и ставит PSO с тесселяцией.
    // Раскладка root signature:
    //  0 - b0, константы объекта
    //  1 - b1, константы кадра (камера и параметры тесселяции)
    //  2 - таблица t0..t2: diffuse, normal map, displacement map
    void BeginGeometryPass(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle);
    void EndGeometryPass(ID3D12GraphicsCommandList* cmdList);

    ID3D12RootSignature* GetGeometryRootSignature() const { return mGeometryRootSignature.Get(); }
    ID3D12PipelineState* GetGeometryPSO() const { return mGeometryPSO.Get(); }

    // каркасный режим, чтобы было видно, как меняется сетка после тесселяции
    void SetWireframe(bool enabled) { mWireframe = enabled; }
    bool IsWireframe() const { return mWireframe; }

    // Lighting pass.
    // Рисует в back buffer сумму освещения от всех источников через additive blending
    void RenderLights(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        const std::vector<Light>& lights,
        const XMFLOAT4X4& viewProj, const XMFLOAT3& cameraPos);

    GBuffer& GetGBuffer() { return mGBuffer; }

private:
    void CreateShadowRootSignatureAndPSO();
    void CreateGeometryRootSignatureAndPSO(DXGI_FORMAT dsvFormat);
    void CreateLightingRootSignatureAndPSO(DXGI_FORMAT backBufferFormat);

private:
    ID3D12Device* mDevice = nullptr;
    UINT mWidth = 0, mHeight = 0;

    GBuffer mGBuffer;
    CascadedShadowMap mShadowMap;

    // shadow pass: своя root signature (b0 объект, b1 каскады) и PSO без пиксельного шейдера
    ComPtr<ID3D12RootSignature> mShadowRootSignature;
    ComPtr<ID3D12PipelineState> mShadowPSO;

    // geometry pass: обычный PSO и такой же, но в режиме каркаса
    ComPtr<ID3D12RootSignature> mGeometryRootSignature;
    ComPtr<ID3D12PipelineState> mGeometryPSO;
    ComPtr<ID3D12PipelineState> mGeometryWireframePSO;
    bool mWireframe = false;

    // lighting pass: своя root signature, b0 (свет и камера), t0..t2 (G-buffer), b1 (каскады), t3 (карта теней)
    ComPtr<ID3D12RootSignature> mLightingRootSignature;
    ComPtr<ID3D12PipelineState> mLightingPSO;

    // константный буфер под все источники света, по одному блоку на свет
    ComPtr<ID3D12Resource> mLightCB;
    BYTE* mLightCBData = nullptr;
    UINT mLightCBElementSize = 0;
    UINT mMaxLights = 32;
};