
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wincodec.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cmath>

#include "Gbuffer.h"
#include "RenderingSystem.h"
// тут подключаем нужные библиотеки, чтобы линкер не ругался
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using namespace DirectX;
using namespace Microsoft::WRL;

// размер окна
const UINT kClientWidth = 1024;
const UINT kClientHeight = 768;
const UINT kFrameCount = 2; // количество бэк-буферов свапчейна


inline void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        sprintf_s(buf, "Ошибка: %s (HRESULT = 0x%08X)", what, (unsigned int)hr);
        throw std::runtime_error(buf);
    }
}

// константные буферы в D3D12 должны быть выровнены по 256 байт,
// эта функция просто округляет размер вверх до 256
inline UINT CalcCBSize(UINT byteSize)
{
    return (byteSize + 255) & ~255;
}


// Вершина нашей модели: позиция, нормаль (для света) и текстурные координаты
struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexC;
};

// Это лежит в константном буфере b0 - данные на каждый объект (сабмеш)
struct ObjectConstants
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 TexTransform; // тут тайлинг + анимация текстуры
    XMFLOAT4   DiffuseAlbedo;
};

// Это лежит в константном буфере b1 - общие данные кадра (камера)
struct PassConstants
{
    XMFLOAT4X4 ViewProj;
};

// Информация о материале, которую мы вытащили из .mtl файла
struct MaterialData
{
    std::string Name;
    XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f }; // цвет Kd
    std::string DiffuseMapFile; // имя файла текстуры (map_Kd), может быть пустым

    // Тайлинг и анимация - в обычном .mtl таких полей нет, поэтому
    // мы их просто выставляем в коде (см. функцию SetupMaterialAnimation)
    XMFLOAT2 TileScale = { 1.0f, 1.0f };
    XMFLOAT2 ScrollSpeed = { 0.0f, 0.0f };

    int SrvIndex = 0; // индекс дескриптора текстуры в куче (0 = дефолтная белая текстура)
};

// Один "кусок" модели, который рисуется одним вызовом DrawIndexed
// и использует один материал
struct Submesh
{
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int MaterialIndex = -1;
};



// маленький помощник - грузит .mtl файл и возвращает список материалов
std::vector<MaterialData> LoadMtl(const std::string& path)
{
    std::vector<MaterialData> materials;
    std::ifstream file(path);
    if (!file.is_open())
        return materials; // если файла нет - вернём пустой список, не страшно

    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream ss(line);
        std::string token;
        ss >> token;

        if (token == "newmtl")
        {
            MaterialData m;
            ss >> m.Name;
            materials.push_back(m);
        }
        else if (token == "Kd" && !materials.empty())
        {
            float r, g, b;
            ss >> r >> g >> b;
            materials.back().DiffuseAlbedo = XMFLOAT4(r, g, b, 1.0f);
        }
        else if (token == "map_Kd" && !materials.empty())
        {
            std::string texName;
            ss >> texName;
            materials.back().DiffuseMapFile = texName;
        }
    }
    return materials;
}

// загрузка модели из .obj. folder - папка, где лежит .obj и всё что он подключает
struct LoadedModel
{
    std::vector<Vertex> Vertices;
    std::vector<uint32_t> Indices;
    std::vector<Submesh> Submeshes;
    std::vector<MaterialData> Materials;
};

