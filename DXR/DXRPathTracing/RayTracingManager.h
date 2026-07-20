#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

class RayTracingManager
{
public:
    static constexpr UINT c_statisticsRayDepthCount = 9;

    struct FrameStatistics
    {
        std::array<UINT64, c_statisticsRayDepthCount> raysByDepth = {};
        UINT64 shadowRays = 0;
        UINT64 hitCount = 0;
        UINT64 missCount = 0;

        UINT64 GetPrimaryRayCount() const { return raysByDepth[0]; }
        UINT64 GetBounceRayCount() const
        {
            UINT64 total = 0;
            for (UINT depth = 1; depth < c_statisticsRayDepthCount; ++depth)
                total += raysByDepth[depth];
            return total;
        }
        double GetAveragePathLength() const
        {
            const UINT64 primaryRays = GetPrimaryRayCount();
            return primaryRays > 0
                ? static_cast<double>(primaryRays + GetBounceRayCount()) /
                    static_cast<double>(primaryRays)
                : 0.0;
        }
    };

    ~RayTracingManager();

    static constexpr UINT c_sceneCornellBox = 0;
    static constexpr UINT c_scenePbrGgx = 1;
    static constexpr UINT c_scenePbrGpuValidation = 2;
    static constexpr UINT c_pbrDebugBeauty = 0;
    static constexpr UINT c_pbrDebugAlbedo = 1;
    static constexpr UINT c_pbrDebugMetallic = 2;
    static constexpr UINT c_pbrDebugRoughness = 3;
    static constexpr UINT c_pbrDebugDepth = 4;
    static constexpr UINT c_pbrDebugMaterialId = 5;
    static constexpr UINT c_pbrDebugNormal = 6;

    bool Initialize(HWND hWnd, ID3D12Device5* device, UINT width, UINT height);
    bool Resize(UINT width, UINT height);
    void DispatchRays(ID3D12GraphicsCommandList4* commandList);
    void SetShowNormalColor(bool showNormalColor);
    void SetMaxBounce(UINT maxBounce);
    void SetEnableAccumulation(bool enableAccumulation);
    void SetSceneType(UINT sceneType);
    void SetSceneFilePath(const std::wstring& sceneFilePath) { m_sceneFilePath = sceneFilePath; }
    void SetComposeModelRoom(bool enabled) { m_composeModelRoom = enabled; }
    void SetPbrDebugView(UINT pbrDebugView);
    void SetPbrMaterial(float metallic, float roughness);
    void SetPbrMaterialOverride(bool enabled);
    void SetIblSettings(bool enableIbl, float intensity);
    bool SetCamera(
        const std::array<float, 3>& position,
        const std::array<float, 3>& target);
    const std::array<float, 3>& GetCameraPosition() const
    {
        return m_cameraPosition;
    }
    const std::array<float, 3>& GetCameraTarget() const
    {
        return m_cameraTarget;
    }
    float GetSceneDiagonal() const;
    void SetEnableStatistics(bool enabled) { m_enableStatistics = enabled; }
    void ReadFrameStatistics();
    void SetValidationSeed(UINT validationSeed) { m_validationSeed = validationSeed; }
    void SetExposure(float exposure);
    void ResetAccumulation() { m_accumulatedSampleCount = 0; }
    UINT GetAccumulatedSampleCount() const { return m_accumulatedSampleCount; }
    const FrameStatistics& GetFrameStatistics() const { return m_frameStatistics; }

    ID3D12Resource* GetOutputResource() const { return m_outputTexture.Get(); }
    ID3D12Resource* GetAccumulationResource() const { return m_accumulationTexture.Get(); }
    ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_descriptorHeap.Get(); }
    ID3D12RootSignature* GetGlobalRootSignature() const { return m_globalRootSignature.Get(); }
    ID3D12StateObject* GetStateObject() const { return m_stateObject.Get(); }
    ID3D12Resource* GetRayGenShaderTable() const { return m_rayGenShaderTable.Get(); }
    UINT GetRayGenShaderRecordSize() const { return m_rayGenShaderRecordSize; }

