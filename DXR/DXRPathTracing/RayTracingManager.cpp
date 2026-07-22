#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RayTracingManager.h"
#include "GltfSceneLoader.h"
#include "SceneManifest.h"
#include "SceneData.h"
#include "SponzaLightConfig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

#pragma comment(lib, "d3d12.lib")

namespace
{
    constexpr wchar_t c_rayGenShaderName[] = L"MyRaygenShader_RadianceRay";
    constexpr wchar_t c_closestHitShaderName[] = L"MyClosestHitShader_RadianceRay";
    constexpr wchar_t c_anyHitShaderName[] = L"MyAnyHitShader_AlphaMask";
    constexpr wchar_t c_missShaderName[] = L"MyMissShader_RadianceRay";
    constexpr wchar_t c_shadowMissShaderName[] = L"MyMissShader_ShadowRay";
    constexpr wchar_t c_hitGroupName[] = L"MyHitGroup_Triangle_RadianceRay";
    constexpr wchar_t c_compiledShaderRelativePath[] = L"Shaders\\Raytracing.dxil";
    constexpr wchar_t c_compiledAtrousShaderRelativePath[] =
        L"Shaders\\AtrousDenoiser.dxil";
    constexpr wchar_t c_environmentMapRelativePath[] = L"Assets\\Textures\\Cubemaps\\HDRI\\autumn_hill_view_4kSpecularHDR.dds";
    constexpr UINT c_environmentDescriptorIndex = 8;
    constexpr UINT c_materialTextureDescriptorIndex = 9;
    constexpr UINT c_materialTextureDescriptorCount = 256;
    constexpr UINT c_atrousDiffuseIndirectSrvIndex =
        c_materialTextureDescriptorIndex + c_materialTextureDescriptorCount;
    constexpr UINT c_atrousSpecularIndirectSrvIndex =
        c_atrousDiffuseIndirectSrvIndex + 1;
    constexpr UINT c_atrousNormalDepthSrvIndex =
        c_atrousSpecularIndirectSrvIndex + 1;
    constexpr UINT c_atrousMaterialGuideSrvIndex =
        c_atrousNormalDepthSrvIndex + 1;
    constexpr UINT c_atrousDiffuseMomentsSrvIndex =
        c_atrousMaterialGuideSrvIndex + 1;
    constexpr UINT c_atrousSpecularMomentsSrvIndex =
        c_atrousDiffuseMomentsSrvIndex + 1;
    constexpr UINT c_atrousTotalSrvIndex =
        c_atrousSpecularMomentsSrvIndex + 1;
    constexpr UINT c_atrousFilteredDiffuseSrvIndex =
        c_atrousTotalSrvIndex + 1;
    constexpr UINT c_atrousFilterASrvIndex =
        c_atrousFilteredDiffuseSrvIndex + 1;
    constexpr UINT c_atrousFilterBSrvIndex =
        c_atrousFilterASrvIndex + 1;
    constexpr UINT c_atrousFilterAUavIndex =
        c_atrousFilterBSrvIndex + 1;
    constexpr UINT c_atrousFilterBUavIndex =
        c_atrousFilterAUavIndex + 1;
    constexpr UINT c_atrousFilteredDiffuseUavIndex =
        c_atrousFilterBUavIndex + 1;
    constexpr UINT c_atrousOutputUavIndex =
        c_atrousFilteredDiffuseUavIndex + 1;
    constexpr UINT c_descriptorCount = c_atrousOutputUavIndex + 1;
    constexpr UINT c_atrousFilterChannelDiffuse = 0;
    constexpr UINT c_atrousFilterChannelSpecular = 1;
    constexpr UINT c_statisticsShadowRayIndex =
        RayTracingManager::c_statisticsRayDepthCount;
    constexpr UINT c_statisticsHitIndex = c_statisticsShadowRayIndex + 1;
    constexpr UINT c_statisticsMissIndex = c_statisticsHitIndex + 1;
    constexpr UINT c_statisticsCounterCount = c_statisticsMissIndex + 1;
    constexpr UINT c_cubeFaceCount = 6;
    constexpr UINT c_ddsHeaderSize = 128;
    constexpr UINT c_ddsMagic = 0x20534444u;
    constexpr UINT c_d3dFormatA16B16G16R16F = 113;
    constexpr UINT c_bytesPerRgba16FloatPixel = 8;
    constexpr float c_verticalFovRadians = 1.221730476f;
    constexpr float c_cameraFrameMargin = 1.15f;
    struct DdsCubemapData
    {
        UINT width = 0;
        UINT height = 0;
        UINT mipCount = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT bytesPerPixel = 0;
        std::vector<std::uint8_t> texels;
    };

    struct RenderSettingsConstants
    {
        UINT showNormalColor;
        UINT frameIndex;
        UINT maxBounce;
        UINT sampleIndex;
        UINT enableAccumulation;
        UINT sceneType;
        UINT pbrDebugView;
        UINT enableIbl;
        float pbrMetallic;
        float pbrRoughness;
        float iblIntensity;
        float exposure;
        UINT validationSeed;
        float cameraPosition[3];
        float cameraTarget[3];
        UINT overridePbrMaterial;
        UINT enableStatistics;
        UINT dynamicObjectMoved;
        UINT enableRussianRoulette;
        UINT lightingMode;
        UINT emissiveTriangleCount;
        UINT environmentResolution;
        UINT environmentTexelCount;
        float areaLightPower;
        float environmentPower;
        UINT enableAtrous;
    };
    static_assert(sizeof(RenderSettingsConstants) == 30 * sizeof(std::uint32_t));

    struct AtrousSettingsConstants
    {
        UINT resolution[2];
        UINT stepWidth;
        UINT inputIsAccumulation;
        UINT finalPass;
        float normalExponent;
        float depthSigma;
        float colorSigma;
        float exposure;
        UINT demodulateDiffuse;
        UINT filterChannel;
    };
    static_assert(
        sizeof(AtrousSettingsConstants) == 11 * sizeof(std::uint32_t));

    struct GpuEmissiveTriangle
    {
        float vertex0[3];
        float area;
        float edge1[3];
        float selectionPdf;
        float edge2[3];
        float selectionCdf;
        float emission[3];
        std::uint32_t primitiveIndex;
    };
    static_assert(sizeof(GpuEmissiveTriangle) == 64);

    struct GpuEnvironmentAliasEntry
    {
        float acceptProbability;
        std::uint32_t aliasIndex;
        float selectionPdf;
        std::uint32_t padding;
    };
    static_assert(sizeof(GpuEnvironmentAliasEntry) == 16);

    float HalfToFloat(std::uint16_t value)
    {
        const bool negative = (value & 0x8000u) != 0u;
        const std::uint32_t exponent =
            (static_cast<std::uint32_t>(value) >> 10u) & 0x1Fu;
        const std::uint32_t mantissa =
            static_cast<std::uint32_t>(value) & 0x03FFu;
        float result = 0.0f;
        if (exponent == 0u)
        {
            result = std::ldexp(
                static_cast<float>(mantissa),
                -24);
        }
        else if (exponent == 31u)
        {
            result = mantissa == 0u
                ? std::numeric_limits<float>::infinity()
                : std::numeric_limits<float>::quiet_NaN();
        }
        else
        {
            result = std::ldexp(
                1.0f +
                    static_cast<float>(mantissa) / 1024.0f,
                static_cast<int>(exponent) - 15);
        }
        return negative ? -result : result;
    }

    double CubemapAreaElement(double x, double y)
    {
        return std::atan2(
            x * y,
            std::sqrt(x * x + y * y + 1.0));
    }

    double CubemapTexelSolidAngle(
        UINT x,
        UINT y,
        UINT resolution)
    {
        const double inverseResolution =
            1.0 / static_cast<double>(resolution);
        const double x0 =
            2.0 * static_cast<double>(x) * inverseResolution - 1.0;
        const double y0 =
            2.0 * static_cast<double>(y) * inverseResolution - 1.0;
        const double x1 =
            2.0 * static_cast<double>(x + 1u) * inverseResolution - 1.0;
        const double y1 =
            2.0 * static_cast<double>(y + 1u) * inverseResolution - 1.0;
        return CubemapAreaElement(x1, y1) -
            CubemapAreaElement(x0, y1) -
            CubemapAreaElement(x1, y0) +
            CubemapAreaElement(x0, y0);
    }

    bool BuildEnvironmentAliasTable(
        const DdsCubemapData& cubemap,
        std::vector<GpuEnvironmentAliasEntry>& entries,
        float& environmentPower)
    {
        if (cubemap.width == 0u ||
            cubemap.width != cubemap.height ||
            cubemap.bytesPerPixel != c_bytesPerRgba16FloatPixel)
        {
            return false;
        }

        const UINT resolution = cubemap.width;
        const UINT faceTexelCount = resolution * resolution;
        const UINT texelCount = c_cubeFaceCount * faceTexelCount;
        std::size_t faceByteCount = 0u;
        for (UINT mip = 0u; mip < cubemap.mipCount; ++mip)
        {
            const UINT mipWidth =
                std::max(cubemap.width >> mip, 1u);
            const UINT mipHeight =
                std::max(cubemap.height >> mip, 1u);
            faceByteCount +=
                static_cast<std::size_t>(mipWidth) *
                mipHeight *
                cubemap.bytesPerPixel;
        }

        std::vector<double> weights(texelCount, 0.0);
        double totalWeight = 0.0;
        for (UINT face = 0u; face < c_cubeFaceCount; ++face)
        {
            const std::size_t faceOffset =
                static_cast<std::size_t>(face) * faceByteCount;
            for (UINT y = 0u; y < resolution; ++y)
            {
                for (UINT x = 0u; x < resolution; ++x)
                {
                    const UINT texelIndex =
                        face * faceTexelCount + y * resolution + x;
                    const std::size_t byteOffset =
                        faceOffset +
                        static_cast<std::size_t>(
                            y * resolution + x) *
                        cubemap.bytesPerPixel;
                    const auto readHalf = [&](std::size_t componentOffset)
                    {
                        const std::uint16_t halfValue =
                            static_cast<std::uint16_t>(
                                cubemap.texels[
                                    byteOffset + componentOffset]) |
                            static_cast<std::uint16_t>(
                                cubemap.texels[
                                    byteOffset + componentOffset + 1u]
                                << 8u);
                        return HalfToFloat(halfValue);
                    };
                    const float red = readHalf(0u);
                    const float green = readHalf(2u);
                    const float blue = readHalf(4u);
                    const double luminance = std::max(
                        0.0,
                        static_cast<double>(red) * 0.2126 +
                        static_cast<double>(green) * 0.7152 +
                        static_cast<double>(blue) * 0.0722);
                    const double weight = std::isfinite(luminance)
                        ? luminance * CubemapTexelSolidAngle(
                            x,
                            y,
                            resolution)
                        : 0.0;
                    weights[texelIndex] = weight;
                    totalWeight += weight;
                }
            }
        }

        environmentPower = totalWeight > 0.0
            ? static_cast<float>(totalWeight)
            : 0.0f;
        entries.assign(texelCount, {});
        std::vector<double> scaledProbabilities(texelCount, 1.0);
        std::vector<UINT> smallEntries;
        std::vector<UINT> largeEntries;
        smallEntries.reserve(texelCount);
        largeEntries.reserve(texelCount);

        for (UINT index = 0u; index < texelCount; ++index)
        {
            const double selectionPdf = totalWeight > 0.0
                ? weights[index] / totalWeight
                : 1.0 / static_cast<double>(texelCount);
            entries[index].selectionPdf =
                static_cast<float>(selectionPdf);
            scaledProbabilities[index] =
                selectionPdf * static_cast<double>(texelCount);
            if (scaledProbabilities[index] < 1.0)
            {
                smallEntries.push_back(index);
            }
            else
            {
                largeEntries.push_back(index);
            }
        }

        while (!smallEntries.empty() && !largeEntries.empty())
        {
            const UINT smallIndex = smallEntries.back();
            smallEntries.pop_back();
            const UINT largeIndex = largeEntries.back();
            largeEntries.pop_back();

            entries[smallIndex].acceptProbability =
                static_cast<float>(std::max(
                    0.0,
                    std::min(
                        1.0,
                        scaledProbabilities[smallIndex])));
            entries[smallIndex].aliasIndex = largeIndex;
            scaledProbabilities[largeIndex] =
                scaledProbabilities[largeIndex] +
                scaledProbabilities[smallIndex] -
                1.0;
            if (scaledProbabilities[largeIndex] < 1.0)
            {
                smallEntries.push_back(largeIndex);
            }
            else
            {
                largeEntries.push_back(largeIndex);
            }
        }

        for (UINT remainingIndex : smallEntries)
        {
            entries[remainingIndex].acceptProbability = 1.0f;
            entries[remainingIndex].aliasIndex = remainingIndex;
        }
        for (UINT remainingIndex : largeEntries)
        {
            entries[remainingIndex].acceptProbability = 1.0f;
            entries[remainingIndex].aliasIndex = remainingIndex;
        }
        return true;
    }