LoadedModel LoadObjModel(const std::string& folder, const std::string& objFileName)
{
    LoadedModel model;

    std::ifstream file(folder + "\\" + objFileName);
    if (!file.is_open())
        throw std::runtime_error("Не получилось открыть " + objFileName + " - положи модель в папку obj!");

    // тут временно копим то, что прочитали из файла
    std::vector<XMFLOAT3> positions;
    std::vector<XMFLOAT3> normals;
    std::vector<XMFLOAT2> texcoords;

    // т.к. в obj у каждого индекса грани свой набор (позиция/uv/нормаль),
    // а нам нужна "одна" вершина со всеми данными сразу - делаем кэш уникальных вершин
    std::unordered_map<std::string, uint32_t> uniqueVerts;

    int currentMaterial = -1;

    // лямбда, которая по строке вида "5/2/1" находит/создаёт вершину и
    // возвращает её индекс в итоговом массиве Vertices
    auto GetVertexIndex = [&](const std::string& token) -> uint32_t
    {
        auto it = uniqueVerts.find(token);
        if (it != uniqueVerts.end())
            return it->second;

        // разбираем строку "posIdx/texIdx/normIdx" (texIdx или normIdx могут отсутствовать)
        int posIdx = 0, texIdx = 0, normIdx = 0;
        size_t firstSlash = token.find('/');
        if (firstSlash == std::string::npos)
        {
            posIdx = std::stoi(token);
        }
        else
        {
            posIdx = std::stoi(token.substr(0, firstSlash));
            size_t secondSlash = token.find('/', firstSlash + 1);
            if (secondSlash == std::string::npos)
            {
                // вида "v//" не бывает на практике без второго слэша, но всё равно проверим
                std::string texPart = token.substr(firstSlash + 1);
                if (!texPart.empty()) texIdx = std::stoi(texPart);
            }
            else
            {
                std::string texPart = token.substr(firstSlash + 1, secondSlash - firstSlash - 1);
                if (!texPart.empty()) texIdx = std::stoi(texPart);

                std::string normPart = token.substr(secondSlash + 1);
                if (!normPart.empty()) normIdx = std::stoi(normPart);
            }
        }

        Vertex v;
        // индексы в obj начинаются с 1, поэтому -1
        v.Pos = (posIdx > 0) ? positions[posIdx - 1] : XMFLOAT3(0, 0, 0);
        v.TexC = (texIdx > 0) ? texcoords[texIdx - 1] : XMFLOAT2(0, 0);
        v.Normal = (normIdx > 0) ? normals[normIdx - 1] : XMFLOAT3(0, 1, 0);

        uint32_t newIndex = (uint32_t)model.Vertices.size();
        model.Vertices.push_back(v);
        uniqueVerts[token] = newIndex;
        return newIndex;
    };

    std::string line;
    while (std::getline(file, line))
    {
        std::istringstream ss(line);
        std::string token;
        ss >> token;

        if (token == "v")
        {
            float x, y, z;
            ss >> x >> y >> z;
            positions.push_back(XMFLOAT3(x, y, z));
        }
        else if (token == "vn")
        {
            float x, y, z;
            ss >> x >> y >> z;
            normals.push_back(XMFLOAT3(x, y, z));
        }
        else if (token == "vt")
        {
            float u, v;
            ss >> u >> v;
            // в OBJ ось V обычно идёт снизу вверх, а в текстурах сверху вниз,
            // поэтому многие модели уже учитывают это сами. Если текстура
            // окажется "вверх ногами" - попробуй раскомментировать строку ниже:
            // v = 1.0f - v;
            texcoords.push_back(XMFLOAT2(u, v));
        }
        else if (token == "mtllib")
        {
            std::string mtlName;
            ss >> mtlName;
            model.Materials = LoadMtl(folder + "\\" + mtlName);
        }
        else if (token == "usemtl")
        {
            std::string matName;
            ss >> matName;
            currentMaterial = -1;
            for (size_t i = 0; i < model.Materials.size(); ++i)
            {
                if (model.Materials[i].Name == matName)
                {
                    currentMaterial = (int)i;
                    break;
                }
            }

            // начинаем новый сабмеш для этого материала
            Submesh sm;
            sm.MaterialIndex = currentMaterial;
            sm.StartIndexLocation = (UINT)model.Indices.size();
            model.Submeshes.push_back(sm);
        }
        else if (token == "f")
        {
            // если ни одного usemtl не было - создадим сабмеш без материала
            if (model.Submeshes.empty())
            {
                Submesh sm;
                sm.MaterialIndex = -1;
                sm.StartIndexLocation = 0;
                model.Submeshes.push_back(sm);
            }

            // читаем все вершины этой грани (может быть 3, 4 и больше - полигон)
            std::vector<uint32_t> faceIndices;
            std::string vertToken;
            while (ss >> vertToken)
                faceIndices.push_back(GetVertexIndex(vertToken));

            // режем полигон на треугольники "веером" от первой вершины
            for (size_t i = 1; i + 1 < faceIndices.size(); ++i)
            {
                model.Indices.push_back(faceIndices[0]);
                model.Indices.push_back(faceIndices[i]);
                model.Indices.push_back(faceIndices[i + 1]);
            }
        }
        // остальные токены (комментарии "#", "s" - сглаживание, и т.д.) просто игнорируем
    }

    // считаем сколько индексов попало в каждый сабмеш
    for (size_t i = 0; i < model.Submeshes.size(); ++i)
    {
        UINT endIndex = (i + 1 < model.Submeshes.size())
            ? model.Submeshes[i + 1].StartIndexLocation
            : (UINT)model.Indices.size();
        model.Submeshes[i].IndexCount = endIndex - model.Submeshes[i].StartIndexLocation;
    }

    return model;
}


class App
{
public:
    void Init(HWND hwnd);
    void Update();
    void Draw();
    void Destroy();

private:
    // ---- базовая инициализация DX12 ----
    void CreateDeviceAndQueue();
    void CreateSwapChain(HWND hwnd);
    void CreateDescriptorHeaps();
    void CreateRenderTargets();
    void CreateDepthStencil();
   // void CreateRootSignature();
    void CreatePipelineState();
    void CreateCommandObjects();
    void CreateFence();

    // ---- загрузка модели и текстур ----
    void LoadModelAndTextures();
    void SetupMaterialAnimation(); // тут вручную выставляем тайлинг/скорость анимации
    void SetupSceneLights(); // расставляем источники света по сцене (Point/Directional/Spot)
    void UploadBufferData(const void* data, UINT64 size, ComPtr<ID3D12Resource>& outDefaultBuffer);
    void CreateTextureFromWicFile(const std::wstring& filename, int srvSlot);
    void CreateDefaultWhiteTexture(int srvSlot);
    void CreateTextureResourceAndUpload(UINT width, UINT height, const BYTE* pixels, UINT srcRowPitch, int srvSlot);

