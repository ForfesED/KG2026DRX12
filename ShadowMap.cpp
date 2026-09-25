// ShadowMap.cpp
#define NOMINMAX

#include "ShadowMap.h"
#include <stdexcept>
#include <cmath>
#include <limits>
#include <algorithm>

void CascadedShadowMap::Init(ID3D12Device* device, UINT mapSize,
    ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT srvSlot)
{
    mMapSize = mapSize;

    // Текстура-массив глубины: kCascadeCount слоёв, по слою на каскад.
    // Формат R32_TYPELESS: как буфер глубины она видится как D32_FLOAT,
    // а как текстура для чтения в шейдере - как R32_FLOAT. Один и тот же формат для обоих
    // вариантов задать нельзя, поэтому ресурс "без типа", а тип указывается в каждом view
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resDesc.Width = mapSize;
    resDesc.Height = mapSize;
    resDesc.DepthOrArraySize = kCascadeCount; // размер массива
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    resDesc.SampleDesc.Count = 1;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // значение очистки: глубина 1 - "дальше ничего нет", всё освещено
    D3D12_CLEAR_VALUE optClear = {};
    optClear.Format = DXGI_FORMAT_D32_FLOAT;
    optClear.DepthStencil.Depth = 1.0f;
    optClear.DepthStencil.Stencil = 0;

    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &optClear, IID_PPV_ARGS(&mShadowMap))))
        throw std::runtime_error("CascadedShadowMap: CreateCommittedResource failed");

    // SRV: весь массив как Texture2DArray с форматом R32_FLOAT, читается в lighting pass
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = kCascadeCount;

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvCpu.ptr += (SIZE_T)srvSlot * srvDescSize;
    device->CreateShaderResourceView(mShadowMap.Get(), &srvDesc, srvCpu);

    mSrvGpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
    mSrvGpu.ptr += (SIZE_T)srvSlot * srvDescSize;

    // своя маленькая куча под один DSV
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap))))
        throw std::runtime_error("CascadedShadowMap: CreateDescriptorHeap DSV failed");

    // DSV: весь массив сразу. Какой слой пишется - решает geometry shader через SV_RenderTargetArrayIndex
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    dsvDesc.Texture2DArray.MipSlice = 0;
    dsvDesc.Texture2DArray.FirstArraySlice = 0;
    dsvDesc.Texture2DArray.ArraySize = kCascadeCount;
    device->CreateDepthStencilView(mShadowMap.Get(), &dsvDesc, mDsvHeap->GetCPUDescriptorHandleForHeapStart());

    // viewport размером с карту теней, а не с окно
    mViewport = { 0.0f, 0.0f, (float)mapSize, (float)mapSize, 0.0f, 1.0f };
    mScissor = { 0, 0, (LONG)mapSize, (LONG)mapSize };

    // константный буфер каскадов, пишется каждый кадр, поэтому в upload куче и замаплен навсегда
    D3D12_HEAP_PROPERTIES uploadProps = {};
    uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = (sizeof(CascadeConstants) + 255) & ~255; // константные буферы выравниваются по 256 байт
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(&uploadProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mCascadeCB))))
        throw std::runtime_error("CascadedShadowMap: CreateCommittedResource CB failed");

    if (FAILED(mCascadeCB->Map(0, nullptr, (void**)&mCascadeCBData)))
        throw std::runtime_error("CascadedShadowMap: Map CB failed");
}

// Углы фрустума в мире: берём 8 углов куба NDC (x и y от -1 до 1, z от 0 до 1 в D3D)
// и переводим их обратной матрицей (view * proj)^-1. После умножения делим на w,
// потому что перспективная проекция нелинейная и w у точек разный
void CascadedShadowMap::GetFrustumCornersWorldSpace(const XMMATRIX& view, const XMMATRIX& proj, XMVECTOR outCorners[8])
{
    XMMATRIX viewProj = view * proj;
    XMMATRIX inv = XMMatrixInverse(nullptr, viewProj);

    int n = 0;
    for (int x = 0; x < 2; ++x)
    {
        for (int y = 0; y < 2; ++y)
        {
            for (int z = 0; z < 2; ++z)
            {
                XMVECTOR pt = XMVector4Transform(
                    XMVectorSet(2.0f * x - 1.0f, 2.0f * y - 1.0f, (float)z, 1.0f), inv);
                outCorners[n++] = XMVectorDivide(pt, XMVectorSplatW(pt));
            }
        }
    }
}