    std::vector<GpuEmissiveTriangle> BuildEmissiveTriangles(
        const SceneData& scene,
        UINT staticIndexCount,
        float& areaLightPower)
    {
        std::vector<GpuEmissiveTriangle> lights;
        const UINT primitiveCount = staticIndexCount / 3u;
        double totalWeight = 0.0;
        for (UINT primitiveIndex = 0;
             primitiveIndex < primitiveCount;
             ++primitiveIndex)
        {
            const std::uint32_t materialIndex =
                scene.primitiveMaterialIndices[primitiveIndex];
            const SceneMaterial& material = scene.materials[materialIndex];
            const float luminance =
                material.emission[0] * 0.2126f +
                material.emission[1] * 0.7152f +
                material.emission[2] * 0.0722f;
            if (luminance <= 0.0f)
                continue;

            const std::uint32_t i0 =
                scene.indices[primitiveIndex * 3u + 0u];
            const std::uint32_t i1 =
                scene.indices[primitiveIndex * 3u + 1u];
            const std::uint32_t i2 =
                scene.indices[primitiveIndex * 3u + 2u];
            const SceneVertex& v0 = scene.vertices[i0];
            const SceneVertex& v1 = scene.vertices[i1];
            const SceneVertex& v2 = scene.vertices[i2];

            GpuEmissiveTriangle light = {};
            for (UINT component = 0; component < 3u; ++component)
            {
                light.vertex0[component] = v0.position[component];
                light.edge1[component] =
                    v1.position[component] - v0.position[component];
                light.edge2[component] =
                    v2.position[component] - v0.position[component];
                light.emission[component] = material.emission[component];
            }
            const float crossX =
                light.edge1[1] * light.edge2[2] -
                light.edge1[2] * light.edge2[1];
            const float crossY =
                light.edge1[2] * light.edge2[0] -
                light.edge1[0] * light.edge2[2];
            const float crossZ =
                light.edge1[0] * light.edge2[1] -
                light.edge1[1] * light.edge2[0];
            light.area = 0.5f * std::sqrt(
                crossX * crossX +
                crossY * crossY +
                crossZ * crossZ);
            if (light.area <= 0.0000001f)
                continue;

            light.selectionPdf = light.area * luminance;
            light.primitiveIndex = primitiveIndex;
            totalWeight += static_cast<double>(light.selectionPdf);
            lights.push_back(light);
        }

        areaLightPower = totalWeight > 0.0
            ? static_cast<float>(totalWeight * 3.14159265358979323846)
            : 0.0f;
        if (totalWeight > 0.0)
        {
            double cumulativeProbability = 0.0;
            for (GpuEmissiveTriangle& light : lights)
            {
                light.selectionPdf = static_cast<float>(
                    static_cast<double>(light.selectionPdf) /
                    totalWeight);
                cumulativeProbability += light.selectionPdf;
                light.selectionCdf = static_cast<float>(
                    cumulativeProbability);
            }
            lights.back().selectionCdf = 1.0f;
        }
        return lights;
    }