    // ---- помощники ----
    void FlushCommandQueue();

private:
    // основные объекты D3D12
    ComPtr<ID3D12Device> mDevice;
    ComPtr<ID3D12CommandQueue> mCommandQueue;
    ComPtr<IDXGISwapChain3> mSwapChain;
    ComPtr<ID3D12CommandAllocator> mCommandAllocator;
    ComPtr<ID3D12GraphicsCommandList> mCommandList;

    // кучи дескрипторов
    ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    ComPtr<ID3D12DescriptorHeap> mDsvHeap;
    ComPtr<ID3D12DescriptorHeap> mSrvHeap; // тут лежат SRV всех текстур
    UINT mRtvDescSize = 0;
    UINT mDsvDescSize = 0;
    UINT mSrvDescSize = 0;

    // бэк-буферы и depth buffer
    ComPtr<ID3D12Resource> mRenderTargets[kFrameCount];
    ComPtr<ID3D12Resource> mDepthStencilBuffer;
    UINT mCurrentBackBuffer = 0;

    // синхронизация CPU/GPU (делаем максимально просто - флуш каждый кадр)
    ComPtr<ID3D12Fence> mFence;
    UINT64 mFenceValue = 0;
    HANDLE mFenceEvent = nullptr;

    // пайплайн (deferred rendering)
    RenderingSystem mRenderingSystem;

    // источники света сцены (Point/Directional/Spot) - см. SetupSceneLights()
    std::vector<Light> mLights;
    // геометрия модели
    ComPtr<ID3D12Resource> mVertexBuffer;
    ComPtr<ID3D12Resource> mIndexBuffer;
    D3D12_VERTEX_BUFFER_VIEW mVbv = {};
    D3D12_INDEX_BUFFER_VIEW mIbv = {};

    // загруженные данные модели/материалов
    LoadedModel mModel;

    // константные буферы (в Upload куче, замапленные на постоянку)
    ComPtr<ID3D12Resource> mObjectCB;
    BYTE* mObjectCBData = nullptr;
    UINT mObjectCBElementSize = 0;

    ComPtr<ID3D12Resource> mPassCB;
    BYTE* mPassCBData = nullptr;

    // вспомогательные ресурсы для загрузки текстур (нужно держать живыми пока GPU не скопирует)
    std::vector<ComPtr<ID3D12Resource>> mTextureUploadHeaps;
    std::vector<ComPtr<ID3D12Resource>> mTextures; // сами текстуры (default heap)

    // таймер
    LARGE_INTEGER mFreq = {};
    LARGE_INTEGER mStartTime = {};

    UINT mClientWidth = kClientWidth;
    UINT mClientHeight = kClientHeight;

    XMFLOAT3 mCameraPos = { 0.0f, 150.0f, -500.0f }; // нужна lighting pass для specular
};


void App::Init(HWND hwnd)
{
    QueryPerformanceFrequency(&mFreq);
    QueryPerformanceCounter(&mStartTime);

    // WIC (загрузка картинок) требует COM
    ThrowIfFailed(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx");

    CreateDeviceAndQueue();
    CreateCommandObjects();
    CreateFence();
    CreateSwapChain(hwnd);
    CreateDescriptorHeaps();
    CreateRenderTargets();
    CreateDepthStencil();
    LoadModelAndTextures();

    mRenderingSystem.Init(mDevice.Get(), mClientWidth, mClientHeight,
        mSrvHeap.Get(), mSrvDescSize, /*srvStartSlot=*/60,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT);

    SetupSceneLights();

    // дожидаемся пока всё что мы накомандовали на загрузку реально выполнится на GPU
    ThrowIfFailed(mCommandList->Close(), "CommandList->Close (init)");
    ID3D12CommandList* lists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, lists);
    FlushCommandQueue();
}

void App::CreateDeviceAndQueue()
{
#if defined(_DEBUG)
    // включаем debug слой, чтобы D3D12 ругался в Output если мы что-то делаем не так
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");

    // пробуем создать устройство на реальной видеокарте
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice));
    if (FAILED(hr))
    {
        // если не получилось (например нет нормальной видеокарты) - берём программный WARP адаптер
        ComPtr<IDXGIAdapter> warpAdapter;
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)), "EnumWarpAdapter");
        ThrowIfFailed(D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&mDevice)), "D3D12CreateDevice (warp)");
    }

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(mDevice->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&mCommandQueue)), "CreateCommandQueue");
}

void App::CreateCommandObjects()
{
    ThrowIfFailed(mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCommandAllocator)), "CreateCommandAllocator");
    ThrowIfFailed(mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&mCommandList)), "CreateCommandList");
    // командный лист создаётся уже открытым, это нам и нужно для загрузки ресурсов в Init()
}

void App::CreateFence()
{
    ThrowIfFailed(mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence)), "CreateFence");
    mFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void App::CreateSwapChain(HWND hwnd)
{
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1 (swapchain)");

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = mClientWidth;
    desc.Height = mClientHeight;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kFrameCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(mCommandQueue.Get(), hwnd, &desc, nullptr, nullptr, &swapChain1), "CreateSwapChainForHwnd");
    ThrowIfFailed(swapChain1.As(&mSwapChain), "SwapChain As IDXGISwapChain3");

    mCurrentBackBuffer = mSwapChain->GetCurrentBackBufferIndex();
}

