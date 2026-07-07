#include "RayTracingManager.h"

#include <cstring>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "d3d12.lib")

namespace
{
    constexpr wchar_t c_rayGenShaderName[] = L"MyRaygenShader_RadianceRay";
    constexpr wchar_t c_closestHitShaderName[] = L"MyClosestHitShader_RadianceRay";
    constexpr wchar_t c_missShaderName[] = L"MyMissShader_RadianceRay";
    constexpr wchar_t c_hitGroupName[] = L"MyHitGroup_Triangle_RadianceRay";
    constexpr wchar_t c_compiledShaderRelativePath[] = L"Shaders\\Raytracing.dxil";
    constexpr float c_shortBlockHalfWidth = 0.42f;
    constexpr float c_shortBlockHeight = 0.58f;
    constexpr float c_shortBlockHalfDepth = 0.42f;
    constexpr float c_shortBlockCenterX = -0.68f;
    constexpr float c_shortBlockCenterZ = 1.28f;
    constexpr float c_shortBlockCosY = 0.951056516f;
    constexpr float c_shortBlockSinY = -0.309016994f;
    constexpr float c_tallBlockHalfWidth = 0.42f;
    constexpr float c_tallBlockHeight = 1.15f;
    constexpr float c_tallBlockHalfDepth = 0.42f;
    constexpr float c_tallBlockCenterX = 0.62f;
    constexpr float c_tallBlockCenterZ = 2.35f;
    constexpr float c_tallBlockCosY = 0.965925826f;
    constexpr float c_tallBlockSinY = 0.258819045f;
    constexpr float c_boxFloorY = -0.85f;
    constexpr float c_boxCeilingY = 1.25f;
    constexpr float c_boxHalfWidth = 2.25f;
    constexpr float c_boxNearZ = 0.0f;
    constexpr float c_boxFarZ = 4.0f;
    constexpr float c_lightY = c_boxCeilingY - 0.002f;
    constexpr float c_lightHalfWidth = 0.55f;
    constexpr float c_lightNearZ = 1.10f;
    constexpr float c_lightFarZ = 2.25f;
    constexpr UINT c_vertexCount = 72;
    constexpr UINT c_indexCount = 108;

    struct Vertex
    {
        float position[3];
    };

    Vertex MakeBlockVertex(float x, float y, float z, float centerX, float centerZ, float cosY, float sinY)
    {
        const float rotatedX = x * cosY + z * sinY;
        const float rotatedZ = -x * sinY + z * cosY;

        return { { centerX + rotatedX, c_boxFloorY + y, centerZ + rotatedZ } };
    }

    struct RenderSettingsConstants
    {
        UINT showNormalColor;
        UINT frameIndex;
        UINT maxBounce;
        UINT sampleIndex;
        UINT enableAccumulation;
    };

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
        !m_hitGroupShaderTable || !m_descriptorHeap || !m_topLevelAS || !m_accumulationTexture)
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
    const bool shouldAccumulate = m_enableAccumulation && !m_showNormalColor;
    RenderSettingsConstants renderSettings = {};
    renderSettings.showNormalColor = m_showNormalColor ? 1u : 0u;
    renderSettings.frameIndex = m_frameIndex++;
    renderSettings.maxBounce = m_maxBounce;
    renderSettings.sampleIndex = shouldAccumulate ? m_accumulatedSampleCount : 0u;
    renderSettings.enableAccumulation = shouldAccumulate ? 1u : 0u;
    commandList->SetComputeRoot32BitConstants(4, 5, &renderSettings, 0);
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