    UINT AlignUp(UINT value, UINT alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    UINT64 AlignUp64(UINT64 value, UINT64 alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    D3D12_RESOURCE_DESC CreateBufferDesc(
        UINT64 sizeInBytes,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = sizeInBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        return desc;
    }

    D3D12_HEAP_PROPERTIES CreateHeapProperties(D3D12_HEAP_TYPE heapType)
    {
        D3D12_HEAP_PROPERTIES properties = {};
        properties.Type = heapType;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    UINT ReadUint32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
    {
        return static_cast<UINT>(bytes[offset + 0]) |
            (static_cast<UINT>(bytes[offset + 1]) << 8) |
            (static_cast<UINT>(bytes[offset + 2]) << 16) |
            (static_cast<UINT>(bytes[offset + 3]) << 24);
    }

    UINT GetMipDimension(UINT baseDimension, UINT mipLevel)
    {
        const UINT dimension = baseDimension >> mipLevel;
        return dimension > 0 ? dimension : 1;
    }

    bool ParseLegacyRgba16FloatCubemapDds(const std::vector<std::uint8_t>& bytes, DdsCubemapData& cubemap)
    {
        if (bytes.size() < c_ddsHeaderSize || ReadUint32(bytes, 0) != c_ddsMagic)
            return false;

        const UINT height = ReadUint32(bytes, 12);
        const UINT width = ReadUint32(bytes, 16);
        UINT mipCount = ReadUint32(bytes, 28);
        const UINT fourCc = ReadUint32(bytes, 84);
        if (width == 0 || height == 0 || fourCc != c_d3dFormatA16B16G16R16F)
            return false;

        if (mipCount == 0)
            mipCount = 1;

        UINT64 requiredBytes = 0;
        for (UINT face = 0; face < c_cubeFaceCount; ++face)
        {
            for (UINT mip = 0; mip < mipCount; ++mip)
            {
                const UINT mipWidth = GetMipDimension(width, mip);
                const UINT mipHeight = GetMipDimension(height, mip);
                requiredBytes += static_cast<UINT64>(mipWidth) * mipHeight * c_bytesPerRgba16FloatPixel;
            }
        }

        if (bytes.size() < static_cast<std::size_t>(c_ddsHeaderSize + requiredBytes))
            return false;

        cubemap.width = width;
        cubemap.height = height;
        cubemap.mipCount = mipCount;
        cubemap.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cubemap.bytesPerPixel = c_bytesPerRgba16FloatPixel;
        cubemap.texels.assign(
            bytes.begin() + c_ddsHeaderSize,
            bytes.begin() + c_ddsHeaderSize + static_cast<std::ptrdiff_t>(requiredBytes));
        return true;
    }

    struct ScopedFileHandle
    {
        explicit ScopedFileHandle(HANDLE handle) : value(handle) {}
        ~ScopedFileHandle()
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
        }

        bool IsValid() const { return value != INVALID_HANDLE_VALUE; }

        HANDLE value = INVALID_HANDLE_VALUE;
    };
}

RayTracingManager::~RayTracingManager()
{
    if (m_buildFenceEvent)
    {
        CloseHandle(m_buildFenceEvent);
        m_buildFenceEvent = nullptr;
    }
}

bool RayTracingManager::Initialize(HWND hWnd, ID3D12Device5* device, UINT width, UINT height)
{
    m_hWnd = hWnd;
    m_device = device;
    m_width = width > 0 ? width : 1;
    m_height = height > 0 ? height : 1;

    if (!m_device)
    {
        ReportMessage(L"D3D12 raytracing device is null.");
        return false;
    }

    if (!CreateOutputTexture())
        return false;

    if (!CreateStatisticsResources())
        return false;

    if (!CreateBuildCommandObjects())
        return false;

    if (!CreateEnvironmentMap())
        return false;

    if (!CreateGlobalRootSignature())
        return false;

    if (!CreateAtrousPipeline())
        return false;

    if (!CreateRaytracingPipelineState())
        return false;

    if (!CreateShaderTables())
        return false;

    if (!CreateAccelerationStructures())
        return false;

    return true;
}

void RayTracingManager::DispatchRays(ID3D12GraphicsCommandList4* commandList)
{
    if (!commandList || !m_stateObject || !m_rayGenShaderTable || !m_missShaderTable ||
        !m_hitGroupShaderTable || !m_descriptorHeap || !m_topLevelAS || !m_accumulationTexture ||
        !m_environmentMap || !m_environmentDistributionBuffer ||
        !m_sceneMaterialBuffer || !m_primitiveMaterialIndexBuffer ||
        !m_instanceMetadataBuffer || !m_emissiveTriangleBuffer ||
        !m_diffuseIndirectAccumulationTexture ||
        !m_specularIndirectAccumulationTexture ||
        !m_diffuseLuminanceMomentsTexture ||
        !m_specularLuminanceMomentsTexture ||
        !m_statisticsBuffer ||
        !m_statisticsResetBuffer || !m_statisticsReadbackBuffer)
    {
        return;
    }

    if (!UpdateTopLevelAccelerationStructure(commandList))
        return;

    if (m_enableStatistics)
    {
        D3D12_RESOURCE_BARRIER statisticsToCopy = {};
        statisticsToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        statisticsToCopy.Transition.pResource = m_statisticsBuffer.Get();
        statisticsToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        statisticsToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        statisticsToCopy.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &statisticsToCopy);
        commandList->CopyBufferRegion(
            m_statisticsBuffer.Get(),
            0,
            m_statisticsResetBuffer.Get(),
            0,
            sizeof(UINT) * c_statisticsCounterCount);
        std::swap(
            statisticsToCopy.Transition.StateBefore,
            statisticsToCopy.Transition.StateAfter);
        commandList->ResourceBarrier(1, &statisticsToCopy);
    }

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_descriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    commandList->SetComputeRootDescriptorTable(0, m_descriptorHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootShaderResourceView(1, m_topLevelAS->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(2, m_vertexBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(3, m_indexBuffer->GetGPUVirtualAddress());
    const bool shouldAccumulate = m_enableAccumulation && !m_showNormalColor && !(m_sceneType == c_scenePbrGgx && m_pbrDebugView != c_pbrDebugBeauty);
    RenderSettingsConstants renderSettings = {};
    renderSettings.showNormalColor = m_showNormalColor ? 1u : 0u;
    renderSettings.frameIndex = m_frameIndex++;
    renderSettings.maxBounce = m_maxBounce;
    renderSettings.sampleIndex = shouldAccumulate ? m_accumulatedSampleCount : 0u;
    renderSettings.enableAccumulation = shouldAccumulate ? 1u : 0u;
    renderSettings.sceneType = m_sceneType;
    renderSettings.pbrDebugView = m_pbrDebugView;
    renderSettings.enableIbl = m_enableIbl ? 1u : 0u;
    renderSettings.pbrMetallic = m_pbrMetallic;
    renderSettings.pbrRoughness = m_pbrRoughness;
    renderSettings.iblIntensity = m_iblIntensity;
    renderSettings.exposure = m_exposure;
    renderSettings.validationSeed = m_validationSeed;
    std::copy(
        m_cameraPosition.begin(),
        m_cameraPosition.end(),
        renderSettings.cameraPosition);
    std::copy(
        m_cameraTarget.begin(),
        m_cameraTarget.end(),
        renderSettings.cameraTarget);
    renderSettings.overridePbrMaterial = m_overridePbrMaterial ? 1u : 0u;
    renderSettings.enableStatistics = m_enableStatistics ? 1u : 0u;
    renderSettings.dynamicObjectMoved =
        m_dynamicObjectMovedThisFrame ? 1u : 0u;
    renderSettings.enableRussianRoulette =
        m_enableRussianRoulette ? 1u : 0u;
    renderSettings.lightingMode = m_lightingMode;
    renderSettings.emissiveTriangleCount = m_emissiveTriangleCount;
    renderSettings.environmentResolution = m_environmentResolution;
    renderSettings.environmentTexelCount = m_environmentTexelCount;
    renderSettings.areaLightPower = m_areaLightPower;
    renderSettings.environmentPower = m_environmentPower;
    renderSettings.enableAtrous = m_enableAtrous ? 1u : 0u;
    commandList->SetComputeRoot32BitConstants(4, 30, &renderSettings, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE environmentHandle = m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    environmentHandle.ptr += static_cast<SIZE_T>(c_environmentDescriptorIndex) * m_descriptorSize;
    commandList->SetComputeRootDescriptorTable(5, environmentHandle);
    commandList->SetComputeRootShaderResourceView(6, m_sceneMaterialBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(7, m_primitiveMaterialIndexBuffer->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE materialTextureHandle =
        m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    materialTextureHandle.ptr +=
        static_cast<SIZE_T>(c_materialTextureDescriptorIndex) * m_descriptorSize;
    commandList->SetComputeRootDescriptorTable(8, materialTextureHandle);
    commandList->SetComputeRootUnorderedAccessView(
        9,
        m_statisticsBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        10,
        m_instanceMetadataBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        11,
        m_emissiveTriangleBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        12,
        m_environmentDistributionBuffer->GetGPUVirtualAddress());
    commandList->SetPipelineState1(m_stateObject.Get());

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderRecordSize;
    dispatchDesc.MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes =
        m_missShaderRecordSize * 2u;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missShaderRecordSize;
    dispatchDesc.HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupShaderRecordSize;
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupShaderRecordSize;
    dispatchDesc.Width = m_width;
    dispatchDesc.Height = m_height;
    dispatchDesc.Depth = 1;

    commandList->DispatchRays(&dispatchDesc);

    if (m_enableStatistics)
    {
        D3D12_RESOURCE_BARRIER statisticsUavBarrier = {};
        statisticsUavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        statisticsUavBarrier.UAV.pResource = m_statisticsBuffer.Get();
        commandList->ResourceBarrier(1, &statisticsUavBarrier);

        D3D12_RESOURCE_BARRIER statisticsToCopy = {};
        statisticsToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        statisticsToCopy.Transition.pResource = m_statisticsBuffer.Get();
        statisticsToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        statisticsToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        statisticsToCopy.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &statisticsToCopy);
        commandList->CopyBufferRegion(
            m_statisticsReadbackBuffer.Get(),
            0,
            m_statisticsBuffer.Get(),
            0,
            sizeof(UINT) * c_statisticsCounterCount);
        std::swap(
            statisticsToCopy.Transition.StateBefore,
            statisticsToCopy.Transition.StateAfter);
        commandList->ResourceBarrier(1, &statisticsToCopy);
    }

    if (m_enableAtrous && shouldAccumulate)
        DispatchAtrousFilter(commandList);

    if (shouldAccumulate)
    {
        ++m_accumulatedSampleCount;
    }
}

void RayTracingManager::DispatchAtrousFilter(
    ID3D12GraphicsCommandList4* commandList)
{
    if (!commandList ||
        !m_atrousRootSignature ||
        !m_atrousPipelineState ||
        !m_accumulationTexture ||
        !m_normalDepthTexture ||
        !m_materialGuideTexture ||
        !m_diffuseIndirectAccumulationTexture ||
        !m_specularIndirectAccumulationTexture ||
        !m_diffuseLuminanceMomentsTexture ||
        !m_specularLuminanceMomentsTexture ||
        !m_atrousFilterTextureA ||
        !m_atrousFilterTextureB ||
        !m_atrousFilteredDiffuseTexture ||
        !m_outputTexture)
    {
        return;
    }

    D3D12_RESOURCE_BARRIER outputUavBarrier = {};
    outputUavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    outputUavBarrier.UAV.pResource = m_outputTexture.Get();
    commandList->ResourceBarrier(1, &outputUavBarrier);

    D3D12_RESOURCE_BARRIER inputTransitions[7] = {};
    ID3D12Resource* inputResources[7] =
    {
        m_diffuseIndirectAccumulationTexture.Get(),
        m_specularIndirectAccumulationTexture.Get(),
        m_normalDepthTexture.Get(),
        m_materialGuideTexture.Get(),
        m_diffuseLuminanceMomentsTexture.Get(),
        m_specularLuminanceMomentsTexture.Get(),
        m_accumulationTexture.Get()
    };
    for (UINT index = 0; index < _countof(inputTransitions); ++index)
    {
        inputTransitions[index].Type =
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        inputTransitions[index].Transition.pResource =
            inputResources[index];
        inputTransitions[index].Transition.StateBefore =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        inputTransitions[index].Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        inputTransitions[index].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(
        _countof(inputTransitions),
        inputTransitions);

    commandList->SetComputeRootSignature(m_atrousRootSignature.Get());
    commandList->SetPipelineState(m_atrousPipelineState.Get());

    const auto gpuDescriptorHandle = [&](UINT descriptorIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr +=
            static_cast<UINT64>(descriptorIndex) * m_descriptorSize;
        return handle;
    };

    const auto dispatchChannel =
        [&](UINT filterChannel,
            UINT accumulationSrvIndex,
            UINT momentsSrvIndex,
            UINT finalUavIndex,
            ID3D12Resource* finalResource)
    {
        for (UINT passIndex = 0;
             passIndex < m_atrousIterationCount;
             ++passIndex)
        {
            const bool firstPass = passIndex == 0u;
            const bool finalPass =
                passIndex + 1u == m_atrousIterationCount;
            const bool sourceIsA =
                !firstPass && ((passIndex - 1u) % 2u == 0u);
            const bool destinationIsA = passIndex % 2u == 0u;

            const UINT sourceDescriptorIndex = firstPass
                ? accumulationSrvIndex
                : (sourceIsA
                    ? c_atrousFilterASrvIndex
                    : c_atrousFilterBSrvIndex);
            const UINT destinationDescriptorIndex = finalPass
                ? finalUavIndex
                : (destinationIsA
                    ? c_atrousFilterAUavIndex
                    : c_atrousFilterBUavIndex);
            ID3D12Resource* destinationResource = finalPass
                ? finalResource
                : (destinationIsA
                    ? m_atrousFilterTextureA.Get()
                    : m_atrousFilterTextureB.Get());

            AtrousSettingsConstants settings = {};
            settings.resolution[0] = m_width;
            settings.resolution[1] = m_height;
            settings.stepWidth = 1u << passIndex;
            settings.inputIsAccumulation = firstPass ? 1u : 0u;
            settings.finalPass = finalPass ? 1u : 0u;
            settings.normalExponent = 64.0f;
            settings.depthSigma = 0.02f;
            settings.colorSigma = m_atrousColorSigma;
            settings.exposure = m_exposure;
            settings.demodulateDiffuse =
                filterChannel == c_atrousFilterChannelDiffuse ? 1u : 0u;
            settings.filterChannel = filterChannel;

            commandList->SetComputeRootDescriptorTable(
                0,
                gpuDescriptorHandle(sourceDescriptorIndex));
            commandList->SetComputeRootDescriptorTable(
                1,
                gpuDescriptorHandle(c_atrousNormalDepthSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                2,
                gpuDescriptorHandle(c_atrousMaterialGuideSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                3,
                gpuDescriptorHandle(momentsSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                4,
                gpuDescriptorHandle(c_atrousDiffuseIndirectSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                5,
                gpuDescriptorHandle(c_atrousSpecularIndirectSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                6,
                gpuDescriptorHandle(c_atrousTotalSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                7,
                gpuDescriptorHandle(
                    filterChannel == c_atrousFilterChannelDiffuse
                    ? c_atrousDiffuseIndirectSrvIndex
                    : c_atrousFilteredDiffuseSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                8,
                gpuDescriptorHandle(destinationDescriptorIndex));
            commandList->SetComputeRoot32BitConstants(
                9,
                11,
                &settings,
                0);
            commandList->Dispatch(
                (m_width + 7u) / 8u,
                (m_height + 7u) / 8u,
                1u);

            D3D12_RESOURCE_BARRIER destinationUavBarrier = {};
            destinationUavBarrier.Type =
                D3D12_RESOURCE_BARRIER_TYPE_UAV;
            destinationUavBarrier.UAV.pResource = destinationResource;
            commandList->ResourceBarrier(1, &destinationUavBarrier);

            if (!finalPass)
            {
                D3D12_RESOURCE_BARRIER destinationToSrv = {};
                destinationToSrv.Type =
                    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                destinationToSrv.Transition.pResource =
                    destinationResource;
                destinationToSrv.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                destinationToSrv.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                destinationToSrv.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &destinationToSrv);
            }

            if (!firstPass)
            {
                ID3D12Resource* sourceResource = sourceIsA
                    ? m_atrousFilterTextureA.Get()
                    : m_atrousFilterTextureB.Get();
                D3D12_RESOURCE_BARRIER sourceToUav = {};
                sourceToUav.Type =
                    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                sourceToUav.Transition.pResource = sourceResource;
                sourceToUav.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                sourceToUav.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                sourceToUav.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &sourceToUav);
            }
        }
    };

    dispatchChannel(
        c_atrousFilterChannelDiffuse,
        c_atrousDiffuseIndirectSrvIndex,
        c_atrousDiffuseMomentsSrvIndex,
        c_atrousFilteredDiffuseUavIndex,
        m_atrousFilteredDiffuseTexture.Get());

    D3D12_RESOURCE_BARRIER filteredDiffuseToSrv = {};
    filteredDiffuseToSrv.Type =
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    filteredDiffuseToSrv.Transition.pResource =
        m_atrousFilteredDiffuseTexture.Get();
    filteredDiffuseToSrv.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    filteredDiffuseToSrv.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    filteredDiffuseToSrv.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &filteredDiffuseToSrv);

    dispatchChannel(
        c_atrousFilterChannelSpecular,
        c_atrousSpecularIndirectSrvIndex,
        c_atrousSpecularMomentsSrvIndex,
        c_atrousOutputUavIndex,
        m_outputTexture.Get());

    std::swap(
        filteredDiffuseToSrv.Transition.StateBefore,
        filteredDiffuseToSrv.Transition.StateAfter);
    commandList->ResourceBarrier(1, &filteredDiffuseToSrv);

    for (UINT index = 0; index < _countof(inputTransitions); ++index)
    {
        std::swap(
            inputTransitions[index].Transition.StateBefore,
            inputTransitions[index].Transition.StateAfter);
    }
    commandList->ResourceBarrier(
        _countof(inputTransitions),
        inputTransitions);
}

bool RayTracingManager::Resize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return true;

    m_width = width;
    m_height = height;
    if (m_autoFrameCamera)
        UpdateCameraFromSceneBounds();
    ResetAccumulation();
    return CreateOutputTexture();
}

void RayTracingManager::SetShowNormalColor(bool showNormalColor)
{
    if (m_showNormalColor == showNormalColor)
        return;

    m_showNormalColor = showNormalColor;
    ResetAccumulation();
}

void RayTracingManager::SetMaxBounce(UINT maxBounce)
{
    UINT clampedMaxBounce = maxBounce;
    if (clampedMaxBounce < 1)
    {
        clampedMaxBounce = 1;
    }
    else if (clampedMaxBounce > c_maxBounce)
    {
        clampedMaxBounce = c_maxBounce;
    }

    if (m_maxBounce == clampedMaxBounce)
        return;

    m_maxBounce = clampedMaxBounce;
    ResetAccumulation();
}

void RayTracingManager::SetRussianRouletteEnabled(bool enabled)
{
    if (m_enableRussianRoulette == enabled)
        return;

    m_enableRussianRoulette = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetAtrousEnabled(bool enabled)
{
    if (m_enableAtrous == enabled)
        return;

    m_enableAtrous = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetLightingMode(UINT lightingMode)
{
    const UINT clampedLightingMode =
        lightingMode <= c_lightingModeMis
        ? lightingMode
        : c_lightingModeBsdf;
    if (m_lightingMode == clampedLightingMode)
        return;

    m_lightingMode = clampedLightingMode;
    ResetAccumulation();
}

void RayTracingManager::SetEnableAccumulation(bool enableAccumulation)
{
    if (m_enableAccumulation == enableAccumulation)
        return;

    m_enableAccumulation = enableAccumulation;
    ResetAccumulation();
}

void RayTracingManager::SetPbrDebugView(UINT pbrDebugView)
{
    const UINT clampedPbrDebugView = pbrDebugView <= c_pbrDebugNormal
        ? pbrDebugView
        : c_pbrDebugBeauty;
    if (m_pbrDebugView == clampedPbrDebugView)
        return;

    m_pbrDebugView = clampedPbrDebugView;
    ResetAccumulation();
}

void RayTracingManager::SetPbrMaterial(float metallic, float roughness)
{
    const float clampedMetallic = metallic < 0.0f ? 0.0f : (metallic > 1.0f ? 1.0f : metallic);
    const float clampedRoughness = roughness < 0.03f ? 0.03f : (roughness > 1.0f ? 1.0f : roughness);
    if (m_pbrMetallic == clampedMetallic && m_pbrRoughness == clampedRoughness)
        return;

    m_pbrMetallic = clampedMetallic;
    m_pbrRoughness = clampedRoughness;
    ResetAccumulation();
}

void RayTracingManager::SetPbrMaterialOverride(bool enabled)
{
    if (m_overridePbrMaterial == enabled)
        return;

    m_overridePbrMaterial = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetIblSettings(bool enableIbl, float intensity)
{
    const float clampedIntensity = intensity < 0.0f ? 0.0f : (intensity > 8.0f ? 8.0f : intensity);
    if (m_enableIbl == enableIbl && m_iblIntensity == clampedIntensity)
        return;

    m_enableIbl = enableIbl;
    m_iblIntensity = clampedIntensity;
    ResetAccumulation();
}

bool RayTracingManager::SetCamera(
    const std::array<float, 3>& position,
    const std::array<float, 3>& target)
{
    float directionLengthSquared = 0.0f;
    bool changed = false;
    for (std::size_t component = 0; component < 3; ++component)
    {
        const float direction = target[component] - position[component];
        directionLengthSquared += direction * direction;
        changed |= std::abs(position[component] - m_cameraPosition[component]) >
            0.000001f;
        changed |= std::abs(target[component] - m_cameraTarget[component]) >
            0.000001f;
    }
    if (directionLengthSquared <= 0.00000001f || !changed)
        return false;

    m_cameraPosition = position;
    m_cameraTarget = target;
    m_autoFrameCamera = false;
    ResetAccumulation();
    return true;
}

void RayTracingManager::SetDynamicSphereAnimationEnabled(bool enabled)
{
    if (m_dynamicSphereAnimationEnabled == enabled)
        return;
    m_dynamicSphereAnimationEnabled = enabled;
}

void RayTracingManager::SetDynamicSphereVisible(bool visible)
{
    if (m_dynamicSphereVisible == visible)
        return;

    m_dynamicSphereVisible = visible;
    m_dynamicSphereVisibilityDirty = m_hasDynamicSphere;
    if (!visible)
    {
        m_dynamicObjectLinearSpeed = 0.0;
        m_dynamicObjectAngularSpeed = 0.0;
    }
    ResetAccumulation();
}

void RayTracingManager::SetDynamicSphereDeterministicTimeline(bool enabled)
{
    if (m_dynamicSphereDeterministicTimeline == enabled)
        return;
    m_dynamicSphereDeterministicTimeline = enabled;
}

void RayTracingManager::ResetDynamicSphereTimeline()
{
    m_dynamicSceneFrameIndex = 0;
    m_dynamicObjectLinearSpeed = 0.0;
    m_dynamicObjectAngularSpeed = 0.0;
}

float RayTracingManager::GetSceneDiagonal() const
{
    float diagonalSquared = 0.0f;
    for (std::size_t component = 0; component < 3; ++component)
    {
        const float extent =
            m_sceneBoundsMax[component] - m_sceneBoundsMin[component];
        diagonalSquared += extent * extent;
    }
    return diagonalSquared > 0.00000001f
        ? std::sqrt(diagonalSquared)
        : 1.0f;
}

void RayTracingManager::SetExposure(float exposure)
{
    const float clampedExposure = exposure < -10.0f ? -10.0f : (exposure > 10.0f ? 10.0f : exposure);
    if (m_exposure == clampedExposure)
        return;

    m_exposure = clampedExposure;
}
void RayTracingManager::SetSceneType(UINT sceneType)
{
    const UINT clampedSceneType = sceneType <= c_sceneIndirectBounceStress
        ? sceneType
        : c_sceneCornellBox;
    if (m_sceneType == clampedSceneType)
        return;

    m_sceneType = clampedSceneType;
    ResetAccumulation();
    if (m_device)
        CreateAccelerationStructures();
}

bool RayTracingManager::CreateOutputTexture()
{
    bool createdDescriptorHeap = false;
    if (!m_descriptorHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = c_descriptorCount;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));
        if (ReportFailure(hr, L"Raytracing descriptor heap creation failed."))
            return false;

        m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        createdDescriptorHeap = true;
    }

    m_outputTexture.Reset();
    m_accumulationTexture.Reset();
    m_normalDepthTexture.Reset();
    m_materialGuideTexture.Reset();
    m_diffuseIndirectAccumulationTexture.Reset();
    m_specularIndirectAccumulationTexture.Reset();
    m_diffuseLuminanceMomentsTexture.Reset();
    m_specularLuminanceMomentsTexture.Reset();
    m_atrousFilterTextureA.Reset();
    m_atrousFilterTextureB.Reset();
    m_atrousFilteredDiffuseTexture.Reset();

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Alignment = 0;
    textureDesc.Width = m_width;
    textureDesc.Height = m_height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = c_outputFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_outputTexture));
    if (ReportFailure(hr, L"Raytracing output texture creation failed."))
        return false;

    m_outputTexture->SetName(L"Raytracing output texture");

    D3D12_RESOURCE_DESC accumulationDesc = textureDesc;
    accumulationDesc.Format = c_accumulationFormat;

    hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &accumulationDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_accumulationTexture));
    if (ReportFailure(hr, L"Raytracing accumulation texture creation failed."))
        return false;

    m_accumulationTexture->SetName(L"Raytracing accumulation texture");

    const auto createFloatTexture =
        [&](const D3D12_RESOURCE_DESC& resourceDesc,
            Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
            const wchar_t* debugName,
            const wchar_t* failureMessage) -> bool
    {
        HRESULT createResult = m_device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&resource));
        if (ReportFailure(createResult, failureMessage))
            return false;
        resource->SetName(debugName);
        return true;
    };

    if (!createFloatTexture(
        accumulationDesc,
        m_normalDepthTexture,
        L"Raytracing primary normal and depth",
        L"Raytracing normal/depth texture creation failed."))
    {
        return false;
    }
    D3D12_RESOURCE_DESC materialGuideDesc = accumulationDesc;
    materialGuideDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!createFloatTexture(
        materialGuideDesc,
        m_materialGuideTexture,
        L"Raytracing primary material guide",
        L"Raytracing material guide texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_diffuseIndirectAccumulationTexture,
        L"Raytracing diffuse indirect accumulation texture",
        L"Raytracing diffuse indirect accumulation texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_specularIndirectAccumulationTexture,
        L"Raytracing specular indirect accumulation texture",
        L"Raytracing specular indirect accumulation texture creation failed."))
    {
        return false;
    }
    D3D12_RESOURCE_DESC momentsDesc = accumulationDesc;
    momentsDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    if (!createFloatTexture(
        momentsDesc,
        m_diffuseLuminanceMomentsTexture,
        L"Raytracing diffuse luminance moments",
        L"Raytracing diffuse moments texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        momentsDesc,
        m_specularLuminanceMomentsTexture,
        L"Raytracing specular luminance moments",
        L"Raytracing specular moments texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_atrousFilterTextureA,
        L"A-Trous filter ping texture",
        L"A-Trous filter ping texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_atrousFilterTextureB,
        L"A-Trous filter pong texture",
        L"A-Trous filter pong texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_atrousFilteredDiffuseTexture,
        L"A-Trous filtered diffuse texture",
        L"A-Trous filtered diffuse texture creation failed."))
    {
        return false;
    }

    const auto descriptorHandle = [&](UINT descriptorIndex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr +=
            static_cast<SIZE_T>(descriptorIndex) * m_descriptorSize;
        return handle;
    };

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc = {};
    outputUavDesc.Format = c_outputFormat;
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = descriptorHandle(0);
    m_device->CreateUnorderedAccessView(
        m_outputTexture.Get(),
        nullptr,
        &outputUavDesc,
        uavHandle);

    D3D12_UNORDERED_ACCESS_VIEW_DESC accumulationUavDesc = {};
    accumulationUavDesc.Format = c_accumulationFormat;
    accumulationUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    uavHandle.ptr += m_descriptorSize;
    m_device->CreateUnorderedAccessView(
        m_accumulationTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        uavHandle);

    m_device->CreateUnorderedAccessView(
        m_normalDepthTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC materialGuideUavDesc = {};
    materialGuideUavDesc.Format = materialGuideDesc.Format;
    materialGuideUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(
        m_materialGuideTexture.Get(),
        nullptr,
        &materialGuideUavDesc,
        descriptorHandle(3));
    m_device->CreateUnorderedAccessView(
        m_diffuseIndirectAccumulationTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(4));
    m_device->CreateUnorderedAccessView(
        m_specularIndirectAccumulationTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(5));
    D3D12_UNORDERED_ACCESS_VIEW_DESC momentsUavDesc = {};
    momentsUavDesc.Format = momentsDesc.Format;
    momentsUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(
        m_diffuseLuminanceMomentsTexture.Get(),
        nullptr,
        &momentsUavDesc,
        descriptorHandle(6));
    m_device->CreateUnorderedAccessView(
        m_specularLuminanceMomentsTexture.Get(),
        nullptr,
        &momentsUavDesc,
        descriptorHandle(7));

    D3D12_SHADER_RESOURCE_VIEW_DESC floatTextureSrvDesc = {};
    floatTextureSrvDesc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    floatTextureSrvDesc.Format = c_accumulationFormat;
    floatTextureSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    floatTextureSrvDesc.Texture2D.MostDetailedMip = 0;
    floatTextureSrvDesc.Texture2D.MipLevels = 1;
    floatTextureSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    m_device->CreateShaderResourceView(
        m_diffuseIndirectAccumulationTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousDiffuseIndirectSrvIndex));
    m_device->CreateShaderResourceView(
        m_specularIndirectAccumulationTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousSpecularIndirectSrvIndex));
    m_device->CreateShaderResourceView(
        m_normalDepthTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousNormalDepthSrvIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC materialGuideSrvDesc =
        floatTextureSrvDesc;
    materialGuideSrvDesc.Format = materialGuideDesc.Format;
    m_device->CreateShaderResourceView(
        m_materialGuideTexture.Get(),
        &materialGuideSrvDesc,
        descriptorHandle(c_atrousMaterialGuideSrvIndex));
    D3D12_SHADER_RESOURCE_VIEW_DESC momentsSrvDesc =
        floatTextureSrvDesc;
    momentsSrvDesc.Format = momentsDesc.Format;
    m_device->CreateShaderResourceView(
        m_diffuseLuminanceMomentsTexture.Get(),
        &momentsSrvDesc,
        descriptorHandle(c_atrousDiffuseMomentsSrvIndex));
    m_device->CreateShaderResourceView(
        m_specularLuminanceMomentsTexture.Get(),
        &momentsSrvDesc,
        descriptorHandle(c_atrousSpecularMomentsSrvIndex));
    m_device->CreateShaderResourceView(
        m_accumulationTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousTotalSrvIndex));
    m_device->CreateShaderResourceView(
        m_atrousFilterTextureA.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousFilterASrvIndex));
    m_device->CreateShaderResourceView(
        m_atrousFilterTextureB.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousFilterBSrvIndex));
    m_device->CreateShaderResourceView(
        m_atrousFilteredDiffuseTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousFilteredDiffuseSrvIndex));

    m_device->CreateUnorderedAccessView(
        m_atrousFilterTextureA.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(c_atrousFilterAUavIndex));
    m_device->CreateUnorderedAccessView(
        m_atrousFilterTextureB.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(c_atrousFilterBUavIndex));
    m_device->CreateUnorderedAccessView(
        m_atrousFilteredDiffuseTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(c_atrousFilteredDiffuseUavIndex));
    m_device->CreateUnorderedAccessView(
        m_outputTexture.Get(),
        nullptr,
        &outputUavDesc,
        descriptorHandle(c_atrousOutputUavIndex));

    if (m_environmentMap)
    {
        const D3D12_RESOURCE_DESC environmentDesc = m_environmentMap->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = environmentDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.MipLevels = environmentDesc.MipLevels;
        srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE environmentSrvHandle = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        environmentSrvHandle.ptr += static_cast<SIZE_T>(c_environmentDescriptorIndex) * m_descriptorSize;
        m_device->CreateShaderResourceView(m_environmentMap.Get(), &srvDesc, environmentSrvHandle);
    }

    if (createdDescriptorHeap)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
        nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrvDesc.Texture2D.MostDetailedMip = 0;
        nullSrvDesc.Texture2D.MipLevels = 1;
        nullSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr +=
            static_cast<SIZE_T>(c_materialTextureDescriptorIndex) * m_descriptorSize;
        for (UINT descriptorIndex = 0;
             descriptorIndex < c_materialTextureDescriptorCount;
             ++descriptorIndex)
        {
            m_device->CreateShaderResourceView(nullptr, &nullSrvDesc, handle);
            handle.ptr += m_descriptorSize;
        }
    }

    return true;
}

bool RayTracingManager::CreateStatisticsResources()
{
    const UINT64 statisticsSize =
        sizeof(UINT) * static_cast<UINT64>(c_statisticsCounterCount);

    const D3D12_HEAP_PROPERTIES defaultHeap =
        CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC statisticsDesc = CreateBufferDesc(
        statisticsSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &statisticsDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_statisticsBuffer));
    if (ReportFailure(hr, L"Statistics UAV creation failed."))
        return false;
    m_statisticsBuffer->SetName(L"Path tracing frame statistics");

    const D3D12_RESOURCE_DESC stagingDesc = CreateBufferDesc(statisticsSize);
    const D3D12_HEAP_PROPERTIES uploadHeap =
        CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    hr = m_device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &stagingDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_statisticsResetBuffer));
    if (ReportFailure(hr, L"Statistics reset buffer creation failed."))
        return false;
    m_statisticsResetBuffer->SetName(L"Path tracing statistics zero buffer");

    void* resetData = nullptr;
    D3D12_RANGE noReadRange = { 0, 0 };
    hr = m_statisticsResetBuffer->Map(0, &noReadRange, &resetData);
    if (ReportFailure(hr, L"Statistics reset buffer mapping failed."))
        return false;
    std::memset(resetData, 0, static_cast<std::size_t>(statisticsSize));
    D3D12_RANGE resetWriteRange = { 0, static_cast<SIZE_T>(statisticsSize) };
    m_statisticsResetBuffer->Unmap(0, &resetWriteRange);

    const D3D12_HEAP_PROPERTIES readbackHeap =
        CreateHeapProperties(D3D12_HEAP_TYPE_READBACK);
    hr = m_device->CreateCommittedResource(
        &readbackHeap,
        D3D12_HEAP_FLAG_NONE,
        &stagingDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_statisticsReadbackBuffer));
    if (ReportFailure(hr, L"Statistics readback buffer creation failed."))
        return false;
    m_statisticsReadbackBuffer->SetName(
        L"Path tracing frame statistics readback");
    return true;
}

void RayTracingManager::ReadFrameStatistics()
{
    if (!m_enableStatistics || !m_statisticsReadbackBuffer)
    {
        m_frameStatistics = {};
        return;
    }

    const SIZE_T statisticsSize =
        sizeof(UINT) * static_cast<SIZE_T>(c_statisticsCounterCount);
    D3D12_RANGE readRange = { 0, statisticsSize };
    UINT* counters = nullptr;
    const HRESULT hr = m_statisticsReadbackBuffer->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&counters));
    if (ReportFailure(hr, L"Statistics readback mapping failed."))
        return;

    for (UINT depth = 0; depth < c_statisticsRayDepthCount; ++depth)
        m_frameStatistics.raysByDepth[depth] = counters[depth];
    m_frameStatistics.shadowRays = counters[c_statisticsShadowRayIndex];
    m_frameStatistics.hitCount = counters[c_statisticsHitIndex];
    m_frameStatistics.missCount = counters[c_statisticsMissIndex];

    D3D12_RANGE noWriteRange = { 0, 0 };
    m_statisticsReadbackBuffer->Unmap(0, &noWriteRange);
}