void App::CreateDescriptorHeaps()
{
    // RTV куча - под бэк-буферы (kFrameCount штук)
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = kFrameCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&mRtvHeap)), "CreateDescriptorHeap RTV");
    mRtvDescSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // DSV куча - под depth buffer (1 штука)
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&mDsvHeap)), "CreateDescriptorHeap DSV");
    mDsvDescSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    // SRV куча - под текстуры. Берём с запасом 16 слотов, нам хватит на любую модельку из домашки
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = 64; //Лимит тестур
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // эта куча видна шейдерам
    ThrowIfFailed(mDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&mSrvHeap)), "CreateDescriptorHeap SRV");
    mSrvDescSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void App::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mRenderTargets[i])), "SwapChain->GetBuffer");
        mDevice->CreateRenderTargetView(mRenderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += mRtvDescSize;
    }
}

void App::CreateDepthStencil()
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = mClientWidth;
    desc.Height = mClientHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    ThrowIfFailed(mDevice->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(&mDepthStencilBuffer)), "CreateCommittedResource DepthStencil");

    mDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(), nullptr, mDsvHeap->GetCPUDescriptorHandleForHeapStart());
}

// 
// КОРНЕВАЯ СИГНАТУРА (Root Signature)
//
// У нас 3 штуки:
//  - b0: константы объекта (мир, тайлинг текстуры, цвет материала) - корневой дескриптор
//  - b1: константы кадра (камера) - корневой дескриптор
//  - t0: таблица с одной SRV (текстура) - таблица дескрипторов
// Плюс статический сэмплер s0, чтобы не делать отдельную кучу под сэмплеры

//void App::CreateRootSignature()
//{
//    D3D12_DESCRIPTOR_RANGE srvRange = {};
//    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
//    srvRange.NumDescriptors = 1;
//    srvRange.BaseShaderRegister = 0; // t0
//
//    D3D12_ROOT_PARAMETER params[3] = {};
//
//    // b0 - per object constants
//    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
//    params[0].Descriptor.ShaderRegister = 0;
//    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
//
//    // b1 - per pass constants
//    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
//    params[1].Descriptor.ShaderRegister = 1;
//    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
//
//    // t0 - текстура (через таблицу дескрипторов)
//    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
//    params[2].DescriptorTable.NumDescriptorRanges = 1;
//    params[2].DescriptorTable.pDescriptorRanges = &srvRange;
//    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
//
//    // статический сэмплер - линейная фильтрация + wrap (для тайлинга это то что нужно)
//    D3D12_STATIC_SAMPLER_DESC sampler = {};
//    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
//    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
//    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
//    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
//    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
//    sampler.MaxLOD = D3D12_FLOAT32_MAX;
//    sampler.ShaderRegister = 0; // s0
//    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
//
//    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
//    rsDesc.NumParameters = (UINT)(sizeof(params) / sizeof(params[0]));
//    rsDesc.pParameters = params;
//    rsDesc.NumStaticSamplers = 1;
//    rsDesc.pStaticSamplers = &sampler;
//    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
//
//    ComPtr<ID3DBlob> serialized, errorBlob;
//    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errorBlob);
//    if (FAILED(hr))
//    {
//        if (errorBlob)
//            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
//        ThrowIfFailed(hr, "D3D12SerializeRootSignature");
//    }
//
//    ThrowIfFailed(mDevice->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)), "CreateRootSignature");
//}

 
// PIPELINE STATE (компилируем шейдеры и собираем PSO)
//void App::CreatePipelineState()
//{
//    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
//
//    UINT compileFlags = 0;
//#if defined(_DEBUG)
//    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
//#endif
//
//    HRESULT hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, nullptr, "VS", "vs_5_0", compileFlags, 0, &vsBlob, &errorBlob);
//    if (FAILED(hr))
//    {
//        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
//        ThrowIfFailed(hr, "Compile VS (проверь что Shaders.hlsl лежит рядом с exe)");
//    }
//
//    hr = D3DCompileFromFile(L"Shaders.hlsl", nullptr, nullptr, "PS", "ps_5_0", compileFlags, 0, &psBlob, &errorBlob);
//    if (FAILED(hr))
//    {
//        if (errorBlob) OutputDebugStringA((char*)errorBlob->GetBufferPointer());
//        ThrowIfFailed(hr, "Compile PS");
//    }
//
//    // описываем формат вершины - должно совпадать со структурой Vertex
//    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
//    {
//        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
//        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
//        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
//    };
//
//    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
//    psoDesc.pRootSignature = mRootSignature.Get();
//    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
//    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
//    psoDesc.InputLayout = { inputLayout, (UINT)(sizeof(inputLayout) / sizeof(inputLayout[0])) };
//
//    // растеризация - обычная, рисуем закрашенные треугольники
//    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
//    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
//    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
//    psoDesc.RasterizerState.DepthClipEnable = TRUE;
//
//    // блендинг - выключен, текстуры у нас без прозрачности
//    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
//
//    // depth test - включен, как обычно для 3D
//    psoDesc.DepthStencilState.DepthEnable = TRUE;
//    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
//    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
//    psoDesc.DepthStencilState.StencilEnable = FALSE;
//
//    psoDesc.SampleMask = UINT_MAX;
//    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
//    psoDesc.NumRenderTargets = 1;
//    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
//    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
//    psoDesc.SampleDesc.Count = 1;
//
//    ThrowIfFailed(mDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPSO)), "CreateGraphicsPipelineState");
//}