bool RayTracingManager::CreateOutputTexture()
{
    if (!m_descriptorHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 2;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));
        if (ReportFailure(hr, L"Raytracing descriptor heap creation failed."))
            return false;

        m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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

    D3D12_ROOT_PARAMETER rootParameters[5] = {};
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
    rootParameters[4].Constants.Num32BitValues = 5;
    rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 0;
    rootSignatureDesc.pStaticSamplers = nullptr;
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
    if (!CreateBuildCommandObjects())
        return false;

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
    const float sw = c_shortBlockHalfWidth;
    const float sh = c_shortBlockHeight;
    const float sd = c_shortBlockHalfDepth;
    const float tw = c_tallBlockHalfWidth;
    const float th = c_tallBlockHeight;
    const float td = c_tallBlockHalfDepth;
    const Vertex vertices[c_vertexCount] =
    {
        MakeBlockVertex(-sw, sh, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, sh, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, 0.0f, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex(-sw, 0.0f, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),

        MakeBlockVertex(-sw, sh,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex(-sw, 0.0f,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, 0.0f,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, sh,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),

        MakeBlockVertex(-sw, sh,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex(-sw, sh, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex(-sw, 0.0f, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex(-sw, 0.0f,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),

        MakeBlockVertex( sw, sh, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, sh,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, 0.0f,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, 0.0f, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),

        MakeBlockVertex(-sw, sh,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, sh,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, sh, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex(-sw, sh, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),

        MakeBlockVertex(-sw, 0.0f, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, 0.0f, -sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex( sw, 0.0f,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),
        MakeBlockVertex(-sw, 0.0f,  sd, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY),

        MakeBlockVertex(-tw, th, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, th, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, 0.0f, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex(-tw, 0.0f, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),

        MakeBlockVertex(-tw, th,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex(-tw, 0.0f,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, 0.0f,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, th,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),

        MakeBlockVertex(-tw, th,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex(-tw, th, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex(-tw, 0.0f, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex(-tw, 0.0f,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),

        MakeBlockVertex( tw, th, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, th,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, 0.0f,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, 0.0f, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),

        MakeBlockVertex(-tw, th,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, th,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, th, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex(-tw, th, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),

        MakeBlockVertex(-tw, 0.0f, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, 0.0f, -td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex( tw, 0.0f,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),
        MakeBlockVertex(-tw, 0.0f,  td, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY),

        { { -c_boxHalfWidth, c_boxFloorY, c_boxNearZ } },
        { { -c_boxHalfWidth, c_boxFloorY, c_boxFarZ } },
        { {  c_boxHalfWidth, c_boxFloorY, c_boxFarZ } },
        { {  c_boxHalfWidth, c_boxFloorY, c_boxNearZ } },

        { { -c_boxHalfWidth, c_boxCeilingY, c_boxNearZ } },
        { {  c_boxHalfWidth, c_boxCeilingY, c_boxNearZ } },
        { {  c_boxHalfWidth, c_boxCeilingY, c_boxFarZ } },
        { { -c_boxHalfWidth, c_boxCeilingY, c_boxFarZ } },

        { { -c_boxHalfWidth, c_boxFloorY, c_boxFarZ } },
        { { -c_boxHalfWidth, c_boxCeilingY, c_boxFarZ } },
        { {  c_boxHalfWidth, c_boxCeilingY, c_boxFarZ } },
        { {  c_boxHalfWidth, c_boxFloorY, c_boxFarZ } },

        { { -c_boxHalfWidth, c_boxFloorY, c_boxNearZ } },
        { { -c_boxHalfWidth, c_boxCeilingY, c_boxNearZ } },
        { { -c_boxHalfWidth, c_boxCeilingY, c_boxFarZ } },
        { { -c_boxHalfWidth, c_boxFloorY, c_boxFarZ } },

        { {  c_boxHalfWidth, c_boxFloorY, c_boxNearZ } },
        { {  c_boxHalfWidth, c_boxFloorY, c_boxFarZ } },
        { {  c_boxHalfWidth, c_boxCeilingY, c_boxFarZ } },
        { {  c_boxHalfWidth, c_boxCeilingY, c_boxNearZ } },

        { { -c_lightHalfWidth, c_lightY, c_lightNearZ } },
        { {  c_lightHalfWidth, c_lightY, c_lightNearZ } },
        { {  c_lightHalfWidth, c_lightY, c_lightFarZ } },
        { { -c_lightHalfWidth, c_lightY, c_lightFarZ } }
    };

    const std::uint32_t indices[c_indexCount] =
    {
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23,
        24, 25, 26, 24, 26, 27,
        28, 29, 30, 28, 30, 31,
        32, 33, 34, 32, 34, 35,
        36, 37, 38, 36, 38, 39,
        40, 41, 42, 40, 42, 43,
        44, 45, 46, 44, 46, 47,
        48, 49, 50, 48, 50, 51,
        52, 53, 54, 52, 54, 55,
        56, 57, 58, 56, 58, 59,
        60, 61, 62, 60, 62, 63,
        64, 65, 66, 64, 66, 67,
        68, 69, 70, 68, 70, 71
    };

    if (!CreateUploadBuffer(vertices, sizeof(vertices), L"Raytracing scene vertex buffer", m_vertexBuffer))
        return false;

    return CreateUploadBuffer(indices, sizeof(indices), L"Raytracing scene index buffer", m_indexBuffer);
}
bool RayTracingManager::BuildBottomLevelAccelerationStructure()
{
    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
    geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometryDesc.Triangles.VertexBuffer.StartAddress = m_vertexBuffer->GetGPUVirtualAddress();
    geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
    geometryDesc.Triangles.VertexCount = c_vertexCount;
    geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometryDesc.Triangles.IndexBuffer = m_indexBuffer->GetGPUVirtualAddress();
    geometryDesc.Triangles.IndexCount = c_indexCount;
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




