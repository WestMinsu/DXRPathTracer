#pragma once

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "RayTracingManager.h"
#include "CameraPath.h"

class D3D12Renderer
{
public:
    ~D3D12Renderer();

    bool Initialize(HWND hWnd);
    void SetSceneFilePath(const std::wstring& sceneFilePath)
    {
        m_sceneFilePath = sceneFilePath;
        if (!m_sceneFilePath.empty())
            m_sceneType = static_cast<int>(RayTracingManager::c_scenePbrGgx);
    }
    void SetComposeModelRoom(bool enabled) { m_composeModelRoom = enabled; }
    void SetSponzaLite(bool enabled) { m_sponzaLite = enabled; }
    void SetSponzaLightConfigPath(const std::wstring& path)
    {
        m_sponzaLightConfigPath = path;
    }
    void SetSceneManifestPath(const std::wstring& path)
    {
        m_sceneManifestPath = path;
    }
    void SetVSyncEnabled(bool enabled) { m_vsyncEnabled = enabled; }
    void SetCollectRayStatistics(bool enabled) { m_collectRayStatistics = enabled; }
    void SetInitialMaxBounce(UINT maxBounce)
    {
        m_maxBounce = static_cast<int>(
            maxBounce < 1u ? 1u : (maxBounce > 8u ? 8u : maxBounce));
    }
    void SetInitialRussianRouletteEnabled(bool enabled)
    {
        m_enableRussianRoulette = enabled;
    }
    void SetInitialLightingMode(UINT lightingMode)
    {
        m_lightingMode = static_cast<int>(
            lightingMode <= RayTracingManager::c_lightingModeMis
            ? lightingMode
            : RayTracingManager::c_lightingModeBsdf);
    }
    void SetInitialAtrousEnabled(bool enabled)
    {
        m_enableAtrous = enabled;
    }
    void SetInitialAtrousIterationCount(UINT iterationCount)
    {
        m_atrousIterations = static_cast<int>(
            iterationCount < 1u ? 1u : (iterationCount > 5u ? 5u : iterationCount));
    }
    void SetInitialAtrousColorSigma(float colorSigma)
    {
        m_atrousColorSigma =
            colorSigma < 0.25f ? 0.25f : (colorSigma > 16.0f ? 16.0f : colorSigma);
    }
    void SetInitialDynamicSphereAnimationEnabled(bool enabled)
    {
        m_animateDynamicSphere = enabled;
    }
    void SetCameraPathFilePath(const std::wstring& filePath)
    {
        m_cameraPathFilePath = filePath;
    }
    void SetCameraPathAutoPlay(bool enabled)
    {
        m_cameraPathAutoPlay = enabled;
    }
    void SetInitialSceneType(UINT sceneType)
    {
        m_sceneType = static_cast<int>(sceneType);
    }
    void ConfigureBenchmark(
        bool enabled,
        const std::wstring& outputPath,
        UINT frameLimit);
    void OnKey(UINT virtualKey, bool pressed);
    void OnRightMouseButton(bool pressed, int x, int y);
    void OnMouseMove(int x, int y);
    void OnFocusLost();
    void Render();
    void Resize(UINT width, UINT height);
    void WaitForGpu();
    void ConfigureAutomatedCapture(
        UINT sampleCount,
        const std::wstring& outputPrefix,
        UINT maxBounce,
        UINT sceneType,
        UINT pbrDebugView,
        float pbrMetallic,
        float pbrRoughness,
        bool overridePbrMaterial,
        bool enableIbl,
        float iblIntensity,
        UINT validationSeed);

private:
    static constexpr UINT c_frameCount = 2;
    static constexpr DXGI_FORMAT c_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr UINT c_gpuTimestampCount = 6;

    enum GpuTimestampIndex : UINT
    {
        c_gpuTotalBegin = 0,
        c_gpuDispatchBegin = 1,
        c_gpuDispatchEnd = 2,
        c_gpuUpscaleBegin = 3,
        c_gpuUpscaleEnd = 4,
        c_gpuTotalEnd = 5
    };