// Загрузка данных в GPU буфер (вершины/индексы).
// Делаем через промежуточный Upload буфер, как рассказывали на лекции про текстуры -
// сначала кладём данные в Upload кучу, потом командой CopyResource переносим в Default кучу.

void App::UploadBufferData(const void* data, UINT64 size, ComPtr<ID3D12Resource>& outDefaultBuffer)
{
    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = size;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // целевой буфер - в быстрой видеопамяти (Default), сразу ставим как COPY_DEST
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&outDefaultBuffer)), "CreateCommittedResource (default buffer)");

    // промежуточный буфер - в Upload куче, сюда CPU может писать напрямую
    ComPtr<ID3D12Resource> uploadBuffer;
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)), "CreateCommittedResource (upload buffer)");

    // копируем наши данные в upload буфер
    void* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, &mapped), "Map upload buffer");
    memcpy(mapped, data, size);
    uploadBuffer->Unmap(0, nullptr);

    // командуем GPU скопировать из upload в default буфер
    mCommandList->CopyResource(outDefaultBuffer.Get(), uploadBuffer.Get());

    // переводим default буфер в состояние, в котором его можно использовать для рисования
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = outDefaultBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mCommandList->ResourceBarrier(1, &barrier);

    // upload буфер должен дожить до того момента когда GPU реально выполнит копирование,
    // поэтому держим его в общем списке и не отпускаем пока не сделаем Flush в конце Init()
    mTextureUploadHeaps.push_back(uploadBuffer);
}
// ЗАГРУЗКА ТЕКСТУРЫ ИЗ ФАЙЛА С ПОМОЩЬЮ WIC (Windows Imaging Component)
//
// WIC - стандартный способ читать PNG/JPG/BMP в Windows без сторонних библиотек.
// Достаём пиксели в формате RGBA8 и заливаем их в текстуру D3D12, как
// показывали на слайдах с CreateTextureFromScratch.

void App::CreateTextureResourceAndUpload(UINT width, UINT height, const BYTE* pixels, UINT srcRowPitch, int srvSlot)
{
    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1; // мипмапы не делаем, для домашки хватит и одного уровня
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "CreateCommittedResource (texture)");

    // узнаём у девайса, сколько байт нужно для промежуточного (upload) буфера
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    mDevice->GetCopyableFootprints(&texDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = totalBytes;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBuffer;
    ThrowIfFailed(mDevice->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuffer)), "CreateCommittedResource (texture upload)");

    // копируем картинку построчно - у исходных пикселей и у GPU буфера может быть разный rowPitch
    BYTE* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, (void**)&mapped), "Map texture upload buffer");
    for (UINT y = 0; y < height; ++y)
    {
        memcpy(mapped + footprint.Footprint.RowPitch * y, pixels + srcRowPitch * y, min((UINT)rowSizeInBytes, srcRowPitch));
    }
    uploadBuffer->Unmap(0, nullptr);

    // командуем GPU скопировать из upload буфера в текстуру
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    mCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // переводим текстуру в состояние "можно читать из пиксельного шейдера"
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mCommandList->ResourceBarrier(1, &barrier);

    // создаём SRV (Shader Resource View) для этой текстуры в нашей куче дескрипторов
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE handle = mSrvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)srvSlot * mSrvDescSize;
    mDevice->CreateShaderResourceView(texture.Get(), &srvDesc, handle);

    // держим upload буфер и саму текстуру живыми
    mTextureUploadHeaps.push_back(uploadBuffer);
    mTextures.push_back(texture);
}

// грузит файл картинки через WIC и создаёт из неё текстуру в указанный слот SRV кучи
void App::CreateTextureFromWicFile(const std::wstring& filename, int srvSlot)
{
    ComPtr<IWICImagingFactory> wicFactory;
    ThrowIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wicFactory)), "CoCreateInstance WICImagingFactory");

    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = wicFactory->CreateDecoderFromFilename(filename.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr))
    {
        // если файл текстуры не нашёлся - не страшно, просто будет белый цвет (* цвет материала)
        OutputDebugStringW((L"Не удалось открыть текстуру: " + filename + L" - использую белую текстуру\n").c_str());
        return;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    ThrowIfFailed(decoder->GetFrame(0, &frame), "WIC GetFrame");

    // переводим картинку в формат RGBA8 - именно его мы и зальём в текстуру
    ComPtr<IWICFormatConverter> converter;
    ThrowIfFailed(wicFactory->CreateFormatConverter(&converter), "CreateFormatConverter");
    ThrowIfFailed(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom), "WIC Converter Initialize");

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);

    UINT rowPitch = width * 4; // 4 байта на пиксель (RGBA8)
    std::vector<BYTE> pixels(rowPitch * height);
    ThrowIfFailed(converter->CopyPixels(nullptr, rowPitch, (UINT)pixels.size(), pixels.data()), "WIC CopyPixels");

    CreateTextureResourceAndUpload(width, height, pixels.data(), rowPitch, srvSlot);
}

