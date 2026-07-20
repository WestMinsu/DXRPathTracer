#include "D3D12Renderer.h"

#include "RayTracingManager.h"

#include "ThirdParty/imgui/imgui.h"
#include "ThirdParty/imgui/backends/imgui_impl_dx12.h"
#include "ThirdParty/imgui/backends/imgui_impl_win32.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <shlobj.h>
#include <sstream>
#include <vector>
#include <wincodec.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace
{
    constexpr std::size_t c_timingHistoryLength = 600;

    double CalculatePercentile(const std::vector<double>& samples, double percentile)
    {
        if (samples.empty())
            return 0.0;

        std::vector<double> sortedSamples = samples;
        std::sort(sortedSamples.begin(), sortedSamples.end());
        const double position = percentile * static_cast<double>(sortedSamples.size() - 1);
        const std::size_t lowerIndex = static_cast<std::size_t>(std::floor(position));
        const std::size_t upperIndex = (std::min)(lowerIndex + 1, sortedSamples.size() - 1);
        const double fraction = position - static_cast<double>(lowerIndex);
        return sortedSamples[lowerIndex] +
            (sortedSamples[upperIndex] - sortedSamples[lowerIndex]) * fraction;
    }

    void AppendTimingSample(std::vector<double>& samples, double value)
    {
        if (samples.size() == c_timingHistoryLength)
            samples.erase(samples.begin());
        samples.push_back(value);
    }

    bool EnsureParentDirectory(const std::wstring& filePath)
    {
        wchar_t absolutePath[MAX_PATH] = {};
        const DWORD absolutePathLength = GetFullPathNameW(
            filePath.c_str(),
            _countof(absolutePath),
            absolutePath,
            nullptr);
        const std::wstring pathSource =
            absolutePathLength > 0 && absolutePathLength < _countof(absolutePath)
            ? std::wstring(absolutePath, absolutePathLength)
            : filePath;

        const std::size_t separator = pathSource.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return true;

        const std::wstring directory = pathSource.substr(0, separator);
        if (directory.empty())
            return true;

        const int result = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
        return result == ERROR_SUCCESS ||
            result == ERROR_ALREADY_EXISTS ||
            result == ERROR_FILE_EXISTS;
    }

    UINT GetClientWidth(HWND hWnd)
    {
        RECT rect = {};
        GetClientRect(hWnd, &rect);
        const LONG width = rect.right - rect.left;
        return static_cast<UINT>(width > 0 ? width : 1);
    }

    UINT GetClientHeight(HWND hWnd)
    {
        RECT rect = {};
        GetClientRect(hWnd, &rect);
        const LONG height = rect.bottom - rect.top;
        return static_cast<UINT>(height > 0 ? height : 1);
    }
}