XMMATRIX CascadedShadowMap::CalcCascadeViewProj(const XMMATRIX& cameraView, float fovY, float aspect,
    float nearZ, float farZ, const XMFLOAT3& lightDir)
{
    // проекция только этого куска фрустума камеры: тот же угол обзора, но свои ближняя и дальняя плоскости
    XMMATRIX sliceProj = XMMatrixPerspectiveFovLH(fovY, aspect, nearZ, farZ);

    XMVECTOR corners[8];
    GetFrustumCornersWorldSpace(cameraView, sliceProj, corners);

    // центр куска - среднее всех углов, на него и смотрит "камера света"
    XMVECTOR center = XMVectorZero();
    for (const XMVECTOR& v : corners)
        center = XMVectorAdd(center, v);
    center = XMVectorScale(center, 1.0f / 8.0f);

    // view матрица света: стоит в центре куска и смотрит вдоль лучей.
    // Вектор "вверх" не должен совпадать с направлением света, иначе матрица вырождается
    XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&lightDir));
    XMVECTOR up = (fabsf(XMVectorGetY(dir)) > 0.99f) ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
    XMMATRIX lightView = XMMatrixLookAtLH(center, XMVectorAdd(center, dir), up);

    // переводим углы в пространство света и находим коробку, в которую они помещаются
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();
    for (const XMVECTOR& v : corners)
    {
        XMFLOAT3 trf;
        XMStoreFloat3(&trf, XMVector3TransformCoord(v, lightView));

        minX = std::min(minX, trf.x);
        maxX = std::max(maxX, trf.x);
        minY = std::min(minY, trf.y);
        maxY = std::max(maxY, trf.y);
        minZ = std::min(minZ, trf.z);
        maxZ = std::max(maxZ, trf.z);
    }

    // Растягиваем коробку по глубине. Тень на кусок могут отбрасывать объекты,
    // которые сами в кусок не попали, но стоят между ним и солнцем.
    // Без растяжения они обрезались бы ближней плоскостью и тени от них пропали бы
    const float zMult = 10.0f;
    minZ = (minZ < 0) ? minZ * zMult : minZ / zMult;
    maxZ = (maxZ < 0) ? maxZ / zMult : maxZ * zMult;

    // у направленного света лучи параллельны, поэтому проекция ортогональная
    XMMATRIX lightProj = XMMatrixOrthographicOffCenterLH(minX, maxX, minY, maxY, minZ, maxZ);

    return lightView * lightProj;
}

void CascadedShadowMap::Update(const XMMATRIX& cameraView, float fovY, float aspect, float cameraNear,
    float shadowDistance, float lambda, const XMFLOAT3& lightDir,
    bool pcfEnabled, bool showCascades)
{
    // Нелинейное распределение каскадов (practical split scheme).
    // Равномерное деление: C = n + (f - n) * i / N - каскады одинаковой длины,
    //   вблизи тени получаются слишком грубыми.
    // Логарифмическое деление: C = n * (f / n)^(i / N) - каждый следующий каскад в одно и то же
    //   число раз длиннее предыдущего, вблизи короткие и детальные, вдали длинные.
    //   Но при маленьком n первый каскад выходит крошечным.
    // Поэтому берём смесь: C = lambda * лог + (1 - lambda) * равном.
    float splits[kCascadeCount + 1];
    splits[0] = cameraNear;
    for (UINT i = 1; i <= kCascadeCount; ++i)
    {
        float p = (float)i / (float)kCascadeCount;
        float logSplit = cameraNear * powf(shadowDistance / cameraNear, p);
        float uniformSplit = cameraNear + (shadowDistance - cameraNear) * p;
        splits[i] = lambda * logSplit + (1.0f - lambda) * uniformSplit;
    }

    CascadeConstants cc = {};
    float distances[kCascadeCount];

    for (UINT i = 0; i < kCascadeCount; ++i)
    {
        XMMATRIX viewProj = CalcCascadeViewProj(cameraView, fovY, aspect, splits[i], splits[i + 1], lightDir);

        XMStoreFloat4x4(&mCascadeViewProj[i], viewProj);
        XMStoreFloat4x4(&cc.ViewProj[i], XMMatrixTranspose(viewProj));
        distances[i] = splits[i + 1];
    }

    cc.Distances = XMFLOAT4(distances[0], distances[1], distances[2], distances[3]);
    XMStoreFloat4x4(&cc.CameraView, XMMatrixTranspose(cameraView));
    cc.Params = XMFLOAT4(1.0f / (float)mMapSize, pcfEnabled ? 1.0f : 0.0f, showCascades ? 1.0f : 0.0f, (float)mMapSize);

    memcpy(mCascadeCBData, &cc, sizeof(cc));
}

void CascadedShadowMap::TransitionTo(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = mShadowMap.Get();
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);
}

void CascadedShadowMap::Clear(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->ClearDepthStencilView(GetDsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
}

D3D12_CPU_DESCRIPTOR_HANDLE CascadedShadowMap::GetDsv() const
{
    return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}