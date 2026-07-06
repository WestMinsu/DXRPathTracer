#include "D3D12Renderer.h"

#include "RayTracingManager.h"

#include "ThirdParty/imgui/imgui.h"
#include "ThirdParty/imgui/backends/imgui_impl_dx12.h"
#include "ThirdParty/imgui/backends/imgui_impl_win32.h"

#include <iomanip>
#include <sstream>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
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

    m_rayTracingManager.reset(new RayTracingManager());
    if (!m_rayTracingManager->Initialize(m_hWnd, m_device.Get(), m_width, m_height))
        return false;

    if (!InitializeImGui())
        return false;

    return true;
}

void D3D12Renderer::Render()
{
    if (!m_swapChain || !m_rayTracingManager || m_width == 0 || m_height == 0)
        return;

    BuildImGuiFrame();
    m_rayTracingManager->SetShowNormalColor(m_showNormalColor);
    m_rayTracingManager->SetMaxBounce(static_cast<UINT>(m_maxBounce));
    m_rayTracingManager->SetEnableAccumulation(m_enableAccumulation);

    HRESULT hr = m_commandAllocator->Reset();
    if (ReportFailure(hr, L"Command allocator reset failed."))
        return;

    hr = m_commandList->Reset(m_commandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"Command list reset failed."))
        return;

    m_rayTracingManager->DispatchRays(m_commandList.Get());

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

    hr = m_commandList->Close();
    if (ReportFailure(hr, L"Command list close failed."))
        return;

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    hr = m_swapChain->Present(1, 0);
    if (ReportFailure(hr, L"Swap chain present failed."))
        return;

    WaitForGpu();
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
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
    ReleaseRenderTargets();

    HRESULT hr = m_swapChain->ResizeBuffers(c_frameCount, width, height, c_backBufferFormat, 0);
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
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = c_backBufferFormat;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = c_frameCount;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

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