D3D12Renderer::~D3D12Renderer()
{
    WaitForGpu();
    SavePendingCaptures();
    CloseBenchmarkCsv();
    ShutdownImGui();
    m_rayTracingManager.reset();

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

bool D3D12Renderer::Initialize(HWND hWnd)
{
    m_hWnd = hWnd;
    m_width = GetClientWidth(hWnd);
    m_height = GetClientHeight(hWnd);

    if (!CreateDevice())
        return false;

    if (!CreateCommandObjects())
        return false;

    if (!CreateSwapChain())
        return false;

    if (!CreateRenderTargetViews())
        return false;

    if (!CreateFence())
        return false;

    if (!CreateGpuTimingResources())
        return false;

    m_rayTracingManager.reset(new RayTracingManager());
    m_rayTracingManager->SetSceneFilePath(m_sceneFilePath);
    m_rayTracingManager->SetComposeModelRoom(m_composeModelRoom);
    m_rayTracingManager->SetSceneType(static_cast<UINT>(m_sceneType));
    if (!m_rayTracingManager->Initialize(m_hWnd, m_device.Get(), m_width, m_height))
        return false;

    if (!InitializeImGui())
        return false;

    if (!OpenBenchmarkCsv())
        return false;

    return true;
}

void D3D12Renderer::Render()
{
    if (!m_swapChain || !m_rayTracingManager || m_width == 0 || m_height == 0)
        return;

    const auto cpuFrameBegin = std::chrono::steady_clock::now();
    BuildImGuiFrame();
    m_rayTracingManager->SetShowNormalColor(m_showNormalColor);
    m_rayTracingManager->SetMaxBounce(static_cast<UINT>(m_maxBounce));
    m_rayTracingManager->SetEnableAccumulation(m_enableAccumulation);
    m_rayTracingManager->SetSceneType(static_cast<UINT>(m_sceneType));
    m_rayTracingManager->SetPbrDebugView(static_cast<UINT>(m_pbrDebugView));
    m_rayTracingManager->SetPbrMaterial(m_pbrMetallic, m_pbrRoughness);
    m_rayTracingManager->SetPbrMaterialOverride(m_overridePbrMaterial);
    m_rayTracingManager->SetIblSettings(m_enableIbl, m_iblIntensity);
    m_rayTracingManager->SetValidationSeed(m_validationSeed);
    m_rayTracingManager->SetExposure(m_exposure);

    HRESULT hr = m_commandAllocator->Reset();
    if (ReportFailure(hr, L"Command allocator reset failed."))
        return;

    hr = m_commandList->Reset(m_commandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"Command list reset failed."))
        return;

    m_commandList->EndQuery(
        m_gpuTimestampQueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        c_gpuTotalBegin);
    m_commandList->EndQuery(
        m_gpuTimestampQueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        c_gpuDispatchBegin);
    m_rayTracingManager->DispatchRays(m_commandList.Get());
    m_commandList->EndQuery(
        m_gpuTimestampQueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        c_gpuDispatchEnd);

    // Reserved for the MAPB bilinear compute pass. Keeping timestamps in the
    // baseline makes benchmark CSV columns stable before upscaling is added.
    m_commandList->EndQuery(
        m_gpuTimestampQueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        c_gpuUpscaleBegin);
    m_commandList->EndQuery(
        m_gpuTimestampQueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        c_gpuUpscaleEnd);

    ID3D12Resource* raytracingOutput = m_rayTracingManager->GetOutputResource();

    D3D12_RESOURCE_BARRIER preCopyBarriers[2] = {};
    preCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preCopyBarriers[0].Transition.pResource = raytracingOutput;
    preCopyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    preCopyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    preCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    preCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preCopyBarriers[1].Transition.pResource = m_renderTargets[m_frameIndex].Get();
    preCopyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    preCopyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    preCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(2, preCopyBarriers);

    m_commandList->CopyResource(m_renderTargets[m_frameIndex].Get(), raytracingOutput);

    const UINT accumulatedSamples = m_rayTracingManager->GetAccumulatedSampleCount();
    const UINT captureTargetSamples = static_cast<UINT>(m_captureTargetSamples > 1 ? m_captureTargetSamples : 1);
    const bool isDebugFrame = m_showNormalColor ||
        (m_sceneType == static_cast<int>(RayTracingManager::c_scenePbrGgx) &&
         m_pbrDebugView != static_cast<int>(RayTracingManager::c_pbrDebugBeauty));
    const bool targetCaptureReached = m_captureActive &&
        (isDebugFrame || accumulatedSamples >= captureTargetSamples);
    if (m_pendingCaptures.empty() && (m_saveCurrentRequested || targetCaptureReached))
    {
        bool queuedCapture = QueueTextureCapture(
            raytracingOutput,
            BuildCaptureFilePath(accumulatedSamples, L".png"),
            CaptureFormat::Png,
            1u);

        const bool canSaveHdr =
            m_enableAccumulation &&
            accumulatedSamples > 0 &&
            !m_showNormalColor &&
            !(m_sceneType == static_cast<int>(RayTracingManager::c_scenePbrGgx) &&
              m_pbrDebugView != static_cast<int>(RayTracingManager::c_pbrDebugBeauty));
        ID3D12Resource* accumulationTexture = m_rayTracingManager->GetAccumulationResource();
        if (canSaveHdr && accumulationTexture)
        {
            D3D12_RESOURCE_BARRIER accumulationToCopy = {};
            accumulationToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            accumulationToCopy.Transition.pResource = accumulationTexture;
            accumulationToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            accumulationToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            accumulationToCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(1, &accumulationToCopy);

            queuedCapture |= QueueTextureCapture(
                accumulationTexture,
                BuildCaptureFilePath(accumulatedSamples, L".pfm"),
                CaptureFormat::Pfm,
                accumulatedSamples);

            std::swap(
                accumulationToCopy.Transition.StateBefore,
                accumulationToCopy.Transition.StateAfter);
            m_commandList->ResourceBarrier(1, &accumulationToCopy);
        }

        if (queuedCapture)
        {
            m_captureActive = false;
            m_saveCurrentRequested = false;
            m_captureStatus = canSaveHdr
                ? "Saving PNG preview and linear HDR PFM..."
                : "Saving PNG preview...";
        }
    }
    D3D12_RESOURCE_BARRIER postCopyBarriers[2] = {};
    postCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postCopyBarriers[0].Transition.pResource = raytracingOutput;
    postCopyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    postCopyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    postCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    postCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postCopyBarriers[1].Transition.pResource = m_renderTargets[m_frameIndex].Get();
    postCopyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    postCopyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    postCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(2, postCopyBarriers);

    RenderImGuiDrawData();

    D3D12_RESOURCE_BARRIER presentBarrier = {};
    presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    presentBarrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &presentBarrier);

    m_commandList->EndQuery(
        m_gpuTimestampQueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        c_gpuTotalEnd);
    m_commandList->ResolveQueryData(
        m_gpuTimestampQueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        0,
        c_gpuTimestampCount,
        m_gpuTimestampReadback.Get(),
        0);

    hr = m_commandList->Close();
    if (ReportFailure(hr, L"Command list close failed."))
        return;

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    const UINT syncInterval = m_vsyncEnabled ? 1u : 0u;
    const UINT presentFlags =
        !m_vsyncEnabled && m_tearingSupported
        ? DXGI_PRESENT_ALLOW_TEARING
        : 0u;
    hr = m_swapChain->Present(syncInterval, presentFlags);
    if (ReportFailure(hr, L"Swap chain present failed."))
        return;

    WaitForGpu();
    ReadGpuTimingResults();
    SavePendingCaptures();
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    const auto cpuFrameEnd = std::chrono::steady_clock::now();
    const double cpuFrameMs = std::chrono::duration<double, std::milli>(
        cpuFrameEnd - cpuFrameBegin).count();
    RecordFrameMetrics(cpuFrameMs);
}

