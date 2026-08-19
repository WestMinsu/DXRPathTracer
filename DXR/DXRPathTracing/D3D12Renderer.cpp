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
#include <limits>
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
    constexpr float c_sphereMetallicValues[] =
    {
        0.0f,
        1.0f
    };
    constexpr float c_sphereRoughnessValues[] =
    {
        0.05f,
        0.2f,
        0.4f,
        0.7f,
        1.0f
    };
    const char c_sphereMetallicLabel[] =
        { 83,112,104,101,114,32,109,101,116,97,108,108,105,99,0 };
    const char c_sphereRoughnessLabel[] =
        { 83,112,104,101,114,32,114,111,117,103,104,110,101,115,115,0 };
    const char c_sphereMetallicItems[] =
        { 48,46,48,0,49,46,48,0,0 };
    const char c_sphereRoughnessItems[] =
        { 48,46,48,53,0,48,46,50,0,48,46,52,0,
          48,46,55,0,49,46,48,0,0 };
    constexpr wchar_t c_sponzaScenePath[] =
        L"Assets/KhronosGlTFSampleAssets/Models/Sponza/glTF/Sponza.gltf";
    constexpr wchar_t c_simpleSkinScenePath[] =
        L"Assets/KhronosGlTFSampleAssets/Models/SimpleSkin/glTF/SimpleSkin.gltf";
    constexpr wchar_t c_brainStemScenePath[] =
        L"Assets/NVIDIA-RTX/RTXPT-Assets/Models/glTF-Sample-Models/2.0/BrainStem/glTF-Binary/BrainStem.glb";

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

    HRESULT EnsureParentDirectory(
        const std::wstring& filePath,
        std::wstring& resolvedDirectory)
    {
        resolvedDirectory.clear();
        if (filePath.empty())
            return E_INVALIDARG;

        const DWORD requiredLength = GetFullPathNameW(
            filePath.c_str(),
            0,
            nullptr,
            nullptr);
        if (requiredLength == 0)
            return HRESULT_FROM_WIN32(GetLastError());

        std::vector<wchar_t> absolutePath(requiredLength);
        const DWORD writtenLength = GetFullPathNameW(
            filePath.c_str(),
            requiredLength,
            absolutePath.data(),
            nullptr);
        if (writtenLength == 0 || writtenLength >= requiredLength)
            return HRESULT_FROM_WIN32(
                writtenLength == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);

        const std::wstring pathSource(absolutePath.data(), writtenLength);
        const std::size_t separator = pathSource.find_last_of(L"\\/");
        if (separator == std::wstring::npos)
            return S_OK;

        resolvedDirectory = pathSource.substr(0, separator);
        if (resolvedDirectory.empty())
            return S_OK;

        DWORD attributes = GetFileAttributesW(resolvedDirectory.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                ? S_OK
                : HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        }

        const int createResult = SHCreateDirectoryExW(
            nullptr,
            resolvedDirectory.c_str(),
            nullptr);
        if (createResult == ERROR_SUCCESS ||
            createResult == ERROR_ALREADY_EXISTS ||
            createResult == ERROR_FILE_EXISTS)
        {
            return S_OK;
        }

        attributes = GetFileAttributesW(resolvedDirectory.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return S_OK;
        }

        return HRESULT_FROM_WIN32(static_cast<DWORD>(createResult));
    }

    double Distance(
        const std::array<float, 3>& a,
        const std::array<float, 3>& b)
    {
        double distanceSquared = 0.0;
        for (std::size_t component = 0; component < 3; ++component)
        {
            const double difference =
                static_cast<double>(a[component]) -
                static_cast<double>(b[component]);
            distanceSquared += difference * difference;
        }
        return std::sqrt(distanceSquared);
    }

    bool CameraForward(
        const CameraPose& pose,
        std::array<double, 3>& direction)
    {
        double lengthSquared = 0.0;
        for (std::size_t component = 0; component < 3; ++component)
        {
            direction[component] =
                static_cast<double>(pose.target[component]) -
                static_cast<double>(pose.position[component]);
            lengthSquared += direction[component] * direction[component];
        }
        if (lengthSquared <= 0.000000000001)
            return false;
        const double inverseLength = 1.0 / std::sqrt(lengthSquared);
        for (double& component : direction)
            component *= inverseLength;
        return true;
    }

    CameraPose InterpolateCameraPose(
        const CameraPose& begin,
        const CameraPose& end,
        double amount)
    {
        const float t = static_cast<float>(
            (std::max)(0.0, (std::min)(1.0, amount)));
        CameraPose pose;
        for (std::size_t component = 0; component < 3; ++component)
        {
            pose.position[component] =
                begin.position[component] +
                (end.position[component] - begin.position[component]) * t;
            pose.target[component] =
                begin.target[component] +
                (end.target[component] - begin.target[component]) * t;
        }
        return pose;
    }

    bool Utf8ToWide(const char* text, std::wstring& converted)
    {
        converted.clear();
        if (!text || !text[0])
            return false;
        const int requiredLength = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text,
            -1,
            nullptr,
            0);
        if (requiredLength <= 1)
            return false;
        std::vector<wchar_t> buffer(static_cast<std::size_t>(requiredLength));
        if (MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                text,
                -1,
                buffer.data(),
                requiredLength) != requiredLength)
        {
            return false;
        }
        converted.assign(buffer.data());
        return true;
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
    if (m_cameraPathRecordingActive)
        StopCameraPathRecording();
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
    if (m_sponzaLite && !m_overlaySceneFilePath.empty())
        m_pbrScenePreset = 3;
    else if (!m_sponzaLite &&
        m_sceneFilePath.find(L"BrainStem") != std::wstring::npos)
        m_pbrScenePreset = 2;
    else if (!m_sponzaLite &&
        m_sceneFilePath.find(L"SimpleSkin") != std::wstring::npos)
        m_pbrScenePreset = 1;
    else
        m_pbrScenePreset = 0;

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
    m_rayTracingManager->SetOverlaySceneFilePath(
        m_overlaySceneFilePath);
    m_rayTracingManager->SetComposeModelRoom(m_composeModelRoom);
    m_rayTracingManager->SetSponzaLite(m_sponzaLite);
    m_rayTracingManager->SetStandaloneBrainStemSceneActive(
        m_pbrScenePreset == 2);
    m_rayTracingManager->SetSponzaLightConfigPath(
        m_sponzaLightConfigPath);
    m_rayTracingManager->SetSceneManifestPath(m_sceneManifestPath);
    m_rayTracingManager->SetSceneType(static_cast<UINT>(m_sceneType));
    m_rayTracingManager->SetMaxBounce(static_cast<UINT>(m_maxBounce));
    m_rayTracingManager->SetRussianRouletteEnabled(
        m_enableRussianRoulette);
    m_rayTracingManager->SetLightingMode(
        static_cast<UINT>(m_lightingMode));
    m_rayTracingManager->SetDirectionalLightRuntimeSettings(
        m_enableDirectionalLight,
        m_directionalLightIntensityScale);
    const bool referenceMode = m_enableAccumulation;
    m_rayTracingManager->SetTemporalReprojectionEnabled(
        !referenceMode && m_enableTemporalReprojection);
    m_rayTracingManager->SetBestTapHistoryGatherEnabled(
        !referenceMode && m_enableBestTapHistoryGather);
    m_rayTracingManager->SetDynamicObjectReprojectionEnabled(
        !referenceMode && m_enableDynamicObjectReprojection);
    m_rayTracingManager->SetStaticBackgroundHistoryFastPathEnabled(
        !referenceMode && m_enableStaticBackgroundHistoryFastPath);
    m_rayTracingManager->SetDisocclusionRepairSettings(
        !referenceMode && m_enableDisocclusionRepair,
        static_cast<UINT>(m_disocclusionRepairSamplesPerPixel));
    m_rayTracingManager->SetDynamicShadowHistoryValidationEnabled(
        !referenceMode && m_enableDynamicShadowHistoryValidation);
    m_rayTracingManager->SetSkinnedDeformationMotionEnabled(
        !referenceMode && m_enableSkinnedDeformationMotion);
    m_rayTracingManager->SetCurrentFrameVisibleResidualEnabled(
        !referenceMode && m_useCurrentFrameVisibleResidual);
    m_rayTracingManager->SetTemporalColorClipEnabled(
        !referenceMode && m_enableTemporalColorClip);
    m_rayTracingManager->SetAtrousEnabled(!referenceMode && m_enableAtrous);
    m_rayTracingManager->SetAtrousIterationCount(
        static_cast<UINT>(m_atrousIterations));
    m_rayTracingManager->SetAtrousSpecularIterationCount(
        static_cast<UINT>(m_atrousSpecularIterations));
    m_rayTracingManager->SetAtrousAdaptiveIterationsEnabled(
        m_enableAtrousAdaptiveIterations);
    m_rayTracingManager->SetAtrousDebugView(
        m_enableAtrousAdaptiveIterations
        ? static_cast<UINT>(m_atrousDebugView)
        : RayTracingManager::c_atrousDebugNone);
    m_rayTracingManager->SetAtrousSpecularMaterialWeightMode(
        static_cast<UINT>(m_atrousSpecularMaterialWeightMode));
    m_rayTracingManager->SetAtrousSpecularRoughnessWeightMode(
        static_cast<UINT>(m_atrousSpecularRoughnessWeightMode));
    m_rayTracingManager->SetAtrousKernelMode(
        static_cast<UINT>(m_atrousKernelMode));
    m_rayTracingManager->SetAtrousNormalExponent(
        m_atrousNormalExponent);
    m_rayTracingManager->SetAtrousDepthSigma(
        m_atrousDepthSigma);
    m_rayTracingManager->SetAtrousAdaptiveEdgeWeightsEnabled(
        m_enableAtrousAdaptiveEdgeWeights);
    m_rayTracingManager->SetAtrousAdaptiveDynamicWeights(
        m_atrousAdaptiveDynamicNormalExponent,
        m_atrousAdaptiveDynamicDepthSigma);
    m_rayTracingManager->SetAtrousAdaptiveLowHistoryWeights(
        m_atrousAdaptiveLowHistoryNormalExponent,
        m_atrousAdaptiveLowHistoryDepthSigma);
    m_rayTracingManager->SetAtrousAdaptiveStableWeights(
        m_atrousAdaptiveStableNormalExponent,
        m_atrousAdaptiveStableDepthSigma);
    m_rayTracingManager->SetAtrousColorSigma(m_atrousColorSigma);
    m_rayTracingManager->SetAtrousLogLuminanceEdgeStop(
        m_enableAtrousLogLuminanceEdgeStop,
        m_atrousLogLuminanceSigma);
    m_rayTracingManager->SetTextureLodSettings(
        !referenceMode && m_enableTextureLod,
        m_textureLodBias,
        !referenceMode && m_enablePropagatedRayCone);
    m_rayTracingManager->SetDynamicSphereAnimationEnabled(
        !m_enableAccumulation && m_animateDynamicSphere);
    m_rayTracingManager->SetDynamicCubeAnimationEnabled(
        !referenceMode && m_animateDynamicCube);
    m_rayTracingManager->SetBrainStemVisible(m_showBrainStem);
    m_rayTracingManager->SetMechDroneVisible(m_showMechDrone);
    m_rayTracingManager->SetSceneAnimationEnabled(
        !referenceMode && m_animateGltfScene);
    m_rayTracingManager->SetEnableStatistics(m_collectRayStatistics);
    if (!m_rayTracingManager->Initialize(m_hWnd, m_device.Get(), m_width, m_height))
        return false;
    m_directionalLightAngularRadiusDegrees =
        m_rayTracingManager->GetDirectionalLightAngularRadius() *
        57.29577951308232f;

    if (!LoadCameraPath())
        return false;
    InitializeFreeCamera();

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
    double frameDeltaSeconds = 1.0 / 60.0;
    if (m_hasLastRenderTime)
    {
        frameDeltaSeconds = std::chrono::duration<double>(
            cpuFrameBegin - m_lastRenderTime).count();
        frameDeltaSeconds =
            (std::max)(1.0 / 1000.0, (std::min)(frameDeltaSeconds, 0.1));
    }
    m_lastRenderTime = cpuFrameBegin;
    m_hasLastRenderTime = true;
    if (m_cameraPathPlaybackActive)
        UpdateCameraPath();
    else
    {
        UpdateFreeCamera(frameDeltaSeconds);
        if (m_cameraPathRecordingActive)
            UpdateCameraPathRecording(frameDeltaSeconds);
    }
    if (m_imguiVisible)
        BuildImGuiFrame();
    EnforceReferenceModeSettings();
    const bool referenceMode = m_enableAccumulation;
    m_rayTracingManager->SetDynamicSphereAnimationEnabled(
        !m_enableAccumulation && m_animateDynamicSphere);
    m_rayTracingManager->SetDynamicCubeAnimationEnabled(
        !referenceMode && m_animateDynamicCube);
    m_rayTracingManager->SetSceneAnimationEnabled(
        !referenceMode && m_animateGltfScene);
    m_rayTracingManager->SetFrameDeltaSeconds(frameDeltaSeconds);
    m_rayTracingManager->SetShowNormalColor(m_showNormalColor);
    m_rayTracingManager->SetMaxBounce(static_cast<UINT>(m_maxBounce));
    m_rayTracingManager->SetSamplesPerPixel(
        static_cast<UINT>(m_samplesPerPixel));
    m_rayTracingManager->SetRussianRouletteEnabled(
        m_enableRussianRoulette);
    m_rayTracingManager->SetLightingMode(
        static_cast<UINT>(m_lightingMode));
    m_rayTracingManager->SetDirectionalLightRuntimeSettings(
        m_enableDirectionalLight,
        m_directionalLightIntensityScale);
    m_rayTracingManager->SetTemporalReprojectionEnabled(
        !referenceMode && m_enableTemporalReprojection);
    m_rayTracingManager->SetBestTapHistoryGatherEnabled(
        !referenceMode && m_enableBestTapHistoryGather);
    m_rayTracingManager->SetDynamicObjectReprojectionEnabled(
        !referenceMode && m_enableDynamicObjectReprojection);
    m_rayTracingManager->SetStaticBackgroundHistoryFastPathEnabled(
        !referenceMode && m_enableStaticBackgroundHistoryFastPath);
    m_rayTracingManager->SetDisocclusionRepairSettings(
        !referenceMode && m_enableDisocclusionRepair,
        static_cast<UINT>(m_disocclusionRepairSamplesPerPixel));
    m_rayTracingManager->SetDynamicShadowHistoryValidationEnabled(
        !referenceMode && m_enableDynamicShadowHistoryValidation);
    m_rayTracingManager->SetSkinnedDeformationMotionEnabled(
        !referenceMode && m_enableSkinnedDeformationMotion);
    m_rayTracingManager->SetCurrentFrameVisibleResidualEnabled(
        !referenceMode && m_useCurrentFrameVisibleResidual);
    m_rayTracingManager->SetTemporalColorClipEnabled(
        !referenceMode && m_enableTemporalColorClip);
    m_rayTracingManager->SetTemporalDebugView(
        !referenceMode && m_enableTemporalReprojection
        ? static_cast<UINT>(m_temporalDebugView)
        : RayTracingManager::c_temporalDebugNone);
    m_rayTracingManager->SetAtrousEnabled(!referenceMode && m_enableAtrous);
    m_rayTracingManager->SetAtrousIterationCount(
        static_cast<UINT>(m_atrousIterations));
    m_rayTracingManager->SetAtrousSpecularIterationCount(
        static_cast<UINT>(m_atrousSpecularIterations));
    m_rayTracingManager->SetAtrousAdaptiveIterationsEnabled(
        m_enableAtrousAdaptiveIterations);
    m_rayTracingManager->SetAtrousDebugView(
        m_enableAtrousAdaptiveIterations
        ? static_cast<UINT>(m_atrousDebugView)
        : RayTracingManager::c_atrousDebugNone);
    m_rayTracingManager->SetAtrousSpecularMaterialWeightMode(
        static_cast<UINT>(m_atrousSpecularMaterialWeightMode));
    m_rayTracingManager->SetAtrousSpecularRoughnessWeightMode(
        static_cast<UINT>(m_atrousSpecularRoughnessWeightMode));
    m_rayTracingManager->SetAtrousKernelMode(
        static_cast<UINT>(m_atrousKernelMode));
    m_rayTracingManager->SetAtrousNormalExponent(
        m_atrousNormalExponent);
    m_rayTracingManager->SetAtrousDepthSigma(
        m_atrousDepthSigma);
    m_rayTracingManager->SetAtrousAdaptiveEdgeWeightsEnabled(
        m_enableAtrousAdaptiveEdgeWeights);
    m_rayTracingManager->SetAtrousAdaptiveDynamicWeights(
        m_atrousAdaptiveDynamicNormalExponent,
        m_atrousAdaptiveDynamicDepthSigma);
    m_rayTracingManager->SetAtrousAdaptiveLowHistoryWeights(
        m_atrousAdaptiveLowHistoryNormalExponent,
        m_atrousAdaptiveLowHistoryDepthSigma);
    m_rayTracingManager->SetAtrousAdaptiveStableWeights(
        m_atrousAdaptiveStableNormalExponent,
        m_atrousAdaptiveStableDepthSigma);
    m_rayTracingManager->SetAtrousColorSigma(m_atrousColorSigma);
    m_rayTracingManager->SetAtrousLogLuminanceEdgeStop(
        m_enableAtrousLogLuminanceEdgeStop,
        m_atrousLogLuminanceSigma);
    m_rayTracingManager->SetEnableAccumulation(m_enableAccumulation);
    m_rayTracingManager->SetSceneType(static_cast<UINT>(m_sceneType));
    m_rayTracingManager->SetPbrDebugView(static_cast<UINT>(m_pbrDebugView));
    m_rayTracingManager->SetPbrMaterial(m_pbrMetallic, m_pbrRoughness);
    m_rayTracingManager->SetPbrMaterialOverride(m_overridePbrMaterial);
    m_rayTracingManager->SetTextureLodSettings(
        !referenceMode && m_enableTextureLod,
        m_textureLodBias,
        !referenceMode && m_enablePropagatedRayCone);
    m_rayTracingManager->SetIblSettings(m_enableIbl, m_iblIntensity);
    m_rayTracingManager->SetValidationSeed(m_validationSeed);
    m_rayTracingManager->SetExposure(m_exposure);
    m_rayTracingManager->SetEnableStatistics(m_collectRayStatistics);

    HRESULT hr = m_commandAllocator->Reset();
    if (ReportFailure(hr, L"Command allocator reset failed."))
        return;

    hr = m_commandList->Reset(m_commandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"Command list reset failed."))
        return;

    WriteGpuTimestamp(c_gpuTotalBegin);
    WriteGpuTimestamp(c_gpuDispatchBegin);
    RayTracingManager::GpuProfileQueries profileQueries = {};
    profileQueries.heap = m_gpuPassProfilingEnabled
        ? m_gpuTimestampQueryHeap.Get()
        : nullptr;
    profileQueries.tlasBegin = c_gpuTlasBegin;
    profileQueries.tlasEnd = c_gpuTlasEnd;
    profileQueries.pathTraceBegin = c_gpuPathTraceBegin;
    profileQueries.pathTraceEnd = c_gpuPathTraceEnd;
    profileQueries.mainPathBegin = c_gpuMainPathBegin;
    profileQueries.mainPathEnd = c_gpuMainPathEnd;
    profileQueries.disocclusionRepairBegin =
        c_gpuDisocclusionRepairBegin;
    profileQueries.disocclusionRepairEnd =
        c_gpuDisocclusionRepairEnd;
    profileQueries.temporalColorClipBegin =
        c_gpuTemporalColorClipBegin;
    profileQueries.temporalColorClipEnd =
        c_gpuTemporalColorClipEnd;
    profileQueries.atrousDiffuseBegin =
        c_gpuAtrousDiffuseBegin;
    profileQueries.atrousDiffuseEnd =
        c_gpuAtrousDiffuseEnd;
    profileQueries.atrousSpecularBegin =
        c_gpuAtrousSpecularBegin;
    profileQueries.atrousSpecularEnd =
        c_gpuAtrousSpecularEnd;
    m_rayTracingManager->DispatchRays(
        m_commandList.Get(),
        m_gpuPassProfilingEnabled ? &profileQueries : nullptr);
    m_objectLinearSpeed =
        m_rayTracingManager->GetDynamicObjectLinearSpeed();
    m_objectAngularSpeed =
        m_rayTracingManager->GetDynamicObjectAngularSpeed();
    WriteGpuTimestamp(c_gpuDispatchEnd);

    // Reserved for the MAPB bilinear compute pass. Keeping timestamps in the
    // baseline makes benchmark CSV columns stable before upscaling is added.
    WriteGpuTimestamp(c_gpuUpscaleBegin);
    WriteGpuTimestamp(c_gpuUpscaleEnd);

    WriteGpuTimestamp(c_gpuOutputCopyBegin);
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
    postCopyBarriers[1].Transition.StateAfter = m_imguiVisible
        ? D3D12_RESOURCE_STATE_RENDER_TARGET
        : D3D12_RESOURCE_STATE_PRESENT;
    postCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(2, postCopyBarriers);

    WriteGpuTimestamp(c_gpuOutputCopyEnd);
    WriteGpuTimestamp(c_gpuUiBegin);
    if (m_imguiVisible)
        RenderImGuiDrawData();
    WriteGpuTimestamp(c_gpuUiEnd);

    if (m_imguiVisible)
    {
        D3D12_RESOURCE_BARRIER presentBarrier = {};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition.pResource =
            m_renderTargets[m_frameIndex].Get();
        presentBarrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_RENDER_TARGET;
        presentBarrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PRESENT;
        presentBarrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_commandList->ResourceBarrier(1, &presentBarrier);
    }

    WriteGpuTimestamp(c_gpuTotalEnd);
    if (m_gpuPassProfilingEnabled)
    {
        m_commandList->ResolveQueryData(
            m_gpuTimestampQueryHeap.Get(),
            D3D12_QUERY_TYPE_TIMESTAMP,
            0,
            c_gpuTimestampCount,
            m_gpuTimestampReadback.Get(),
            0);
    }

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
    if (m_gpuPassProfilingEnabled)
        ReadGpuTimingResults();
    m_rayTracingManager->ReadFrameStatistics();
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

    ReportFailure(E_FAIL, L"D3D12 raytracing tier 1.1 is not supported on this system.");
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
    if (FAILED(hr) || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
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

bool D3D12Renderer::LoadCameraPath()
{
    if (m_cameraPathFilePath.empty())
        return true;

    if (!m_cameraPath.Load(m_cameraPathFilePath, &m_cameraPathError))
    {
        std::wstring message = L"Camera path loading failed.\n";
        message += m_cameraPathFilePath;
        if (!m_cameraPathError.empty())
        {
            message += L"\n";
            message += m_cameraPathError;
        }
        MessageBoxW(m_hWnd, message.c_str(), L"Camera Path Error", MB_OK | MB_ICONERROR);
        return false;
    }

    m_cameraPathLoaded = true;
    m_cameraPathPlaybackActive = m_cameraPathAutoPlay;
    m_cameraPathFrameIndex = 0;
    m_hasPreviousCameraPose = false;
    if (m_cameraPathPlaybackActive && m_rayTracingManager)
    {
        m_rayTracingManager->SetDynamicSphereDeterministicTimeline(true);
        m_rayTracingManager->ResetDynamicSphereTimeline();
    }
    if (m_cameraPathPlaybackActive &&
        m_benchmarkEnabled &&
        m_benchmarkFrameLimit == 0)
    {
        m_benchmarkFrameLimit = static_cast<UINT>(
            std::ceil(
                m_cameraPath.GetDurationSeconds() *
                m_cameraPath.GetFramesPerSecond())) + 1u;
    }
    return true;
}

bool D3D12Renderer::LoadCameraPathForPlayback()
{
    std::wstring filePath;
    if (!Utf8ToWide(m_cameraPlaybackPath, filePath))
    {
        m_cameraPlaybackStatus = "Invalid UTF-8 playback path.";
        return false;
    }

    if (!m_cameraPath.Load(filePath, &m_cameraPathError))
    {
        m_cameraPathPlaybackActive = false;
        m_cameraPathLoaded = false;
        m_cameraPlaybackStatus =
            "Camera path load failed. Check the file path and JSON.";
        return false;
    }

    m_cameraPathFilePath = filePath;
    m_cameraPathLoaded = true;
    m_cameraPathFrameIndex = 0;
    m_hasPreviousCameraPose = false;
    m_cameraPlaybackStatus = "Loaded playback path.";
    return true;
}

void D3D12Renderer::StartCameraPathPlayback()
{
    if (!m_cameraPathLoaded || !m_rayTracingManager)
        return;
    if (m_cameraPathRecordingActive)
        StopCameraPathRecording();
    m_cameraPathPlaybackActive = true;
    m_cameraPathFrameIndex = 0;
    m_hasPreviousCameraPose = false;
    m_cameraLinearSpeed = 0.0;
    m_cameraAngularSpeed = 0.0;
    m_rayTracingManager->SetDynamicSphereDeterministicTimeline(true);
    m_rayTracingManager->ResetDynamicSphereTimeline();
}

void D3D12Renderer::StopCameraPathPlayback()
{
    if (!m_cameraPathPlaybackActive)
        return;
    m_cameraPathPlaybackActive = false;
    m_hasPreviousCameraPose = false;
    m_cameraLinearSpeed = 0.0;
    m_cameraAngularSpeed = 0.0;
    if (m_rayTracingManager)
    {
        m_rayTracingManager->SetDynamicSphereDeterministicTimeline(false);
        m_rayTracingManager->ResetDynamicSphereTimeline();
    }
    m_freeCameraInitialized = false;
    InitializeFreeCamera();
}

CameraPose D3D12Renderer::GetCurrentCameraPose() const
{
    CameraPose pose;
    if (!m_rayTracingManager)
        return pose;
    pose.position = m_rayTracingManager->GetCameraPosition();
    pose.target = m_rayTracingManager->GetCameraTarget();
    return pose;
}

void D3D12Renderer::StartCameraPathRecording()
{
    if (!m_rayTracingManager)
        return;
    if (m_cameraPathPlaybackActive)
        StopCameraPathPlayback();

    m_recordedCameraKeyframes.clear();
    m_cameraRecordingElapsedSeconds = 0.0;
    m_cameraRecordingNextSampleSeconds =
        1.0 / c_cameraRecordingFramesPerSecond;
    m_cameraRecordingPreviousPose = GetCurrentCameraPose();
    CameraPath::Keyframe first;
    first.timeSeconds = 0.0;
    first.pose = m_cameraRecordingPreviousPose;
    m_recordedCameraKeyframes.push_back(first);
    m_cameraPathRecordingActive = true;
    m_cameraRecordingStatus = "Recording camera at 60 Hz...";
}

void D3D12Renderer::UpdateCameraPathRecording(double deltaSeconds)
{
    if (!m_cameraPathRecordingActive || !m_rayTracingManager)
        return;

    const double frameDuration = (std::max)(deltaSeconds, 1.0e-6);
    const double frameBeginTime = m_cameraRecordingElapsedSeconds;
    const CameraPose currentPose = GetCurrentCameraPose();
    m_cameraRecordingElapsedSeconds += frameDuration;
    const double sampleInterval =
        1.0 / c_cameraRecordingFramesPerSecond;
    while (m_cameraRecordingNextSampleSeconds <=
           m_cameraRecordingElapsedSeconds + 1.0e-9)
    {
        const double interpolation =
            (m_cameraRecordingNextSampleSeconds - frameBeginTime) /
            frameDuration;
        CameraPath::Keyframe keyframe;
        keyframe.timeSeconds = m_cameraRecordingNextSampleSeconds;
        keyframe.pose = InterpolateCameraPose(
            m_cameraRecordingPreviousPose,
            currentPose,
            interpolation);
        m_recordedCameraKeyframes.push_back(keyframe);
        m_cameraRecordingNextSampleSeconds += sampleInterval;
    }
    m_cameraRecordingPreviousPose = currentPose;
}

bool D3D12Renderer::SaveRecordedCameraPath()
{
    std::wstring filePath;
    if (!Utf8ToWide(m_cameraRecordingPath, filePath))
    {
        m_cameraRecordingStatus = "Invalid UTF-8 output path.";
        return false;
    }

    std::wstring directory;
    const HRESULT directoryResult =
        EnsureParentDirectory(filePath, directory);
    if (FAILED(directoryResult))
    {
        m_cameraRecordingStatus = "Output directory creation failed.";
        return false;
    }

    CameraPath recordedPath;
    if (!recordedPath.SetKeyframes(
            m_recordedCameraKeyframes,
            c_cameraRecordingFramesPerSecond,
            false,
            &m_cameraPathError) ||
        !recordedPath.Save(
            filePath,
            "Recorded free-camera path for deterministic benchmarks",
            &m_cameraPathError))
    {
        m_cameraRecordingStatus = "Camera path save failed.";
        return false;
    }

    m_cameraRecordingStatus =
        "Saved. Enter this file under Playback path JSON to play it.";
    return true;
}

void D3D12Renderer::StopCameraPathRecording()
{
    if (!m_cameraPathRecordingActive)
        return;

    const CameraPose finalPose = GetCurrentCameraPose();
    if (m_recordedCameraKeyframes.empty() ||
        m_cameraRecordingElapsedSeconds >
            m_recordedCameraKeyframes.back().timeSeconds + 1.0e-9)
    {
        CameraPath::Keyframe keyframe;
        keyframe.timeSeconds = m_cameraRecordingElapsedSeconds;
        keyframe.pose = finalPose;
        m_recordedCameraKeyframes.push_back(keyframe);
    }
    m_cameraPathRecordingActive = false;
    SaveRecordedCameraPath();
}

void D3D12Renderer::OnKey(UINT virtualKey, bool pressed)
{
    if (virtualKey < m_keyPressed.size())
    {
        const bool wasPressed = m_keyPressed[virtualKey];
        if (virtualKey == VK_F1 && pressed && !wasPressed)
            m_imguiVisible = !m_imguiVisible;
        if (virtualKey == VK_F2 && pressed && !wasPressed)
        {
            SetGpuPassProfilingEnabled(
                !m_gpuPassProfilingEnabled);
        }
        m_keyPressed[virtualKey] = pressed;
    }
}

void D3D12Renderer::OnRightMouseButton(bool pressed, int x, int y)
{
    m_rightMouseDragging = pressed;
    m_lastMousePosition = { x, y };
    if (pressed)
        SetCapture(m_hWnd);
    else if (GetCapture() == m_hWnd)
        ReleaseCapture();
}

void D3D12Renderer::OnMouseMove(int x, int y)
{
    if (m_rightMouseDragging && !m_cameraPathPlaybackActive)
    {
        constexpr double mouseSensitivity = 0.003;
        m_pendingMouseYaw +=
            static_cast<double>(x - m_lastMousePosition.x) *
            mouseSensitivity;
        m_pendingMousePitch -=
            static_cast<double>(y - m_lastMousePosition.y) *
            mouseSensitivity;
    }
    m_lastMousePosition = { x, y };
}

void D3D12Renderer::OnFocusLost()
{
    m_keyPressed.fill(false);
    m_rightMouseDragging = false;
    m_pendingMouseYaw = 0.0;
    m_pendingMousePitch = 0.0;
    if (GetCapture() == m_hWnd)
        ReleaseCapture();
}

void D3D12Renderer::InitializeFreeCamera()
{
    if (!m_rayTracingManager)
        return;
    const std::array<float, 3>& position =
        m_rayTracingManager->GetCameraPosition();
    const std::array<float, 3>& target =
        m_rayTracingManager->GetCameraTarget();
    const double dx = static_cast<double>(target[0] - position[0]);
    const double dy = static_cast<double>(target[1] - position[1]);
    const double dz = static_cast<double>(target[2] - position[2]);
    const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length <= 1.0e-8)
        return;
    m_freeCameraLookDistance = length;
    m_freeCameraYaw = std::atan2(dx, dz);
    m_freeCameraPitch = std::asin(
        (std::max)(-1.0, (std::min)(1.0, dy / length)));
    m_freeCameraInitialized = true;
}