bool RayTracingManager::CreateEnvironmentMap()
{
    std::vector<std::uint8_t> ddsBytes;
    if (!ReadBinaryFile(GetEnvironmentMapPath(), ddsBytes))
    {
        std::wstring message = L"Environment DDS was not found.\nExpected: ";
        message += GetEnvironmentMapPath();
        ReportMessage(message);
        return false;
    }

    DdsCubemapData cubemap;
    if (!ParseLegacyRgba16FloatCubemapDds(ddsBytes, cubemap))
    {
        ReportMessage(L"Environment DDS must be a legacy A16B16G16R16F cubemap generated by iblbaker.");
        return false;
    }

    std::vector<GpuEnvironmentAliasEntry> environmentDistribution;
    if (!BuildEnvironmentAliasTable(
        cubemap,
        environmentDistribution,
        m_environmentPower))
    {
        ReportMessage(
            L"Environment importance distribution creation failed.");
        return false;
    }
    m_environmentResolution = cubemap.width;
    m_environmentTexelCount =
        static_cast<UINT>(environmentDistribution.size());
    if (!CreateUploadBuffer(
        environmentDistribution.data(),
        sizeof(GpuEnvironmentAliasEntry) *
            environmentDistribution.size(),
        L"Environment importance alias table",
        m_environmentDistributionBuffer))
    {
        return false;
    }

    const UINT subresourceCount = c_cubeFaceCount * cubemap.mipCount;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Alignment = 0;
    textureDesc.Width = cubemap.width;
    textureDesc.Height = cubemap.height;
    textureDesc.DepthOrArraySize = static_cast<UINT16>(c_cubeFaceCount);
    textureDesc.MipLevels = static_cast<UINT16>(cubemap.mipCount);
    textureDesc.Format = cubemap.format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_HEAP_PROPERTIES defaultHeapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_environmentMap));
    if (ReportFailure(hr, L"Environment cubemap creation failed."))
        return false;

    m_environmentMap->SetName(L"IBL environment cubemap");

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
    std::vector<UINT> rowCounts(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);
    UINT64 uploadBufferSize = 0;
    m_device->GetCopyableFootprints(
        &textureDesc,
        0,
        subresourceCount,
        0,
        footprints.data(),
        rowCounts.data(),
        rowSizes.data(),
        &uploadBufferSize);

    const D3D12_HEAP_PROPERTIES uploadHeapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC uploadBufferDesc = CreateBufferDesc(uploadBufferSize);
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    hr = m_device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));
    if (ReportFailure(hr, L"Environment cubemap upload buffer creation failed."))
        return false;

    uploadBuffer->SetName(L"IBL environment cubemap upload buffer");

    std::uint8_t* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData));
    if (ReportFailure(hr, L"Environment cubemap upload buffer mapping failed."))
        return false;

    std::memset(mappedData, 0, static_cast<std::size_t>(uploadBufferSize));
    std::size_t sourceOffset = 0;
    for (UINT face = 0; face < c_cubeFaceCount; ++face)
    {
        for (UINT mip = 0; mip < cubemap.mipCount; ++mip)
        {
            const UINT subresourceIndex = face * cubemap.mipCount + mip;
            const UINT mipWidth = GetMipDimension(cubemap.width, mip);
            const UINT mipHeight = GetMipDimension(cubemap.height, mip);
            const std::size_t sourceRowPitch = static_cast<std::size_t>(mipWidth) * cubemap.bytesPerPixel;
            const std::uint8_t* source = cubemap.texels.data() + sourceOffset;
            std::uint8_t* destination = mappedData + footprints[subresourceIndex].Offset;

            for (UINT row = 0; row < mipHeight; ++row)
            {
                std::memcpy(
                    destination + static_cast<std::size_t>(row) * footprints[subresourceIndex].Footprint.RowPitch,
                    source + static_cast<std::size_t>(row) * sourceRowPitch,
                    sourceRowPitch);
            }

            sourceOffset += sourceRowPitch * mipHeight;
        }
    }

    D3D12_RANGE writeRange = { 0, static_cast<SIZE_T>(uploadBufferSize) };
    uploadBuffer->Unmap(0, &writeRange);

    hr = m_buildCommandAllocator->Reset();
    if (ReportFailure(hr, L"Environment upload command allocator reset failed."))
        return false;

    hr = m_buildCommandList->Reset(m_buildCommandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"Environment upload command list reset failed."))
        return false;

    for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
    {
        D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
        sourceLocation.pResource = uploadBuffer.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprints[subresourceIndex];

        D3D12_TEXTURE_COPY_LOCATION destinationLocation = {};
        destinationLocation.pResource = m_environmentMap.Get();
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = subresourceIndex;

        m_buildCommandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_environmentMap.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_buildCommandList->ResourceBarrier(1, &barrier);

    if (!ExecuteBuildCommandListAndWait())
        return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = cubemap.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = cubemap.mipCount;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(c_environmentDescriptorIndex) * m_descriptorSize;
    m_device->CreateShaderResourceView(m_environmentMap.Get(), &srvDesc, srvHandle);
    return true;
}

bool RayTracingManager::CreateGlobalRootSignature()
{
    D3D12_DESCRIPTOR_RANGE outputRanges[2] = {};
    outputRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRanges[0].NumDescriptors = 2;
    outputRanges[0].BaseShaderRegister = 0;
    outputRanges[0].RegisterSpace = 0;
    outputRanges[0].OffsetInDescriptorsFromTableStart = 0;
    outputRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRanges[1].NumDescriptors = 6;
    outputRanges[1].BaseShaderRegister = 3;
    outputRanges[1].RegisterSpace = 0;
    outputRanges[1].OffsetInDescriptorsFromTableStart = 2;

    D3D12_DESCRIPTOR_RANGE environmentRange = {};
    environmentRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    environmentRange.NumDescriptors = 1;
    environmentRange.BaseShaderRegister = 3;
    environmentRange.RegisterSpace = 0;
    environmentRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE materialTextureRange = {};
    materialTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialTextureRange.NumDescriptors = c_materialTextureDescriptorCount;
    materialTextureRange.BaseShaderRegister = 6;
    materialTextureRange.RegisterSpace = 0;
    materialTextureRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[13] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges =
        _countof(outputRanges);
    rootParameters[0].DescriptorTable.pDescriptorRanges = outputRanges;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[1].Descriptor.ShaderRegister = 0;
    rootParameters[1].Descriptor.RegisterSpace = 0;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[2].Descriptor.ShaderRegister = 1;
    rootParameters[2].Descriptor.RegisterSpace = 0;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[3].Descriptor.ShaderRegister = 2;
    rootParameters[3].Descriptor.RegisterSpace = 0;
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[4].Constants.ShaderRegister = 0;
    rootParameters[4].Constants.RegisterSpace = 0;
    rootParameters[4].Constants.Num32BitValues = 30;
    rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[5].DescriptorTable.pDescriptorRanges = &environmentRange;
    rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[6].Descriptor.ShaderRegister = 4;
    rootParameters[6].Descriptor.RegisterSpace = 0;
    rootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[7].Descriptor.ShaderRegister = 5;
    rootParameters[7].Descriptor.RegisterSpace = 0;
    rootParameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[8].DescriptorTable.pDescriptorRanges = &materialTextureRange;
    rootParameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParameters[9].Descriptor.ShaderRegister = 2;
    rootParameters[9].Descriptor.RegisterSpace = 0;
    rootParameters[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[10].Descriptor.ShaderRegister = 262;
    rootParameters[10].Descriptor.RegisterSpace = 0;
    rootParameters[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[11].Descriptor.ShaderRegister = 263;
    rootParameters[11].Descriptor.RegisterSpace = 0;
    rootParameters[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[12].Descriptor.ShaderRegister = 264;
    rootParameters[12].Descriptor.RegisterSpace = 0;
    rootParameters[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].MipLODBias = 0.0f;
    staticSamplers[0].MaxAnisotropy = 1;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    staticSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSamplers[0].MinLOD = 0.0f;
    staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].RegisterSpace = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    staticSamplers[1] = staticSamplers[0];
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = _countof(staticSamplers);
    rootSignatureDesc.pStaticSamplers = staticSamplers;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &error);
    if (ReportFailure(hr, L"Raytracing root signature serialization failed."))
        return false;

    hr = m_device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_globalRootSignature));
    if (ReportFailure(hr, L"Raytracing global root signature creation failed."))
        return false;

    m_globalRootSignature->SetName(L"Raytracing global root signature");
    return true;
}

bool RayTracingManager::CreateAtrousPipeline()
{
    D3D12_DESCRIPTOR_RANGE descriptorRanges[9] = {};
    for (UINT rangeIndex = 0; rangeIndex < 8; ++rangeIndex)
    {
        descriptorRanges[rangeIndex].RangeType =
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        descriptorRanges[rangeIndex].NumDescriptors = 1;
        descriptorRanges[rangeIndex].BaseShaderRegister = rangeIndex;
        descriptorRanges[rangeIndex].OffsetInDescriptorsFromTableStart = 0;
    }
    descriptorRanges[8].RangeType =
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptorRanges[8].NumDescriptors = 1;
    descriptorRanges[8].BaseShaderRegister = 0;
    descriptorRanges[8].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParameters[10] = {};
    for (UINT parameterIndex = 0; parameterIndex < 9; ++parameterIndex)
    {
        rootParameters[parameterIndex].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[parameterIndex].DescriptorTable.NumDescriptorRanges =
            1;
        rootParameters[parameterIndex].DescriptorTable.pDescriptorRanges =
            &descriptorRanges[parameterIndex];
        rootParameters[parameterIndex].ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;
    }
    rootParameters[9].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[9].Constants.ShaderRegister = 0;
    rootParameters[9].Constants.Num32BitValues = 11;
    rootParameters[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &error);
    if (ReportFailure(
        hr,
        L"A-Trous root signature serialization failed."))
    {
        return false;
    }

    hr = m_device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_atrousRootSignature));
    if (ReportFailure(hr, L"A-Trous root signature creation failed."))
        return false;
    m_atrousRootSignature->SetName(L"A-Trous root signature");

    std::vector<std::uint8_t> shaderBytes;
    if (!LoadCompiledAtrousShader(shaderBytes))
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
    pipelineDesc.pRootSignature = m_atrousRootSignature.Get();
    pipelineDesc.CS.pShaderBytecode = shaderBytes.data();
    pipelineDesc.CS.BytecodeLength = shaderBytes.size();
    hr = m_device->CreateComputePipelineState(
        &pipelineDesc,
        IID_PPV_ARGS(&m_atrousPipelineState));
    if (ReportFailure(hr, L"A-Trous pipeline state creation failed."))
        return false;
    m_atrousPipelineState->SetName(L"A-Trous pipeline state");
    return true;
}

bool RayTracingManager::CreateRaytracingPipelineState()
{
    std::vector<std::uint8_t> shaderBytes;
    if (!LoadCompiledShader(shaderBytes))
        return false;

    D3D12_EXPORT_DESC shaderExports[5] = {};
    shaderExports[0].Name = c_rayGenShaderName;
    shaderExports[0].ExportToRename = nullptr;
    shaderExports[0].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[1].Name = c_closestHitShaderName;
    shaderExports[1].ExportToRename = nullptr;
    shaderExports[1].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[2].Name = c_missShaderName;
    shaderExports[2].ExportToRename = nullptr;
    shaderExports[2].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[3].Name = c_shadowMissShaderName;
    shaderExports[3].ExportToRename = nullptr;
    shaderExports[3].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[4].Name = c_anyHitShaderName;
    shaderExports[4].ExportToRename = nullptr;
    shaderExports[4].Flags = D3D12_EXPORT_FLAG_NONE;

    D3D12_DXIL_LIBRARY_DESC dxilLibraryDesc = {};
    dxilLibraryDesc.DXILLibrary.pShaderBytecode = shaderBytes.data();
    dxilLibraryDesc.DXILLibrary.BytecodeLength = shaderBytes.size();
    dxilLibraryDesc.NumExports = _countof(shaderExports);
    dxilLibraryDesc.pExports = shaderExports;

    D3D12_HIT_GROUP_DESC hitGroupDesc = {};
    hitGroupDesc.HitGroupExport = c_hitGroupName;
    hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroupDesc.ClosestHitShaderImport = c_closestHitShaderName;
    hitGroupDesc.AnyHitShaderImport = c_anyHitShaderName;

    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes = c_shaderPayloadSize;
    shaderConfig.MaxAttributeSizeInBytes = c_shaderAttributeSize;

    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSignature = {};
    globalRootSignature.pGlobalRootSignature = m_globalRootSignature.Get();

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = c_maxRecursionDepth;

    D3D12_STATE_SUBOBJECT subobjects[5] = {};
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0].pDesc = &dxilLibraryDesc;
    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[1].pDesc = &hitGroupDesc;
    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[2].pDesc = &shaderConfig;
    subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[3].pDesc = &globalRootSignature;
    subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[4].pDesc = &pipelineConfig;

    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = _countof(subobjects);
    stateObjectDesc.pSubobjects = subobjects;

    HRESULT hr = m_device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_stateObject));
    if (ReportFailure(hr, L"Raytracing pipeline state object creation failed."))
        return false;

    m_stateObject->SetName(L"Raytracing pipeline state object");
    return true;
}

bool RayTracingManager::CreateShaderTables()
{
    return CreateShaderTable(
        c_rayGenShaderName,
        m_rayGenShaderTable.ReleaseAndGetAddressOf(),
        &m_rayGenShaderRecordSize,
        L"RayGen shader table") &&
        CreateMissShaderTable() &&
        CreateShaderTable(
            c_hitGroupName,
            m_hitGroupShaderTable.ReleaseAndGetAddressOf(),
            &m_hitGroupShaderRecordSize,
            L"HitGroup shader table");
}