void D3D12Renderer::Resize(UINT width, UINT height)
{
    if (!m_swapChain)
        return;

    if (width == 0 || height == 0)
    {
        m_width = 0;
        m_height = 0;
        return;
    }

    if (width == m_width && height == m_height)
        return;

    WaitForGpu();
    SavePendingCaptures();
    ReleaseRenderTargets();

    const UINT swapChainFlags = m_tearingSupported
        ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        : 0u;
    HRESULT hr = m_swapChain->ResizeBuffers(
        c_frameCount,
        width,
        height,
        c_backBufferFormat,
        swapChainFlags);
    if (ReportFailure(hr, L"Swap chain resize failed."))
        return;

    m_width = width;
    m_height = height;
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (!CreateRenderTargetViews())
        return;

    if (m_rayTracingManager)
    {
        m_rayTracingManager->Resize(m_width, m_height);
    }
}

void D3D12Renderer::WaitForGpu()
{
    if (!m_commandQueue || !m_fence || !m_fenceEvent)
        return;

    const UINT64 fenceToWaitFor = ++m_fenceValue;
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), fenceToWaitFor);
    if (ReportFailure(hr, L"Fence signal failed."))
        return;

    if (m_fence->GetCompletedValue() < fenceToWaitFor)
    {
        hr = m_fence->SetEventOnCompletion(fenceToWaitFor, m_fenceEvent);
        if (ReportFailure(hr, L"Fence event setup failed."))
            return;

        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

bool D3D12Renderer::CreateDevice()
{
    UINT factoryFlags = 0;

#if defined(_DEBUG)
    Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        factoryFlags = DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
    if (ReportFailure(hr, L"DXGI factory creation failed."))
        return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0;
         m_factory->EnumAdapters1(adapterIndex, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
         ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (CreateDxrDevice(adapter.Get()))
            return true;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
    hr = m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
    if (ReportFailure(hr, L"WARP adapter lookup failed."))
        return false;

    if (CreateDxrDevice(warpAdapter.Get()))
        return true;

    ReportFailure(E_FAIL, L"D3D12 raytracing tier 1.0 is not supported on this system.");
    return false;
}

bool D3D12Renderer::CreateDxrDevice(IDXGIAdapter* adapter)
{
    Microsoft::WRL::ComPtr<ID3D12Device> baseDevice;
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&baseDevice));
    if (FAILED(hr))
        return false;

    Microsoft::WRL::ComPtr<ID3D12Device5> dxrDevice;
    hr = baseDevice.As(&dxrDevice);
    if (FAILED(hr))
        return false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    hr = dxrDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    if (FAILED(hr) || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_0)
        return false;

    m_device = dxrDevice;
    return true;
}

bool D3D12Renderer::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (ReportFailure(hr, L"Command queue creation failed."))
        return false;

    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator));
    if (ReportFailure(hr, L"Command allocator creation failed."))
        return false;

    hr = m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_commandList));
    if (ReportFailure(hr, L"Command list creation failed."))
        return false;

    hr = m_commandList->Close();
    return !ReportFailure(hr, L"Initial command list close failed.");
}

bool D3D12Renderer::CreateSwapChain()
{
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(m_factory.As(&factory5)))
    {
        const HRESULT tearingHr = factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing));
        m_tearingSupported = SUCCEEDED(tearingHr) && allowTearing == TRUE;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = c_backBufferFormat;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = c_frameCount;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags = m_tearingSupported
        ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        : 0u;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    HRESULT hr = m_factory->CreateSwapChainForHwnd(
        m_commandQueue.Get(),
        m_hWnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain);
    if (ReportFailure(hr, L"Swap chain creation failed."))
        return false;

    m_factory->MakeWindowAssociation(m_hWnd, DXGI_MWA_NO_ALT_ENTER);

    hr = swapChain.As(&m_swapChain);
    if (ReportFailure(hr, L"Swap chain interface query failed."))
        return false;

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    return true;
}

bool D3D12Renderer::CreateRenderTargetViews()
{
    if (!m_rtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = c_frameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

        HRESULT hr = m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
        if (ReportFailure(hr, L"RTV heap creation failed."))
            return false;

        m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < c_frameCount; ++i)
    {
        HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (ReportFailure(hr, L"Swap chain back buffer lookup failed."))
            return false;

        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += m_rtvDescriptorSize;
    }

    return true;
}

bool D3D12Renderer::CreateFence()
{
    HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (ReportFailure(hr, L"Fence creation failed."))
        return false;

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent)
        return false;

    return true;
}