// создаёт текстуру 1x1 белого цвета - используем для материалов без картинки (map_Kd),
// тогда итоговый цвет будет просто = DiffuseAlbedo материала
void App::CreateDefaultWhiteTexture(int srvSlot)
{
    BYTE whitePixel[4] = { 255, 255, 255, 255 };
    CreateTextureResourceAndUpload(1, 1, whitePixel, 4, srvSlot);
}
// Тут мы вручную настраиваем тайлинг и анимацию для текстур по их названию материала.
// Это и есть та часть домашки, где "Добавьте текстурную анимацию и тайлинг".
// Если хочешь поменять под свою модель - просто измени тут значения

void App::SetupMaterialAnimation()
{
    for (auto& m : mModel.Materials)
    {
        // тайлинг - сколько раз текстура повторяется по U и V
        m.TileScale = XMFLOAT2(2.0f, 2.0f);

        // скорость анимации (сдвиг текстурных координат в секунду)
        // если поставить 0,0 - текстура будет статичная, без анимации
        m.ScrollSpeed = XMFLOAT2(0.1f, 0.05f);
    }
}
 
// Расставляем источники света по сцене: один Directional (солнце с ambient),
// и несколько Point/Spot, разбросанных вокруг модели (для Sponza - по всему холлу)
 
void App::SetupSceneLights()
{
    mLights.clear();

    // ---- Directional - "солнце", единственный источник, дающий ambient (слайд 13) ----
    Light sun;
    sun.Type = (int)LightType::Directional;
    sun.Direction = XMFLOAT3(0.5f, -1.0f, 0.3f);
    sun.Color = XMFLOAT3(1.0f, 0.95f, 0.85f);
    sun.Intensity = 1.0f;
    mLights.push_back(sun);

    // ---- Point light 1 ----
    Light p1;
    p1.Type = (int)LightType::Point;
    p1.Position = XMFLOAT3(-300.0f, 200.0f, 0.0f);
    p1.Range = 600.0f;
    p1.Color = XMFLOAT3(1.0f, 0.3f, 0.2f);
    p1.Intensity = 3.0f;
    mLights.push_back(p1);

    // ---- Point light 2 ----
    Light p2;
    p2.Type = (int)LightType::Point;
    p2.Position = XMFLOAT3(300.0f, 200.0f, 0.0f);
    p2.Range = 600.0f;
    p2.Color = XMFLOAT3(0.2f, 0.4f, 1.0f);
    p2.Intensity = 3.0f;
    mLights.push_back(p2);

    // ---- Point light 3 (в глубине сцены) ----
    Light p3;
    p3.Type = (int)LightType::Point;
    p3.Position = XMFLOAT3(0.0f, 300.0f, 800.0f);
    p3.Range = 800.0f;
    p3.Color = XMFLOAT3(0.3f, 1.0f, 0.3f);
    p3.Intensity = 2.5f;
    mLights.push_back(p3);

    // ---- Spot light - "прожектор" сверху, направленный вниз на центр сцены ----
    Light spot;
    spot.Type = (int)LightType::Spot;
    spot.Position = XMFLOAT3(0.0f, 600.0f, 0.0f);
    spot.Direction = XMFLOAT3(0.0f, -1.0f, 0.0f);
    spot.Range = 1000.0f;
    spot.SpotPower = 8.0f;
    spot.Color = XMFLOAT3(1.0f, 1.0f, 1.0f);
    spot.Intensity = 4.0f;
    mLights.push_back(spot);
}

// Загружаем модель и все её текстуры