void D3D12Renderer::UpdateFreeCamera(double deltaSeconds)
{
    m_cameraLinearSpeed = 0.0;
    m_cameraAngularSpeed = 0.0;
    if (!m_rayTracingManager)
        return;
    if (!m_freeCameraInitialized)
        InitializeFreeCamera();
    if (!m_freeCameraInitialized)
        return;

    const double previousYaw = m_freeCameraYaw;
    const double previousPitch = m_freeCameraPitch;
    m_freeCameraYaw += m_pendingMouseYaw;
    m_freeCameraPitch += m_pendingMousePitch;
    const double keyboardPrecision =
        m_keyPressed[VK_CONTROL] ? 0.25 : 1.0;
    constexpr double degreesToRadians =
        0.0174532925199432957692;
    const double keyboardTurnStep =
        static_cast<double>(m_keyboardTurnSpeedDegrees) *
        degreesToRadians *
        keyboardPrecision *
        deltaSeconds;
    if (m_keyPressed[VK_LEFT])
        m_freeCameraYaw -= keyboardTurnStep;
    if (m_keyPressed[VK_RIGHT])
        m_freeCameraYaw += keyboardTurnStep;
    if (m_keyPressed[VK_UP])
        m_freeCameraPitch += keyboardTurnStep;
    if (m_keyPressed[VK_DOWN])
        m_freeCameraPitch -= keyboardTurnStep;
    constexpr double pitchLimit = 1.5533430342749532;
    m_freeCameraPitch =
        (std::max)(-pitchLimit, (std::min)(pitchLimit, m_freeCameraPitch));
    m_pendingMouseYaw = 0.0;
    m_pendingMousePitch = 0.0;

    const double cosPitch = std::cos(m_freeCameraPitch);
    const std::array<double, 3> forward =
    {
        std::sin(m_freeCameraYaw) * cosPitch,
        std::sin(m_freeCameraPitch),
        std::cos(m_freeCameraYaw) * cosPitch
    };
    const std::array<double, 3> right =
    {
        forward[2],
        0.0,
        -forward[0]
    };

    std::array<double, 3> movement = {};
    const auto add = [&movement](
        const std::array<double, 3>& direction,
        double scale)
    {
        for (std::size_t component = 0; component < 3; ++component)
            movement[component] += direction[component] * scale;
    };
    if (m_keyPressed['W']) add(forward, 1.0);
    if (m_keyPressed['S']) add(forward, -1.0);
    if (m_keyPressed['D']) add(right, 1.0);
    if (m_keyPressed['A']) add(right, -1.0);
    if (m_keyPressed['E']) movement[1] += 1.0;
    if (m_keyPressed['Q']) movement[1] -= 1.0;

    double movementLengthSquared = 0.0;
    for (const double component : movement)
        movementLengthSquared += component * component;
    std::array<float, 3> position =
        m_rayTracingManager->GetCameraPosition();
    if (movementLengthSquared > 1.0e-12)
    {
        const double inverseLength = 1.0 / std::sqrt(movementLengthSquared);
        const double speedMultiplier =
            m_keyPressed[VK_SHIFT] ? 3.0 : 1.0;
        const double speed =
            static_cast<double>(m_rayTracingManager->GetSceneDiagonal()) *
            0.20 * speedMultiplier;
        for (std::size_t component = 0; component < 3; ++component)
        {
            position[component] += static_cast<float>(
                movement[component] * inverseLength *
                speed * deltaSeconds);
        }
        m_cameraLinearSpeed = speed;
    }

    std::array<float, 3> target = {};
    for (std::size_t component = 0; component < 3; ++component)
    {
        target[component] = position[component] + static_cast<float>(
            forward[component] * m_freeCameraLookDistance);
    }
    const double yawDelta = m_freeCameraYaw - previousYaw;
    const double pitchDelta = m_freeCameraPitch - previousPitch;
    constexpr double radiansToDegrees = 57.2957795130823208768;
    m_cameraAngularSpeed = std::sqrt(
        yawDelta * yawDelta + pitchDelta * pitchDelta) *
        radiansToDegrees / (std::max)(deltaSeconds, 1.0e-6);

    if (movementLengthSquared > 1.0e-12 ||
        std::abs(yawDelta) > 1.0e-12 ||
        std::abs(pitchDelta) > 1.0e-12)
    {
        m_rayTracingManager->SetCamera(position, target);
    }
}