bool D3D12Renderer::CreateGpuTimingResources()
{
    D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
    queryHeapDesc.Count = c_gpuTimestampCount;
    queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    HRESULT hr = m_device->CreateQueryHeap(
        &queryHeapDesc,
        IID_PPV_ARGS(&m_gpuTimestampQueryHeap));
    if (ReportFailure(hr, L"GPU timestamp query heap creation failed."))
        return false;

    hr = m_commandQueue->GetTimestampFrequency(&m_gpuTimestampFrequency);
    if (ReportFailure(hr, L"GPU timestamp frequency query failed.") ||
        m_gpuTimestampFrequency == 0)
    {
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(UINT64) * c_gpuTimestampCount;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_gpuTimestampReadback));
    if (ReportFailure(hr, L"GPU timestamp readback buffer creation failed."))
        return false;

    m_gpuTimestampQueryHeap->SetName(L"Frame GPU timestamp queries");
    m_gpuTimestampReadback->SetName(L"Frame GPU timestamp readback");
    return true;
}

void D3D12Renderer::ReadGpuTimingResults()
{
    if (!m_gpuTimestampReadback || m_gpuTimestampFrequency == 0)
        return;

    const SIZE_T dataSize = sizeof(UINT64) * c_gpuTimestampCount;
    D3D12_RANGE readRange = { 0, dataSize };
    UINT64* timestamps = nullptr;
    const HRESULT hr = m_gpuTimestampReadback->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&timestamps));
    if (ReportFailure(hr, L"GPU timestamp readback mapping failed."))
        return;

    const double tickToMilliseconds =
        1000.0 / static_cast<double>(m_gpuTimestampFrequency);
    const auto elapsedMilliseconds =
        [timestamps, tickToMilliseconds](UINT begin, UINT end)
        {
            return timestamps[end] >= timestamps[begin]
                ? static_cast<double>(timestamps[end] - timestamps[begin]) *
                    tickToMilliseconds
                : 0.0;
        };

    m_gpuDispatchMs = elapsedMilliseconds(c_gpuDispatchBegin, c_gpuDispatchEnd);
    m_gpuUpscaleMs = elapsedMilliseconds(c_gpuUpscaleBegin, c_gpuUpscaleEnd);
    m_gpuTotalMs = elapsedMilliseconds(c_gpuTotalBegin, c_gpuTotalEnd);

    D3D12_RANGE writeRange = { 0, 0 };
    m_gpuTimestampReadback->Unmap(0, &writeRange);
}

bool D3D12Renderer::OpenBenchmarkCsv()
{
    if (!m_benchmarkEnabled)
        return true;

    if (m_benchmarkOutputPath.empty())
        m_benchmarkOutputPath = L"BenchmarkOutput\\baseline.csv";

    if (!EnsureParentDirectory(m_benchmarkOutputPath))
        return !ReportFailure(E_FAIL, L"Benchmark output directory creation failed.");

    errno_t openError = _wfopen_s(
        &m_benchmarkCsv,
        m_benchmarkOutputPath.c_str(),
        L"wb");
    if (openError != 0 || !m_benchmarkCsv)
        return !ReportFailure(E_FAIL, L"Benchmark CSV creation failed.");

    std::fprintf(
        m_benchmarkCsv,
        "frame,cpu_ms,gpu_dispatch_ms,gpu_upscale_ms,gpu_total_ms,"
        "profile,internal_scale,max_bounce,camera_linear_speed,"
        "camera_angular_speed,object_linear_speed,object_angular_speed,"
        "primary_rays,shadow_rays,bounce_rays,average_path_length,"
        "hit_count,miss_count,accumulated_samples\n");
    return true;
}

void D3D12Renderer::RecordFrameMetrics(double cpuFrameMs)
{
    m_cpuFrameMs = cpuFrameMs;
    AppendTimingSample(m_cpuTimingHistory, m_cpuFrameMs);
    AppendTimingSample(m_gpuTimingHistory, m_gpuTotalMs);

    m_cpuMedianMs = CalculatePercentile(m_cpuTimingHistory, 0.50);
    m_cpuP95Ms = CalculatePercentile(m_cpuTimingHistory, 0.95);
    m_cpuP99Ms = CalculatePercentile(m_cpuTimingHistory, 0.99);
    m_gpuMedianMs = CalculatePercentile(m_gpuTimingHistory, 0.50);
    m_gpuP95Ms = CalculatePercentile(m_gpuTimingHistory, 0.95);
    m_gpuP99Ms = CalculatePercentile(m_gpuTimingHistory, 0.99);

    if (!m_benchmarkEnabled || m_benchmarkFinished || !m_benchmarkCsv)
        return;

    const UINT64 primaryRayCount =
        static_cast<UINT64>(m_width) * static_cast<UINT64>(m_height);
    const UINT accumulatedSamples = m_rayTracingManager
        ? m_rayTracingManager->GetAccumulatedSampleCount()
        : 0u;
    std::fprintf(
        m_benchmarkCsv,
        "%llu,%.6f,%.6f,%.6f,%.6f,fixed,1.000000,%d,"
        "0.000000,0.000000,0.000000,0.000000,%llu,,,,,,%u\n",
        static_cast<unsigned long long>(m_benchmarkFramesWritten),
        m_cpuFrameMs,
        m_gpuDispatchMs,
        m_gpuUpscaleMs,
        m_gpuTotalMs,
        m_maxBounce,
        static_cast<unsigned long long>(primaryRayCount),
        accumulatedSamples);

    ++m_benchmarkFramesWritten;
    if ((m_benchmarkFramesWritten % 60u) == 0u)
        std::fflush(m_benchmarkCsv);

    if (m_benchmarkFrameLimit > 0 &&
        m_benchmarkFramesWritten >= m_benchmarkFrameLimit)
    {
        m_benchmarkFinished = true;
        CloseBenchmarkCsv();
        PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
    }
}

