#include "RayTracingManager.h"

#include <cstring>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "d3d12.lib")

namespace
{
    constexpr wchar_t c_rayGenShaderName[] = L"MyRaygenShader_RadianceRay";
    constexpr wchar_t c_compiledShaderRelativePath[] = L"Shaders\\Raytracing.dxil";

    UINT AlignUp(UINT value, UINT alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    D3D12_RESOURCE_DESC CreateBufferDesc(UINT64 sizeInBytes)
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
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
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

    if (!CreateRayGenShaderTable())
        return false;

    return true;
}

bool RayTracingManager::Resize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return true;

    m_width = width;
    m_height = height;
    return CreateOutputTexture();
}

bool RayTracingManager::CreateOutputTexture()
{
    if (!m_descriptorHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 1;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));
        if (ReportFailure(hr, L"Raytracing descriptor heap creation failed."))
            return false;

        m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    m_outputTexture.Reset();

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

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = c_outputFormat;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    m_device->CreateUnorderedAccessView(
        m_outputTexture.Get(),
        nullptr,
        &uavDesc,
        m_descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    return true;
}

bool RayTracingManager::CreateGlobalRootSignature()
{
    D3D12_DESCRIPTOR_RANGE outputRange = {};
    outputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRange.NumDescriptors = 1;
    outputRange.BaseShaderRegister = 0;
    outputRange.RegisterSpace = 0;
    outputRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameter.DescriptorTable.NumDescriptorRanges = 1;
    rootParameter.DescriptorTable.pDescriptorRanges = &outputRange;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = 1;
    rootSignatureDesc.pParameters = &rootParameter;
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

    D3D12_EXPORT_DESC rayGenExport = {};
    rayGenExport.Name = c_rayGenShaderName;
    rayGenExport.ExportToRename = nullptr;
    rayGenExport.Flags = D3D12_EXPORT_FLAG_NONE;

    D3D12_DXIL_LIBRARY_DESC dxilLibraryDesc = {};
    dxilLibraryDesc.DXILLibrary.pShaderBytecode = shaderBytes.data();
    dxilLibraryDesc.DXILLibrary.BytecodeLength = shaderBytes.size();
    dxilLibraryDesc.NumExports = 1;
    dxilLibraryDesc.pExports = &rayGenExport;

    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes = c_shaderPayloadSize;
    shaderConfig.MaxAttributeSizeInBytes = c_shaderAttributeSize;

    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSignature = {};
    globalRootSignature.pGlobalRootSignature = m_globalRootSignature.Get();

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = c_maxRecursionDepth;

    D3D12_STATE_SUBOBJECT subobjects[4] = {};
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0].pDesc = &dxilLibraryDesc;
    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[1].pDesc = &shaderConfig;
    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[2].pDesc = &globalRootSignature;
    subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[3].pDesc = &pipelineConfig;

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

bool RayTracingManager::CreateRayGenShaderTable()
{
    Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
    HRESULT hr = m_stateObject.As(&stateObjectProperties);
    if (ReportFailure(hr, L"Raytracing state object properties query failed."))
        return false;

    void* shaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_rayGenShaderName);
    if (!shaderIdentifier)
    {
        ReportMessage(L"RayGen shader identifier was not found in the state object.");
        return false;
    }

    m_rayGenShaderRecordSize = AlignUp(
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    const UINT shaderTableSize = AlignUp(
        m_rayGenShaderRecordSize,
        D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC bufferDesc = CreateBufferDesc(shaderTableSize);

    hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_rayGenShaderTable));
    if (ReportFailure(hr, L"RayGen shader table creation failed."))
        return false;

    m_rayGenShaderTable->SetName(L"RayGen shader table");

    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_rayGenShaderTable->Map(0, &readRange, &mappedData);
    if (ReportFailure(hr, L"RayGen shader table mapping failed."))
        return false;

    std::memset(mappedData, 0, shaderTableSize);
    std::memcpy(mappedData, shaderIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    m_rayGenShaderTable->Unmap(0, nullptr);

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