void D3D12Renderer::UpdateCameraPath()
{
    if (!m_cameraPathPlaybackActive || !m_rayTracingManager)
        return;

    const double framesPerSecond = m_cameraPath.GetFramesPerSecond();
    const double deltaSeconds = 1.0 / framesPerSecond;
    const double pathTime =
        static_cast<double>(m_cameraPathFrameIndex) / framesPerSecond;
    CameraPose pose;
    if (!m_cameraPath.Sample(pathTime, pose))
        return;

    m_cameraLinearSpeed = 0.0;
    m_cameraAngularSpeed = 0.0;
    if (m_hasPreviousCameraPose)
    {
        m_cameraLinearSpeed =
            Distance(pose.position, m_previousCameraPose.position) /
            deltaSeconds;

        std::array<double, 3> previousForward = {};
        std::array<double, 3> currentForward = {};
        if (CameraForward(m_previousCameraPose, previousForward) &&
            CameraForward(pose, currentForward))
        {
            double cosine = 0.0;
            for (std::size_t component = 0; component < 3; ++component)
                cosine += previousForward[component] * currentForward[component];
            cosine = (std::max)(-1.0, (std::min)(1.0, cosine));
            constexpr double radiansToDegrees =
                57.2957795130823208768;
            m_cameraAngularSpeed = cosine >= 1.0 - 0.000000000001
                ? 0.0
                : std::acos(cosine) * radiansToDegrees / deltaSeconds;
        }
    }

    m_rayTracingManager->SetCamera(pose.position, pose.target);
    m_previousCameraPose = pose;
    m_hasPreviousCameraPose = true;
    ++m_cameraPathFrameIndex;
    if (!m_cameraPath.IsLooping() &&
        pathTime >= m_cameraPath.GetDurationSeconds())
    {
        StopCameraPathPlayback();
    }
}