void D3D12Renderer::CloseBenchmarkCsv()
{
    if (!m_benchmarkCsv)
        return;

    std::fflush(m_benchmarkCsv);
    std::fclose(m_benchmarkCsv);
    m_benchmarkCsv = nullptr;
}

bool D3D12Renderer::InitializeImGui()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_imguiDescriptorHeap));
    if (ReportFailure(hr, L"ImGui descriptor heap creation failed."))
        return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplWin32_Init(m_hWnd))
    {
        ReportFailure(E_FAIL, L"ImGui Win32 initialization failed.");
        return false;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = m_imguiDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = m_imguiDescriptorHeap->GetGPUDescriptorHandleForHeapStart();

    ImGui_ImplDX12_InitInfo initInfo;
    initInfo.Device = m_device.Get();
    initInfo.CommandQueue = m_commandQueue.Get();
    initInfo.NumFramesInFlight = c_frameCount;
    initInfo.RTVFormat = c_backBufferFormat;
    initInfo.SrvDescriptorHeap = m_imguiDescriptorHeap.Get();
    initInfo.LegacySingleSrvCpuDescriptor = cpuHandle;
    initInfo.LegacySingleSrvGpuDescriptor = gpuHandle;

    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        ReportFailure(E_FAIL, L"ImGui DX12 initialization failed.");
        return false;
    }

    m_imguiInitialized = true;
    return true;
}

void D3D12Renderer::ShutdownImGui()
{
    if (!m_imguiInitialized)
        return;

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_imguiInitialized = false;
    m_imguiDescriptorHeap.Reset();
}

