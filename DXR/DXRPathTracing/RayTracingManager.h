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
    static constexpr UINT c_sceneIndirectBounceStress = 3;
    static constexpr UINT c_lightingModeBsdf = 0;
    static constexpr UINT c_lightingModeNee = 1;
    static constexpr UINT c_lightingModeMis = 2;
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
    void SetRussianRouletteEnabled(bool enabled);
    void SetLightingMode(UINT lightingMode);
    void SetAtrousEnabled(bool enabled);
    void SetAtrousIterationCount(UINT iterationCount)
    {
        m_atrousIterationCount =
            iterationCount < 1u ? 1u : (iterationCount > 5u ? 5u : iterationCount);
    }
    void SetAtrousColorSigma(float colorSigma)
    {
        m_atrousColorSigma =
            colorSigma < 0.25f ? 0.25f : (colorSigma > 16.0f ? 16.0f : colorSigma);
    }
    void SetEnableAccumulation(bool enableAccumulation);
    void SetSceneType(UINT sceneType);
    void SetSceneFilePath(const std::wstring& sceneFilePath) { m_sceneFilePath = sceneFilePath; }
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
    double GetDynamicObjectLinearSpeed() const
    {
        return m_dynamicObjectLinearSpeed;
    }
    double GetDynamicObjectAngularSpeed() const
    {
        return m_dynamicObjectAngularSpeed;
    }
    bool HasDynamicSphere() const { return m_hasDynamicSphere; }
    bool IsDynamicSphereVisible() const
    {
        return m_dynamicSphereVisible;
    }
    void SetDynamicSphereVisible(bool visible);
    void SetDynamicSphereAnimationEnabled(bool enabled);
    void SetDynamicSphereDeterministicTimeline(bool enabled);
    void ResetDynamicSphereTimeline();
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
    static constexpr UINT c_shaderPayloadSize = 13 * sizeof(float);
    static constexpr UINT c_shaderAttributeSize = 2 * sizeof(float);
    static constexpr UINT c_maxBounce = 8;
    // At most one visibility ray is nested below a radiance vertex. The
    // terminal radiance vertex exits before tracing NEE or another bounce.
    static constexpr UINT c_maxRecursionDepth = c_maxBounce + 1;
    static constexpr UINT c_tlasFrameCount = 2;
    struct GeometryRange
    {
        UINT vertexOffset = 0;
        UINT vertexCount = 0;
        UINT indexOffset = 0;
        UINT indexCount = 0;
        UINT primitiveOffset = 0;
    };
    bool CreateOutputTexture();
    bool CreateStatisticsResources();
    bool CreateEnvironmentMap();
    bool CreateGlobalRootSignature();
    bool CreateAtrousPipeline();
    bool CreateRaytracingPipelineState();
    bool CreateShaderTables();
    bool CreateMissShaderTable();
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
    bool BuildBottomLevelAccelerationStructure(
        const GeometryRange& geometry,
        const wchar_t* debugName,
        Microsoft::WRL::ComPtr<ID3D12Resource>& accelerationStructure,
        Microsoft::WRL::ComPtr<ID3D12Resource>& scratchBuffer);
    bool BuildTopLevelAccelerationStructure();
    bool UpdateTopLevelAccelerationStructure(
        ID3D12GraphicsCommandList4* commandList);
    bool WriteInstanceDescriptors(
        UINT frameIndex,
        float spherePositionX,
        float sphereRollRadians);
    void UpdateDynamicSphereMotion();
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
    bool LoadCompiledAtrousShader(
        std::vector<std::uint8_t>& shaderBytes) const;
    bool ReadBinaryFile(const std::wstring& path, std::vector<std::uint8_t>& bytes) const;
    std::wstring GetCompiledShaderPath() const;
    std::wstring GetCompiledAtrousShaderPath() const;
    std::wstring GetEnvironmentMapPath() const;
    void DispatchAtrousFilter(ID3D12GraphicsCommandList4* commandList);
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
    bool m_enableRussianRoulette = false;
    bool m_enableAtrous = false;
    UINT m_atrousIterationCount = 2;
    float m_atrousColorSigma = 4.0f;
    UINT m_lightingMode = c_lightingModeBsdf;
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
    std::wstring m_sceneManifestPath;
    std::wstring m_sponzaLightConfigPath;
    bool m_composeModelRoom = false;
    bool m_sponzaLite = false;
    bool m_autoFrameCamera = false;
    bool m_hasDynamicSphere = false;
    bool m_dynamicSphereVisible = true;
    bool m_dynamicSphereVisibilityDirty = false;
    bool m_dynamicSphereAnimationEnabled = true;
    bool m_dynamicSphereDeterministicTimeline = false;
    float m_dynamicSphereRadius = 0.0f;
    float m_dynamicSphereTrackCenterX = 0.0f;
    float m_dynamicSphereCenterY = 0.0f;
    float m_dynamicSphereCenterZ = 0.0f;
    float m_dynamicSphereMotionAmplitude = 0.0f;
    float m_dynamicSpherePositionX = 0.0f;
    float m_dynamicSphereRollRadians = 0.0f;
    double m_dynamicObjectLinearSpeed = 0.0;
    double m_dynamicObjectAngularSpeed = 0.0;
    bool m_dynamicObjectMovedThisFrame = false;
    UINT64 m_dynamicSceneFrameIndex = 0;
    GeometryRange m_staticGeometry;
    GeometryRange m_dynamicSphereGeometry;
    std::array<float, 3> m_sceneBoundsMin = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_sceneBoundsMax = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_cameraPosition = { 0.0f, 0.15f, -1.2f };
    std::array<float, 3> m_cameraTarget = { 0.0f, 0.0f, 0.0f };

    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_outputTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_accumulationTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normalDepthTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialGuideTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_indirectAccumulationTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_luminanceMomentsTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_atrousFilterTextureA;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_atrousFilterTextureB;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statisticsBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statisticsResetBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_statisticsReadbackBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_environmentMap;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_environmentDistributionBuffer;
    UINT m_environmentResolution = 0;
    UINT m_environmentTexelCount = 0;
    float m_environmentPower = 0.0f;
    float m_areaLightPower = 0.0f;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_globalRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_atrousRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_atrousPipelineState;
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_instanceMetadataBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_emissiveTriangleBuffer;
    UINT m_emissiveTriangleCount = 0;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_materialTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bottomLevelAS;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dynamicSphereBottomLevelAS;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_topLevelAS;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,
        c_tlasFrameCount> m_instanceDescBuffers;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_blasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dynamicSphereBlasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tlasScratchBuffer;
    FrameStatistics m_frameStatistics;
};