bool RayTracingManager::CreateShaderTable(
    const wchar_t* shaderExportName,
    ID3D12Resource** shaderTable,
    UINT* shaderRecordSize,
    const wchar_t* debugName)
{
    Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
    HRESULT hr = m_stateObject.As(&stateObjectProperties);
    if (ReportFailure(hr, L"Raytracing state object properties query failed."))
        return false;

    void* shaderIdentifier = stateObjectProperties->GetShaderIdentifier(shaderExportName);
    if (!shaderIdentifier)
    {
        std::wstring message = L"Shader identifier was not found: ";
        message += shaderExportName;
        ReportMessage(message);
        return false;
    }

    *shaderRecordSize = AlignUp(
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    const UINT shaderTableSize = AlignUp(
        *shaderRecordSize,
        D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC bufferDesc = CreateBufferDesc(shaderTableSize);

    hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(shaderTable));
    if (ReportFailure(hr, L"Shader table creation failed."))
        return false;

    (*shaderTable)->SetName(debugName);

    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = (*shaderTable)->Map(0, &readRange, &mappedData);
    if (ReportFailure(hr, L"Shader table mapping failed."))
        return false;

    std::memset(mappedData, 0, shaderTableSize);
    std::memcpy(mappedData, shaderIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    (*shaderTable)->Unmap(0, nullptr);

    return true;
}

bool RayTracingManager::CreateAccelerationStructures()
{
    if (!m_buildCommandQueue || !m_buildCommandAllocator || !m_buildCommandList || !m_buildFence)
    {
        if (!CreateBuildCommandObjects())
            return false;
    }

    if (!CreateStaticGeometryBuffers())
        return false;

    HRESULT hr = m_buildCommandAllocator->Reset();
    if (ReportFailure(hr, L"AS build command allocator reset failed."))
        return false;

    hr = m_buildCommandList->Reset(m_buildCommandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"AS build command list reset failed."))
        return false;

    if (!BuildBottomLevelAccelerationStructure())
        return false;

    D3D12_RESOURCE_BARRIER blasBarriers[2] = {};
    blasBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    blasBarriers[0].UAV.pResource = m_bottomLevelAS.Get();
    UINT blasBarrierCount = 1;
    if (m_hasDynamicSphere)
    {
        blasBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        blasBarriers[1].UAV.pResource =
            m_dynamicSphereBottomLevelAS.Get();
        blasBarrierCount = 2;
    }
    m_buildCommandList->ResourceBarrier(
        blasBarrierCount,
        blasBarriers);

    if (!BuildTopLevelAccelerationStructure())
        return false;

    D3D12_RESOURCE_BARRIER tlasBarrier = {};
    tlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    tlasBarrier.UAV.pResource = m_topLevelAS.Get();
    m_buildCommandList->ResourceBarrier(1, &tlasBarrier);

    const bool buildSucceeded = ExecuteBuildCommandListAndWait();
    m_blasScratchBuffer.Reset();
    m_dynamicSphereBlasScratchBuffer.Reset();
    if (!m_hasDynamicSphere)
        m_tlasScratchBuffer.Reset();
    return buildSucceeded;
}

bool RayTracingManager::CreateMissShaderTable()
{
    Microsoft::WRL::ComPtr<ID3D12StateObjectProperties>
        stateObjectProperties;
    HRESULT hr = m_stateObject.As(&stateObjectProperties);
    if (ReportFailure(
        hr,
        L"Raytracing state object properties query failed."))
    {
        return false;
    }

    const wchar_t* shaderNames[] =
    {
        c_missShaderName,
        c_shadowMissShaderName
    };
    m_missShaderRecordSize = AlignUp(
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    const UINT tableSize = AlignUp(
        m_missShaderRecordSize * _countof(shaderNames),
        D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    const D3D12_HEAP_PROPERTIES heapProperties =
        CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC bufferDesc =
        CreateBufferDesc(tableSize);
    hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_missShaderTable));
    if (ReportFailure(hr, L"Miss shader table creation failed."))
        return false;

    m_missShaderTable->SetName(L"Miss shader table");
    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_missShaderTable->Map(0, &readRange, &mappedData);
    if (ReportFailure(hr, L"Miss shader table mapping failed."))
        return false;

    std::memset(mappedData, 0, tableSize);
    for (UINT shaderIndex = 0;
         shaderIndex < _countof(shaderNames);
         ++shaderIndex)
    {
        void* shaderIdentifier =
            stateObjectProperties->GetShaderIdentifier(
                shaderNames[shaderIndex]);
        if (!shaderIdentifier)
        {
            m_missShaderTable->Unmap(0, nullptr);
            ReportMessage(L"Miss shader identifier was not found.");
            return false;
        }
        std::memcpy(
            static_cast<std::uint8_t*>(mappedData) +
                shaderIndex * m_missShaderRecordSize,
            shaderIdentifier,
            D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    m_missShaderTable->Unmap(0, nullptr);
    return true;
}

bool RayTracingManager::CreateBuildCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_buildCommandQueue));
    if (ReportFailure(hr, L"AS build command queue creation failed."))
        return false;

    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_buildCommandAllocator));
    if (ReportFailure(hr, L"AS build command allocator creation failed."))
        return false;

    hr = m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_buildCommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_buildCommandList));
    if (ReportFailure(hr, L"AS build command list creation failed."))
        return false;

    hr = m_buildCommandList->Close();
    if (ReportFailure(hr, L"Initial AS build command list close failed."))
        return false;

    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_buildFence));
    if (ReportFailure(hr, L"AS build fence creation failed."))
        return false;

    m_buildFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_buildFenceEvent)
    {
        ReportMessage(L"AS build fence event creation failed.");
        return false;
    }

    return true;
}

bool RayTracingManager::CreateStaticGeometryBuffers()
{
    SceneData scene;
    SceneBounds modelBounds = {};
    GltfLoadReport loadReport;
    bool hasModelBounds = false;
    bool hasLoadReport = false;
    std::size_t areaLightCount = 0;
    m_hasDynamicSphere = false;
    m_dynamicSceneFrameIndex = 0;
    m_staticGeometry = {};
    m_dynamicSphereGeometry = {};
    const bool isPbrScene = m_sceneType == c_scenePbrGgx ||
        m_sceneType == c_scenePbrGpuValidation;
    if (isPbrScene && !m_sceneFilePath.empty())
    {
        std::wstring errorMessage;
        GltfLoadOptions loadOptions;
        loadOptions.skipNonOpaquePrimitives = m_sponzaLite;
        if (!LoadGltfSceneData(
            m_sceneFilePath,
            scene,
            errorMessage,
            loadOptions,
            &loadReport))
        {
            ReportMessage(
                L"glTF scene load failed.\nPath: " + m_sceneFilePath +
                L"\nReason: " + errorMessage);
            return false;
        }
        hasLoadReport = true;
        if (!scene.IsValid() || !ComputeSceneBounds(scene, modelBounds))
        {
            ReportMessage(L"Loaded glTF scene data or bounds are invalid.");
            return false;
        }
        hasModelBounds = true;
        if (m_sponzaLite)
        {
            std::vector<SceneAreaLight> lights;
            if (!LoadSponzaLightConfig(
                m_sponzaLightConfigPath,
                lights,
                errorMessage))
            {
                ReportMessage(
                    L"Sponza light config load failed.\nPath: " +
                    m_sponzaLightConfigPath +
                    L"\nReason: " + errorMessage);
                return false;
            }
            if (lights.size() != 12)
            {
                ReportMessage(
                    L"Sponza-lite requires exactly 12 area lights.");
                return false;
            }
            if (!AppendAreaLights(scene, lights))
            {
                ReportMessage(
                    L"Failed to append the Sponza area-light geometry.");
                return false;
            }
            areaLightCount = lights.size();
        }
        if (m_composeModelRoom && !AppendPbrModelRoom(scene, modelBounds))
        {
            ReportMessage(L"Failed to compose the PBR model room.");
            return false;
        }
    }
    else
    {
        if (isPbrScene)
            scene = CreatePbrGgxSceneData();
        else if (m_sceneType == c_sceneIndirectBounceStress)
            scene = CreateIndirectBounceStressSceneData();
        else
            scene = CreateCornellBoxSceneData();
    }

    m_staticGeometry.vertexCount =
        static_cast<UINT>(scene.vertices.size());
    m_staticGeometry.indexCount =
        static_cast<UINT>(scene.indices.size());
    for (std::uint32_t materialIndex : scene.primitiveMaterialIndices)
    {
        if (scene.materials[materialIndex].alphaCutoff >= 0.0f)
        {
            m_staticGeometry.containsAlphaMask = true;
            break;
        }
    }

    if (m_sponzaLite && hasModelBounds)
    {
        const float extentX =
            modelBounds.maximum[0] - modelBounds.minimum[0];
        const float extentY =
            modelBounds.maximum[1] - modelBounds.minimum[1];
        const float extentZ =
            modelBounds.maximum[2] - modelBounds.minimum[2];
        const float sceneDiagonal = std::sqrt(
            extentX * extentX +
            extentY * extentY +
            extentZ * extentZ);
        m_dynamicSphereRadius =
            (std::max)(sceneDiagonal * 0.015f, 0.20f);
        // The complete left-to-right travel range is 3% of the scene
        // diagonal, so this value is the half-range around the center.
        m_dynamicSphereMotionAmplitude = sceneDiagonal * 0.015f;
        m_dynamicSphereTrackCenterX =
            (modelBounds.minimum[0] + modelBounds.maximum[0]) * 0.5f;
        m_dynamicSphereCenterZ =
            (modelBounds.minimum[2] + modelBounds.maximum[2]) * 0.5f;
        const float maximumGroundHeight =
            modelBounds.minimum[1] + extentY * 0.20f;
        float groundHeight = modelBounds.minimum[1];
        bool foundGround = false;
        for (int sampleIndex = -2; sampleIndex <= 2; ++sampleIndex)
        {
            const float sampleX =
                m_dynamicSphereTrackCenterX +
                m_dynamicSphereMotionAmplitude *
                static_cast<float>(sampleIndex) * 0.5f;
            float sampleHeight = 0.0f;
            if (FindWalkableSurfaceHeight(
                scene,
                sampleX,
                m_dynamicSphereCenterZ,
                maximumGroundHeight,
                sampleHeight))
            {
                groundHeight = foundGround
                    ? (std::max)(groundHeight, sampleHeight)
                    : sampleHeight;
                foundGround = true;
            }
        }
        m_dynamicSphereCenterY =
            groundHeight + m_dynamicSphereRadius;
        m_dynamicSpherePositionX =
            m_dynamicSphereTrackCenterX -
            m_dynamicSphereMotionAmplitude;
        m_dynamicSphereRollRadians = 0.0f;

        SceneData sphere =
            CreateRollingMetalSphereSceneData(m_dynamicSphereRadius);
        if (!sphere.IsValid())
        {
            ReportMessage(L"Generated rolling metal sphere data is invalid.");
            return false;
        }

        m_dynamicSphereGeometry.vertexOffset =
            static_cast<UINT>(scene.vertices.size());
        m_dynamicSphereGeometry.vertexCount =
            static_cast<UINT>(sphere.vertices.size());
        m_dynamicSphereGeometry.indexOffset =
            static_cast<UINT>(scene.indices.size());
        m_dynamicSphereGeometry.indexCount =
            static_cast<UINT>(sphere.indices.size());
        m_dynamicSphereGeometry.primitiveOffset =
            static_cast<UINT>(scene.primitiveMaterialIndices.size());
        const std::uint32_t materialOffset =
            static_cast<std::uint32_t>(scene.materials.size());

        scene.vertices.insert(
            scene.vertices.end(),
            sphere.vertices.begin(),
            sphere.vertices.end());
        scene.indices.insert(
            scene.indices.end(),
            sphere.indices.begin(),
            sphere.indices.end());
        scene.materials.insert(
            scene.materials.end(),
            sphere.materials.begin(),
            sphere.materials.end());
        for (const std::uint32_t materialIndex :
             sphere.primitiveMaterialIndices)
        {
            scene.primitiveMaterialIndices.push_back(
                materialOffset + materialIndex);
        }
        m_hasDynamicSphere = true;
    }

    if (!scene.IsValid())
    {
        ReportMessage(L"Generated scene data is invalid.");
        return false;
    }

    m_autoFrameCamera = hasModelBounds;
    if (m_autoFrameCamera)
    {
        for (std::size_t component = 0; component < 3; ++component)
        {
            m_sceneBoundsMin[component] = modelBounds.minimum[component];
            m_sceneBoundsMax[component] = modelBounds.maximum[component];
        }
        UpdateCameraFromSceneBounds();
    }
    else if (m_sceneType == c_sceneIndirectBounceStress)
    {
        m_cameraPosition = { 0.0f, 0.10f, -4.25f };
        m_cameraTarget = { 1.45f, 0.05f, -0.70f };
    }
    else
    {
        m_cameraPosition = { 0.0f, 0.15f, -1.2f };
        m_cameraTarget = { 0.0f, 0.0f, 0.0f };
    }

    m_vertexCount = static_cast<UINT>(scene.vertices.size());
    m_indexCount = static_cast<UINT>(scene.indices.size());

    std::vector<SceneInstanceMetadata> instanceMetadata(1);
    if (m_hasDynamicSphere)
    {
        instanceMetadata.push_back(
            {
                m_dynamicSphereGeometry.vertexOffset,
                m_dynamicSphereGeometry.indexOffset,
                m_dynamicSphereGeometry.primitiveOffset,
                0u
            });
    }

    std::vector<GpuEmissiveTriangle> emissiveTriangles =
        BuildEmissiveTriangles(
            scene,
            m_staticGeometry.indexCount,
            m_areaLightPower);
    m_emissiveTriangleCount =
        static_cast<UINT>(emissiveTriangles.size());
    if (emissiveTriangles.empty())
        emissiveTriangles.push_back({});

    if (!CreateUploadBuffer(
        scene.vertices.data(),
        sizeof(SceneVertex) * scene.vertices.size(),
        L"Raytracing scene vertex buffer",
        m_vertexBuffer))
    {
        return false;
    }

    if (!CreateUploadBuffer(
        scene.indices.data(),
        sizeof(std::uint32_t) * scene.indices.size(),
        L"Raytracing scene index buffer",
        m_indexBuffer))
    {
        return false;
    }

    if (!CreateUploadBuffer(
        scene.materials.data(),
        sizeof(SceneMaterial) * scene.materials.size(),
        L"Raytracing scene material buffer",
        m_sceneMaterialBuffer))
    {
        return false;
    }

    if (!CreateUploadBuffer(
        scene.primitiveMaterialIndices.data(),
        sizeof(std::uint32_t) * scene.primitiveMaterialIndices.size(),
        L"Raytracing primitive material index buffer",
        m_primitiveMaterialIndexBuffer))
    {
        return false;
    }

    if (!CreateUploadBuffer(
        instanceMetadata.data(),
        sizeof(SceneInstanceMetadata) * instanceMetadata.size(),
        L"Raytracing instance metadata buffer",
        m_instanceMetadataBuffer))
    {
        return false;
    }

    if (!CreateUploadBuffer(
        emissiveTriangles.data(),
        sizeof(GpuEmissiveTriangle) * emissiveTriangles.size(),
        L"Raytracing emissive triangle buffer",
        m_emissiveTriangleBuffer))
    {
        return false;
    }

    if (m_sponzaLite && hasLoadReport)
    {
        const std::wstring manifestPath = m_sceneManifestPath.empty()
            ? L"BenchmarkOutput\\SponzaLite\\scene_manifest.json"
            : m_sceneManifestPath;
        SponzaSceneManifestSettings manifestSettings;
        manifestSettings.areaLightCount =
            static_cast<std::uint32_t>(areaLightCount);
        manifestSettings.dynamicMetalSphere = m_hasDynamicSphere;
        std::wstring errorMessage;
        if (!WriteSponzaSceneManifest(
            manifestPath,
            m_sceneFilePath,
            loadReport,
            manifestSettings,
            errorMessage))
        {
            ReportMessage(
                L"Sponza-lite manifest creation failed.\nPath: " +
                manifestPath + L"\nReason: " + errorMessage);
            return false;
        }
    }

    return CreateMaterialTextures(scene);
}