void D3D12Renderer::BuildImGuiFrame()
{
    if (!m_imguiInitialized)
        return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("DXR Debug");
    const char* sceneNames[] =
    {
        "Cornell Box",
        "PBR",
        "PBR GPU Validation"
    };
    if (ImGui::Combo("Scene", &m_sceneType, sceneNames, _countof(sceneNames)) && m_rayTracingManager)
    {
        m_captureActive = false;
        m_saveCurrentRequested = false;
        m_captureStatus.clear();
        m_rayTracingManager->SetSceneType(static_cast<UINT>(m_sceneType));
    }
    if (m_sceneType == static_cast<int>(RayTracingManager::c_scenePbrGgx))
    {
        const char* pbrDebugNames[] =
        {
            "Beauty",
            "Albedo",
            "Metallic",
            "Roughness",
            "Depth",
            "Material ID",
            "Normal"
        };
        if (ImGui::Combo("PBR Debug", &m_pbrDebugView, pbrDebugNames, _countof(pbrDebugNames)) && m_rayTracingManager)
        {
            m_captureActive = false;
            m_saveCurrentRequested = false;
            m_captureStatus.clear();
            m_rayTracingManager->SetPbrDebugView(static_cast<UINT>(m_pbrDebugView));
        }

        bool restoreGltfMaterial = false;
        if (!m_sceneFilePath.empty())
        {
            ImGui::TextDisabled(
                m_overridePbrMaterial
                ? "Material source: uniform slider values"
                : "Material source: glTF factors and textures");
            if (m_overridePbrMaterial)
            {
                restoreGltfMaterial = ImGui::Button(
                    "Restore glTF Metallic/Roughness");
            }
            else
            {
                ImGui::TextDisabled(
                    "Changing a slider starts a uniform material test.");
            }
        }

        bool pbrMaterialChanged = false;
        pbrMaterialChanged |= ImGui::SliderFloat("PBR Metallic", &m_pbrMetallic, 0.0f, 1.0f, "%.2f");
        pbrMaterialChanged |= ImGui::SliderFloat("PBR Roughness", &m_pbrRoughness, 0.03f, 1.0f, "%.2f");
        if (!m_sceneFilePath.empty() && pbrMaterialChanged)
            m_overridePbrMaterial = true;
        if (restoreGltfMaterial)
            m_overridePbrMaterial = false;

        if ((restoreGltfMaterial || pbrMaterialChanged) && m_rayTracingManager)
        {
            m_captureActive = false;
            m_saveCurrentRequested = false;
            m_captureStatus.clear();
            m_rayTracingManager->SetPbrMaterialOverride(m_overridePbrMaterial);
            m_rayTracingManager->SetPbrMaterial(m_pbrMetallic, m_pbrRoughness);
        }

        bool iblChanged = false;
        iblChanged |= ImGui::Checkbox("Enable IBL", &m_enableIbl);
        iblChanged |= ImGui::SliderFloat("IBL Intensity", &m_iblIntensity, 0.0f, 4.0f, "%.2f");
        if (iblChanged && m_rayTracingManager)
        {
            m_captureActive = false;
            m_saveCurrentRequested = false;
            m_captureStatus.clear();
            m_rayTracingManager->SetIblSettings(m_enableIbl, m_iblIntensity);
        }
    }
    ImGui::SliderFloat("Exposure (EV)", &m_exposure, -8.0f, 8.0f, "%.2f");
    ImGui::Checkbox("Show normal color", &m_showNormalColor);
    ImGui::Checkbox("Accumulate samples", &m_enableAccumulation);
    ImGui::SliderInt("Max Bounce", &m_maxBounce, 1, 8);
    if (ImGui::Button("Reset samples") && m_rayTracingManager)
    {
        m_rayTracingManager->ResetAccumulation();
    }
    const UINT accumulatedSamples = m_rayTracingManager
        ? m_rayTracingManager->GetAccumulatedSampleCount()
        : 0u;
    ImGui::Text("Samples: %u", accumulatedSamples);
    const ImGuiIO& io = ImGui::GetIO();
    const float frameTimeMs = io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f;
    ImGui::Text("Frame: %.2f ms (%.1f FPS)", frameTimeMs, io.Framerate);
    ImGui::Checkbox("VSync", &m_vsyncEnabled);
    ImGui::Text(
        "CPU current/median: %.2f / %.2f ms",
        m_cpuFrameMs,
        m_cpuMedianMs);
    ImGui::Text(
        "CPU p95/p99: %.2f / %.2f ms",
        m_cpuP95Ms,
        m_cpuP99Ms);
    ImGui::Text(
        "GPU dispatch/upscale: %.2f / %.2f ms",
        m_gpuDispatchMs,
        m_gpuUpscaleMs);
    ImGui::Text(
        "GPU total median/p95/p99: %.2f / %.2f / %.2f ms",
        m_gpuMedianMs,
        m_gpuP95Ms,
        m_gpuP99Ms);
    ImGui::Separator();
    ImGui::InputInt("Target Samples", &m_captureTargetSamples);
    if (m_captureTargetSamples < 1)
    {
        m_captureTargetSamples = 1;
    }

    if (ImGui::Button("Start Capture") && m_rayTracingManager)
    {
        m_showNormalColor = false;
        m_pbrDebugView = static_cast<int>(RayTracingManager::c_pbrDebugBeauty);
        m_enableAccumulation = true;
        m_captureActive = true;
        m_saveCurrentRequested = false;
        const UINT captureTargetSamples = static_cast<UINT>(m_captureTargetSamples);
        if (accumulatedSamples > captureTargetSamples)
        {
            m_rayTracingManager->ResetAccumulation();
            m_captureStatus = "Capturing...";
        }
        else
        {
            m_captureStatus = accumulatedSamples == captureTargetSamples
                ? "Saving capture..."
                : "Capturing...";
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Current"))
    {
        m_saveCurrentRequested = true;
        m_captureStatus = "Saving current frame...";
    }

    if (m_captureActive)
    {
        ImGui::Text("Capture: %u / %d", accumulatedSamples, m_captureTargetSamples);
    }
    if (!m_captureStatus.empty())
    {
        ImGui::TextUnformatted(m_captureStatus.c_str());
    }
    ImGui::End();

    ImGui::Render();
}

void D3D12Renderer::RenderImGuiDrawData()
{
    if (!m_imguiInitialized)
        return;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCurrentRenderTargetView();
    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_imguiDescriptorHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, descriptorHeaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
}

void D3D12Renderer::ReleaseRenderTargets()
{
    for (auto& renderTarget : m_renderTargets)
    {
        renderTarget.Reset();
    }
}

bool D3D12Renderer::QueueTextureCapture(
    ID3D12Resource* sourceTexture,
    const std::wstring& filePath,
    CaptureFormat format,
    UINT sampleCount)
{
    if (!sourceTexture || !m_device || !m_commandList)
        return false;

    const D3D12_RESOURCE_DESC sourceDesc = sourceTexture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    m_device->GetCopyableFootprints(
        &sourceDesc,
        0,
        1,
        0,
        &footprint,
        &numRows,
        &rowSizeInBytes,
        &totalBytes);
    const UINT64 readbackSize = footprint.Offset +
        static_cast<UINT64>(footprint.Footprint.RowPitch) * numRows;

    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = readbackSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> readbackBuffer;
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&readbackBuffer));
    if (ReportFailure(hr, L"Capture readback buffer creation failed."))
        return false;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readbackBuffer.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = sourceTexture;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    PendingCapture pendingCapture;
    pendingCapture.readbackBuffer = readbackBuffer;
    pendingCapture.filePath = filePath;
    pendingCapture.width = static_cast<UINT>(sourceDesc.Width);
    pendingCapture.height = sourceDesc.Height;
    pendingCapture.rowPitch = footprint.Footprint.RowPitch;
    pendingCapture.readbackSize = readbackSize;
    pendingCapture.sampleCount = sampleCount > 0 ? sampleCount : 1u;
    pendingCapture.format = format;
    m_pendingCaptures.push_back(pendingCapture);
    return true;
}

void D3D12Renderer::ConfigureBenchmark(
    bool enabled,
    const std::wstring& outputPath,
    UINT frameLimit)
{
    m_benchmarkEnabled = enabled;
    m_benchmarkOutputPath = outputPath;
    m_benchmarkFrameLimit = frameLimit;
}