void D3D12Renderer::SetGpuPassProfilingEnabled(bool enabled)
{
    if (m_gpuPassProfilingEnabled == enabled)
        return;

    m_gpuPassProfilingEnabled = enabled;
    ResetGpuTimingResults();

    // Keep CPU histories from different profiling modes separate. GPU
    // timestamps cover command-list intervals, while CPU frame time also
    // includes Present, the GPU wait, and timestamp readback.
    m_cpuFrameMs = 0.0;
    m_cpuMedianMs = 0.0;
    m_cpuP95Ms = 0.0;
    m_cpuP99Ms = 0.0;
    m_cpuTimingHistory.clear();
}

void D3D12Renderer::WriteGpuTimestamp(UINT queryIndex)
{
    if (!m_gpuPassProfilingEnabled ||
        !m_commandList ||
        !m_gpuTimestampQueryHeap)
    {
        return;
    }

    m_commandList->EndQuery(
        m_gpuTimestampQueryHeap.Get(),
        D3D12_QUERY_TYPE_TIMESTAMP,
        queryIndex);
}

void D3D12Renderer::ResetGpuTimingResults()
{
    m_gpuDispatchMs = 0.0;
    m_gpuTlasMs = 0.0;
    m_gpuPathTraceMs = 0.0;
    m_gpuMainPathMs = 0.0;
    m_gpuDisocclusionRepairMs = 0.0;
    m_gpuTemporalColorClipMs = 0.0;
    m_gpuAtrousDiffuseMs = 0.0;
    m_gpuAtrousSpecularMs = 0.0;
    m_gpuUpscaleMs = 0.0;
    m_gpuOutputCopyMs = 0.0;
    m_gpuUiMs = 0.0;
    m_gpuTotalMs = 0.0;
    m_gpuMedianMs = 0.0;
    m_gpuP95Ms = 0.0;
    m_gpuP99Ms = 0.0;
    m_gpuTimingHistory.clear();
}

