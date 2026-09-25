// ShadowMap.h
// Каскадная карта теней (Cascaded Shadow Maps) для направленного света.
//
// Идея: вблизи камеры тени нужны детальные, вдали можно грубее.
// Поэтому фрустум камеры режется по глубине на несколько частей (каскадов),
// и для каждой части рисуется своя карта теней, плотно подогнанная под этот кусок.
// Все карты лежат в одной текстуре-массиве (Texture2DArray), по слою на каскад.
//
// Для направленного света у каждого каскада ортогональная проекция:
// лучи солнца параллельны, перспективы у них нет.

#pragma once

#include <wrl/client.h>
#include <d3d12.h>
#include <DirectXMath.h>

using namespace DirectX;

// Константы каскадов. Одни и те же для shadow pass (geometry shader) и lighting pass (пиксельный шейдер).
// Раскладка должна совпадать с cbCascades в Shaders.hlsl
struct CascadeConstants
{
    XMFLOAT4X4 ViewProj[4]; // матрица view * proj света для каждого каскада (транспонированная для HLSL)
    XMFLOAT4   Distances;   // дальняя граница каждого каскада по глубине во view space камеры
    XMFLOAT4X4 CameraView;  // view матрица камеры, по ней шейдер находит глубину пикселя
    XMFLOAT4   Params;      // x - 1/размер карты, y - PCF вкл (1/0), z - раскрасить каскады (1/0), w - размер карты
};

class CascadedShadowMap
{
public:
    static const UINT kCascadeCount = 4;

    // Создаёт текстуру-массив глубины на kCascadeCount слоёв размером mapSize x mapSize,
    // DSV на весь массив, SRV в общую кучу в слот srvSlot и константный буфер каскадов
    void Init(ID3D12Device* device, UINT mapSize,
        ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT srvSlot);

    // Пересчитывает каскады под текущую камеру и записывает константы:
    //  cameraView, fovY, aspect, cameraNear - параметры камеры
    //  shadowDistance - до какой глубины от камеры вообще есть тени
    //  lambda - насколько распределение каскадов логарифмическое (0 - равномерное, 1 - чисто логарифмическое)
    //  lightDir - направление лучей света
    void Update(const XMMATRIX& cameraView, float fovY, float aspect, float cameraNear,
        float shadowDistance, float lambda, const XMFLOAT3& lightDir,
        bool pcfEnabled, bool showCascades);

    // переводит текстуру теней между "читается шейдером" и "пишется как буфер глубины"
    void TransitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    // очищает все слои глубины в 1 (максимально далеко)
    void Clear(ID3D12GraphicsCommandList* cmdList);

    D3D12_CPU_DESCRIPTOR_HANDLE GetDsv() const;
    D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle() const { return mSrvGpu; }
    D3D12_GPU_VIRTUAL_ADDRESS GetConstantsGpuAddress() const { return mCascadeCB->GetGPUVirtualAddress(); }
    const D3D12_VIEWPORT& GetViewport() const { return mViewport; }
    const D3D12_RECT& GetScissor() const { return mScissor; }

    // не транспонированная матрица каскада - для отсечения объектов на CPU
    XMMATRIX GetCascadeViewProj(UINT index) const { return XMLoadFloat4x4(&mCascadeViewProj[index]); }

private:
    // 8 углов фрустума камеры в мировых координатах
    static void GetFrustumCornersWorldSpace(const XMMATRIX& view, const XMMATRIX& proj, XMVECTOR outCorners[8]);

    // матрица view * proj света, которая плотно охватывает кусок фрустума камеры от nearZ до farZ
    static XMMATRIX CalcCascadeViewProj(const XMMATRIX& cameraView, float fovY, float aspect,
        float nearZ, float farZ, const XMFLOAT3& lightDir);

private:
    UINT mMapSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> mShadowMap;       // Texture2DArray глубины
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;   // своя куча на один DSV
    D3D12_GPU_DESCRIPTOR_HANDLE mSrvGpu = {};

    Microsoft::WRL::ComPtr<ID3D12Resource> mCascadeCB;       // константы каскадов в upload куче
    BYTE* mCascadeCBData = nullptr;

    XMFLOAT4X4 mCascadeViewProj[kCascadeCount] = {};

    D3D12_VIEWPORT mViewport = {};
    D3D12_RECT mScissor = {};
};