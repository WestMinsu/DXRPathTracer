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

    struct GpuProfileQueries
    {
        ID3D12QueryHeap* heap = nullptr;
        UINT tlasBegin = 0;
        UINT tlasEnd = 0;
        UINT pathTraceBegin = 0;
        UINT pathTraceEnd = 0;
        UINT temporalColorClipBegin = 0;
        UINT temporalColorClipEnd = 0;
        UINT atrousDiffuseBegin = 0;
        UINT atrousDiffuseEnd = 0;
        UINT atrousSpecularBegin = 0;
        UINT atrousSpecularEnd = 0;
    };

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
    static constexpr UINT c_sceneDynamicTransformTest = 4;
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
    static constexpr UINT c_temporalDebugNone = 0;
    static constexpr UINT c_temporalDebugHistoryLength = 1;
    static constexpr UINT c_temporalDebugRejectionMask = 2;
    static constexpr UINT c_temporalDebugMotionVector = 3;
    static constexpr UINT c_temporalDebugRejectionReason = 4;
    static constexpr UINT c_temporalDebugSurfaceError = 5;
    static constexpr UINT c_temporalDebugRadianceHistoryDifference = 6;
    static constexpr UINT c_specularMaterialWeightNone = 0;
    static constexpr UINT c_specularMaterialWeightAlbedo = 1;
    static constexpr UINT c_specularMaterialWeightF0 = 2;
    static constexpr UINT c_specularRoughnessWeightNone = 0;
    static constexpr UINT c_specularRoughnessWeightRoughness = 1;
    static constexpr UINT c_atrousKernel3x3 = 0;
    static constexpr UINT c_atrousKernel5x5 = 1;
    static constexpr UINT c_atrousDebugNone = 0;
    static constexpr UINT c_atrousDebugIterationCount = 1;

    bool Initialize(HWND hWnd, ID3D12Device5* device, UINT width, UINT height);
    bool Resize(UINT width, UINT height);
    void DispatchRays(
        ID3D12GraphicsCommandList4* commandList,
        const GpuProfileQueries* profileQueries = nullptr);
    void SetShowNormalColor(bool showNormalColor);
    void SetMaxBounce(UINT maxBounce);
    void SetSamplesPerPixel(UINT samplesPerPixel);
    void SetRussianRouletteEnabled(bool enabled);
    void SetLightingMode(UINT lightingMode);
    void SetTemporalReprojectionEnabled(bool enabled);
    void SetDynamicObjectReprojectionEnabled(bool enabled);
    void SetCurrentFrameVisibleResidualEnabled(bool enabled);
    void SetTemporalColorClipEnabled(bool enabled);
    void SetTemporalDebugView(UINT debugView);
    void SetAtrousEnabled(bool enabled);
    void SetAtrousIterationCount(UINT iterationCount)
    {
        m_atrousIterationCount =
            iterationCount < 1u ? 1u : (iterationCount > 8u ? 8u : iterationCount);
    }
    void SetAtrousSpecularIterationCount(UINT iterationCount)
    {
        m_atrousSpecularIterationCount =
            iterationCount < 1u ? 1u : (iterationCount > 8u ? 8u : iterationCount);
    }
    void SetAtrousAdaptiveIterationsEnabled(bool enabled)
    {
        m_enableAtrousAdaptiveIterations = enabled;
    }
    void SetAtrousDebugView(UINT debugView)
    {
        m_atrousDebugView = debugView > c_atrousDebugIterationCount
            ? c_atrousDebugNone
            : debugView;
    }
    void SetAtrousSpecularMaterialWeightMode(UINT mode)
    {
        m_atrousSpecularMaterialWeightMode = mode > c_specularMaterialWeightF0
            ? c_specularMaterialWeightF0
            : mode;
    }
    void SetAtrousSpecularRoughnessWeightMode(UINT mode)
    {
        m_atrousSpecularRoughnessWeightMode =
            mode > c_specularRoughnessWeightRoughness
            ? c_specularRoughnessWeightRoughness
            : mode;
    }
    void SetAtrousKernelMode(UINT mode)
    {
        m_atrousKernelMode = mode > c_atrousKernel5x5
            ? c_atrousKernel5x5
            : mode;
    }
    void SetAtrousNormalExponent(float normalExponent)
    {
        m_atrousNormalExponent = normalExponent < 1.0f
            ? 1.0f
            : (normalExponent > 256.0f ? 256.0f : normalExponent);
    }
    void SetAtrousDepthSigma(float depthSigma)
    {
        m_atrousDepthSigma = depthSigma < 0.0001f
            ? 0.0001f
            : (depthSigma > 0.1f ? 0.1f : depthSigma);
    }
    void SetAtrousAdaptiveEdgeWeightsEnabled(bool enabled)
    {
        m_enableAtrousAdaptiveEdgeWeights = enabled;
    }
    void SetAtrousAdaptiveDynamicWeights(
        float normalExponent,
        float depthSigma)
    {
        m_atrousAdaptiveDynamicNormalExponent = normalExponent < 1.0f
            ? 1.0f
            : (normalExponent > 256.0f ? 256.0f : normalExponent);
        m_atrousAdaptiveDynamicDepthSigma = depthSigma < 0.0001f
            ? 0.0001f
            : (depthSigma > 0.1f ? 0.1f : depthSigma);
    }
    void SetAtrousAdaptiveLowHistoryWeights(
        float normalExponent,
        float depthSigma)
    {
        m_atrousAdaptiveLowHistoryNormalExponent = normalExponent < 1.0f
            ? 1.0f
            : (normalExponent > 256.0f ? 256.0f : normalExponent);
        m_atrousAdaptiveLowHistoryDepthSigma = depthSigma < 0.0001f
            ? 0.0001f
            : (depthSigma > 0.1f ? 0.1f : depthSigma);
    }
    void SetAtrousAdaptiveStableWeights(
        float normalExponent,
        float depthSigma)
    {
        m_atrousAdaptiveStableNormalExponent = normalExponent < 1.0f
            ? 1.0f
            : (normalExponent > 256.0f ? 256.0f : normalExponent);
        m_atrousAdaptiveStableDepthSigma = depthSigma < 0.0001f
            ? 0.0001f
            : (depthSigma > 0.1f ? 0.1f : depthSigma);
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
    void SetTextureLodSettings(bool enabled, float bias);
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
    bool HasDynamicCube() const { return m_hasDynamicCube; }
    bool IsDynamicSphereVisible() const
    {
        return m_dynamicSphereVisible;
    }
    void SetDynamicSphereVisible(bool visible);
    void SetDynamicSphereAnimationEnabled(bool enabled);
    void SetDynamicCubeVisible(bool visible);
    void SetDynamicCubeAnimationEnabled(bool enabled);
    void SetDynamicTestSphereMaterialPreset(UINT preset);
    void SetDynamicTestCubeMaterialPreset(UINT preset);
    void SetDynamicSphereDeterministicTimeline(bool enabled);
    void ResetDynamicSphereTimeline();
    void SetEnableStatistics(bool enabled) { m_enableStatistics = enabled; }
    void ReadFrameStatistics();
    void SetValidationSeed(UINT validationSeed) { m_validationSeed = validationSeed; }
    void SetExposure(float exposure);
    void ResetAccumulation();
    UINT GetAccumulatedSampleCount() const { return m_accumulatedSampleCount; }
    const FrameStatistics& GetFrameStatistics() const { return m_frameStatistics; }

    ID3D12Resource* GetOutputResource() const { return m_outputTexture.Get(); }
    ID3D12Resource* GetAccumulationResource() const
    {
        return m_enableTemporalReprojection
            ? m_previousAccumulationTexture.Get()
            : m_accumulationTexture.Get();
    }
    ID3D12DescriptorHeap* GetDescriptorHeap() const { return m_descriptorHeap.Get(); }
    ID3D12RootSignature* GetGlobalRootSignature() const { return m_globalRootSignature.Get(); }
    ID3D12StateObject* GetStateObject() const { return m_stateObject.Get(); }
    ID3D12Resource* GetRayGenShaderTable() const { return m_rayGenShaderTable.Get(); }
    UINT GetRayGenShaderRecordSize() const { return m_rayGenShaderRecordSize; }

private:
    static constexpr DXGI_FORMAT c_outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT c_accumulationFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
    static constexpr UINT c_shaderPayloadSize = 16 * sizeof(float);
    static constexpr UINT c_shaderAttributeSize = 2 * sizeof(float);
    static constexpr UINT c_maxBounce = 8;
    static constexpr UINT c_tlasFrameCount = 2;
    struct GeometryRange
    {
        UINT vertexOffset = 0;
        UINT vertexCount = 0;
        UINT indexOffset = 0;
        UINT indexCount = 0;
        UINT primitiveOffset = 0;
        bool containsAlphaMask = false;
    };
    bool CreateOutputTexture();
    bool CreateStatisticsResources();
    bool CreateEnvironmentMap();
    bool CreateGlobalRootSignature();
    bool CreateAtrousPipeline();
    bool CreateTemporalColorClipPipeline();
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
        const GeometryRange* geometries,
        UINT geometryCount,
        const wchar_t* debugName,
        Microsoft::WRL::ComPtr<ID3D12Resource>& accelerationStructure,
        Microsoft::WRL::ComPtr<ID3D12Resource>& scratchBuffer);
    bool BuildTopLevelAccelerationStructure();
    bool UpdateTopLevelAccelerationStructure(
        ID3D12GraphicsCommandList4* commandList);
    bool WriteInstanceDescriptors(
        UINT frameIndex);
    bool WritePreviousInstanceTransforms(UINT frameIndex);
    void UpdateDynamicObjectMotion();
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
    bool LoadCompiledTemporalColorClipShader(
        std::vector<std::uint8_t>& shaderBytes) const;
    bool ReadBinaryFile(const std::wstring& path, std::vector<std::uint8_t>& bytes) const;
    std::wstring GetCompiledShaderPath() const;
    std::wstring GetCompiledAtrousShaderPath() const;
    std::wstring GetCompiledTemporalColorClipShaderPath() const;
    std::wstring GetEnvironmentMapPath() const;
    void DispatchAtrousFilter(
        ID3D12GraphicsCommandList4* commandList,
        const GpuProfileQueries* profileQueries);
    void DispatchTemporalColorClip(ID3D12GraphicsCommandList4* commandList);
    void WriteTemporalHistoryDescriptors();
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
    UINT m_temporalHistoryFrameCount = 0;
    UINT m_vertexCount = 0;
    UINT m_indexCount = 0;
    UINT64 m_buildFenceValue = 0;
    HANDLE m_buildFenceEvent = nullptr;
    bool m_showNormalColor = true;
    bool m_enableAccumulation = true;
    bool m_enableRussianRoulette = false;
    bool m_enableTemporalReprojection = false;
    bool m_enableDynamicObjectReprojection = true;
    bool m_useCurrentFrameVisibleResidual = true;
    bool m_enableTemporalColorClip = true;
    bool m_enableAtrous = false;
    UINT m_temporalDebugView = c_temporalDebugNone;
    UINT m_atrousIterationCount = 5;
    UINT m_atrousSpecularIterationCount = 4;
    bool m_enableAtrousAdaptiveIterations = false;
    UINT m_atrousDebugView = c_atrousDebugNone;
    UINT m_atrousSpecularMaterialWeightMode = c_specularMaterialWeightF0;
    UINT m_atrousSpecularRoughnessWeightMode =
        c_specularRoughnessWeightRoughness;
    UINT m_atrousKernelMode = c_atrousKernel3x3;
    float m_atrousNormalExponent = 32.0f;
    float m_atrousDepthSigma = 0.01f;
    bool m_enableAtrousAdaptiveEdgeWeights = false;
    float m_atrousAdaptiveDynamicNormalExponent = 1.0f;
    float m_atrousAdaptiveDynamicDepthSigma = 0.1f;
    float m_atrousAdaptiveLowHistoryNormalExponent = 8.0f;
    float m_atrousAdaptiveLowHistoryDepthSigma = 0.02f;
    float m_atrousAdaptiveStableNormalExponent = 32.0f;
    float m_atrousAdaptiveStableDepthSigma = 0.01f;
    float m_atrousColorSigma = 4.0f;
    UINT m_samplesPerPixel = 1;
    UINT m_lightingMode = c_lightingModeBsdf;
    UINT m_maxBounce = 3;
    UINT m_sceneType = c_sceneCornellBox;
    UINT m_pbrDebugView = c_pbrDebugBeauty;
    float m_pbrMetallic = 1.0f;
    float m_pbrRoughness = 0.35f;
    bool m_overridePbrMaterial = false;
    bool m_enableTextureLod = true;
    float m_textureLodBias = 0.0f;
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
    bool m_hasDynamicCube = false;
    bool m_dynamicSphereVisible = true;
    bool m_dynamicCubeVisible = true;
    bool m_dynamicSphereVisibilityDirty = false;
    bool m_dynamicCubeVisibilityDirty = false;
    bool m_dynamicSphereAnimationEnabled = true;
    bool m_dynamicCubeAnimationEnabled = true;
    bool m_dynamicSphereDeterministicTimeline = false;
    float m_dynamicSphereRadius = 0.0f;
    float m_dynamicSphereTrackCenterX = 0.0f;
    float m_dynamicSphereCenterY = 0.0f;
    float m_dynamicSphereCenterZ = 0.0f;
    float m_dynamicSphereMotionAmplitude = 0.0f;
    float m_dynamicSpherePositionX = 0.0f;
    float m_dynamicSphereRollRadians = 0.0f;
    float m_dynamicCubeHalfExtent = 0.68f;
    float m_dynamicCubeCenterX = 1.35f;
    float m_dynamicCubeCenterY = -0.32f;
    float m_dynamicCubeCenterZ = 1.90f;
    float m_dynamicCubePositionX = 1.35f;
    float m_dynamicCubePositionZ = 1.90f;
    float m_dynamicCubeRotationY = 0.0f;
    UINT m_dynamicTestSphereMaterialPreset = 0u;
    UINT m_dynamicTestCubeMaterialPreset = 3u;
    double m_dynamicObjectLinearSpeed = 0.0;
    double m_dynamicObjectAngularSpeed = 0.0;
    bool m_dynamicObjectMovedThisFrame = false;
    UINT64 m_dynamicSceneFrameIndex = 0;
    GeometryRange m_staticGeometry;
    GeometryRange m_staticAlphaGeometry;
    GeometryRange m_dynamicSphereGeometry;
    GeometryRange m_dynamicCubeGeometry;
    bool m_hasStaticAlphaGeometry = false;
    std::array<float, 3> m_sceneBoundsMin = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_sceneBoundsMax = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_cameraPosition = { 0.0f, 0.15f, -1.2f };
    std::array<float, 3> m_cameraTarget = { 0.0f, 0.0f, 0.0f };
    std::array<float, 3> m_previousCameraPosition =
        { 0.0f, 0.15f, -1.2f };
    std::array<float, 3> m_previousCameraTarget =
        { 0.0f, 0.0f, 0.0f };

    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_outputTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_accumulationTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_previousAccumulationTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_normalDepthTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_previousNormalDepthTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_materialGuideTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_previousMaterialGuideTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_metallicGuideTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_diffuseIndirectAccumulationTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_previousDiffuseIndirectAccumulationTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_specularIndirectAccumulationTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_previousSpecularIndirectAccumulationTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_diffuseLuminanceMomentsTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_previousDiffuseLuminanceMomentsTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_specularLuminanceMomentsTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_previousSpecularLuminanceMomentsTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_atrousFilterTextureA;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_atrousFilterTextureB;
    Microsoft::WRL::ComPtr<ID3D12Resource>
        m_atrousFilteredDiffuseTexture;
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
    Microsoft::WRL::ComPtr<ID3D12RootSignature>
        m_temporalColorClipRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>
        m_temporalColorClipPipelineState;
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
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sceneMetadataBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_emissiveTriangleBuffer;
    UINT m_emissiveTriangleCount = 0;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_materialTextures;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bottomLevelAS;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dynamicSphereBottomLevelAS;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dynamicCubeBottomLevelAS;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_topLevelAS;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,
        c_tlasFrameCount> m_instanceDescBuffers;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,
        c_tlasFrameCount> m_previousInstanceTransformBuffers;
    std::vector<std::array<float, 12>> m_currentInstanceTransforms;
    std::vector<std::array<float, 12>> m_previousInstanceTransforms;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_blasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dynamicSphereBlasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dynamicCubeBlasScratchBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_tlasScratchBuffer;
    FrameStatistics m_frameStatistics;
};


