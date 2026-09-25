// ParticleSystem.h
// Система непрозрачных частиц, которая живёт целиком на GPU.
//
// CPU только решает, сколько частиц родить в этом кадре, и записывает параметры в константы.
// Рождение, движение и смерть частиц считают compute шейдеры (EmitCS и SimulateCS),
// а сколько частиц сейчас живо, знает только GPU, поэтому рисование идёт через
// ExecuteIndirect: аргументы draw call'а (число вершин = число живых частиц) лежат в буфере на GPU.

#pragma once

#include <wrl/client.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>

using namespace DirectX;

// Одна частица в пуле. Раскладка должна совпадать со struct Particle в Particles.hlsl
struct GpuParticle
{
    XMFLOAT3 Position;
    float    Age;
    XMFLOAT3 Velocity;
    float    LifeSpan;
    XMFLOAT4 StartColor;
    XMFLOAT4 EndColor;
    float    StartSize;
    float    EndSize;
    float    Weight;
    UINT     Alive;
};

// Параметры симуляции (cbParticleSim в Particles.hlsl)
struct ParticleSimConstants
{
    XMFLOAT3 EmitterPos;
    float    DeltaTime;
    XMFLOAT3 Gravity;
    UINT     EmitCount;
    XMFLOAT3 Wind;
    UINT     RandomSeed;
    float    GroundHeight;
    UINT     MaxParticles;
    XMFLOAT2 Pad;
};

// Параметры камеры для рисования (cbParticleRender в Particles.hlsl)
struct ParticleRenderConstants
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4X4 InvView;
    float      Curvature;
    XMFLOAT3   Pad;
};

class ParticleSystem
{
public:
    // Создаёт все буферы, UAV (3 слота подряд в SRV-куче, начиная с uavStartSlot),
    // root signature и PSO для compute и для рисования.
    // cmdList должен быть открыт: через него заливаются начальные данные
    void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
        ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT uavStartSlot,
        UINT maxParticles, const DXGI_FORMAT* gbufferFormats, UINT gbufferCount, DXGI_FORMAT dsvFormat);

    // CPU-часть кадра: решает, сколько частиц родить, и записывает константы симуляции и камеры
    void Update(float elapsedTime, const XMMATRIX& view, const XMMATRIX& proj);

    // GPU-часть симуляции: EmitCS, потом SimulateCS, потом готовит аргументы для ExecuteIndirect.
    // Вызывать в начале кадра, до рисования. Куча дескрипторов уже должна быть выставлена
    void Simulate(ID3D12GraphicsCommandList* cmdList);

    // Рисует живые частицы в G-buffer. Вызывать внутри geometry pass (рендертаргеты уже выставлены)
    void Render(ID3D12GraphicsCommandList* cmdList);

    // сколько частиц было живо в прошлом кадре (читается с GPU через readback-буфер)
    UINT GetAliveCount() const { return mAliveCountReadbackData ? *mAliveCountReadbackData : 0; }

    void SetEmitterPosition(const XMFLOAT3& pos) { mEmitterPos = pos; }

private:
    void CreateBuffers(ID3D12GraphicsCommandList* cmdList);
    void CreateViews(ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT uavStartSlot);
    void CreateComputePipeline();
    void CreateRenderPipeline(const DXGI_FORMAT* gbufferFormats, UINT gbufferCount, DXGI_FORMAT dsvFormat);

    // буфер в default куче (в видеопамяти)
    Microsoft::WRL::ComPtr<ID3D12Resource> CreateDefaultBuffer(UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);

    // заливает данные в default-буфер через промежуточный upload-буфер
    void UploadToBuffer(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* dst, const void* data, UINT64 size);

    static void Transition(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

private:
    ID3D12Device* mDevice = nullptr;
    UINT mMaxParticles = 0;

    // Particle Pool: все частицы, живые и свободные ячейки
    Microsoft::WRL::ComPtr<ID3D12Resource> mParticlePool;
    // Dead List: номера свободных ячеек и отдельный буфер под его счётчик
    Microsoft::WRL::ComPtr<ID3D12Resource> mDeadList;
    Microsoft::WRL::ComPtr<ID3D12Resource> mDeadListCounter;
    // Alive List: номера живых частиц этого кадра и его счётчик
    Microsoft::WRL::ComPtr<ID3D12Resource> mAliveList;
    Microsoft::WRL::ComPtr<ID3D12Resource> mAliveListCounter;

    // копия счётчика Dead List в виде константного буфера для EmitCS
    Microsoft::WRL::ComPtr<ID3D12Resource> mDeadCountCB;
    // аргументы для ExecuteIndirect: число вершин, число инстансов, первая вершина, первый инстанс
    Microsoft::WRL::ComPtr<ID3D12Resource> mIndirectArgs;
    // четыре нулевых байта, из них каждый кадр обнуляется счётчик Alive List
    Microsoft::WRL::ComPtr<ID3D12Resource> mZeroUpload;
    // сюда копируется счётчик Alive List, чтобы CPU мог показать число частиц
    Microsoft::WRL::ComPtr<ID3D12Resource> mAliveCountReadback;
    UINT* mAliveCountReadbackData = nullptr;

    // константы симуляции и камеры, в upload куче, замаплены навсегда
    Microsoft::WRL::ComPtr<ID3D12Resource> mSimCB;
    Microsoft::WRL::ComPtr<ID3D12Resource> mRenderCB;
    BYTE* mSimCBData = nullptr;
    BYTE* mRenderCBData = nullptr;

    // промежуточные буферы начальной загрузки, живут до конца инициализации
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> mUploadBuffers;

    D3D12_GPU_DESCRIPTOR_HANDLE mUavTableGpu = {};

    // compute: общая root signature и два PSO
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mEmitPSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mSimulatePSO;

    // рисование
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRenderRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mRenderPSO;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> mCommandSignature;

    // эмиттер
    XMFLOAT3 mEmitterPos = { 0.0f, 10.0f, 0.0f };
    float mEmitRate = 3000.0f;       // в среднем частиц в секунду
    float mEmitVariance = 500.0f;    // случайный разброс частоты
    float mAccumulatedTime = 0.0f;   // накопленная дробная часть: сколько частиц "задолжали" прошлым кадрам
    UINT mFrameIndex = 0;            // номер кадра, из него получается зерно случайных чисел

    ParticleSimConstants mSimConstants = {};
};