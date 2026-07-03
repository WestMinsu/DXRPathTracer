#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

class RayTracingManager
{
public:
    bool Initialize(HWND hWnd, ID3D12Device5* device, UINT width, UINT height);
    bool Resize(UINT width, UINT height);

    ID3D12Resource* GetOutputResource() const { return m_outputTexture.Get(); }
    ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_descriptorHeap.Get(); }
    ID3D12RootSignature* GetGlobalRootSignature() const { return m_globalRootSignature.Get(); }
    ID3D12StateObject* GetStateObject() const { return m_stateObject.Get(); }
    ID3D12Resource* GetRayGenShaderTable() const { return m_rayGenShaderTable.Get(); }
    UINT GetRayGenShaderRecordSize() const { return m_rayGenShaderRecordSize; }

private:
    static constexpr DXGI_FORMAT c_outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr UINT c_shaderPayloadSize = 4 * sizeof(float);
    static constexpr UINT c_shaderAttributeSize = 2 * sizeof(float);
    static constexpr UINT c_maxRecursionDepth = 1;

    bool CreateOutputTexture();
    bool CreateGlobalRootSignature();
    bool CreateRaytracingPipelineState();
    bool CreateRayGenShaderTable();
    bool LoadCompiledShader(std::vector<std::uint8_t>& shaderBytes) const;
    bool ReadBinaryFile(const std::wstring& path, std::vector<std::uint8_t>& bytes) const;
    std::wstring GetCompiledShaderPath() const;
    bool ReportFailure(HRESULT hr, const wchar_t* message) const;
    void ReportMessage(const std::wstring& message) const;

    HWND m_hWnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_descriptorSize = 0;
    UINT m_rayGenShaderRecordSize = 0;

    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_outputTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_globalRootSignature;
    Microsoft::WRL::ComPtr<ID3D12StateObject> m_stateObject;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_rayGenShaderTable;
};