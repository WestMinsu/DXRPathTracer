#pragma once

#include <array>
#include <memory>
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

    D3D12_CPU_DESCRIPTOR_HANDLE GetCurrentRenderTargetView() const;
    void TransitionCurrentBackBuffer(D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    bool ReportFailure(HRESULT hr, const wchar_t* message) const;

    HWND m_hWnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_frameIndex = 0;
    UINT m_rtvDescriptorSize = 0;
    UINT64 m_fenceValue = 0;
    HANDLE m_fenceEvent = nullptr;
    bool m_imguiInitialized = false;
    bool m_showNormalColor = true;

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
};