void App::LoadModelAndTextures()
{
    // ------ грузим геометрию и материалы из obj/model.obj ------
    mModel = LoadObjModel("obj", "model.obj");

    if (mModel.Vertices.empty())
        throw std::runtime_error("Модель пустая - проверь файл obj/model.obj");

    // настраиваем тайлинг/анимацию текстур
    SetupMaterialAnimation();
    
    // ------ вершинный и индексный буфер ------
    UploadBufferData(mModel.Vertices.data(), mModel.Vertices.size() * sizeof(Vertex), mVertexBuffer);
    UploadBufferData(mModel.Indices.data(), mModel.Indices.size() * sizeof(uint32_t), mIndexBuffer);

    mVbv.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
    mVbv.StrideInBytes = sizeof(Vertex);
    mVbv.SizeInBytes = (UINT)(mModel.Vertices.size() * sizeof(Vertex));

    mIbv.BufferLocation = mIndexBuffer->GetGPUVirtualAddress();
    mIbv.Format = DXGI_FORMAT_R32_UINT;
    mIbv.SizeInBytes = (UINT)(mModel.Indices.size() * sizeof(uint32_t));

    // ------ текстуры ------
    // слот 0 в SRV куче - всегда дефолтная белая текстура (для материалов без картинки)
    CreateDefaultWhiteTexture(0);
    int nextSrvSlot = 1;

    for (auto& mat : mModel.Materials)
    {
        if (!mat.DiffuseMapFile.empty())
        {
            // путь вида "obj/имя_файла.png"
            std::string fullPath = "obj\\" + mat.DiffuseMapFile;
            OutputDebugStringA(("Гружу: " + fullPath + "\n").c_str()); // добавь эту строку
            std::wstring wPath(fullPath.begin(), fullPath.end());

            mat.SrvIndex = nextSrvSlot;
            CreateTextureFromWicFile(wPath, nextSrvSlot);
            nextSrvSlot++;
        }
        else
        {
            // нет текстуры в материале - используем дефолтную белую (слот 0)
            mat.SrvIndex = 0;
        }
    }

    // если в модели вообще нет материалов (например, .obj без mtllib) -
    // на всякий случай добьём список одним материалом по умолчанию
    if (mModel.Materials.empty())
    {
        MaterialData def;
        def.SrvIndex = 0;
        def.TileScale = XMFLOAT2(1, 1);
        mModel.Materials.push_back(def);
        for (auto& sm : mModel.Submeshes)
            if (sm.MaterialIndex < 0) sm.MaterialIndex = 0;
    }

    // ------ константные буферы ------
    // под per-object данные - один блок на каждый сабмеш
    mObjectCBElementSize = CalcCBSize(sizeof(ObjectConstants));
    UINT objectCBTotalSize = mObjectCBElementSize * (UINT)mModel.Submeshes.size();

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = objectCBTotalSize;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mObjectCB)), "CreateCommittedResource ObjectCB");
    ThrowIfFailed(mObjectCB->Map(0, nullptr, (void**)&mObjectCBData), "Map ObjectCB");

    // под per-pass данные (камера) - один блок на 256 байт
    D3D12_RESOURCE_DESC passDesc = cbDesc;
    passDesc.Width = CalcCBSize(sizeof(PassConstants));
    ThrowIfFailed(mDevice->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &passDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mPassCB)), "CreateCommittedResource PassCB");
    ThrowIfFailed(mPassCB->Map(0, nullptr, (void**)&mPassCBData), "Map PassCB");
}
// UPDATE - тут считаем камеру и обновляем константные буферы

void App::Update()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float totalTime = (float)(now.QuadPart - mStartTime.QuadPart) / (float)mFreq.QuadPart;

    // камера крутится вокруг модели по кругу, чтобы было видно объект со всех сторон
    float radius = 5.0f;
    float camX = sinf(totalTime * 0.5f) * radius;
    float camZ = cosf(totalTime * 0.5f) * radius;

    XMVECTOR eyePos = XMVectorSet(mCameraPos.x, mCameraPos.y, mCameraPos.z, 1.0f);
    XMVECTOR target = XMVectorSet(0.0f, 150.0f, 0.0f, 1.0f);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(eyePos, target, up);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, (float)mClientWidth / (float)mClientHeight, 1.0f, 10000.0f);//дальность прорисовки

    PassConstants passCB;
    XMStoreFloat4x4(&passCB.ViewProj, XMMatrixTranspose(view * proj));
    memcpy(mPassCBData, &passCB, sizeof(passCB));

    // для каждого сабмеша обновляем мировую матрицу и матрицу текстурного трансформа
    for (size_t i = 0; i < mModel.Submeshes.size(); ++i)
    {
        const Submesh& sm = mModel.Submeshes[i];
        const MaterialData& mat = mModel.Materials[sm.MaterialIndex >= 0 ? sm.MaterialIndex : 0];

        ObjectConstants objCB;
        XMStoreFloat4x4(&objCB.World, XMMatrixIdentity()); // модель стоит в центре, можно тут крутить/двигать

        // тайлинг - просто масштаб UV координат
        XMMATRIX tileScale = XMMatrixScaling(mat.TileScale.x, mat.TileScale.y, 1.0f);

        // анимация - сдвигаем UV координаты по времени (текстура "ползёт")
        float offsetU = totalTime * mat.ScrollSpeed.x;
        float offsetV = totalTime * mat.ScrollSpeed.y;
        XMMATRIX scroll = XMMatrixTranslation(offsetU, offsetV, 0.0f);

        XMMATRIX texTransform = tileScale * scroll;
        XMStoreFloat4x4(&objCB.TexTransform, XMMatrixTranspose(texTransform));

        objCB.DiffuseAlbedo = mat.DiffuseAlbedo;

        memcpy(mObjectCBData + i * mObjectCBElementSize, &objCB, sizeof(objCB));
    }
}


// DRAW - тут рисуем кадр: deferred rendering из двух проходов
//   1) Geometry pass - заполняем G-buffer (без освещения)
//   2) Lighting pass - для каждого источника света накапливаем освещение в бэк-буфере