void RayTracingManager::UpdateCameraFromSceneBounds()
{
    const float aspectRatio = static_cast<float>(std::max(m_width, 1u)) /
        static_cast<float>(std::max(m_height, 1u));
    const float tanHalfVerticalFov = std::tan(c_verticalFovRadians * 0.5f);
    const float tanHalfHorizontalFov = tanHalfVerticalFov * aspectRatio;

    std::array<float, 3> halfExtent = {};
    for (std::size_t component = 0; component < 3; ++component)
    {
        m_cameraTarget[component] =
            (m_sceneBoundsMin[component] + m_sceneBoundsMax[component]) * 0.5f;
        halfExtent[component] =
            (m_sceneBoundsMax[component] - m_sceneBoundsMin[component]) * 0.5f;
    }

    const float verticalFitDistance =
        halfExtent[1] / std::max(tanHalfVerticalFov, 1.0e-4f);
    const float horizontalFitDistance =
        halfExtent[0] / std::max(tanHalfHorizontalFov, 1.0e-4f);
    const float maximumExtent = std::max(
        halfExtent[0],
        std::max(halfExtent[1], halfExtent[2]));
    const float fitDistance = std::max(
        std::max(verticalFitDistance, horizontalFitDistance),
        std::max(maximumExtent * 0.05f, 0.01f));
    const float cameraDistance =
        halfExtent[2] + fitDistance * c_cameraFrameMargin;

    m_cameraPosition =
    {
        m_cameraTarget[0],
        m_cameraTarget[1],
        m_cameraTarget[2] - cameraDistance
    };
}

bool RayTracingManager::CreateMaterialTextures(const SceneData& scene)
{
    if (scene.textures.size() > c_materialTextureDescriptorCount)
    {
        ReportMessage(L"The scene exceeds the 256 material texture limit.");
        return false;
    }

    m_materialTextures.clear();

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
    nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrvDesc.Texture2D.MostDetailedMip = 0;
    nullSrvDesc.Texture2D.MipLevels = 1;
    nullSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle =
        m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    descriptorHandle.ptr +=
        static_cast<SIZE_T>(c_materialTextureDescriptorIndex) * m_descriptorSize;
    for (UINT descriptorIndex = 0;
         descriptorIndex < c_materialTextureDescriptorCount;
         ++descriptorIndex)
    {
        m_device->CreateShaderResourceView(nullptr, &nullSrvDesc, descriptorHandle);
        descriptorHandle.ptr += m_descriptorSize;
    }

    if (scene.textures.empty())
        return true;

    HRESULT hr = m_buildCommandAllocator->Reset();
    if (ReportFailure(hr, L"Material texture command allocator reset failed."))
        return false;
    hr = m_buildCommandList->Reset(m_buildCommandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"Material texture command list reset failed."))
        return false;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadBuffers;
    uploadBuffers.reserve(scene.textures.size());
    m_materialTextures.reserve(scene.textures.size());

    for (std::size_t textureIndex = 0;
         textureIndex < scene.textures.size();
         ++textureIndex)
    {
        const SceneTexture& source = scene.textures[textureIndex];
        if (source.mips.empty() ||
            source.mips.size() > std::numeric_limits<UINT16>::max())
        {
            ReportMessage(L"A material texture has an invalid mip chain.");
            return false;
        }
        const UINT mipCount = static_cast<UINT>(source.mips.size());
        const SceneTextureMip& baseMip = source.mips.front();

        D3D12_RESOURCE_DESC textureDesc = {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Alignment = 0;
        textureDesc.Width = baseMip.width;
        textureDesc.Height = baseMip.height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = static_cast<UINT16>(mipCount);
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12Resource> gpuTexture;
        const D3D12_HEAP_PROPERTIES defaultHeap =
            CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        hr = m_device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&gpuTexture));
        if (ReportFailure(hr, L"Material texture creation failed."))
            return false;

        std::wstring textureName =
            L"Material texture " + std::to_wstring(textureIndex);
        gpuTexture->SetName(textureName.c_str());

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
        std::vector<UINT> rowCounts(mipCount);
        std::vector<UINT64> rowSizesInBytes(mipCount);
        UINT64 uploadSize = 0;
        m_device->GetCopyableFootprints(
            &textureDesc,
            0,
            mipCount,
            0,
            footprints.data(),
            rowCounts.data(),
            rowSizesInBytes.data(),
            &uploadSize);

        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
        const D3D12_HEAP_PROPERTIES uploadHeap =
            CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC uploadDesc = CreateBufferDesc(uploadSize);
        hr = m_device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&uploadBuffer));
        if (ReportFailure(hr, L"Material texture upload buffer creation failed."))
            return false;

        void* mappedData = nullptr;
        const D3D12_RANGE readRange = { 0, 0 };
        hr = uploadBuffer->Map(0, &readRange, &mappedData);
        if (ReportFailure(hr, L"Material texture upload buffer mapping failed."))
            return false;

        for (UINT mipIndex = 0; mipIndex < mipCount; ++mipIndex)
        {
            const SceneTextureMip& sourceMip = source.mips[mipIndex];
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint =
                footprints[mipIndex];
            const std::size_t sourceRowPitch =
                static_cast<std::size_t>(sourceMip.width) * 4u;
            std::uint8_t* destination =
                static_cast<std::uint8_t*>(mappedData) + footprint.Offset;
            for (UINT row = 0; row < sourceMip.height; ++row)
            {
                std::memcpy(
                    destination +
                        static_cast<std::size_t>(row) *
                        footprint.Footprint.RowPitch,
                    sourceMip.rgba8.data() +
                        static_cast<std::size_t>(row) * sourceRowPitch,
                    sourceRowPitch);
            }
        }
        uploadBuffer->Unmap(0, nullptr);

        for (UINT mipIndex = 0; mipIndex < mipCount; ++mipIndex)
        {
            D3D12_TEXTURE_COPY_LOCATION destinationLocation = {};
            destinationLocation.pResource = gpuTexture.Get();
            destinationLocation.Type =
                D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destinationLocation.SubresourceIndex = mipIndex;

            D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
            sourceLocation.pResource = uploadBuffer.Get();
            sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sourceLocation.PlacedFootprint = footprints[mipIndex];
            m_buildCommandList->CopyTextureRegion(
                &destinationLocation,
                0,
                0,
                0,
                &sourceLocation,
                nullptr);
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = gpuTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_buildCommandList->ResourceBarrier(1, &barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = source.isSrgb != 0
            ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            : DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = mipCount;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
            m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandle.ptr +=
            static_cast<SIZE_T>(c_materialTextureDescriptorIndex + textureIndex) *
            m_descriptorSize;
        m_device->CreateShaderResourceView(gpuTexture.Get(), &srvDesc, srvHandle);

        m_materialTextures.push_back(gpuTexture);
        uploadBuffers.push_back(uploadBuffer);
    }

    return ExecuteBuildCommandListAndWait();
}

bool RayTracingManager::BuildBottomLevelAccelerationStructure()
{
    if (!BuildBottomLevelAccelerationStructure(
        m_staticGeometry,
        L"Static scene bottom level acceleration structure",
        m_bottomLevelAS,
        m_blasScratchBuffer))
    {
        return false;
    }

    if (m_hasDynamicSphere &&
        !BuildBottomLevelAccelerationStructure(
            m_dynamicSphereGeometry,
            L"Rolling sphere bottom level acceleration structure",
            m_dynamicSphereBottomLevelAS,
            m_dynamicSphereBlasScratchBuffer))
    {
        return false;
    }
    return true;
}