void D3D12Renderer::ConfigureAutomatedCapture(
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
    UINT validationSeed)
{
    m_captureTargetSamples = static_cast<int>(sampleCount > 0 ? sampleCount : 1u);
    m_captureOutputPrefix = outputPrefix;
    const UINT clampedMaxBounce = maxBounce < 1u ? 1u : (maxBounce > 8u ? 8u : maxBounce);
    m_maxBounce = static_cast<int>(clampedMaxBounce);
    m_sceneType = sceneType <= RayTracingManager::c_scenePbrGpuValidation
        ? static_cast<int>(sceneType)
        : static_cast<int>(RayTracingManager::c_sceneCornellBox);
    m_pbrMetallic = pbrMetallic;
    m_pbrRoughness = pbrRoughness;
    m_overridePbrMaterial = overridePbrMaterial;
    m_enableIbl = enableIbl;
    m_iblIntensity = iblIntensity;
    m_validationSeed = validationSeed;
    m_showNormalColor = false;
    m_pbrDebugView = static_cast<int>(
        pbrDebugView <= RayTracingManager::c_pbrDebugNormal
        ? pbrDebugView
        : RayTracingManager::c_pbrDebugBeauty);
    m_enableAccumulation = true;
    m_captureActive = true;
    m_exitAfterCapture = true;
    m_captureStatus = "Automated capture running...";
}

void D3D12Renderer::SavePendingCaptures()
{
    if (m_pendingCaptures.empty())
        return;

    bool allSaved = true;
    for (PendingCapture& pendingCapture : m_pendingCaptures)
    {
        if (!pendingCapture.readbackBuffer)
        {
            allSaved = false;
            continue;
        }

        void* mappedPixels = nullptr;
        D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(pendingCapture.readbackSize) };
        HRESULT hr = pendingCapture.readbackBuffer->Map(0, &readRange, &mappedPixels);
        if (ReportFailure(hr, L"Capture readback mapping failed."))
        {
            allSaved = false;
            continue;
        }

        bool saved = false;
        if (pendingCapture.format == CaptureFormat::Pfm)
        {
            saved = SavePfmFile(
                pendingCapture.filePath,
                pendingCapture.width,
                pendingCapture.height,
                pendingCapture.rowPitch,
                pendingCapture.sampleCount,
                mappedPixels);
        }
        else
        {
            saved = SavePngFile(
                pendingCapture.filePath,
                pendingCapture.width,
                pendingCapture.height,
                pendingCapture.rowPitch,
                mappedPixels);
        }

        D3D12_RANGE writeRange = { 0, 0 };
        pendingCapture.readbackBuffer->Unmap(0, &writeRange);
        allSaved &= saved;
    }

    m_pendingCaptures.clear();
    m_captureStatus = allSaved
        ? "Saved PNG preview and linear HDR PFM."
        : "One or more capture files failed to save.";

    if (m_exitAfterCapture)
    {
        m_exitAfterCapture = false;
        PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
    }
}

bool D3D12Renderer::SavePngFile(
    const std::wstring& filePath,
    UINT width,
    UINT height,
    UINT rowPitch,
    const void* pixels) const
{
    const UINT tightRowPitch = width * 4;
    const UINT tightImageSize = tightRowPitch * height;
    std::vector<BYTE> tightPixels(tightImageSize);

    const BYTE* sourcePixels = static_cast<const BYTE*>(pixels);
    for (UINT y = 0; y < height; ++y)
    {
        std::memcpy(
            tightPixels.data() + static_cast<std::size_t>(y) * tightRowPitch,
            sourcePixels + static_cast<std::size_t>(y) * rowPitch,
            tightRowPitch);
    }

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool shouldUninitialize = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
        return !ReportFailure(coHr, L"COM initialization for PNG capture failed.");

    auto fail = [&]() -> bool
    {
        DeleteFileW(filePath.c_str());
        if (shouldUninitialize)
            CoUninitialize();
        return false;
    };

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (ReportFailure(hr, L"WIC factory creation failed."))
        return fail();

    Microsoft::WRL::ComPtr<IWICBitmap> sourceBitmap;
    hr = factory->CreateBitmapFromMemory(
        width,
        height,
        GUID_WICPixelFormat32bppRGBA,
        tightRowPitch,
        tightImageSize,
        tightPixels.data(),
        &sourceBitmap);
    if (ReportFailure(hr, L"WIC capture bitmap creation failed."))
        return fail();

    Microsoft::WRL::ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (ReportFailure(hr, L"WIC stream creation failed."))
        return fail();

    hr = stream->InitializeFromFilename(filePath.c_str(), GENERIC_WRITE);
    if (ReportFailure(hr, L"Capture PNG file creation failed."))
        return fail();

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (ReportFailure(hr, L"WIC PNG encoder creation failed."))
        return fail();

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (ReportFailure(hr, L"WIC PNG encoder initialization failed."))
        return fail();

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (ReportFailure(hr, L"WIC PNG frame creation failed."))
        return fail();

    hr = frame->Initialize(nullptr);
    if (ReportFailure(hr, L"WIC PNG frame initialization failed."))
        return fail();

    hr = frame->SetSize(width, height);
    if (ReportFailure(hr, L"WIC PNG size setup failed."))
        return fail();

    WICPixelFormatGUID framePixelFormat = GUID_WICPixelFormat32bppRGBA;
    hr = frame->SetPixelFormat(&framePixelFormat);
    if (ReportFailure(hr, L"WIC PNG pixel format setup failed."))
        return fail();

    if (IsEqualGUID(framePixelFormat, GUID_WICPixelFormat32bppRGBA))
    {
        hr = frame->WritePixels(
            height,
            tightRowPitch,
            tightImageSize,
            tightPixels.data());
        if (ReportFailure(hr, L"WIC PNG pixel write failed."))
            return fail();
    }
    else
    {
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (ReportFailure(hr, L"WIC PNG format converter creation failed."))
            return fail();

        hr = converter->Initialize(
            sourceBitmap.Get(),
            framePixelFormat,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);
        if (ReportFailure(hr, L"WIC PNG format conversion failed."))
            return fail();

        hr = frame->WriteSource(converter.Get(), nullptr);
        if (ReportFailure(hr, L"WIC PNG converted pixel write failed."))
            return fail();
    }

    hr = frame->Commit();
    if (ReportFailure(hr, L"WIC PNG frame commit failed."))
        return fail();

    hr = encoder->Commit();
    if (ReportFailure(hr, L"WIC PNG encoder commit failed."))
        return fail();

    if (shouldUninitialize)
        CoUninitialize();

    return true;
}