void App::Draw()
{
    ThrowIfFailed(mCommandAllocator->Reset(), "CommandAllocator->Reset");
    // PSO передаём nullptr - RenderingSystem сам выставит нужный PSO в начале каждого прохода
    ThrowIfFailed(mCommandList->Reset(mCommandAllocator.Get(), nullptr), "CommandList->Reset");

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)mClientWidth, (float)mClientHeight, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG)mClientWidth, (LONG)mClientHeight };
    mCommandList->RSSetViewports(1, &viewport);
    mCommandList->RSSetScissorRects(1, &scissor);

    // подключаем кучу с текстурами / G-buffer SRV
    ID3D12DescriptorHeap* heaps[] = { mSrvHeap.Get() };
    mCommandList->SetDescriptorHeaps(1, heaps);

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = mDsvHeap->GetCPUDescriptorHandleForHeapStart();

    
    // PASS 1: Geometry (opaque) - заполняем G-buffer (слайд 9, 17, 18)
    // =================================================================================
    mRenderingSystem.BeginGeometryPass(mCommandList.Get(), dsvHandle);

    // b1 - константы кадра (камера), общие для всех объектов
    mCommandList->SetGraphicsRootConstantBufferView(1, mPassCB->GetGPUVirtualAddress());

    mCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mCommandList->IASetVertexBuffers(0, 1, &mVbv);
    mCommandList->IASetIndexBuffer(&mIbv);

    for (size_t i = 0; i < mModel.Submeshes.size(); ++i)
    {
        const Submesh& sm = mModel.Submeshes[i];
        if (sm.IndexCount == 0)
            continue;

        const MaterialData& mat = mModel.Materials[sm.MaterialIndex >= 0 ? sm.MaterialIndex : 0];

        D3D12_GPU_VIRTUAL_ADDRESS objCbAddress = mObjectCB->GetGPUVirtualAddress() + i * mObjectCBElementSize;
        mCommandList->SetGraphicsRootConstantBufferView(0, objCbAddress);

        D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = mSrvHeap->GetGPUDescriptorHandleForHeapStart();
        srvHandle.ptr += (SIZE_T)mat.SrvIndex * mSrvDescSize;
        mCommandList->SetGraphicsRootDescriptorTable(2, srvHandle);

        mCommandList->DrawIndexedInstanced(sm.IndexCount, 1, sm.StartIndexLocation, 0, 0);
    }

    mRenderingSystem.EndGeometryPass(mCommandList.Get());

    // =================================================================================
    // PASS 2: Lighting - переводим бэк-буфер в render target, очищаем и
    // накапливаем освещение от каждого источника света (слайды 11, 13, 21, 22)
    // =================================================================================
    D3D12_RESOURCE_BARRIER toRT = {};
    toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRT.Transition.pResource = mRenderTargets[mCurrentBackBuffer].Get();
    toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mCommandList->ResourceBarrier(1, &toRT);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += mCurrentBackBuffer * mRtvDescSize;

    // очищаем бэк-буфер в чёрный - lighting pass будет складывать яркость сверху (слайд 13)
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    mCommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    // камера и матрица ViewProj нужны lighting pass'у для расчёта specular/затухания
    XMFLOAT4X4 viewProj;
    memcpy(&viewProj, mPassCBData, sizeof(XMFLOAT4X4)); // ViewProj лежит первым полем в PassConstants

    mRenderingSystem.RenderLights(mCommandList.Get(), rtvHandle, mLights, viewProj, mCameraPos);

    // переводим бэк-буфер обратно в состояние "готов к показу"
    D3D12_RESOURCE_BARRIER toPresent = {};
    toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toPresent.Transition.pResource = mRenderTargets[mCurrentBackBuffer].Get();
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    mCommandList->ResourceBarrier(1, &toPresent);

    ThrowIfFailed(mCommandList->Close(), "CommandList->Close (draw)");

    ID3D12CommandList* lists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, lists);

    ThrowIfFailed(mSwapChain->Present(1, 0), "Present");

    FlushCommandQueue();

    mCurrentBackBuffer = mSwapChain->GetCurrentBackBufferIndex();
}

void App::FlushCommandQueue()
{
    mFenceValue++;
    ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mFenceValue), "CommandQueue->Signal");

    if (mFence->GetCompletedValue() < mFenceValue)
    {
        ThrowIfFailed(mFence->SetEventOnCompletion(mFenceValue, mFenceEvent), "Fence->SetEventOnCompletion");
        WaitForSingleObject(mFenceEvent, INFINITE);
    }
}

void App::Destroy()
{
    FlushCommandQueue();

    if (mObjectCB) mObjectCB->Unmap(0, nullptr);
    if (mPassCB) mPassCB->Unmap(0, nullptr);

    if (mFenceEvent) CloseHandle(mFenceEvent);
    CoUninitialize();
}


// WIN32 - окно и главный цикл

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
            PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // создаём окно
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DX12HomeworkWindow";
    RegisterClassEx(&wc);

    RECT rect = { 0, 0, (LONG)kClientWidth, (LONG)kClientHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindow(
        wc.lpszClassName, L"DX12 - Текстуры, материалы, тайлинг и анимация",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInstance, nullptr);

    ShowWindow(hwnd, SW_SHOW);

    App app;
    try
    {
        app.Init(hwnd);
    }
    catch (const std::exception& e)
    {
        MessageBoxA(hwnd, e.what(), "Ошибка инициализации", MB_OK | MB_ICONERROR);
        return -1;
    }

    // основной цикл сообщений + рендер
    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            try
            {
                app.Update();
                app.Draw();
            }
            catch (const std::exception& e)
            {
                MessageBoxA(hwnd, e.what(), "Ошибка во время рендера", MB_OK | MB_ICONERROR);
                break;
            }
        }
    }

    app.Destroy();
    return 0;
}