private:
    static constexpr DXGI_FORMAT c_outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT c_accumulationFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    static constexpr UINT c_shaderPayloadSize = 4 * sizeof(float);
    static constexpr UINT c_shaderAttributeSize = 2 * sizeof(float);
    static constexpr UINT c_maxBounce = 8;
    static constexpr UINT c_maxRecursionDepth = c_maxBounce + 1;
    bool CreateOutputTexture();
    bool CreateStatisticsResources();
    bool CreateEnvironmentMap();
    bool CreateGlobalRootSignature();
    bool CreateRaytracingPipelineState();
    bool CreateShaderTables();
    bool CreateShaderTable(const wchar_t* shaderExportName,
        ID3D12Resource** shaderTable,
        UINT* shaderRecordSize,
        const wchar_t* debugName);
    bool CreateAccelerationStructures();
    bool CreateBuildCommandObjects();
    bool CreateStaticGeometryBuffers();
    bool CreateMaterialTextures(const struct SceneData& scene);
    void UpdateCameraFromSceneBounds();
    bool BuildBottomLevelAccelerationStructure();
    bool BuildTopLevelAccelerationStructure();
    bool ExecuteBuildCommandListAndWait();
    bool CreateUploadBuffer(const void* data,
        UINT64 sizeInBytes,
        const wchar_t* debugName,
        Microsoft::WRL::ComPtr<ID3D12Resource>& resource);
    bool CreateAccelerationStructureBuffer(UINT64 sizeInBytes,
        const wchar_t* debugName,
        Microsoft::WRL::ComPtr<ID3D12Resource>& resource);
    bool CreateScratchBuffer(UINT64 sizeInBytes,
        const wchar_t* debugName,
        Microsoft::WRL::ComPtr<ID3D12Resource>& resource);
    bool LoadCompiledShader(std::vector<std::uint8_t>& shaderBytes) const;
    bool ReadBinaryFile(const std::wstring& path, std::vector<std::uint8_t>& bytes) const;
    std::wstring GetCompiledShaderPath() const;
    std::wstring GetEnvironmentMapPath() const;
    bool ReportFailure(HRESULT hr, const wchar_t* message) const;
    void ReportMessage(const std::wstring& message) const;

    HWND m_hWnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_descriptorSize = 0;
    UINT m_rayGenShaderRecordSize = 0;
    UINT m_missShaderRecordSize = 0;
    UINT m_hitGroupShaderRecordSize = 0;
    UINT m_frameIndex = 0;
    UINT m_accumulatedSampleCount = 0;
    UINT m_vertexCount = 0;
    UINT m_indexCount = 0;
    UINT64 m_buildFenceValue = 0;
    HANDLE m_buildFenceEvent = nullptr;
    bool m_showNormalColor = true;
    bool m_enableAccumulation = true;
    UINT m_maxBounce = 3;
    UINT m_sceneType = c_sceneCornellBox;
    UINT m_pbrDebugView = c_pbrDebugBeauty;
    float m_pbrMetallic = 1.0f;
    float m_pbrRoughness = 0.35f;
    bool m_overridePbrMaterial = false;
    bool m_enableIbl = true;
    bool m_enableStatistics = false;
    float m_iblIntensity = 1.0f;
    UINT m_validationSeed = 0;
    float m_exposure = 0.0f;
    std::wstring m_sceneFilePath;
    bool m_composeModelRoom = false;
    bool m_autoFrameCamera = false;
    std::array<float, 3> m_sceneBoundsMin = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_sceneBoundsMax = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_cameraPosition = { 0.0f, 0.15f, -1.2f };
    std::array<float, 3> m_cameraTarget = { 0.0f, 0.0f, 0.0f };

    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_outputTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_accumulationTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statisticsBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statisticsResetBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statisticsReadbackBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_environmentMap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_globalRootSignature;
    Microsoft::WRL::ComPtr<ID3D12StateObject> m_stateObject;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_rayGenShaderTable;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_missShaderTable;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_hitGroupShaderTable;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_buildCommandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_buildCommandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> m_buildCommandList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_buildFence;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneMaterialBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_primitiveMaterialIndexBuffer;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_materialTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bottomLevelAS;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_topLevelAS;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_instanceDescBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_blasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tlasScratchBuffer;
    FrameStatistics m_frameStatistics;
};


