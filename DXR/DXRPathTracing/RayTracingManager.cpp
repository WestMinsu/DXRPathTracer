#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RayTracingManager.h"
#include "GltfSceneLoader.h"
#include "SceneData.h"

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
    constexpr wchar_t c_missShaderName[] = L"MyMissShader_RadianceRay";
    constexpr wchar_t c_hitGroupName[] = L"MyHitGroup_Triangle_RadianceRay";
    constexpr wchar_t c_compiledShaderRelativePath[] = L"Shaders\\Raytracing.dxil";
    constexpr wchar_t c_environmentMapRelativePath[] = L"Assets\\Textures\\Cubemaps\\HDRI\\autumn_hill_view_4kSpecularHDR.dds";
    constexpr UINT c_materialTextureDescriptorIndex = 3;
    constexpr UINT c_materialTextureDescriptorCount = 64;
    constexpr UINT c_descriptorCount =
        c_materialTextureDescriptorIndex + c_materialTextureDescriptorCount;
    constexpr UINT c_environmentDescriptorIndex = 2;
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
    };
    static_assert(sizeof(RenderSettingsConstants) == 19 * sizeof(std::uint32_t));

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

    if (!CreateBuildCommandObjects())
        return false;

    if (!CreateEnvironmentMap())
        return false;

    if (!CreateGlobalRootSignature())
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
        !m_environmentMap || !m_sceneMaterialBuffer || !m_primitiveMaterialIndexBuffer)
    {
        return;
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
    commandList->SetComputeRoot32BitConstants(4, 19, &renderSettings, 0);
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
    commandList->SetPipelineState1(m_stateObject.Get());

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderRecordSize;
    dispatchDesc.MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes = m_missShaderRecordSize;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missShaderRecordSize;
    dispatchDesc.HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupShaderRecordSize;
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupShaderRecordSize;
    dispatchDesc.Width = m_width;
    dispatchDesc.Height = m_height;
    dispatchDesc.Depth = 1;

    commandList->DispatchRays(&dispatchDesc);

    if (shouldAccumulate)
    {
        ++m_accumulatedSampleCount;
    }
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

void RayTracingManager::SetIblSettings(bool enableIbl, float intensity)
{
    const float clampedIntensity = intensity < 0.0f ? 0.0f : (intensity > 8.0f ? 8.0f : intensity);
    if (m_enableIbl == enableIbl && m_iblIntensity == clampedIntensity)
        return;

    m_enableIbl = enableIbl;
    m_iblIntensity = clampedIntensity;
    ResetAccumulation();
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
    const UINT clampedSceneType = sceneType <= c_scenePbrGpuValidation
        ? sceneType
        : c_sceneCornellBox;
    if (m_sceneType == clampedSceneType)
        return;

    m_sceneType = clampedSceneType;
    ResetAccumulation();
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

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc = {};
    outputUavDesc.Format = c_outputFormat;
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
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
    D3D12_DESCRIPTOR_RANGE outputRange = {};
    outputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRange.NumDescriptors = 2;
    outputRange.BaseShaderRegister = 0;
    outputRange.RegisterSpace = 0;
    outputRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

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

    D3D12_ROOT_PARAMETER rootParameters[9] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[0].DescriptorTable.pDescriptorRanges = &outputRange;
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
    rootParameters[4].Constants.Num32BitValues = 19;
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

bool RayTracingManager::CreateRaytracingPipelineState()
{
    std::vector<std::uint8_t> shaderBytes;
    if (!LoadCompiledShader(shaderBytes))
        return false;

    D3D12_EXPORT_DESC shaderExports[3] = {};
    shaderExports[0].Name = c_rayGenShaderName;
    shaderExports[0].ExportToRename = nullptr;
    shaderExports[0].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[1].Name = c_closestHitShaderName;
    shaderExports[1].ExportToRename = nullptr;
    shaderExports[1].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[2].Name = c_missShaderName;
    shaderExports[2].ExportToRename = nullptr;
    shaderExports[2].Flags = D3D12_EXPORT_FLAG_NONE;

    D3D12_DXIL_LIBRARY_DESC dxilLibraryDesc = {};
    dxilLibraryDesc.DXILLibrary.pShaderBytecode = shaderBytes.data();
    dxilLibraryDesc.DXILLibrary.BytecodeLength = shaderBytes.size();
    dxilLibraryDesc.NumExports = _countof(shaderExports);
    dxilLibraryDesc.pExports = shaderExports;

    D3D12_HIT_GROUP_DESC hitGroupDesc = {};
    hitGroupDesc.HitGroupExport = c_hitGroupName;
    hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroupDesc.ClosestHitShaderImport = c_closestHitShaderName;

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
        CreateShaderTable(
            c_missShaderName,
            m_missShaderTable.ReleaseAndGetAddressOf(),
            &m_missShaderRecordSize,
            L"Miss shader table") &&
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

    D3D12_RESOURCE_BARRIER blasBarrier = {};
    blasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    blasBarrier.UAV.pResource = m_bottomLevelAS.Get();
    m_buildCommandList->ResourceBarrier(1, &blasBarrier);

    if (!BuildTopLevelAccelerationStructure())
        return false;

    D3D12_RESOURCE_BARRIER tlasBarrier = {};
    tlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    tlasBarrier.UAV.pResource = m_topLevelAS.Get();
    m_buildCommandList->ResourceBarrier(1, &tlasBarrier);

    const bool buildSucceeded = ExecuteBuildCommandListAndWait();
    m_blasScratchBuffer.Reset();
    m_tlasScratchBuffer.Reset();
    return buildSucceeded;
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
    const bool isPbrScene = m_sceneType == c_scenePbrGgx ||
        m_sceneType == c_scenePbrGpuValidation;
    if (isPbrScene && !m_sceneFilePath.empty())
    {
        std::wstring errorMessage;
        if (!LoadGltfSceneData(m_sceneFilePath, scene, errorMessage))
        {
            ReportMessage(
                L"glTF scene load failed.\nPath: " + m_sceneFilePath +
                L"\nReason: " + errorMessage);
            return false;
        }
    }
    else
    {
        scene = isPbrScene
            ? CreatePbrGgxSceneData()
            : CreateCornellBoxSceneData();
    }
    if (!scene.IsValid())
    {
        ReportMessage(L"Generated scene data is invalid.");
        return false;
    }

    m_autoFrameCamera = isPbrScene && !m_sceneFilePath.empty();
    if (m_autoFrameCamera)
    {
        m_sceneBoundsMin =
        {
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()
        };
        m_sceneBoundsMax =
        {
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()
        };
        for (const SceneVertex& vertex : scene.vertices)
        {
            for (std::size_t component = 0; component < 3; ++component)
            {
                m_sceneBoundsMin[component] = std::min(
                    m_sceneBoundsMin[component],
                    vertex.position[component]);
                m_sceneBoundsMax[component] = std::max(
                    m_sceneBoundsMax[component],
                    vertex.position[component]);
            }
        }
        UpdateCameraFromSceneBounds();
    }
    else
    {
        m_cameraPosition = { 0.0f, 0.15f, -1.2f };
        m_cameraTarget = { 0.0f, 0.0f, 0.0f };
    }

    m_vertexCount = static_cast<UINT>(scene.vertices.size());
    m_indexCount = static_cast<UINT>(scene.indices.size());

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
        ReportMessage(L"The scene exceeds the 64 material texture limit.");
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

        D3D12_RESOURCE_DESC textureDesc = {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Alignment = 0;
        textureDesc.Width = source.width;
        textureDesc.Height = source.height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = 1;
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

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rowCount = 0;
        UINT64 rowSizeInBytes = 0;
        UINT64 uploadSize = 0;
        m_device->GetCopyableFootprints(
            &textureDesc,
            0,
            1,
            0,
            &footprint,
            &rowCount,
            &rowSizeInBytes,
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

        const std::size_t sourceRowPitch =
            static_cast<std::size_t>(source.width) * 4u;
        std::uint8_t* destination =
            static_cast<std::uint8_t*>(mappedData) + footprint.Offset;
        for (UINT row = 0; row < source.height; ++row)
        {
            std::memcpy(
                destination + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                source.rgba8.data() + static_cast<std::size_t>(row) * sourceRowPitch,
                sourceRowPitch);
        }
        uploadBuffer->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destinationLocation = {};
        destinationLocation.pResource = gpuTexture.Get();
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
        sourceLocation.pResource = uploadBuffer.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;
        m_buildCommandList->CopyTextureRegion(
            &destinationLocation,
            0,
            0,
            0,
            &sourceLocation,
            nullptr);

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
        srvDesc.Texture2D.MipLevels = 1;
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
    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
    geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometryDesc.Triangles.VertexBuffer.StartAddress = m_vertexBuffer->GetGPUVirtualAddress();
    geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(SceneVertex);
    geometryDesc.Triangles.VertexCount = m_vertexCount;
    geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometryDesc.Triangles.IndexBuffer = m_indexBuffer->GetGPUVirtualAddress();
    geometryDesc.Triangles.IndexCount = m_indexCount;
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

    m_blasScratchBuffer.Reset();
    if (!CreateScratchBuffer(prebuildInfo.ScratchDataSizeInBytes, L"BLAS scratch buffer", m_blasScratchBuffer))
        return false;

    if (!CreateAccelerationStructureBuffer(prebuildInfo.ResultDataMaxSizeInBytes, L"Bottom level acceleration structure", m_bottomLevelAS))
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = m_blasScratchBuffer->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData = m_bottomLevelAS->GetGPUVirtualAddress();

    m_buildCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    return true;
}

bool RayTracingManager::BuildTopLevelAccelerationStructure()
{
    D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
    instanceDesc.Transform[0][0] = 1.0f;
    instanceDesc.Transform[1][1] = 1.0f;
    instanceDesc.Transform[2][2] = 1.0f;
    instanceDesc.InstanceMask = 0xFF;
    instanceDesc.AccelerationStructure = m_bottomLevelAS->GetGPUVirtualAddress();

    if (!CreateUploadBuffer(&instanceDesc, sizeof(instanceDesc), L"TLAS instance descriptor", m_instanceDescBuffer))
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 1;
    inputs.InstanceDescs = m_instanceDescBuffer->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
    if (prebuildInfo.ResultDataMaxSizeInBytes == 0)
    {
        ReportMessage(L"TLAS prebuild info returned an empty result size.");
        return false;
    }

    m_tlasScratchBuffer.Reset();
    if (!CreateScratchBuffer(prebuildInfo.ScratchDataSizeInBytes, L"TLAS scratch buffer", m_tlasScratchBuffer))
        return false;

    if (!CreateAccelerationStructureBuffer(prebuildInfo.ResultDataMaxSizeInBytes, L"Top level acceleration structure", m_topLevelAS))
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = m_tlasScratchBuffer->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData = m_topLevelAS->GetGPUVirtualAddress();

    m_buildCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
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