bool D3D12Renderer::SavePfmFile(
    const std::wstring& filePath,
    UINT width,
    UINT height,
    UINT rowPitch,
    UINT sampleCount,
    const void* pixels) const
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, filePath.c_str(), L"wb") != 0 || !file)
        return false;

    if (std::fprintf(file, "PF\n%u %u\n-1.0\n", width, height) < 0)
    {
        std::fclose(file);
        DeleteFileW(filePath.c_str());
        return false;
    }

    const float inverseSampleCount = 1.0f / static_cast<float>(sampleCount > 0 ? sampleCount : 1u);
    const BYTE* sourcePixels = static_cast<const BYTE*>(pixels);
    bool saved = true;
    for (UINT outputY = 0; outputY < height && saved; ++outputY)
    {
        const UINT sourceY = height - 1u - outputY;
        const float* sourceRow = reinterpret_cast<const float*>(
            sourcePixels + static_cast<std::size_t>(sourceY) * rowPitch);

        for (UINT x = 0; x < width; ++x)
        {
            const float rgb[3] =
            {
                sourceRow[x * 4 + 0] * inverseSampleCount,
                sourceRow[x * 4 + 1] * inverseSampleCount,
                sourceRow[x * 4 + 2] * inverseSampleCount
            };
            if (std::fwrite(rgb, sizeof(float), 3, file) != 3)
            {
                saved = false;
                break;
            }
        }
    }

    if (std::fclose(file) != 0)
        saved = false;

    if (!saved)
        DeleteFileW(filePath.c_str());

    return saved;
}

std::wstring D3D12Renderer::BuildCaptureFilePath(UINT sampleCount, const wchar_t* extension) const
{
    if (!m_captureOutputPrefix.empty())
    {
        const std::wstring filePath = m_captureOutputPrefix + extension;
        wchar_t absolutePath[MAX_PATH] = {};
        const DWORD absolutePathLength = GetFullPathNameW(
            filePath.c_str(),
            _countof(absolutePath),
            absolutePath,
            nullptr);
        const std::wstring directorySource =
            absolutePathLength > 0 && absolutePathLength < _countof(absolutePath)
            ? std::wstring(absolutePath, absolutePathLength)
            : filePath;
        const size_t separator = directorySource.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
        {
            const std::wstring directory = directorySource.substr(0, separator);
            SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
        }
        return filePath;
    }

    CreateDirectoryW(L"Captures", nullptr);

    SYSTEMTIME time = {};
    GetLocalTime(&time);

    std::wostringstream path;
    path << L"Captures\\capture_"
         << std::setfill(L'0')
         << std::setw(4) << time.wYear
         << std::setw(2) << time.wMonth
         << std::setw(2) << time.wDay
         << L"_"
         << std::setw(2) << time.wHour
         << std::setw(2) << time.wMinute
         << std::setw(2) << time.wSecond
         << L"_"
         << sampleCount
         << L"spp"
         << extension;

    return path.str();
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::GetCurrentRenderTargetView() const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvDescriptorSize;
    return handle;
}

void D3D12Renderer::TransitionCurrentBackBuffer(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);
}

bool D3D12Renderer::ReportFailure(HRESULT hr, const wchar_t* message) const
{
    if (SUCCEEDED(hr))
        return false;

    std::wostringstream text;
    text << message
         << L"\nHRESULT: 0x"
         << std::uppercase
         << std::hex
         << std::setw(8)
         << std::setfill(L'0')
         << static_cast<unsigned int>(hr);

    MessageBoxW(m_hWnd, text.str().c_str(), L"D3D12 Error", MB_OK | MB_ICONERROR);
    return true;
}