void D3D12Renderer::ReadGpuTimingResults()
{
    if (!m_gpuPassProfilingEnabled ||
        !m_gpuTimestampReadback ||
        m_gpuTimestampFrequency == 0)
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
    m_gpuTlasMs = elapsedMilliseconds(c_gpuTlasBegin, c_gpuTlasEnd);
    m_gpuPathTraceMs = elapsedMilliseconds(
        c_gpuPathTraceBegin,
        c_gpuPathTraceEnd);
    m_gpuMainPathMs = elapsedMilliseconds(
        c_gpuMainPathBegin,
        c_gpuMainPathEnd);
    m_gpuDisocclusionRepairMs = elapsedMilliseconds(
        c_gpuDisocclusionRepairBegin,
        c_gpuDisocclusionRepairEnd);
    m_gpuTemporalColorClipMs = elapsedMilliseconds(
        c_gpuTemporalColorClipBegin,
        c_gpuTemporalColorClipEnd);
    m_gpuAtrousDiffuseMs = elapsedMilliseconds(
        c_gpuAtrousDiffuseBegin,
        c_gpuAtrousDiffuseEnd);
    m_gpuAtrousSpecularMs = elapsedMilliseconds(
        c_gpuAtrousSpecularBegin,
        c_gpuAtrousSpecularEnd);
    m_gpuUpscaleMs = elapsedMilliseconds(c_gpuUpscaleBegin, c_gpuUpscaleEnd);
    m_gpuOutputCopyMs = elapsedMilliseconds(
        c_gpuOutputCopyBegin,
        c_gpuOutputCopyEnd);
    m_gpuUiMs = elapsedMilliseconds(c_gpuUiBegin, c_gpuUiEnd);
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

    const DWORD outputAttributes =
        GetFileAttributesW(m_benchmarkOutputPath.c_str());
    if ((outputAttributes != INVALID_FILE_ATTRIBUTES &&
         (outputAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) ||
        m_benchmarkOutputPath.back() == L'\\' ||
        m_benchmarkOutputPath.back() == L'/')
    {
        if (m_benchmarkOutputPath.back() != L'\\' &&
            m_benchmarkOutputPath.back() != L'/')
        {
            m_benchmarkOutputPath.push_back(L'\\');
        }
        m_benchmarkOutputPath += L"baseline.csv";
    }

    std::wstring resolvedDirectory;
    const HRESULT directoryResult = EnsureParentDirectory(
        m_benchmarkOutputPath,
        resolvedDirectory);
    if (FAILED(directoryResult))
    {
        std::wostringstream message;
        message << L"Benchmark output directory creation failed."
                << L"\nOutput: " << m_benchmarkOutputPath;
        if (!resolvedDirectory.empty())
            message << L"\nDirectory: " << resolvedDirectory;
        return !ReportFailure(directoryResult, message.str().c_str());
    }

    errno_t openError = _wfopen_s(
        &m_benchmarkCsv,
        m_benchmarkOutputPath.c_str(),
        L"wb");
    if (openError != 0 || !m_benchmarkCsv)
    {
        std::wostringstream message;
        message << L"Benchmark CSV creation failed."
                << L"\nOutput: " << m_benchmarkOutputPath
                << L"\nC runtime errno: " << openError;
        return !ReportFailure(HRESULT_FROM_WIN32(ERROR_OPEN_FAILED),
            message.str().c_str());
    }

    std::fprintf(
        m_benchmarkCsv,
        "frame,cpu_ms,gpu_dispatch_ms,gpu_tlas_ms,"
        "gpu_path_trace_temporal_ms,gpu_main_path_ms,"
        "gpu_disocclusion_repair_ms,gpu_temporal_color_clip_ms,"
        "gpu_atrous_diffuse_ms,gpu_atrous_specular_ms,"
        "gpu_upscale_ms,gpu_output_copy_ms,gpu_ui_ms,gpu_total_ms,"
        "profile,internal_scale,max_bounce,russian_roulette,lighting_mode,"
        "camera_linear_speed,"
        "camera_angular_speed,object_linear_speed,object_angular_speed,"
        "primary_rays,primary_guide_rays,shadow_rays,nee_shadow_rays,"
        "history_validation_shadow_rays,bounce_rays,average_path_length,"
        "hit_count,miss_count,accumulated_samples,ray_depth_0,ray_depth_1,"
        "ray_depth_2,ray_depth_3,ray_depth_4,ray_depth_5,ray_depth_6,"
        "ray_depth_7,ray_depth_8\n");
    return true;
}

void D3D12Renderer::RecordFrameMetrics(double cpuFrameMs)
{
    m_cpuFrameMs = cpuFrameMs;
    AppendTimingSample(m_cpuTimingHistory, m_cpuFrameMs);
    if (m_gpuPassProfilingEnabled)
        AppendTimingSample(m_gpuTimingHistory, m_gpuTotalMs);

    m_cpuMedianMs = CalculatePercentile(m_cpuTimingHistory, 0.50);
    m_cpuP95Ms = CalculatePercentile(m_cpuTimingHistory, 0.95);
    m_cpuP99Ms = CalculatePercentile(m_cpuTimingHistory, 0.99);
    if (m_gpuPassProfilingEnabled)
    {
        m_gpuMedianMs = CalculatePercentile(
            m_gpuTimingHistory,
            0.50);
        m_gpuP95Ms = CalculatePercentile(
            m_gpuTimingHistory,
            0.95);
        m_gpuP99Ms = CalculatePercentile(
            m_gpuTimingHistory,
            0.99);
    }

    if (!m_benchmarkEnabled || m_benchmarkFinished || !m_benchmarkCsv)
        return;

    const RayTracingManager::FrameStatistics& statistics =
        m_rayTracingManager->GetFrameStatistics();
    const UINT accumulatedSamples = m_rayTracingManager
        ? m_rayTracingManager->GetAccumulatedSampleCount()
        : 0u;
    std::fprintf(
        m_benchmarkCsv,
        "%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
        "%.6f,%.6f,%.6f,%.6f,fixed,1.000000,%d,%d,%d,"
        "%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%.6f,%llu,%llu,%u",
        static_cast<unsigned long long>(m_benchmarkFramesWritten),
        m_cpuFrameMs,
        m_gpuDispatchMs,
        m_gpuTlasMs,
        m_gpuPathTraceMs,
        m_gpuMainPathMs,
        m_gpuDisocclusionRepairMs,
        m_gpuTemporalColorClipMs,
        m_gpuAtrousDiffuseMs,
        m_gpuAtrousSpecularMs,
        m_gpuUpscaleMs,
        m_gpuOutputCopyMs,
        m_gpuUiMs,
        m_gpuTotalMs,
        m_maxBounce,
        m_enableRussianRoulette ? 1 : 0,
        m_lightingMode,
        m_cameraLinearSpeed,
        m_cameraAngularSpeed,
        m_objectLinearSpeed,
        m_objectAngularSpeed,
        static_cast<unsigned long long>(statistics.GetPrimaryRayCount()),
        static_cast<unsigned long long>(statistics.primaryGuideRays),
        static_cast<unsigned long long>(statistics.shadowRays),
        static_cast<unsigned long long>(statistics.neeShadowRays),
        static_cast<unsigned long long>(
            statistics.historyValidationShadowRays),
        static_cast<unsigned long long>(statistics.GetBounceRayCount()),
        statistics.GetAveragePathLength(),
        static_cast<unsigned long long>(statistics.hitCount),
        static_cast<unsigned long long>(statistics.missCount),
        accumulatedSamples);
    for (UINT depth = 0;
         depth < RayTracingManager::c_statisticsRayDepthCount;
         ++depth)
    {
        std::fprintf(
            m_benchmarkCsv,
            ",%llu",
            static_cast<unsigned long long>(statistics.raysByDepth[depth]));
    }
    std::fprintf(m_benchmarkCsv, "\n");

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

bool D3D12Renderer::SwitchPbrScenePreset(int preset)
{
    if (!m_rayTracingManager || preset < 0 || preset > 3)
        return false;

    std::wstring sceneFilePath;
    std::wstring overlaySceneFilePath;
    bool composeModelRoom = false;
    bool sponzaLite = false;
    if (preset == 0)
    {
        sceneFilePath = c_sponzaScenePath;
        sponzaLite = true;
    }
    else if (preset == 1)
    {
        sceneFilePath = c_simpleSkinScenePath;
    }
    else if (preset == 2)
    {
        sceneFilePath = c_brainStemScenePath;
    }
    else if (preset == 3)
    {
        sceneFilePath = c_sponzaScenePath;
        overlaySceneFilePath = c_brainStemScenePath;
        sponzaLite = true;
    }
    if (m_cameraPathPlaybackActive)
        StopCameraPathPlayback();
    if (m_cameraPathRecordingActive)
        StopCameraPathRecording();
    m_captureActive = false;
    m_saveCurrentRequested = false;
    m_captureStatus.clear();

    // Scene presets can be switched after startup. Keep the manager in sync
    // with the renderer-owned light configuration before rebuilding.
    m_rayTracingManager->SetSponzaLightConfigPath(
        m_sponzaLightConfigPath);

    WaitForGpu();
    const std::wstring previousSceneFilePath = m_sceneFilePath;
    const std::wstring previousOverlaySceneFilePath =
        m_overlaySceneFilePath;
    const bool previousComposeModelRoom = m_composeModelRoom;
    const bool previousSponzaLite = m_sponzaLite;
    const bool previousBrainStemSceneActive = m_pbrScenePreset == 2;
    m_rayTracingManager->SetStandaloneBrainStemSceneActive(
        preset == 2);
    if (!m_rayTracingManager->ReloadPbrScene(
            sceneFilePath,
            composeModelRoom,
            sponzaLite,
            overlaySceneFilePath))
    {
        m_rayTracingManager->SetStandaloneBrainStemSceneActive(
            previousBrainStemSceneActive);
        m_rayTracingManager->ReloadPbrScene(
            previousSceneFilePath,
            previousComposeModelRoom,
            previousSponzaLite,
            previousOverlaySceneFilePath);
        m_sceneSwitchStatus = "Scene switch failed; previous scene restored.";
        return false;
    }

    m_directionalLightAngularRadiusDegrees =
        m_rayTracingManager->GetDirectionalLightAngularRadius() *
        57.29577951308232f;
    m_sceneType =
        static_cast<int>(RayTracingManager::c_scenePbrGgx);
    m_sceneFilePath = sceneFilePath;
    m_overlaySceneFilePath = overlaySceneFilePath;
    m_composeModelRoom = composeModelRoom;
    m_sponzaLite = sponzaLite;
    m_pbrScenePreset = preset;
    m_dynamicSphereMaterialSelectionInitialized = false;
    m_animateGltfScene = true;
    m_rayTracingManager->SetSceneAnimationEnabled(
        !m_enableAccumulation && m_animateGltfScene);
    m_rayTracingManager->SetBrainStemVisible(m_showBrainStem);
    m_rayTracingManager->SetMechDroneVisible(m_showMechDrone);
    m_rayTracingManager->SetDynamicSphereVisible(
        m_showDynamicSphere);
    m_rayTracingManager->SetDynamicSphereAnimationEnabled(
        !m_enableAccumulation && m_animateDynamicSphere);
    m_freeCameraInitialized = false;
    InitializeFreeCamera();
    m_hasLastRenderTime = false;
    ResetGpuTimingResults();
    if (preset == 0)
        m_sceneSwitchStatus = "Loaded PBR Sponza.";
    else if (preset == 1)
        m_sceneSwitchStatus = "Loaded SimpleSkin GPU skinning test.";
    else if (preset == 2)
        m_sceneSwitchStatus = "Loaded BrainStem GPU skinning test.";
    else
        m_sceneSwitchStatus =
            "Loaded Sponza with animated BrainStem.";
    return true;
}

void D3D12Renderer::EnforceReferenceModeSettings()
{
    if (!m_enableAccumulation)
        return;

    m_animateDynamicSphere = false;
    m_animateDynamicCube = false;
    m_animateGltfScene = false;
    m_enableTemporalReprojection = false;
    m_enableBestTapHistoryGather = false;
    m_enableDynamicObjectReprojection = false;
    m_enableStaticBackgroundHistoryFastPath = false;
    m_enableDisocclusionRepair = false;
    m_enableDynamicShadowHistoryValidation = false;
    m_enableSkinnedDeformationMotion = false;
    m_useCurrentFrameVisibleResidual = false;
    m_enableTemporalColorClip = false;
    m_enableAtrous = false;
    m_enableAtrousAdaptiveIterations = false;
    m_enableAtrousAdaptiveEdgeWeights = false;
    m_enableAtrousLogLuminanceEdgeStop = false;
    m_enableTextureLod = false;
    m_enablePropagatedRayCone = false;
    m_temporalDebugView = static_cast<int>(
        RayTracingManager::c_temporalDebugNone);
    m_atrousDebugView = static_cast<int>(
        RayTracingManager::c_atrousDebugNone);
}

void D3D12Renderer::BuildImGuiFrame()
{
    if (!m_imguiInitialized || !m_imguiVisible)
        return;

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    EnforceReferenceModeSettings();

    ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(300.0f, 200.0f),
        ImVec2(
            1000.0f,
            (std::max)(240.0f, ImGui::GetIO().DisplaySize.y - 32.0f)));
    ImGui::Begin("DXR Debug");
    ImGui::TextDisabled("F1: Toggle ImGui rendering");
    const char* sceneNames[] =
    {
        "Cornell Box",
        "PBR",
        "PBR GPU Validation",
        "Indirect Bounce Stress",
        "Dynamic Transform Test"
    };
    if (ImGui::Combo("Scene", &m_sceneType, sceneNames, _countof(sceneNames)) && m_rayTracingManager)
    {
        m_captureActive = false;
        m_saveCurrentRequested = false;
        m_captureStatus.clear();
        WaitForGpu();
        m_rayTracingManager->SetSceneType(static_cast<UINT>(m_sceneType));
        m_freeCameraInitialized = false;
        InitializeFreeCamera();
    }
    if (m_sceneType == static_cast<int>(RayTracingManager::c_scenePbrGgx))
    {
        const char* pbrSceneNames[] =
        {
            "Sponza",
            "SimpleSkin GPU Test",
            "BrainStem GPU Skinning Test",
            "Sponza + Animated BrainStem"
        };
        int requestedPbrScene = m_pbrScenePreset;
        if (ImGui::Combo(
                "PBR Test Scene",
                &requestedPbrScene,
                pbrSceneNames,
                _countof(pbrSceneNames)))
        {
            SwitchPbrScenePreset(requestedPbrScene);
        }
        if (!m_sceneSwitchStatus.empty())
            ImGui::TextDisabled("%s", m_sceneSwitchStatus.c_str());

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

        if (m_rayTracingManager &&
            m_rayTracingManager->HasSceneAnimation())
        {
            ImGui::SeparatorText("glTF node animation");
            ImGui::BeginDisabled(m_enableAccumulation);
            if (ImGui::Checkbox(
                "Play glTF animation",
                &m_animateGltfScene))
            {
                m_rayTracingManager->SetSceneAnimationEnabled(
                    m_animateGltfScene);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(m_enableAccumulation);
            if (ImGui::Button("Restart animation"))
                m_rayTracingManager->ResetSceneAnimation();
            ImGui::EndDisabled();
            const UINT animationClipCount =
                m_rayTracingManager->GetSceneAnimationClipCount();
            if (animationClipCount > 1u)
            {
                const UINT selectedClip =
                    m_rayTracingManager->GetSceneAnimationClipIndex();
                const std::string selectedClipName =
                    m_rayTracingManager->GetSceneAnimationClipName(
                        selectedClip);
                if (ImGui::BeginCombo(
                        "Animation clip",
                        selectedClipName.c_str()))
                {
                    for (UINT clipIndex = 0u;
                         clipIndex < animationClipCount;
                         ++clipIndex)
                    {
                        const std::string clipName =
                            m_rayTracingManager->
                                GetSceneAnimationClipName(clipIndex);
                        const bool selected = clipIndex == selectedClip;
                        if (ImGui::Selectable(
                                clipName.c_str(), selected))
                        {
                            m_rayTracingManager->
                                SetSceneAnimationClip(clipIndex);
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::TextDisabled(
                "%s: %.2f / %.2f s",
                m_rayTracingManager->GetSceneAnimationName().c_str(),
                m_rayTracingManager->GetSceneAnimationTime(),
                m_rayTracingManager->GetSceneAnimationDuration());
            ImGui::TextDisabled(
                m_rayTracingManager->GetImportedSkinCount() > 0u
                    ? "Node/joint animation: CPU transform evaluation"
                    : "Rigid node animation: TLAS transform update");
            ImGui::TextDisabled(
                "Unique mesh BLAS: %u, mesh-node TLAS instances: %u",
                m_rayTracingManager->GetImportedMeshBlasCount(),
                m_rayTracingManager->GetImportedMeshInstanceCount());
        }

        if (m_rayTracingManager &&
            m_rayTracingManager->GetImportedSkinCount() > 0u)
        {
            ImGui::SeparatorText("glTF skin import");
            ImGui::TextDisabled(
                "Skins: %u, joints: %u, influenced vertices: %u",
                m_rayTracingManager->GetImportedSkinCount(),
                m_rayTracingManager->GetImportedSkinJointCount(),
                m_rayTracingManager->GetImportedSkinnedVertexCount());
            ImGui::TextDisabled(
                "Animated joints: %u, max transform delta: %.5f",
                m_rayTracingManager->GetAnimatedSkinJointCount(),
                m_rayTracingManager->GetSkinJointTransformDelta());
            ImGui::TextDisabled(
                m_rayTracingManager->IsGpuSkinningActive()
                    ? "GPU skinning: active (dynamic BLAS update)"
                    : "GPU skinning: unavailable");
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

        ImGui::BeginDisabled(m_enableAccumulation);
        ImGui::Checkbox("Ray Cone Texture LOD", &m_enableTextureLod);
        ImGui::EndDisabled();
        if (m_enableTextureLod)
        {
            ImGui::Checkbox(
                "Propagate Ray Cone Through Bounces",
                &m_enablePropagatedRayCone);
            ImGui::TextDisabled(
                m_enablePropagatedRayCone
                    ? "LOD mode: propagated path distance + BSDF lobe spread"
                    : "LOD mode: legacy camera-to-hit distance");
            ImGui::SliderFloat(
                "Texture LOD Bias",
                &m_textureLodBias,
                -2.0f,
                2.0f,
                "%.2f");
            ImGui::TextDisabled(
                "Higher bias reduces distant texture/normal-map aliasing.");
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

        if (m_rayTracingManager &&
            m_rayTracingManager->HasDirectionalLight())
        {
            bool directionalChanged = false;
            directionalChanged |= ImGui::Checkbox(
                "Enable Directional Light",
                &m_enableDirectionalLight);
            directionalChanged |= ImGui::SliderFloat(
                "Directional Light Intensity",
                &m_directionalLightIntensityScale,
                0.0f,
                4.0f,
                "%.2f");
            directionalChanged |= ImGui::SliderFloat(
                "Directional Sun Angular Radius",
                &m_directionalLightAngularRadiusDegrees,
                0.0f,
                15.0f,
                "%.2f deg");
            const std::array<float, 3>& propagationDirection =
                m_rayTracingManager->GetDirectionalLightDirection();
            float directionLengthSquared = 0.0f;
            for (float component : propagationDirection)
                directionLengthSquared += component * component;
            if (directionLengthSquared > 1.0e-8f)
            {
                constexpr float radiansToDegrees =
                    57.29577951308232f;
                constexpr float degreesToRadians =
                    0.017453292519943295f;
                const float inverseDirectionLength =
                    1.0f / std::sqrt(directionLengthSquared);
                const std::array<float, 3> sourceDirection =
                {
                    -propagationDirection[0] * inverseDirectionLength,
                    -propagationDirection[1] * inverseDirectionLength,
                    -propagationDirection[2] * inverseDirectionLength
                };
                float sourceAzimuthDegrees =
                    std::atan2(
                        sourceDirection[2],
                        sourceDirection[0]) *
                    radiansToDegrees;
                float sourceElevationDegrees =
                    std::asin((std::max)(
                        -1.0f,
                        (std::min)(1.0f, sourceDirection[1]))) *
                    radiansToDegrees;
                bool directionChanged = false;
                directionChanged |= ImGui::SliderFloat(
                    "Directional Source Azimuth",
                    &sourceAzimuthDegrees,
                    -180.0f,
                    180.0f,
                    "%.1f deg");
                directionChanged |= ImGui::SliderFloat(
                    "Directional Source Elevation",
                    &sourceElevationDegrees,
                    -89.0f,
                    89.0f,
                    "%.1f deg");
                if (directionChanged)
                {
                    const float azimuthRadians =
                        sourceAzimuthDegrees * degreesToRadians;
                    const float elevationRadians =
                        sourceElevationDegrees * degreesToRadians;
                    const float horizontalScale =
                        std::cos(elevationRadians);
                    const std::array<float, 3> newSourceDirection =
                    {
                        horizontalScale * std::cos(azimuthRadians),
                        std::sin(elevationRadians),
                        horizontalScale * std::sin(azimuthRadians)
                    };
                    m_rayTracingManager->SetDirectionalLightDirection(
                        {
                            -newSourceDirection[0],
                            -newSourceDirection[1],
                            -newSourceDirection[2]
                        });
                    directionalChanged = true;
                }
            }
            ImGui::TextDisabled(
                "0 deg = hard directional shadow; larger values soften the sun disk.");
            ImGui::TextDisabled(
                "Shares one NEE shadow-ray sample with area lights and IBL.");
            ImGui::TextDisabled(
                "Directional lights have no finite position; azimuth/elevation rotate the source at infinity.");
            if (directionalChanged)
            {
                m_captureActive = false;
                m_saveCurrentRequested = false;
                m_captureStatus.clear();
                m_rayTracingManager->SetDirectionalLightRuntimeSettings(
                    m_enableDirectionalLight,
                    m_directionalLightIntensityScale);
                m_rayTracingManager->SetDirectionalLightAngularRadius(
                    m_directionalLightAngularRadiusDegrees *
                    0.01745329251994329577f);
            }
        }
    }
    ImGui::SliderFloat("Exposure (EV)", &m_exposure, -8.0f, 8.0f, "%.2f");
    ImGui::Checkbox("Show normal color", &m_showNormalColor);
    int renderMode = m_enableAccumulation ? 0 : 1;
    if (ImGui::Combo("Mode", &renderMode, "Reference\0Realtime\0\0") &&
        m_rayTracingManager)
    {
        m_enableAccumulation = renderMode == 0;
        m_captureActive = false;
        m_saveCurrentRequested = false;
        m_captureStatus.clear();
        m_rayTracingManager->SetEnableAccumulation(m_enableAccumulation);
    }
    EnforceReferenceModeSettings();
    ImGui::TextDisabled(
        m_enableAccumulation
            ? "Reference: static scene; accumulation on, temporal/motion/LOD/A-Trous off."
            : "Realtime: no cross-frame accumulation; temporal and motion settings are configurable.");
    ImGui::SliderInt("Max Bounce", &m_maxBounce, 1, 8);
    ImGui::SliderInt("Samples Per Pixel / Frame", &m_samplesPerPixel, 1, 8);
    const char* lightingModeNames[] =
    {
        "BSDF Only",
        "NEE (Area + Environment + Directional)",
        "MIS (Area + Environment + Directional)"
    };
    if (ImGui::Combo(
        "Lighting",
        &m_lightingMode,
        lightingModeNames,
        _countof(lightingModeNames)) &&
        m_rayTracingManager)
    {
        m_captureActive = false;
        m_saveCurrentRequested = false;
        m_captureStatus.clear();
        m_rayTracingManager->SetLightingMode(
            static_cast<UINT>(m_lightingMode));
    }
    if (ImGui::Checkbox(
        "Russian Roulette (from bounce 3)",
        &m_enableRussianRoulette) &&
        m_rayTracingManager)
    {
        m_rayTracingManager->SetRussianRouletteEnabled(
            m_enableRussianRoulette);
    }
    ImGui::SeparatorText("Denoising");
    ImGui::BeginDisabled(m_enableAccumulation);
    ImGui::Checkbox(
        "Temporal Reprojection",
        &m_enableTemporalReprojection);
    if (m_enableTemporalReprojection)
    {
        ImGui::Checkbox(
            "Bilinear / Best-Tap History Gather",
            &m_enableBestTapHistoryGather);
        ImGui::TextDisabled(
            "All valid: bilinear. Partial valid: highest-weight tap only.");
        if (m_rayTracingManager &&
            (m_rayTracingManager->HasDynamicSphere() ||
             m_rayTracingManager->HasDynamicCube() ||
             m_rayTracingManager->HasSceneAnimation()))
        {
            ImGui::Checkbox(
                "Dynamic Object Reprojection",
                &m_enableDynamicObjectReprojection);
            ImGui::TextDisabled(
                "ON: reproject rigid transforms or the previous skinned pose.");
            ImGui::Checkbox(
                "Static Background History Fast Path",
                &m_enableStaticBackgroundHistoryFastPath);
            ImGui::TextDisabled(
                "ON: use same-pixel history validation for static background while objects move.");
            ImGui::Checkbox(
                "Disocclusion Repair Ray Pass",
                &m_enableDisocclusionRepair);
            if (m_enableDisocclusionRepair)
            {
                ImGui::SliderInt(
                    "Disocclusion Repair SPP",
                    &m_disocclusionRepairSamplesPerPixel,
                    1,
                    8);
            }
            ImGui::TextDisabled(
                "Adds isolated rays only where moving geometry reveals rejected static history.");
            if (m_rayTracingManager->HasDirectionalLight())
            {
                ImGui::Checkbox(
                    "Directional Shadow History Validation",
                    &m_enableDynamicShadowHistoryValidation);
                ImGui::TextDisabled(
                    "Rejects history only where deterministic directional shadow visibility changes.");
            }
            if (m_rayTracingManager->IsGpuSkinningActive())
            {
                ImGui::Checkbox(
                    "Deformation Motion Vector",
                    &m_enableSkinnedDeformationMotion);
                ImGui::TextDisabled(
                    "OFF: instance-transform baseline, ON: previous skinned vertices.");
            }
        }
        ImGui::Checkbox(
            "Current-frame Visible Emission / Environment",
            &m_useCurrentFrameVisibleResidual);
        ImGui::TextDisabled(
            "Keeps visible emitters/environment out of temporal history; diffuse/specular lighting still accumulates.");
        ImGui::Checkbox(
            "Dynamic Object Color Clipping (with A-Trous)",
            &m_enableTemporalColorClip);
        ImGui::TextDisabled(
            "Applies 5x5 edge-aware history clipping only around moving objects.");
        const char* temporalDebugNames[] =
        {
            "None",
            "History Length",
            "History Rejection Mask",
            "Motion Magnitude",
            "Motion X",
            "Motion Y",
            "History Rejection Reason",
            "Reprojection Surface Error",
            "Radiance History Difference"
        };
        ImGui::Combo(
            "Temporal Debug View",
            &m_temporalDebugView,
            temporalDebugNames,
            _countof(temporalDebugNames));
        if (m_temporalDebugView == static_cast<int>(
            RayTracingManager::c_temporalDebugHistoryLength))
        {
            ImGui::TextDisabled("History Length: black = 1, white = 32 or more.");
        }
        else if (m_temporalDebugView == static_cast<int>(
            RayTracingManager::c_temporalDebugRejectionMask))
        {
            ImGui::TextDisabled("History: green = accepted, red = rejected, blue = unavailable.");
        }
        else if (m_temporalDebugView == static_cast<int>(
            RayTracingManager::c_temporalDebugMotionMagnitude))
        {
            ImGui::TextDisabled(
                "Magnitude: black = zero, white = 16 pixels or more.");
        }
        else if (m_temporalDebugView == static_cast<int>(
            RayTracingManager::c_temporalDebugMotionX))
        {
            ImGui::TextDisabled(
                "Motion X: gray = zero, white = positive, black = negative.");
        }
        else if (m_temporalDebugView == static_cast<int>(
            RayTracingManager::c_temporalDebugMotionY))
        {
            ImGui::TextDisabled(
                "Motion Y: gray = zero, white = positive, black = negative.");
        }
        else if (m_temporalDebugView == static_cast<int>(
            RayTracingManager::c_temporalDebugRejectionReason))
        {
            ImGui::TextDisabled(
                "Reason: green accept, magenta projection, blue outside, red hit/miss.");
            ImGui::TextDisabled(
                "Orange instance, yellow distance, cyan normal, violet material, gray no history, white coverage.");
        }
        else if (m_temporalDebugView == static_cast<int>(
            RayTracingManager::c_temporalDebugSurfaceError))
        {
            ImGui::TextDisabled(
                "Surface error: green 0%%, yellow 2.5%%, red >= 5%%; black = unavailable.");
        }
        else if (m_temporalDebugView == static_cast<int>(
            RayTracingManager::c_temporalDebugRadianceHistoryDifference))
        {
            ImGui::TextDisabled(
                "Radiance difference: green = match, red = brighter history, blue = darker history.");
            ImGui::TextDisabled(
                "Uses a 5x5 edge-aware current estimate; full color at >= 50%% RGB difference.");
        }
        ImGui::TextDisabled(
            "Temporal history: up to 256 frames for valid pixels.");
        ImGui::TextDisabled(
            "Static reference views keep accumulating when progressive mode is enabled.");
    }

    ImGui::EndDisabled();
    ImGui::BeginDisabled(m_enableAccumulation);
    ImGui::Checkbox("A-Trous Filter", &m_enableAtrous);
    ImGui::EndDisabled();
    if (m_enableAtrous)
    {
        ImGui::Checkbox(
            "Adaptive Iterations",
            &m_enableAtrousAdaptiveIterations);
        ImGui::SliderInt(
            "Manual Diffuse Iterations",
            &m_atrousIterations,
            1,
            8);
        ImGui::SliderInt(
            "Manual Specular Iterations",
            &m_atrousSpecularIterations,
            1,
            8);
        if (m_enableAtrousAdaptiveIterations)
        {
            ImGui::Combo(
                "A-Trous Debug View",
                &m_atrousDebugView,
                "Beauty\0Adaptive Iteration Count\0");
            ImGui::TextDisabled(
                "Auto: dynamic/low history 5; stable diffuse 4;");
            ImGui::TextDisabled(
                "stable specular 2/3/4 by roughness.");
            ImGui::TextDisabled(
                "Wide steps stop when fewer than 3 compatible neighbors remain.");
            ImGui::TextDisabled(
                "Manual iteration sliders are used only when Adaptive is OFF.");
            if (m_atrousDebugView == static_cast<int>(
                    RayTracingManager::c_atrousDebugIterationCount))
            {
                ImGui::TextDisabled(
                    "Effective passes: blue 1, cyan 2, green 3, yellow 4, red 5.");
            }
        }
        ImGui::Combo(
            "Specular Material Weight",
            &m_atrousSpecularMaterialWeightMode,
            "None\0Albedo\0F0\0");
        ImGui::Combo(
            "Specular Roughness Weight",
            &m_atrousSpecularRoughnessWeightMode,
            "None\0Roughness\0");
        ImGui::Combo(
            "A-Trous Kernel",
            &m_atrousKernelMode,
            "3x3\0" "5x5\0");
        ImGui::Checkbox(
            "Adaptive Edge Weights",
            &m_enableAtrousAdaptiveEdgeWeights);
        ImGui::SliderFloat(
            "Manual Normal Exponent",
            &m_atrousNormalExponent,
            1.0f,
            256.0f,
            "%.1f",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat(
            "Manual Depth Sigma",
            &m_atrousDepthSigma,
            0.0001f,
            0.1f,
            "%.4f",
            ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat(
            "A-Trous Color Sigma",
            &m_atrousColorSigma,
            0.25f,
            16.0f,
            "%.2f");
        ImGui::Checkbox(
            "Diffuse Log-Luminance Edge Stop",
            &m_enableAtrousLogLuminanceEdgeStop);
        if (m_enableAtrousLogLuminanceEdgeStop)
        {
            ImGui::SliderFloat(
                "Diffuse Log-Luminance Sigma",
                &m_atrousLogLuminanceSigma,
                1.0f,
                10.0f,
                "%.3f",
                ImGuiSliderFlags_Logarithmic);
            ImGui::TextDisabled(
                "Fixed relative-brightness edge stop; lower values block shadow-boundary leakage.");
        }
        else
        {
            ImGui::TextDisabled(
                "Diffuse color edge stop uses variance-scaled A-Trous Color Sigma.");
        }
        ImGui::TextDisabled(
            "Diffuse: albedo; Specular guides are independently selectable.");
        ImGui::TextDisabled(
            "Lower normal exponent smooths curves; lower depth sigma blocks silhouettes.");
        if (m_enableAtrousAdaptiveEdgeWeights)
        {
            if (ImGui::TreeNode("Adaptive Weight Tuning"))
            {
                ImGui::SliderFloat(
                    "Dynamic Normal",
                    &m_atrousAdaptiveDynamicNormalExponent,
                    1.0f,
                    256.0f,
                    "%.1f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat(
                    "Dynamic Depth",
                    &m_atrousAdaptiveDynamicDepthSigma,
                    0.0001f,
                    0.1f,
                    "%.4f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat(
                    "Low History Normal",
                    &m_atrousAdaptiveLowHistoryNormalExponent,
                    1.0f,
                    256.0f,
                    "%.1f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat(
                    "Low History Depth",
                    &m_atrousAdaptiveLowHistoryDepthSigma,
                    0.0001f,
                    0.1f,
                    "%.4f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat(
                    "Stable Normal Base",
                    &m_atrousAdaptiveStableNormalExponent,
                    1.0f,
                    256.0f,
                    "%.1f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat(
                    "Stable Depth",
                    &m_atrousAdaptiveStableDepthSigma,
                    0.0001f,
                    0.1f,
                    "%.4f",
                    ImGuiSliderFlags_Logarithmic);
                ImGui::TreePop();
            }
            ImGui::TextDisabled(
                "Manual sliders are used only when Adaptive is OFF.");
        }
        ImGui::TextDisabled(
            "Diffuse and specular use independent filter widths.");
        ImGui::TextDisabled(
            "HDR PFM accumulation remains unfiltered.");
    }
    if (ImGui::Button("Reset samples") && m_rayTracingManager)
    {
        m_rayTracingManager->ResetAccumulation();
    }
    const UINT accumulatedSamples = m_rayTracingManager
        ? m_rayTracingManager->GetAccumulatedSampleCount()
        : 0u;
    ImGui::Text("Samples: %u", accumulatedSamples);
    if (m_rayTracingManager &&
        m_sceneType == static_cast<int>(
            RayTracingManager::c_sceneDynamicTransformTest))
    {
        ImGui::SeparatorText("Dynamic transform test");
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
        if (ImGui::Combo(
            "Test PBR Debug",
            &m_pbrDebugView,
            pbrDebugNames,
            _countof(pbrDebugNames)))
        {
            m_rayTracingManager->SetPbrDebugView(
                static_cast<UINT>(m_pbrDebugView));
        }
        const char* materialPresetNames[] =
        {
            "Polished gold metal",
            "Rough red dielectric",
            "Rough steel metal",
            "Glossy blue dielectric"
        };
        if (ImGui::Combo(
            "Sphere material",
            &m_dynamicTestSphereMaterialPreset,
            materialPresetNames,
            _countof(materialPresetNames)))
        {
            m_rayTracingManager->SetDynamicTestSphereMaterialPreset(
                static_cast<UINT>(m_dynamicTestSphereMaterialPreset));
        }
        if (ImGui::Combo(
            "Cube material",
            &m_dynamicTestCubeMaterialPreset,
            materialPresetNames,
            _countof(materialPresetNames)))
        {
            m_rayTracingManager->SetDynamicTestCubeMaterialPreset(
                static_cast<UINT>(m_dynamicTestCubeMaterialPreset));
        }
        bool testIblChanged = false;
        testIblChanged |= ImGui::Checkbox(
            "Enable test IBL",
            &m_enableIbl);
        testIblChanged |= ImGui::SliderFloat(
            "Test IBL Intensity",
            &m_iblIntensity,
            0.0f,
            4.0f,
            "%.2f");
        if (testIblChanged)
        {
            m_rayTracingManager->SetIblSettings(
                m_enableIbl,
                m_iblIntensity);
        }
        if (ImGui::Checkbox(
            "Show test sphere",
            &m_showDynamicSphere))
        {
            m_rayTracingManager->SetDynamicSphereVisible(
                m_showDynamicSphere);
        }
        ImGui::BeginDisabled(m_enableAccumulation);
        if (m_showDynamicSphere &&
            ImGui::Checkbox(
                "Animate test sphere",
                &m_animateDynamicSphere))
        {
            m_rayTracingManager->SetDynamicSphereAnimationEnabled(
                m_animateDynamicSphere);
            m_rayTracingManager->ResetDynamicSphereTimeline();
        }
        ImGui::EndDisabled();
        if (ImGui::Checkbox(
            "Show test cube",
            &m_showDynamicCube))
        {
            m_rayTracingManager->SetDynamicCubeVisible(
                m_showDynamicCube);
        }
        ImGui::BeginDisabled(m_enableAccumulation);
        if (m_showDynamicCube &&
            ImGui::Checkbox(
                "Animate test cube",
                &m_animateDynamicCube))
        {
            m_rayTracingManager->SetDynamicCubeAnimationEnabled(
                m_animateDynamicCube);
            m_rayTracingManager->ResetDynamicSphereTimeline();
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "Sphere: translation + rolling. Cube: rotation + short orbit.");
        ImGui::TextDisabled(
            "Use one object at a time, then both, to isolate history errors.");
    }
    else if (m_rayTracingManager && m_rayTracingManager->HasDynamicSphere())
    {
        if (!m_dynamicSphereMaterialSelectionInitialized)
        {
            const float currentMetallic =
                m_rayTracingManager->GetDynamicSphereMetallic();
            const float currentRoughness =
                m_rayTracingManager->GetDynamicSphereRoughness();
            m_dynamicSphereMetallicIndex = 0;
            m_dynamicSphereRoughnessIndex = 0;
            for (int index = 1;
                 index < static_cast<int>(_countof(c_sphereMetallicValues));
                 ++index)
            {
                if (std::abs(
                    c_sphereMetallicValues[index] - currentMetallic) <
                    std::abs(
                        c_sphereMetallicValues[m_dynamicSphereMetallicIndex] -
                        currentMetallic))
                {
                    m_dynamicSphereMetallicIndex = index;
                }
            }
            for (int index = 1;
                 index < static_cast<int>(_countof(c_sphereRoughnessValues));
                 ++index)
            {
                if (std::abs(
                    c_sphereRoughnessValues[index] - currentRoughness) <
                    std::abs(
                        c_sphereRoughnessValues[m_dynamicSphereRoughnessIndex] -
                        currentRoughness))
                {
                    m_dynamicSphereRoughnessIndex = index;
                }
            }
            m_dynamicSphereMaterialSelectionInitialized = true;
        }

        bool sphereMaterialChanged = false;
        sphereMaterialChanged |= ImGui::Combo(
            c_sphereMetallicLabel,
            &m_dynamicSphereMetallicIndex,
            c_sphereMetallicItems);
        sphereMaterialChanged |= ImGui::Combo(
            c_sphereRoughnessLabel,
            &m_dynamicSphereRoughnessIndex,
            c_sphereRoughnessItems);
        if (sphereMaterialChanged)
        {
            m_captureActive = false;
            m_saveCurrentRequested = false;
            m_captureStatus.clear();
            m_rayTracingManager->SetDynamicSphereMaterial(
                c_sphereMetallicValues[m_dynamicSphereMetallicIndex],
                c_sphereRoughnessValues[m_dynamicSphereRoughnessIndex]);
        }
        const char sphereMaterialStatusFormat[] =
            { 83,112,104,101,114,101,32,109,97,116,101,114,105,97,108,58,32,
              109,101,116,97,108,108,105,99,32,37,46,50,102,44,32,
              114,111,117,103,104,110,101,115,115,32,37,46,50,102,0 };
        ImGui::TextDisabled(
            sphereMaterialStatusFormat,
            m_rayTracingManager->GetDynamicSphereMetallic(),
            m_rayTracingManager->GetDynamicSphereRoughness());
        if (ImGui::Checkbox(
            "Show metal sphere",
            &m_showDynamicSphere))
        {
            m_captureActive = false;
            m_saveCurrentRequested = false;
            m_captureStatus.clear();
            m_rayTracingManager->SetDynamicSphereVisible(
                m_showDynamicSphere);
        }
        if (m_showDynamicSphere)
        {
            ImGui::BeginDisabled(m_enableAccumulation);
            if (ImGui::Checkbox(
                "Animate rolling sphere",
                &m_animateDynamicSphere))
            {
                m_rayTracingManager->SetDynamicSphereAnimationEnabled(
                    m_animateDynamicSphere);
                m_rayTracingManager->ResetDynamicSphereTimeline();
            }
            ImGui::EndDisabled();
        }
    }
    ImGui::SeparatorText("Camera controls and recording");
    if (m_rayTracingManager &&
        (m_rayTracingManager->HasBrainStemScene() ||
         m_rayTracingManager->HasMechDroneScene()))
    {
        const char composedObjectsLabel[] =
            { 67,111,109,112,111,115,101,100,32,115,99,101,110,101,32,111,98,106,101,99,116,115,0 };
        const char brainStemLabel[] =
            { 83,104,111,119,32,66,114,97,105,110,83,116,101,109,0 };
        const char mechDroneLabel[] =
            { 83,104,111,119,32,109,101,99,104,32,100,114,111,110,101,0 };
        ImGui::SeparatorText(composedObjectsLabel);
        if (m_rayTracingManager->HasBrainStemScene() &&
            ImGui::Checkbox(brainStemLabel, &m_showBrainStem))
        {
            m_captureActive = false;
            m_saveCurrentRequested = false;
            m_captureStatus.clear();
            m_rayTracingManager->SetBrainStemVisible(m_showBrainStem);
        }
        if (m_rayTracingManager->HasMechDroneScene() &&
            ImGui::Checkbox(mechDroneLabel, &m_showMechDrone))
        {
            m_captureActive = false;
            m_saveCurrentRequested = false;
            m_captureStatus.clear();
            m_rayTracingManager->SetMechDroneVisible(m_showMechDrone);
        }
    }
    ImGui::SliderFloat(
        "Arrow-key turn speed",
        &m_keyboardTurnSpeedDegrees,
        5.0f,
        120.0f,
        "%.1f deg/s");
    ImGui::InputText(
        "Recording output JSON",
        m_cameraRecordingPath,
        sizeof(m_cameraRecordingPath));
    if (m_cameraPathRecordingActive)
    {
        if (ImGui::Button("Stop and save camera recording"))
            StopCameraPathRecording();
        ImGui::SameLine();
        ImGui::Text(
            "%.2f s / %zu poses",
            m_cameraRecordingElapsedSeconds,
            m_recordedCameraKeyframes.size());
    }
    else if (ImGui::Button("Start camera recording"))
    {
        StartCameraPathRecording();
    }
    if (!m_cameraRecordingStatus.empty())
        ImGui::TextDisabled("%s", m_cameraRecordingStatus.c_str());

    ImGui::InputText(
        "Playback path JSON",
        m_cameraPlaybackPath,
        sizeof(m_cameraPlaybackPath));
    if (!m_cameraPathRecordingActive)
    {
        if (m_cameraPathPlaybackActive)
        {
            if (ImGui::Button("Stop deterministic camera path"))
                StopCameraPathPlayback();
        }
        else if (ImGui::Button("Load and play camera path"))
        {
            if (LoadCameraPathForPlayback())
                StartCameraPathPlayback();
        }
    }
    if (!m_cameraPlaybackStatus.empty())
        ImGui::TextDisabled("%s", m_cameraPlaybackStatus.c_str());
    ImGui::TextDisabled(
        m_cameraPathPlaybackActive
        ? "Camera: deterministic JSON path playing"
        : (m_cameraPathRecordingActive
            ? "Camera: recording free-camera motion at 60 Hz"
            : "Camera: WASD/QE, Shift, arrows look, Ctrl = precise turn"));
    if (m_rayTracingManager)
    {
        const std::array<float, 3>& cameraPosition =
            m_rayTracingManager->GetCameraPosition();
        const std::array<float, 3>& cameraTarget =
            m_rayTracingManager->GetCameraTarget();
        ImGui::Text(
            "Camera pos: %.3f %.3f %.3f",
            cameraPosition[0],
            cameraPosition[1],
            cameraPosition[2]);
        ImGui::Text(
            "Camera target: %.3f %.3f %.3f",
            cameraTarget[0],
            cameraTarget[1],
            cameraTarget[2]);
        if (ImGui::Button("Copy fixed camera JSON"))
        {
            char fixedCameraJson[1024] = {};
            constexpr char fixedCameraFormat[] = R"json({
  "frames_per_second": 60,
  "loop": false,
  "description": "Fixed RR benchmark camera",
  "keyframes": [
    { "time": 0.0, "position": [%.9g, %.9g, %.9g], "target": [%.9g, %.9g, %.9g] },
    { "time": 15.0, "position": [%.9g, %.9g, %.9g], "target": [%.9g, %.9g, %.9g] }
  ]
}
)json";
            std::snprintf(
                fixedCameraJson,
                sizeof(fixedCameraJson),
                fixedCameraFormat,
                cameraPosition[0],
                cameraPosition[1],
                cameraPosition[2],
                cameraTarget[0],
                cameraTarget[1],
                cameraTarget[2],
                cameraPosition[0],
                cameraPosition[1],
                cameraPosition[2],
                cameraTarget[0],
                cameraTarget[1],
                cameraTarget[2]);
            ImGui::SetClipboardText(fixedCameraJson);
        }
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float frameTimeMs = io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f;
    ImGui::Text(
        "Display frame (ImGui rolling): %.2f ms (%.1f FPS)",
        frameTimeMs,
        io.Framerate);
    ImGui::Checkbox("VSync", &m_vsyncEnabled);
    ImGui::Checkbox("Collect ray statistics", &m_collectRayStatistics);
    bool gpuPassProfilingEnabled =
        m_gpuPassProfilingEnabled;
    if (ImGui::Checkbox(
            "GPU Pass Profiling (F2)",
            &gpuPassProfilingEnabled))
    {
        SetGpuPassProfilingEnabled(
            gpuPassProfilingEnabled);
    }
    ImGui::Text(
        "CPU frame current/median: %.2f / %.2f ms",
        m_cpuFrameMs,
        m_cpuMedianMs);
    ImGui::TextDisabled(
        "CPU frame includes Present, GPU wait, and profiling readback.");
    ImGui::Text(
        "CPU p95/p99: %.2f / %.2f ms",
        m_cpuP95Ms,
        m_cpuP99Ms);
    if (m_gpuPassProfilingEnabled)
    {
        ImGui::Text(
            "GPU dispatch/upscale: %.2f / %.2f ms",
            m_gpuDispatchMs,
            m_gpuUpscaleMs);
        ImGui::Text(
            "GPU TLAS / Path+Temporal / Color Clip: %.2f / %.2f / %.2f ms",
            m_gpuTlasMs,
            m_gpuPathTraceMs,
            m_gpuTemporalColorClipMs);
        ImGui::Text(
            "GPU Main Path / Disocclusion Repair: %.2f / %.2f ms",
            m_gpuMainPathMs,
            m_gpuDisocclusionRepairMs);
        ImGui::Text(
            "GPU A-Trous diffuse/specular: %.2f / %.2f ms",
            m_gpuAtrousDiffuseMs,
            m_gpuAtrousSpecularMs);
        ImGui::Text(
            "GPU output copy / UI: %.2f / %.2f ms",
            m_gpuOutputCopyMs,
            m_gpuUiMs);
        ImGui::Text(
            "GPU total median/p95/p99: %.2f / %.2f / %.2f ms",
            m_gpuMedianMs,
            m_gpuP95Ms,
            m_gpuP99Ms);
    }
    else
    {
        ImGui::TextDisabled(
            "GPU timestamps and ResolveQueryData are disabled.");
        ImGui::TextDisabled(
            "For ON/OFF comparison, use CPU median after warm-up.");
    }
    if (m_cameraPathPlaybackActive)
    {
        const double pathTime = static_cast<double>(m_cameraPathFrameIndex) /
            m_cameraPath.GetFramesPerSecond();
        ImGui::Text(
            "Camera path: %.2f / %.2f s",
            pathTime,
            m_cameraPath.GetDurationSeconds());
        ImGui::Text(
            "Camera speed: %.3f units/s, %.2f deg/s",
            m_cameraLinearSpeed,
            m_cameraAngularSpeed);
    }
    if (m_collectRayStatistics && m_rayTracingManager)
    {
        const RayTracingManager::FrameStatistics& statistics =
            m_rayTracingManager->GetFrameStatistics();
        ImGui::Text(
            "Rays path primary/guide/bounce: %llu / %llu / %llu",
            static_cast<unsigned long long>(statistics.GetPrimaryRayCount()),
            static_cast<unsigned long long>(statistics.primaryGuideRays),
            static_cast<unsigned long long>(statistics.GetBounceRayCount()));
        ImGui::Text(
            "Shadow rays total/NEE/history: %llu / %llu / %llu",
            static_cast<unsigned long long>(statistics.shadowRays),
            static_cast<unsigned long long>(statistics.neeShadowRays),
            static_cast<unsigned long long>(
                statistics.historyValidationShadowRays));
        ImGui::Text(
            "Path avg/hit/miss: %.2f / %llu / %llu",
            statistics.GetAveragePathLength(),
            static_cast<unsigned long long>(statistics.hitCount),
            static_cast<unsigned long long>(statistics.missCount));
        ImGui::Text(
            "Depth rays: %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            static_cast<unsigned long long>(statistics.raysByDepth[0]),
            static_cast<unsigned long long>(statistics.raysByDepth[1]),
            static_cast<unsigned long long>(statistics.raysByDepth[2]),
            static_cast<unsigned long long>(statistics.raysByDepth[3]),
            static_cast<unsigned long long>(statistics.raysByDepth[4]),
            static_cast<unsigned long long>(statistics.raysByDepth[5]),
            static_cast<unsigned long long>(statistics.raysByDepth[6]),
            static_cast<unsigned long long>(statistics.raysByDepth[7]),
            static_cast<unsigned long long>(statistics.raysByDepth[8]));
    }
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
    if (!m_imguiInitialized || !m_imguiVisible)
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
    if (enabled)
        m_collectRayStatistics = true;
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
    m_sceneType = sceneType <= RayTracingManager::c_sceneDynamicTransformTest
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
    // Automated captures represent one fixed animation time. Advancing the
    // node every frame would mix different poses in progressive accumulation.
    m_animateGltfScene = false;
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

    const BYTE* sourcePixels = static_cast<const BYTE*>(pixels);
    bool saved = true;
    for (UINT outputY = 0; outputY < height && saved; ++outputY)
    {
        const UINT sourceY = height - 1u - outputY;
        const float* sourceRow = reinterpret_cast<const float*>(
            sourcePixels + static_cast<std::size_t>(sourceY) * rowPitch);

        for (UINT x = 0; x < width; ++x)
        {
            const float storedSampleCount =
                std::abs(sourceRow[x * 4 + 3]);
            const float pixelSampleCount = storedSampleCount > 0.0f
                ? storedSampleCount
                : static_cast<float>(sampleCount > 0 ? sampleCount : 1u);
            const float inverseSampleCount = 1.0f / pixelSampleCount;
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