    bool CreateDevice();
    bool CreateDxrDevice(IDXGIAdapter* adapter);
    bool CreateCommandObjects();
    bool CreateSwapChain();
    bool CreateRenderTargetViews();
    bool CreateFence();
    bool CreateGpuTimingResources();
    bool LoadCameraPath();
    void StartCameraPathPlayback();
    void StopCameraPathPlayback();
    void UpdateCameraPath();
    void InitializeFreeCamera();
    void UpdateFreeCamera(double deltaSeconds);
    void ReadGpuTimingResults();
    bool OpenBenchmarkCsv();
    void RecordFrameMetrics(double cpuFrameMs);
    void CloseBenchmarkCsv();
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
    bool m_vsyncEnabled = true;
    bool m_tearingSupported = false;
    bool m_collectRayStatistics = false;
    bool m_cameraPathLoaded = false;
    bool m_cameraPathPlaybackActive = false;
    bool m_cameraPathAutoPlay = false;
    bool m_animateDynamicSphere = false;
    bool m_showDynamicSphere = true;
    bool m_hasPreviousCameraPose = false;
    bool m_freeCameraInitialized = false;
    bool m_rightMouseDragging = false;
    bool m_hasLastRenderTime = false;
    bool m_showNormalColor = false;
    bool m_enableAccumulation = true;
    bool m_enableRussianRoulette = false;
    bool m_enableAtrous = false;
    int m_atrousIterations = 3;
    float m_atrousColorSigma = 4.0f;
    int m_lightingMode = static_cast<int>(
        RayTracingManager::c_lightingModeBsdf);
    bool m_captureActive = false;
    bool m_saveCurrentRequested = false;
    int m_captureTargetSamples = 256;
    int m_maxBounce = 8;
    int m_sceneType = 0;
    int m_pbrDebugView = 0;
    float m_pbrMetallic = 1.0f;
    float m_pbrRoughness = 0.35f;
    bool m_overridePbrMaterial = false;
    bool m_enableIbl = true;
    float m_iblIntensity = 1.0f;
    UINT m_validationSeed = 0;
    float m_exposure = 0.0f;
    std::string m_captureStatus;
    bool m_exitAfterCapture = false;
    std::wstring m_captureOutputPrefix;
    std::wstring m_sceneFilePath;
    std::wstring m_sceneManifestPath;
    std::wstring m_sponzaLightConfigPath;
    std::wstring m_cameraPathFilePath;
    std::wstring m_cameraPathError;
    bool m_composeModelRoom = false;
    bool m_sponzaLite = false;
    bool m_benchmarkEnabled = false;
    bool m_benchmarkFinished = false;
    UINT m_benchmarkFrameLimit = 600;
    UINT64 m_benchmarkFramesWritten = 0;
    std::wstring m_benchmarkOutputPath;
    FILE* m_benchmarkCsv = nullptr;
    UINT64 m_gpuTimestampFrequency = 0;
    double m_gpuDispatchMs = 0.0;
    double m_gpuUpscaleMs = 0.0;
    double m_gpuTotalMs = 0.0;
    double m_gpuMedianMs = 0.0;
    double m_gpuP95Ms = 0.0;
    double m_gpuP99Ms = 0.0;
    double m_cpuFrameMs = 0.0;
    double m_cpuMedianMs = 0.0;
    double m_cpuP95Ms = 0.0;
    double m_cpuP99Ms = 0.0;
    double m_cameraLinearSpeed = 0.0;
    double m_cameraAngularSpeed = 0.0;
    double m_objectLinearSpeed = 0.0;
    double m_objectAngularSpeed = 0.0;
    double m_freeCameraYaw = 0.0;
    double m_freeCameraPitch = 0.0;
    double m_freeCameraLookDistance = 1.0;
    double m_pendingMouseYaw = 0.0;
    double m_pendingMousePitch = 0.0;
    POINT m_lastMousePosition = {};
    std::array<bool, 256> m_keyPressed = {};
    std::chrono::steady_clock::time_point m_lastRenderTime;
    UINT64 m_cameraPathFrameIndex = 0;
    CameraPath m_cameraPath;
    CameraPose m_previousCameraPose;
    std::vector<double> m_gpuTimingHistory;
    std::vector<double> m_cpuTimingHistory;

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
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_gpuTimestampQueryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_gpuTimestampReadback;
    std::unique_ptr<RayTracingManager> m_rayTracingManager;
    std::vector<PendingCapture> m_pendingCaptures;
};
