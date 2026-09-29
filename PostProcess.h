
// PostProcess.h
// Пост-обработка: всё, что делается с картинкой после lighting pass.
//
// Как устроено:
//  1) lighting pass рисует не в back buffer, а в текстуру сцены (SceneTex) формата R16G16B16A16_FLOAT.
//     Это HDR: в ней яркость может быть больше 1, ничего не обрезается
//  2) Bloom: яркие пиксели сцены -> BloomTexA, потом размытие A -> B (по горизонтали) и B -> A (по вертикали)
//  3) Итоговый проход читает SceneTex, BloomTexA и G-буфер и пишет в back buffer
//
// Каждый проход - это прямоугольник на весь экран (full-screen quad) из 4 вершин без вершинного буфера.
//
// Раскладка root signature:
//  0 - 4 числа прямо в root signature (b0): bloom вкл, аберрация вкл, режим просмотра, запас
//  1 - таблица t0..t2: SceneTex, BloomTexA, BloomTexB
//  2 - таблица t3..t5: G-буфер (альбедо, позиции, нормали)
//  s0 - линейный сэмплер с clamp

#pragma once

#include <wrl/client.h>
#include <d3d12.h>

class PostProcess
{
public:
    // формат текстуры сцены и текстур bloom: 16-битный float на канал, чтобы хранить яркость больше 1
    static const DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    // сколько слотов SRV кучи занимает пост-обработка (SceneTex, BloomTexA, BloomTexB подряд)
    static const UINT kSrvCount = 3;

    // сколько раз повторять пару проходов размытия (горизонталь + вертикаль). Больше - шире свечение
    static const UINT kBlurPasses = 4;

    // режимы просмотра по клавише G
    static const UINT kViewModeCount = 5; // 0 итог, 1 альбедо, 2 нормали, 3 позиции, 4 только свечение

    // создаёт текстуры, RTV, SRV (в слотах srvStartSlot .. srvStartSlot + 2), root signature и PSO
    void Init(ID3D12Device* device, UINT width, UINT height,
        ID3D12DescriptorHeap* srvHeap, UINT srvDescSize, UINT srvStartSlot,
        DXGI_FORMAT backBufferFormat);

    // Перед lighting pass: текстура сцены становится render target и очищается в чёрный.
    // Потом в неё рисует RenderLights, RTV берётся из GetSceneRtv
    void BeginScene(ID3D12GraphicsCommandList* cmdList);
    D3D12_CPU_DESCRIPTOR_HANDLE GetSceneRtv() const { return GetRtv(0); }

    // Все проходы пост-обработки. back buffer к этому моменту уже должен быть в состоянии RENDER_TARGET.
    // gbufferSrv - начало таблицы SRV G-буфера (GBuffer::GetSrvGpuHandle)
    void Render(ID3D12GraphicsCommandList* cmdList,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv,
        D3D12_GPU_DESCRIPTOR_HANDLE gbufferSrv,
        bool bloomEnabled, bool chromaEnabled, UINT viewMode);

private:
    // создаёт одну текстуру-рендертаргет, её RTV (номер index в своей RTV куче) и SRV (слот srvStartSlot + index)
    void CreateTarget(UINT index, UINT width, UINT height);
    void CreateRootSignature();
    void CreatePSOs(DXGI_FORMAT backBufferFormat);

    // рисует quad на весь viewport с указанным PSO в указанный рендертаргет
    void DrawQuad(ID3D12GraphicsCommandList* cmdList, ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE rtv);

    // барьер для одной текстуры: из состояния before в after
    void Transition(ID3D12GraphicsCommandList* cmdList, UINT index,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

    D3D12_CPU_DESCRIPTOR_HANDLE GetRtv(UINT index) const;

private:
    // номера текстур в массиве mTargets
    static const UINT kScene = 0;
    static const UINT kBloomA = 1;
    static const UINT kBloomB = 2;
    static const UINT kTargetCount = 3;

    ID3D12Device* mDevice = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> mTargets[kTargetCount];

    // своя маленькая RTV куча на 3 рендертаргета
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    UINT mRtvDescSize = 0;

    // SRV лежат в общей куче приложения, запоминаем где
    ID3D12DescriptorHeap* mSrvHeap = nullptr;
    UINT mSrvDescSize = 0;
    UINT mSrvStartSlot = 0;

    // полный размер экрана (для сцены и итога) и половинный (для bloom)
    D3D12_VIEWPORT mFullViewport = {};
    D3D12_RECT mFullScissor = {};
    D3D12_VIEWPORT mHalfViewport = {};
    D3D12_RECT mHalfScissor = {};

    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBrightPSO; // яркие пиксели -> BloomTexA
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBlurHPSO;  // A -> B по горизонтали
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mBlurVPSO;  // B -> A по вертикали
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mFinalPSO;  // итог в back buffer
};