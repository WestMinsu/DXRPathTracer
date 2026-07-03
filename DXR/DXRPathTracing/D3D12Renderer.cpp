#include "D3D12Renderer.h"

#include <iomanip>
#include <sstream>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
    constexpr float c_clearColor[] = { 0.00f, 0.00f, 1.00f, 1.0f };

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

    return true;
}

void D3D12Renderer::Render()
{
    if (!m_swapChain || m_width == 0 || m_height == 0)
        return;

    HRESULT hr = m_commandAllocator->Reset();
    if (ReportFailure(hr, L"Command allocator reset failed."))
        return;

    hr = m_commandList->Reset(m_commandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"Command list reset failed."))
        return;

    TransitionCurrentBackBuffer(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCurrentRenderTargetView();
    m_commandList->ClearRenderTargetView(rtvHandle, c_clearColor, 0, nullptr);

    TransitionCurrentBackBuffer(D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

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
    CreateRenderTargetViews();
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

        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
        if (SUCCEEDED(hr))
            return true;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> warpAdapter;
    hr = m_factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
    if (ReportFailure(hr, L"WARP adapter lookup failed."))
        return false;

    hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
    return !ReportFailure(hr, L"D3D12 device creation failed.");
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
