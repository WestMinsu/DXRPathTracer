#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "RayTracingManager.h"

class D3D12Renderer
{
public:
    ~D3D12Renderer();

    bool Initialize(HWND hWnd);
    void Render();
    void Resize(UINT width, UINT height);
    void WaitForGpu();
    void ConfigureAutomatedCapture(
        UINT sampleCount,
        const std::wstring& outputPrefix,
        UINT maxBounce,
        UINT sceneType,
        float pbrMetallic,
        float pbrRoughness,
        bool enableIbl,
        float iblIntensity,
        UINT validationSeed);

private:
    static constexpr UINT c_frameCount = 2;
    static constexpr DXGI_FORMAT c_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    bool CreateDevice();
    bool CreateDxrDevice(IDXGIAdapter* adapter);
    bool CreateCommandObjects();
    bool CreateSwapChain();
    bool CreateRenderTargetViews();
    bool CreateFence();
    bool InitializeImGui();
    void ShutdownImGui();
    void BuildImGuiFrame();
    void RenderImGuiDrawData();
    void ReleaseRenderTargets();
    enum class CaptureFormat
    {
        Png,
        Pfm
    };

    bool QueueTextureCapture(ID3D12Resource* sourceTexture,
        const std::wstring& filePath,
        CaptureFormat format,
        UINT sampleCount);
    void SavePendingCaptures();
    bool SavePngFile(const std::wstring& filePath,
        UINT width,
        UINT height,
        UINT rowPitch,
        const void* pixels) const;
    bool SavePfmFile(const std::wstring& filePath,
        UINT width,
        UINT height,
        UINT rowPitch,
        UINT sampleCount,
        const void* pixels) const;
    std::wstring BuildCaptureFilePath(UINT sampleCount, const wchar_t* extension) const;

    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRenderTargetView() const;
    void TransitionCurrentBackBuffer(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    bool ReportFailure(HRESULT hr, const wchar_t* message) const;

    struct PendingCapture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
        std::wstring filePath;
        UINT width = 0;
        UINT height = 0;
        UINT rowPitch = 0;
        UINT64 readbackSize = 0;
        UINT sampleCount = 1;
        CaptureFormat format = CaptureFormat::Png;
    };

    HWND m_hWnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_frameIndex = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;
    bool m_imguiInitialized = false;
    bool m_showNormalColor = false;
    bool m_enableAccumulation = true;
    bool m_captureActive = false;
    bool m_saveCurrentRequested = false;
    int m_captureTargetSamples = 256;
    int m_maxBounce = 8;
    int m_sceneType = 0;
    int m_pbrDebugView = 0;
    float m_pbrMetallic = 1.0f;
    float m_pbrRoughness = 0.35f;
    bool m_enableIbl = true;
    float m_iblIntensity = 0.5f;
    UINT m_validationSeed = 0;
    float m_exposure = 0.0f;
    std::string m_captureStatus;
    bool m_exitAfterCapture = false;
    std::wstring m_captureOutputPrefix;

    Microsoft::WRL::ComPtr<IDXGIFactory4> m_factory;
    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> m_commandList;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_imguiDescriptorHeap;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, c_frameCount> m_renderTargets;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    std::unique_ptr<RayTracingManager> m_rayTracingManager;
    std::vector<PendingCapture> m_pendingCaptures;
};