bool RayTracingManager::BuildBottomLevelAccelerationStructure(
    const GeometryRange& geometry,
    const wchar_t* debugName,
    Microsoft::WRL::ComPtr<ID3D12Resource>& accelerationStructure,
    Microsoft::WRL::ComPtr<ID3D12Resource>& scratchBuffer)
{
    if (geometry.vertexCount == 0 || geometry.indexCount == 0)
    {
        ReportMessage(L"BLAS geometry range is empty.");
        return false;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
    geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometryDesc.Flags = geometry.containsAlphaMask
        ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
        : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometryDesc.Triangles.VertexBuffer.StartAddress =
        m_vertexBuffer->GetGPUVirtualAddress() +
        static_cast<UINT64>(geometry.vertexOffset) *
        sizeof(SceneVertex);
    geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(SceneVertex);
    geometryDesc.Triangles.VertexCount = geometry.vertexCount;
    geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometryDesc.Triangles.IndexBuffer =
        m_indexBuffer->GetGPUVirtualAddress() +
        static_cast<UINT64>(geometry.indexOffset) *
        sizeof(std::uint32_t);
    geometryDesc.Triangles.IndexCount = geometry.indexCount;
    geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geometryDesc.Triangles.Transform3x4 = 0;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &geometryDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
    if (prebuildInfo.ResultDataMaxSizeInBytes == 0)
    {
        ReportMessage(L"BLAS prebuild info returned an empty result size.");
        return false;
    }

    scratchBuffer.Reset();
    if (!CreateScratchBuffer(
        prebuildInfo.ScratchDataSizeInBytes,
        L"BLAS scratch buffer",
        scratchBuffer))
        return false;

    accelerationStructure.Reset();
    if (!CreateAccelerationStructureBuffer(
        prebuildInfo.ResultDataMaxSizeInBytes,
        debugName,
        accelerationStructure))
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData =
        scratchBuffer->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData =
        accelerationStructure->GetGPUVirtualAddress();

    m_buildCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    return true;
}

bool RayTracingManager::BuildTopLevelAccelerationStructure()
{
    const UINT instanceCount = m_hasDynamicSphere ? 2u : 1u;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(instanceCount);
    instanceDescs[0].Transform[0][0] = 1.0f;
    instanceDescs[0].Transform[1][1] = 1.0f;
    instanceDescs[0].Transform[2][2] = 1.0f;
    instanceDescs[0].InstanceID = 0;
    instanceDescs[0].InstanceMask = 0xFF;
    instanceDescs[0].AccelerationStructure =
        m_bottomLevelAS->GetGPUVirtualAddress();
    if (m_hasDynamicSphere)
    {
        const float cosine = std::cos(m_dynamicSphereRollRadians);
        const float sine = std::sin(m_dynamicSphereRollRadians);
        D3D12_RAYTRACING_INSTANCE_DESC& sphereDesc = instanceDescs[1];
        sphereDesc.Transform[0][0] = cosine;
        sphereDesc.Transform[0][1] = -sine;
        sphereDesc.Transform[0][3] = m_dynamicSpherePositionX;
        sphereDesc.Transform[1][0] = sine;
        sphereDesc.Transform[1][1] = cosine;
        sphereDesc.Transform[1][3] = m_dynamicSphereCenterY;
        sphereDesc.Transform[2][2] = 1.0f;
        sphereDesc.Transform[2][3] = m_dynamicSphereCenterZ;
        sphereDesc.InstanceID = 1;
        sphereDesc.InstanceMask =
            m_dynamicSphereVisible ? 0xFF : 0x00;
        sphereDesc.AccelerationStructure =
            m_dynamicSphereBottomLevelAS->GetGPUVirtualAddress();
    }

    for (UINT frameIndex = 0;
         frameIndex < c_tlasFrameCount;
         ++frameIndex)
    {
        if (!CreateUploadBuffer(
            instanceDescs.data(),
            sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instanceCount,
            L"TLAS instance descriptor",
            m_instanceDescBuffers[frameIndex]))
        {
            return false;
        }
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (m_hasDynamicSphere)
    {
        inputs.Flags |=
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = instanceCount;
    inputs.InstanceDescs =
        m_instanceDescBuffers[0]->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
    if (prebuildInfo.ResultDataMaxSizeInBytes == 0)
    {
        ReportMessage(L"TLAS prebuild info returned an empty result size.");
        return false;
    }

    m_tlasScratchBuffer.Reset();
    const UINT64 scratchSize = (std::max)(
        prebuildInfo.ScratchDataSizeInBytes,
        prebuildInfo.UpdateScratchDataSizeInBytes);
    if (!CreateScratchBuffer(
        scratchSize,
        L"TLAS scratch buffer",
        m_tlasScratchBuffer))
        return false;

    if (!CreateAccelerationStructureBuffer(prebuildInfo.ResultDataMaxSizeInBytes, L"Top level acceleration structure", m_topLevelAS))
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = m_tlasScratchBuffer->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData = m_topLevelAS->GetGPUVirtualAddress();

    m_buildCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    m_dynamicSphereVisibilityDirty = false;
    return true;
}

bool RayTracingManager::WriteInstanceDescriptors(
    UINT frameIndex,
    float spherePositionX,
    float sphereRollRadians)
{
    if (frameIndex >= c_tlasFrameCount ||
        !m_instanceDescBuffers[frameIndex])
    {
        return false;
    }

    D3D12_RAYTRACING_INSTANCE_DESC* instanceDescs = nullptr;
    const D3D12_RANGE readRange = { 0, 0 };
    HRESULT hr = m_instanceDescBuffers[frameIndex]->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&instanceDescs));
    if (ReportFailure(hr, L"TLAS instance descriptor mapping failed."))
        return false;

    std::memset(
        instanceDescs,
        0,
        sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * 2u);
    instanceDescs[0].Transform[0][0] = 1.0f;
    instanceDescs[0].Transform[1][1] = 1.0f;
    instanceDescs[0].Transform[2][2] = 1.0f;
    instanceDescs[0].InstanceID = 0;
    instanceDescs[0].InstanceMask = 0xFF;
    instanceDescs[0].AccelerationStructure =
        m_bottomLevelAS->GetGPUVirtualAddress();

    const float cosine = std::cos(sphereRollRadians);
    const float sine = std::sin(sphereRollRadians);
    D3D12_RAYTRACING_INSTANCE_DESC& sphereDesc = instanceDescs[1];
    sphereDesc.Transform[0][0] = cosine;
    sphereDesc.Transform[0][1] = -sine;
    sphereDesc.Transform[0][3] = spherePositionX;
    sphereDesc.Transform[1][0] = sine;
    sphereDesc.Transform[1][1] = cosine;
    sphereDesc.Transform[1][3] = m_dynamicSphereCenterY;
    sphereDesc.Transform[2][2] = 1.0f;
    sphereDesc.Transform[2][3] = m_dynamicSphereCenterZ;
    sphereDesc.InstanceID = 1;
    sphereDesc.InstanceMask =
        m_dynamicSphereVisible ? 0xFF : 0x00;
    sphereDesc.AccelerationStructure =
        m_dynamicSphereBottomLevelAS->GetGPUVirtualAddress();

    m_instanceDescBuffers[frameIndex]->Unmap(0, nullptr);
    return true;
}

void RayTracingManager::UpdateDynamicSphereMotion()
{
    constexpr double framesPerSecond = 60.0;
    constexpr double motionStartSeconds = 20.0;
    constexpr double motionDurationSeconds = 5.0;
    constexpr double twoPi = 6.28318530717958647692;
    constexpr double radiansToDegrees = 57.2957795130823208768;

    const double timeSeconds =
        static_cast<double>(m_dynamicSceneFrameIndex) / framesPerSecond;
    double position =
        static_cast<double>(m_dynamicSphereTrackCenterX) -
        m_dynamicSphereMotionAmplitude;
    double linearVelocity = 0.0;
    if (m_dynamicSphereAnimationEnabled &&
        m_dynamicSphereDeterministicTimeline &&
        timeSeconds >= motionStartSeconds &&
        timeSeconds <= motionStartSeconds + motionDurationSeconds)
    {
        const double normalizedTime =
            (timeSeconds - motionStartSeconds) /
            motionDurationSeconds;
        const double phase = twoPi * normalizedTime;
        position =
            static_cast<double>(m_dynamicSphereTrackCenterX) -
            static_cast<double>(m_dynamicSphereMotionAmplitude) *
            std::cos(phase);
        linearVelocity =
            static_cast<double>(m_dynamicSphereMotionAmplitude) *
            (twoPi / motionDurationSeconds) *
            std::sin(phase);
    }
    else if (m_dynamicSphereAnimationEnabled &&
             !m_dynamicSphereDeterministicTimeline)
    {
        const double loopTime = std::fmod(timeSeconds, motionDurationSeconds);
        const double phase = twoPi * loopTime / motionDurationSeconds;
        position =
            static_cast<double>(m_dynamicSphereTrackCenterX) -
            static_cast<double>(m_dynamicSphereMotionAmplitude) *
            std::cos(phase);
        linearVelocity =
            static_cast<double>(m_dynamicSphereMotionAmplitude) *
            (twoPi / motionDurationSeconds) *
            std::sin(phase);
    }

    m_dynamicSpherePositionX = static_cast<float>(position);
    const double traveledDistance =
        position -
        (static_cast<double>(m_dynamicSphereTrackCenterX) -
         static_cast<double>(m_dynamicSphereMotionAmplitude));
    m_dynamicSphereRollRadians = static_cast<float>(
        -traveledDistance /
        (std::max)(static_cast<double>(m_dynamicSphereRadius), 0.000001));
    m_dynamicObjectLinearSpeed = std::abs(linearVelocity);
    m_dynamicObjectAngularSpeed =
        m_dynamicObjectLinearSpeed /
        (std::max)(static_cast<double>(m_dynamicSphereRadius), 0.000001) *
        radiansToDegrees;
    ++m_dynamicSceneFrameIndex;
}

bool RayTracingManager::UpdateTopLevelAccelerationStructure(
    ID3D12GraphicsCommandList4* commandList)
{
    m_dynamicObjectMovedThisFrame = false;
    if (!m_hasDynamicSphere)
        return true;
    if (!commandList || !m_topLevelAS || !m_tlasScratchBuffer)
        return false;

    const bool visibilityChanged =
        m_dynamicSphereVisibilityDirty;
    const float previousPosition = m_dynamicSpherePositionX;
    const float previousRoll = m_dynamicSphereRollRadians;
    if (m_dynamicSphereVisible)
    {
        UpdateDynamicSphereMotion();
    }
    else
    {
        m_dynamicObjectLinearSpeed = 0.0;
        m_dynamicObjectAngularSpeed = 0.0;
    }
    const bool transformChanged =
        std::abs(previousPosition - m_dynamicSpherePositionX) > 1.0e-7f ||
        std::abs(previousRoll - m_dynamicSphereRollRadians) > 1.0e-7f;
    if (!visibilityChanged && !transformChanged)
    {
        return true;
    }

    const UINT descriptorFrame =
        static_cast<UINT>(m_frameIndex % c_tlasFrameCount);
    if (!WriteInstanceDescriptors(
        descriptorFrame,
        m_dynamicSpherePositionX,
        m_dynamicSphereRollRadians))
    {
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs.Type =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    buildDesc.Inputs.Flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildDesc.Inputs.NumDescs = 2;
    buildDesc.Inputs.InstanceDescs =
        m_instanceDescBuffers[descriptorFrame]->GetGPUVirtualAddress();
    buildDesc.SourceAccelerationStructureData =
        m_topLevelAS->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData =
        m_topLevelAS->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData =
        m_tlasScratchBuffer->GetGPUVirtualAddress();

    commandList->BuildRaytracingAccelerationStructure(
        &buildDesc,
        0,
        nullptr);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_topLevelAS.Get();
    commandList->ResourceBarrier(1, &barrier);
    m_dynamicSphereVisibilityDirty = false;
    m_dynamicObjectMovedThisFrame = true;
    return true;
}

bool RayTracingManager::ExecuteBuildCommandListAndWait()
{
    HRESULT hr = m_buildCommandList->Close();
    if (ReportFailure(hr, L"AS build command list close failed."))
        return false;

    ID3D12CommandList* commandLists[] = { m_buildCommandList.Get() };
    m_buildCommandQueue->ExecuteCommandLists(1, commandLists);

    const UINT64 fenceToWaitFor = ++m_buildFenceValue;
    hr = m_buildCommandQueue->Signal(m_buildFence.Get(), fenceToWaitFor);
    if (ReportFailure(hr, L"AS build fence signal failed."))
        return false;

    if (m_buildFence->GetCompletedValue() < fenceToWaitFor)
    {
        hr = m_buildFence->SetEventOnCompletion(fenceToWaitFor, m_buildFenceEvent);
        if (ReportFailure(hr, L"AS build fence event setup failed."))
            return false;

        WaitForSingleObject(m_buildFenceEvent, INFINITE);
    }

    return true;
}

bool RayTracingManager::CreateUploadBuffer(
    const void* data,
    UINT64 sizeInBytes,
    const wchar_t* debugName,
    Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC bufferDesc = CreateBufferDesc(sizeInBytes);

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (ReportFailure(hr, L"Upload buffer creation failed."))
        return false;

    resource->SetName(debugName);

    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = resource->Map(0, &readRange, &mappedData);
    if (ReportFailure(hr, L"Upload buffer mapping failed."))
        return false;

    std::memcpy(mappedData, data, static_cast<std::size_t>(sizeInBytes));
    resource->Unmap(0, nullptr);

    return true;
}

bool RayTracingManager::CreateAccelerationStructureBuffer(
    UINT64 sizeInBytes,
    const wchar_t* debugName,
    Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
    const UINT64 alignedSize = AlignUp64(
        sizeInBytes,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC bufferDesc = CreateBufferDesc(
        alignedSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (ReportFailure(hr, L"Acceleration structure buffer creation failed."))
        return false;

    resource->SetName(debugName);
    return true;
}

bool RayTracingManager::CreateScratchBuffer(
    UINT64 sizeInBytes,
    const wchar_t* debugName,
    Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
    const UINT64 alignedSize = AlignUp64(
        sizeInBytes,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC bufferDesc = CreateBufferDesc(
        alignedSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (ReportFailure(hr, L"Scratch buffer creation failed."))
        return false;

    resource->SetName(debugName);
    return true;
}

bool RayTracingManager::LoadCompiledShader(std::vector<std::uint8_t>& shaderBytes) const
{
    const std::wstring shaderPath = GetCompiledShaderPath();
    if (ReadBinaryFile(shaderPath, shaderBytes))
        return true;

    std::wstring message = L"Compiled raytracing shader was not found.\n";
    message += L"Expected: ";
    message += shaderPath;
    message += L"\nBuild the project once so the HLSL custom build step can create it.";
    ReportMessage(message);
    return false;
}

bool RayTracingManager::LoadCompiledAtrousShader(
    std::vector<std::uint8_t>& shaderBytes) const
{
    const std::wstring shaderPath = GetCompiledAtrousShaderPath();
    if (ReadBinaryFile(shaderPath, shaderBytes))
        return true;

    std::wstring message = L"Compiled A-Trous shader was not found.\n";
    message += L"Expected: ";
    message += shaderPath;
    message +=
        L"\nBuild the project once so the HLSL custom build step can create it.";
    ReportMessage(message);
    return false;
}

bool RayTracingManager::ReadBinaryFile(const std::wstring& path, std::vector<std::uint8_t>& bytes) const
{
    ScopedFileHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    if (!file.IsValid())
        return false;

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(file.value, &fileSize) || fileSize.QuadPart <= 0)
        return false;

    bytes.resize(static_cast<std::size_t>(fileSize.QuadPart));

    DWORD bytesRead = 0;
    const BOOL readSucceeded = ReadFile(
        file.value,
        bytes.data(),
        static_cast<DWORD>(bytes.size()),
        &bytesRead,
        nullptr);

    return readSucceeded && bytesRead == bytes.size();
}

std::wstring RayTracingManager::GetEnvironmentMapPath() const
{
    std::wstring modulePath(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, &modulePath[0], static_cast<DWORD>(modulePath.size()));
    while (length == modulePath.size())
    {
        modulePath.resize(modulePath.size() * 2);
        length = GetModuleFileNameW(nullptr, &modulePath[0], static_cast<DWORD>(modulePath.size()));
    }

    modulePath.resize(length);
    const std::wstring::size_type slash = modulePath.find_last_of(L"\\/");
    const std::wstring executableDir = slash == std::wstring::npos
        ? std::wstring()
        : modulePath.substr(0, slash + 1);

    return executableDir + c_environmentMapRelativePath;
}
std::wstring RayTracingManager::GetCompiledShaderPath() const
{
    std::wstring modulePath(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, &modulePath[0], static_cast<DWORD>(modulePath.size()));
    while (length == modulePath.size())
    {
        modulePath.resize(modulePath.size() * 2);
        length = GetModuleFileNameW(nullptr, &modulePath[0], static_cast<DWORD>(modulePath.size()));
    }

    modulePath.resize(length);
    const std::wstring::size_type slash = modulePath.find_last_of(L"\\/");
    const std::wstring executableDir = slash == std::wstring::npos
        ? std::wstring()
        : modulePath.substr(0, slash + 1);

    return executableDir + c_compiledShaderRelativePath;
}

std::wstring RayTracingManager::GetCompiledAtrousShaderPath() const
{
    std::wstring modulePath(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(
        nullptr,
        &modulePath[0],
        static_cast<DWORD>(modulePath.size()));
    while (length == modulePath.size())
    {
        modulePath.resize(modulePath.size() * 2);
        length = GetModuleFileNameW(
            nullptr,
            &modulePath[0],
            static_cast<DWORD>(modulePath.size()));
    }

    modulePath.resize(length);
    const std::wstring::size_type slash =
        modulePath.find_last_of(L"\\/");
    const std::wstring executableDir = slash == std::wstring::npos
        ? std::wstring()
        : modulePath.substr(0, slash + 1);
    return executableDir + c_compiledAtrousShaderRelativePath;
}

bool RayTracingManager::ReportFailure(HRESULT hr, const wchar_t* message) const
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

    MessageBoxW(m_hWnd, text.str().c_str(), L"DXR Error", MB_OK | MB_ICONERROR);
    return true;
}

void RayTracingManager::ReportMessage(const std::wstring& message) const
{
    MessageBoxW(m_hWnd, message.c_str(), L"DXR Error", MB_OK | MB_ICONERROR);
}




