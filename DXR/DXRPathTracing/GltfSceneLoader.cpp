#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "GltfSceneLoader.h"

#define CGLTF_IMPLEMENTATION
#include "ThirdParty/cgltf/cgltf.h"

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace
{
    constexpr float c_normalLengthEpsilon = 1.0e-20f;
    constexpr float c_transformDeterminantEpsilon = 1.0e-12f;

    struct NodeTransform
    {
        float world[16] = {};
        float normal[9] = {};
        float determinant = 1.0f;
    };

    struct LoadedTextureKey
    {
        const cgltf_texture* texture = nullptr;
        bool isSrgb = false;
        std::uint32_t sceneTextureIndex = c_invalidSceneTextureIndex;
    };

    float Clamp01(float value)
    {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    bool Utf8ToWide(const char* text, std::wstring& converted)
    {
        converted.clear();
        if (!text)
            return false;

        const int requiredLength = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text,
            -1,
            nullptr,
            0);
        if (requiredLength <= 0)
            return false;

        converted.resize(static_cast<std::size_t>(requiredLength));
        const int convertedLength = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text,
            -1,
            &converted[0],
            requiredLength);
        if (convertedLength <= 0)
        {
            converted.clear();
            return false;
        }
        converted.resize(static_cast<std::size_t>(convertedLength - 1));
        return true;
    }

    bool WideToUtf8(const std::wstring& text, std::string& converted)
    {
        converted.clear();
        if (text.empty())
            return false;

        const int requiredLength = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.c_str(),
            -1,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (requiredLength <= 0)
            return false;

        converted.resize(static_cast<std::size_t>(requiredLength));
        const int convertedLength = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.c_str(),
            -1,
            &converted[0],
            requiredLength,
            nullptr,
            nullptr);
        if (convertedLength <= 0)
        {
            converted.clear();
            return false;
        }
        converted.resize(static_cast<std::size_t>(convertedLength - 1));
        return true;
    }

    void* AllocateFileMemory(
        const cgltf_memory_options* memoryOptions,
        cgltf_size size)
    {
        if (memoryOptions && memoryOptions->alloc_func)
            return memoryOptions->alloc_func(memoryOptions->user_data, size);
        return std::malloc(size);
    }

    void ReleaseFileMemory(
        const cgltf_memory_options* memoryOptions,
        const cgltf_file_options*,
        void* data)
    {
        if (!data)
            return;
        if (memoryOptions && memoryOptions->free_func)
        {
            memoryOptions->free_func(memoryOptions->user_data, data);
            return;
        }
        std::free(data);
    }

    cgltf_result ReadFileUtf8(
        const cgltf_memory_options* memoryOptions,
        const cgltf_file_options*,
        const char* path,
        cgltf_size* size,
        void** data)
    {
        if (!path || !size || !data)
            return cgltf_result_invalid_options;

        std::wstring widePath;
        if (!Utf8ToWide(path, widePath))
            return cgltf_result_io_error;

        FILE* file = nullptr;
        if (_wfopen_s(&file, widePath.c_str(), L"rb") != 0 || !file)
            return cgltf_result_file_not_found;

        if (_fseeki64(file, 0, SEEK_END) != 0)
        {
            std::fclose(file);
            return cgltf_result_io_error;
        }

        const __int64 fileSize = _ftelli64(file);
        if (fileSize < 0 || _fseeki64(file, 0, SEEK_SET) != 0)
        {
            std::fclose(file);
            return cgltf_result_io_error;
        }

        void* fileData = AllocateFileMemory(
            memoryOptions,
            static_cast<cgltf_size>(fileSize));
        if (!fileData && fileSize > 0)
        {
            std::fclose(file);
            return cgltf_result_out_of_memory;
        }

        const std::size_t readSize = std::fread(
            fileData,
            1,
            static_cast<std::size_t>(fileSize),
            file);
        std::fclose(file);
        if (readSize != static_cast<std::size_t>(fileSize))
        {
            ReleaseFileMemory(memoryOptions, nullptr, fileData);
            return cgltf_result_io_error;
        }

        *size = static_cast<cgltf_size>(fileSize);
        *data = fileData;
        return cgltf_result_success;
    }

    const wchar_t* ResultName(cgltf_result result)
    {
        switch (result)
        {
        case cgltf_result_success: return L"success";
        case cgltf_result_data_too_short: return L"data too short";
        case cgltf_result_unknown_format: return L"unknown format";
        case cgltf_result_invalid_json: return L"invalid JSON";
        case cgltf_result_invalid_gltf: return L"invalid glTF";
        case cgltf_result_invalid_options: return L"invalid options";
        case cgltf_result_file_not_found: return L"file not found";
        case cgltf_result_io_error: return L"I/O error";
        case cgltf_result_out_of_memory: return L"out of memory";
        case cgltf_result_legacy_gltf: return L"legacy glTF is unsupported";
        default: return L"unknown error";
        }
    }

    bool Fail(std::wstring& errorMessage, const std::wstring& message)
    {
        errorMessage = message;
        return false;
    }

    bool ReadBinaryFile(const std::wstring& path, std::vector<std::uint8_t>& bytes)
    {
        bytes.clear();
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
            return false;

        if (_fseeki64(file, 0, SEEK_END) != 0)
        {
            std::fclose(file);
            return false;
        }
        const __int64 size = _ftelli64(file);
        if (size <= 0 || _fseeki64(file, 0, SEEK_SET) != 0 ||
            static_cast<unsigned long long>(size) >
                static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        {
            std::fclose(file);
            return false;
        }

        bytes.resize(static_cast<std::size_t>(size));
        const std::size_t readSize = std::fread(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
        return readSize == bytes.size();
    }

    int Base64Value(char value)
    {
        if (value >= 'A' && value <= 'Z') return value - 'A';
        if (value >= 'a' && value <= 'z') return value - 'a' + 26;
        if (value >= '0' && value <= '9') return value - '0' + 52;
        if (value == '+') return 62;
        if (value == '/') return 63;
        return -1;
    }

    bool DecodeBase64(const char* encoded, std::vector<std::uint8_t>& decoded)
    {
        decoded.clear();
        if (!encoded)
            return false;

        unsigned int accumulator = 0;
        int bitCount = 0;
        for (const char* cursor = encoded; *cursor; ++cursor)
        {
            if (*cursor == '=')
                break;
            const int value = Base64Value(*cursor);
            if (value < 0)
            {
                if (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
                    continue;
                return false;
            }
            accumulator = (accumulator << 6) | static_cast<unsigned int>(value);
            bitCount += 6;
            if (bitCount >= 8)
            {
                bitCount -= 8;
                decoded.push_back(static_cast<std::uint8_t>((accumulator >> bitCount) & 0xFFu));
            }
        }
        return !decoded.empty();
    }

    bool LoadImageBytes(
        const cgltf_image& image,
        const std::wstring& gltfPath,
        std::vector<std::uint8_t>& bytes,
        std::wstring& errorMessage)
    {
        bytes.clear();
        if (image.buffer_view)
        {
            const std::uint8_t* data = cgltf_buffer_view_data(image.buffer_view);
            if (!data || image.buffer_view->size == 0)
                return Fail(errorMessage, L"A texture image bufferView is empty.");
            bytes.assign(data, data + image.buffer_view->size);
            return true;
        }

        if (!image.uri || !image.uri[0])
            return Fail(errorMessage, L"A texture image has neither a URI nor a bufferView.");

        constexpr char c_dataPrefix[] = "data:";
        if (std::strncmp(image.uri, c_dataPrefix, sizeof(c_dataPrefix) - 1) == 0)
        {
            const char* comma = std::strchr(image.uri, ',');
            if (!comma || comma == image.uri ||
                std::string(
                    image.uri,
                    static_cast<std::size_t>(comma - image.uri)).find(";base64") ==
                        std::string::npos ||
                !DecodeBase64(comma + 1, bytes))
            {
                return Fail(errorMessage, L"Only base64-encoded image data URIs are supported.");
            }
            return true;
        }

        std::vector<char> decodedUri(image.uri, image.uri + std::strlen(image.uri) + 1);
        const cgltf_size decodedLength = cgltf_decode_uri(decodedUri.data());
        decodedUri.resize(decodedLength + 1);
        decodedUri[decodedLength] = '\0';

        std::wstring imagePath;
        if (!Utf8ToWide(decodedUri.data(), imagePath))
            return Fail(errorMessage, L"A texture image URI is not valid UTF-8.");

        const bool absolutePath =
            (imagePath.size() >= 2 && imagePath[1] == L':') ||
            (!imagePath.empty() && (imagePath[0] == L'\\' || imagePath[0] == L'/'));
        if (!absolutePath)
        {
            const std::size_t separator = gltfPath.find_last_of(L"\\/");
            imagePath = separator == std::wstring::npos
                ? imagePath
                : gltfPath.substr(0, separator + 1) + imagePath;
        }

        if (!ReadBinaryFile(imagePath, bytes))
            return Fail(errorMessage, L"Failed to read texture image: " + imagePath);
        return true;
    }

    bool DecodeRgba8(
        const std::vector<std::uint8_t>& encoded,
        SceneTexture& texture,
        std::wstring& errorMessage)
    {
        if (encoded.empty() || encoded.size() > std::numeric_limits<DWORD>::max())
            return Fail(errorMessage, L"A texture image is empty or too large for WIC.");

        const HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(initializeResult);
        if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE)
            return Fail(errorMessage, L"COM initialization failed while decoding a texture.");

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;

        HRESULT hr = CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(
            const_cast<BYTE*>(encoded.data()),
            static_cast<DWORD>(encoded.size()));
        if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) hr = converter->Initialize(
            frame.Get(),
            GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0,
            WICBitmapPaletteTypeCustom);

        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
        const std::uint64_t rowPitch = static_cast<std::uint64_t>(width) * 4u;
        const std::uint64_t byteCount = rowPitch * height;
        if (SUCCEEDED(hr) && (width == 0 || height == 0 ||
            rowPitch > std::numeric_limits<UINT>::max() ||
            byteCount > std::numeric_limits<UINT>::max()))
        {
            hr = E_INVALIDARG;
        }
        if (SUCCEEDED(hr))
        {
            texture.rgba8.resize(static_cast<std::size_t>(byteCount));
            hr = converter->CopyPixels(
                nullptr,
                static_cast<UINT>(rowPitch),
                static_cast<UINT>(byteCount),
                texture.rgba8.data());
        }

        converter.Reset();
        frame.Reset();
        decoder.Reset();
        stream.Reset();
        factory.Reset();
        if (uninitialize)
            CoUninitialize();
        if (FAILED(hr))
            return Fail(errorMessage, L"WIC failed to decode a glTF texture image to RGBA8.");

        texture.width = width;
        texture.height = height;
        return true;
    }

    bool ValidateTextureView(
        const cgltf_texture_view& view,
        std::wstring& errorMessage)
    {
        if (!view.texture)
            return true;
        if (view.texcoord != 0)
            return Fail(errorMessage, L"Only TEXCOORD_0 material textures are supported.");
        if (view.has_transform)
            return Fail(errorMessage, L"KHR_texture_transform is not supported yet.");
        if (view.texture->has_basisu || view.texture->has_webp || !view.texture->image)
            return Fail(errorMessage, L"BasisU, WebP, or missing glTF texture images are not supported.");
        if (view.texture->sampler)
        {
            const cgltf_sampler& sampler = *view.texture->sampler;
            if ((sampler.wrap_s != cgltf_wrap_mode_repeat && sampler.wrap_s != 0) ||
                (sampler.wrap_t != cgltf_wrap_mode_repeat && sampler.wrap_t != 0) ||
                sampler.mag_filter == cgltf_filter_type_nearest ||
                sampler.min_filter == cgltf_filter_type_nearest ||
                sampler.min_filter == cgltf_filter_type_nearest_mipmap_nearest ||
                sampler.min_filter == cgltf_filter_type_nearest_mipmap_linear)
            {
                return Fail(errorMessage, L"Only repeat-wrapped linear material texture sampling is supported.");
            }
        }
        return true;
    }

    bool FindOrLoadTexture(
        const cgltf_texture_view& view,
        bool isSrgb,
        const std::wstring& gltfPath,
        SceneData& scene,
        std::vector<LoadedTextureKey>& loadedTextures,
        std::uint32_t& textureIndex,
        std::wstring& errorMessage)
    {
        textureIndex = c_invalidSceneTextureIndex;
        if (!view.texture)
            return true;
        if (!ValidateTextureView(view, errorMessage))
            return false;

        for (const LoadedTextureKey& loaded : loadedTextures)
        {
            if (loaded.texture == view.texture && loaded.isSrgb == isSrgb)
            {
                textureIndex = loaded.sceneTextureIndex;
                return true;
            }
        }

        if (scene.textures.size() >= std::numeric_limits<std::uint32_t>::max())
            return Fail(errorMessage, L"The glTF contains too many material textures.");

        std::vector<std::uint8_t> encoded;
        if (!LoadImageBytes(*view.texture->image, gltfPath, encoded, errorMessage))
            return false;

        SceneTexture texture;
        texture.isSrgb = isSrgb ? 1u : 0u;
        if (!DecodeRgba8(encoded, texture, errorMessage))
            return false;

        textureIndex = static_cast<std::uint32_t>(scene.textures.size());
        scene.textures.push_back(std::move(texture));
        loadedTextures.push_back({ view.texture, isSrgb, textureIndex });
        return true;
    }

    SceneMaterial MakeDefaultMaterial()
    {
        SceneMaterial material = {};
        material.baseColor[0] = 1.0f;
        material.baseColor[1] = 1.0f;
        material.baseColor[2] = 1.0f;
        material.metallic = 1.0f;
        material.roughness = 1.0f;
        material.baseColorTextureIndex = c_invalidSceneTextureIndex;
        material.metallicRoughnessTextureIndex = c_invalidSceneTextureIndex;
        material.normalTextureIndex = c_invalidSceneTextureIndex;
        material.normalTextureScale = 1.0f;
        return material;
    }

    bool HasUnsupportedMaterialFeature(const cgltf_material& material)
    {
        if (material.has_pbr_specular_glossiness ||
            material.has_clearcoat ||
            material.has_transmission ||
            material.has_volume ||
            material.has_ior ||
            material.has_specular ||
            material.has_sheen ||
            material.has_iridescence ||
            material.has_diffuse_transmission ||
            material.has_anisotropy ||
            material.has_dispersion ||
            material.unlit)
        {
            return true;
        }

        return material.occlusion_texture.texture ||
            material.emissive_texture.texture ||
            material.extensions_count > 0;
    }

    bool ConvertMaterials(
        const cgltf_data& data,
        const std::wstring& gltfPath,
        SceneData& scene,
        std::wstring& errorMessage,
        std::uint32_t& defaultMaterialIndex)
    {
        scene.materials.reserve(data.materials_count + 1);
        std::vector<LoadedTextureKey> loadedTextures;
        for (cgltf_size materialIndex = 0;
             materialIndex < data.materials_count;
             ++materialIndex)
        {
            const cgltf_material& source = data.materials[materialIndex];
            if (source.alpha_mode != cgltf_alpha_mode_opaque)
            {
                return Fail(
                    errorMessage,
                    L"Material " + std::to_wstring(materialIndex) +
                    L" uses alpha masking or blending, which is not supported yet.");
            }
            if (HasUnsupportedMaterialFeature(source))
            {
                return Fail(
                    errorMessage,
                    L"Material " + std::to_wstring(materialIndex) +
                    L" uses a texture or PBR extension that is not supported yet.");
            }

            SceneMaterial material = MakeDefaultMaterial();
            if (source.has_pbr_metallic_roughness)
            {
                material.baseColor[0] = Clamp01(source.pbr_metallic_roughness.base_color_factor[0]);
                material.baseColor[1] = Clamp01(source.pbr_metallic_roughness.base_color_factor[1]);
                material.baseColor[2] = Clamp01(source.pbr_metallic_roughness.base_color_factor[2]);
                material.metallic = Clamp01(source.pbr_metallic_roughness.metallic_factor);
                material.roughness = Clamp01(source.pbr_metallic_roughness.roughness_factor);

                if (!FindOrLoadTexture(
                    source.pbr_metallic_roughness.base_color_texture,
                    true,
                    gltfPath,
                    scene,
                    loadedTextures,
                    material.baseColorTextureIndex,
                    errorMessage) ||
                    !FindOrLoadTexture(
                        source.pbr_metallic_roughness.metallic_roughness_texture,
                        false,
                        gltfPath,
                        scene,
                        loadedTextures,
                        material.metallicRoughnessTextureIndex,
                        errorMessage))
                {
                    return false;
                }
            }

            if (!FindOrLoadTexture(
                source.normal_texture,
                false,
                gltfPath,
                scene,
                loadedTextures,
                material.normalTextureIndex,
                errorMessage))
            {
                return false;
            }
            material.normalTextureScale = source.normal_texture.texture
                ? source.normal_texture.scale
                : 1.0f;

            const float emissiveStrength = source.has_emissive_strength
                ? std::max(source.emissive_strength.emissive_strength, 0.0f)
                : 1.0f;
            material.emission[0] = std::max(source.emissive_factor[0], 0.0f) * emissiveStrength;
            material.emission[1] = std::max(source.emissive_factor[1], 0.0f) * emissiveStrength;
            material.emission[2] = std::max(source.emissive_factor[2], 0.0f) * emissiveStrength;
            material.pbrParameterMode = c_pbrParameterModeFixed;
            scene.materials.push_back(material);
        }

        if (scene.materials.size() >= std::numeric_limits<std::uint32_t>::max())
            return Fail(errorMessage, L"The glTF contains too many materials.");

        defaultMaterialIndex = static_cast<std::uint32_t>(scene.materials.size());
        scene.materials.push_back(MakeDefaultMaterial());
        return true;
    }

    bool BuildNodeTransform(
        const cgltf_node& node,
        NodeTransform& transform,
        std::wstring& errorMessage)
    {
        cgltf_node_transform_world(&node, transform.world);

        const float a00 = transform.world[0];
        const float a01 = transform.world[4];
        const float a02 = transform.world[8];
        const float a10 = transform.world[1];
        const float a11 = transform.world[5];
        const float a12 = transform.world[9];
        const float a20 = transform.world[2];
        const float a21 = transform.world[6];
        const float a22 = transform.world[10];

        const float c00 = a11 * a22 - a12 * a21;
        const float c01 = a12 * a20 - a10 * a22;
        const float c02 = a10 * a21 - a11 * a20;
        const float c10 = a02 * a21 - a01 * a22;
        const float c11 = a00 * a22 - a02 * a20;
        const float c12 = a01 * a20 - a00 * a21;
        const float c20 = a01 * a12 - a02 * a11;
        const float c21 = a02 * a10 - a00 * a12;
        const float c22 = a00 * a11 - a01 * a10;
        const float determinant = a00 * c00 + a01 * c01 + a02 * c02;
        if (std::fabs(determinant) <= c_transformDeterminantEpsilon)
            return Fail(errorMessage, L"A mesh node has a singular transform.");
        transform.determinant = determinant;

        const float inverseDeterminant = 1.0f / determinant;
        transform.normal[0] = c00 * inverseDeterminant;
        transform.normal[1] = c01 * inverseDeterminant;
        transform.normal[2] = c02 * inverseDeterminant;
        transform.normal[3] = c10 * inverseDeterminant;
        transform.normal[4] = c11 * inverseDeterminant;
        transform.normal[5] = c12 * inverseDeterminant;
        transform.normal[6] = c20 * inverseDeterminant;
        transform.normal[7] = c21 * inverseDeterminant;
        transform.normal[8] = c22 * inverseDeterminant;
        return true;
    }

    SceneVertex TransformVertex(
        const float position[3],
        const float normal[3],
        const float texCoord[2],
        const float tangent[4],
        bool hasTangent,
        const NodeTransform& transform)
    {
        SceneVertex vertex = {};
        vertex.position[0] = transform.world[0] * position[0] +
            transform.world[4] * position[1] +
            transform.world[8] * position[2] +
            transform.world[12];
        vertex.position[1] = transform.world[1] * position[0] +
            transform.world[5] * position[1] +
            transform.world[9] * position[2] +
            transform.world[13];
        vertex.position[2] = -(transform.world[2] * position[0] +
            transform.world[6] * position[1] +
            transform.world[10] * position[2] +
            transform.world[14]);

        float transformedNormal[3] =
        {
            transform.normal[0] * normal[0] + transform.normal[1] * normal[1] + transform.normal[2] * normal[2],
            transform.normal[3] * normal[0] + transform.normal[4] * normal[1] + transform.normal[5] * normal[2],
            transform.normal[6] * normal[0] + transform.normal[7] * normal[1] + transform.normal[8] * normal[2]
        };
        const float lengthSquared =
            transformedNormal[0] * transformedNormal[0] +
            transformedNormal[1] * transformedNormal[1] +
            transformedNormal[2] * transformedNormal[2];
        if (lengthSquared > c_normalLengthEpsilon)
        {
            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            vertex.normal[0] = transformedNormal[0] * inverseLength;
            vertex.normal[1] = transformedNormal[1] * inverseLength;
            vertex.normal[2] = -transformedNormal[2] * inverseLength;
        }

        vertex.texCoord[0] = texCoord[0];
        vertex.texCoord[1] = texCoord[1];

        if (hasTangent)
        {
            float transformedTangent[3] =
            {
                transform.world[0] * tangent[0] + transform.world[4] * tangent[1] + transform.world[8] * tangent[2],
                transform.world[1] * tangent[0] + transform.world[5] * tangent[1] + transform.world[9] * tangent[2],
                -(transform.world[2] * tangent[0] + transform.world[6] * tangent[1] + transform.world[10] * tangent[2])
            };
            const float normalProjection =
                transformedTangent[0] * vertex.normal[0] +
                transformedTangent[1] * vertex.normal[1] +
                transformedTangent[2] * vertex.normal[2];
            transformedTangent[0] -= vertex.normal[0] * normalProjection;
            transformedTangent[1] -= vertex.normal[1] * normalProjection;
            transformedTangent[2] -= vertex.normal[2] * normalProjection;
            const float tangentLengthSquared =
                transformedTangent[0] * transformedTangent[0] +
                transformedTangent[1] * transformedTangent[1] +
                transformedTangent[2] * transformedTangent[2];
            if (tangentLengthSquared > c_normalLengthEpsilon)
            {
                const float inverseLength = 1.0f / std::sqrt(tangentLengthSquared);
                vertex.tangent[0] = transformedTangent[0] * inverseLength;
                vertex.tangent[1] = transformedTangent[1] * inverseLength;
                vertex.tangent[2] = transformedTangent[2] * inverseLength;
                vertex.tangent[3] = tangent[3] * (transform.determinant < 0.0f ? 1.0f : -1.0f);
            }
        }
        return vertex;
    }

    void GenerateTangents(
        SceneData& scene,
        std::uint32_t baseVertex,
        cgltf_size vertexCount,
        std::size_t indexStart,
        cgltf_size indexCount)
    {
        std::vector<float> tangentSums(vertexCount * 3, 0.0f);
        std::vector<float> bitangentSums(vertexCount * 3, 0.0f);
        for (cgltf_size triangleOffset = 0; triangleOffset < indexCount; triangleOffset += 3)
        {
            const std::uint32_t indices[3] =
            {
                scene.indices[indexStart + triangleOffset + 0] - baseVertex,
                scene.indices[indexStart + triangleOffset + 1] - baseVertex,
                scene.indices[indexStart + triangleOffset + 2] - baseVertex
            };
            const SceneVertex& v0 = scene.vertices[baseVertex + indices[0]];
            const SceneVertex& v1 = scene.vertices[baseVertex + indices[1]];
            const SceneVertex& v2 = scene.vertices[baseVertex + indices[2]];

            const float edge1[3] =
            {
                v1.position[0] - v0.position[0],
                v1.position[1] - v0.position[1],
                v1.position[2] - v0.position[2]
            };
            const float edge2[3] =
            {
                v2.position[0] - v0.position[0],
                v2.position[1] - v0.position[1],
                v2.position[2] - v0.position[2]
            };
            const float du1 = v1.texCoord[0] - v0.texCoord[0];
            const float dv1 = v1.texCoord[1] - v0.texCoord[1];
            const float du2 = v2.texCoord[0] - v0.texCoord[0];
            const float dv2 = v2.texCoord[1] - v0.texCoord[1];
            const float determinant = du1 * dv2 - du2 * dv1;
            if (std::fabs(determinant) <= c_transformDeterminantEpsilon)
                continue;

            const float inverseDeterminant = 1.0f / determinant;
            const float tangent[3] =
            {
                (edge1[0] * dv2 - edge2[0] * dv1) * inverseDeterminant,
                (edge1[1] * dv2 - edge2[1] * dv1) * inverseDeterminant,
                (edge1[2] * dv2 - edge2[2] * dv1) * inverseDeterminant
            };
            const float bitangent[3] =
            {
                (edge2[0] * du1 - edge1[0] * du2) * inverseDeterminant,
                (edge2[1] * du1 - edge1[1] * du2) * inverseDeterminant,
                (edge2[2] * du1 - edge1[2] * du2) * inverseDeterminant
            };
            for (std::uint32_t localIndex : indices)
            {
                for (int component = 0; component < 3; ++component)
                {
                    tangentSums[localIndex * 3 + component] += tangent[component];
                    bitangentSums[localIndex * 3 + component] += bitangent[component];
                }
            }
        }

        for (cgltf_size localIndex = 0; localIndex < vertexCount; ++localIndex)
        {
            SceneVertex& vertex = scene.vertices[baseVertex + localIndex];
            const float* normal = vertex.normal;
            float tangent[3] =
            {
                tangentSums[localIndex * 3 + 0],
                tangentSums[localIndex * 3 + 1],
                tangentSums[localIndex * 3 + 2]
            };
            const float normalProjection =
                tangent[0] * normal[0] + tangent[1] * normal[1] + tangent[2] * normal[2];
            tangent[0] -= normal[0] * normalProjection;
            tangent[1] -= normal[1] * normalProjection;
            tangent[2] -= normal[2] * normalProjection;
            float tangentLengthSquared =
                tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2];
            if (tangentLengthSquared <= c_normalLengthEpsilon)
            {
                if (std::fabs(normal[2]) < 0.999f)
                {
                    tangent[0] = -normal[1];
                    tangent[1] = normal[0];
                    tangent[2] = 0.0f;
                }
                else
                {
                    tangent[0] = 0.0f;
                    tangent[1] = -normal[2];
                    tangent[2] = normal[1];
                }
                tangentLengthSquared =
                    tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2];
            }
            const float inverseLength = 1.0f / std::sqrt(tangentLengthSquared);
            vertex.tangent[0] = tangent[0] * inverseLength;
            vertex.tangent[1] = tangent[1] * inverseLength;
            vertex.tangent[2] = tangent[2] * inverseLength;

            const float crossNormalTangent[3] =
            {
                normal[1] * vertex.tangent[2] - normal[2] * vertex.tangent[1],
                normal[2] * vertex.tangent[0] - normal[0] * vertex.tangent[2],
                normal[0] * vertex.tangent[1] - normal[1] * vertex.tangent[0]
            };
            const float handedness =
                crossNormalTangent[0] * bitangentSums[localIndex * 3 + 0] +
                crossNormalTangent[1] * bitangentSums[localIndex * 3 + 1] +
                crossNormalTangent[2] * bitangentSums[localIndex * 3 + 2];
            vertex.tangent[3] = handedness < 0.0f ? -1.0f : 1.0f;
        }
    }

    bool AppendPrimitive(
        const cgltf_data& data,
        const cgltf_primitive& primitive,
        const NodeTransform& transform,
        std::uint32_t defaultMaterialIndex,
        SceneData& scene,
        std::wstring& errorMessage)
    {
        if (primitive.type != cgltf_primitive_type_triangles)
            return Fail(errorMessage, L"Only triangle-list glTF primitives are supported.");
        if (primitive.has_draco_mesh_compression)
            return Fail(errorMessage, L"Draco-compressed glTF primitives are not supported yet.");
        if (primitive.targets_count > 0)
            return Fail(errorMessage, L"Morph targets are not supported yet.");

        std::uint32_t materialIndex = defaultMaterialIndex;
        if (primitive.material)
        {
            const std::ptrdiff_t sourceMaterialIndex = primitive.material - data.materials;
            if (sourceMaterialIndex < 0 ||
                static_cast<cgltf_size>(sourceMaterialIndex) >= data.materials_count)
            {
                return Fail(errorMessage, L"A primitive references an invalid material.");
            }
            materialIndex = static_cast<std::uint32_t>(sourceMaterialIndex);
        }
        const SceneMaterial& material = scene.materials[materialIndex];
        const bool requiresTexCoord =
            material.baseColorTextureIndex != c_invalidSceneTextureIndex ||
            material.metallicRoughnessTextureIndex != c_invalidSceneTextureIndex ||
            material.normalTextureIndex != c_invalidSceneTextureIndex;
        const bool requiresTangent =
            material.normalTextureIndex != c_invalidSceneTextureIndex;

        const cgltf_accessor* positions = cgltf_find_accessor(
            &primitive,
            cgltf_attribute_type_position,
            0);
        const cgltf_accessor* normals = cgltf_find_accessor(
            &primitive,
            cgltf_attribute_type_normal,
            0);
        const cgltf_accessor* texCoords = cgltf_find_accessor(
            &primitive,
            cgltf_attribute_type_texcoord,
            0);
        const cgltf_accessor* tangents = cgltf_find_accessor(
            &primitive,
            cgltf_attribute_type_tangent,
            0);
        if (!positions || !normals)
            return Fail(errorMessage, L"Each primitive must provide POSITION and NORMAL attributes.");
        if (positions->type != cgltf_type_vec3 ||
            normals->type != cgltf_type_vec3 ||
            positions->count != normals->count)
        {
            return Fail(errorMessage, L"POSITION and NORMAL must be matching VEC3 accessors.");
        }
        if (requiresTexCoord && !texCoords)
            return Fail(errorMessage, L"A textured primitive must provide TEXCOORD_0.");
        if (texCoords &&
            (texCoords->type != cgltf_type_vec2 || texCoords->count != positions->count))
        {
            return Fail(errorMessage, L"TEXCOORD_0 must be a VEC2 accessor matching POSITION.");
        }
        if (tangents &&
            (tangents->type != cgltf_type_vec4 || tangents->count != positions->count))
        {
            return Fail(errorMessage, L"TANGENT must be a VEC4 accessor matching POSITION.");
        }
        if (positions->count == 0 ||
            scene.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
            positions->count >
                std::numeric_limits<std::uint32_t>::max() - scene.vertices.size())
        {
            return Fail(errorMessage, L"A primitive has an invalid or excessively large vertex count.");
        }

        const std::uint32_t baseVertex = static_cast<std::uint32_t>(scene.vertices.size());
        scene.vertices.reserve(scene.vertices.size() + positions->count);
        for (cgltf_size vertexIndex = 0; vertexIndex < positions->count; ++vertexIndex)
        {
            float position[3] = {};
            float normal[3] = {};
            float texCoord[2] = {};
            float tangent[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
            if (!cgltf_accessor_read_float(positions, vertexIndex, position, 3) ||
                !cgltf_accessor_read_float(normals, vertexIndex, normal, 3) ||
                (texCoords && !cgltf_accessor_read_float(texCoords, vertexIndex, texCoord, 2)) ||
                (tangents && !cgltf_accessor_read_float(tangents, vertexIndex, tangent, 4)))
            {
                return Fail(errorMessage, L"Failed to read a vertex attribute accessor.");
            }

            const SceneVertex vertex = TransformVertex(
                position,
                normal,
                texCoord,
                tangent,
                tangents != nullptr,
                transform);
            const float normalLengthSquared =
                vertex.normal[0] * vertex.normal[0] +
                vertex.normal[1] * vertex.normal[1] +
                vertex.normal[2] * vertex.normal[2];
            if (normalLengthSquared <= c_normalLengthEpsilon)
                return Fail(errorMessage, L"A primitive contains a zero-length normal.");
            scene.vertices.push_back(vertex);
        }

        const cgltf_size indexCount = primitive.indices
            ? primitive.indices->count
            : positions->count;
        if (indexCount == 0 || indexCount % 3 != 0)
            return Fail(errorMessage, L"Triangle primitive index count must be a non-zero multiple of three.");
        if (scene.indices.size() > std::numeric_limits<std::uint32_t>::max() ||
            indexCount > std::numeric_limits<std::uint32_t>::max() - scene.indices.size())
            return Fail(errorMessage, L"The glTF contains too many indices.");

        const std::size_t indexStart = scene.indices.size();
        scene.indices.reserve(scene.indices.size() + indexCount);
        for (cgltf_size triangleOffset = 0;
             triangleOffset < indexCount;
             triangleOffset += 3)
        {
            cgltf_size localIndices[3] = {};
            for (cgltf_size triangleVertex = 0; triangleVertex < 3; ++triangleVertex)
            {
                const cgltf_size index = triangleOffset + triangleVertex;
                localIndices[triangleVertex] = primitive.indices
                    ? cgltf_accessor_read_index(primitive.indices, index)
                    : index;
                if (localIndices[triangleVertex] >= positions->count ||
                    localIndices[triangleVertex] >
                        std::numeric_limits<std::uint32_t>::max() - baseVertex)
                {
                    return Fail(errorMessage, L"A primitive index is outside its POSITION accessor.");
                }
            }

            scene.indices.push_back(baseVertex + static_cast<std::uint32_t>(localIndices[0]));
            scene.indices.push_back(baseVertex + static_cast<std::uint32_t>(localIndices[2]));
            scene.indices.push_back(baseVertex + static_cast<std::uint32_t>(localIndices[1]));
        }

        if (requiresTangent && !tangents)
            GenerateTangents(scene, baseVertex, positions->count, indexStart, indexCount);

        scene.primitiveMaterialIndices.insert(
            scene.primitiveMaterialIndices.end(),
            indexCount / 3,
            materialIndex);
        return true;
    }

    bool AppendNode(
        const cgltf_data& data,
        const cgltf_node& node,
        std::uint32_t defaultMaterialIndex,
        SceneData& scene,
        std::wstring& errorMessage)
    {
        if (node.skin)
            return Fail(errorMessage, L"Skinned mesh nodes are not supported yet.");
        if (node.has_mesh_gpu_instancing)
            return Fail(errorMessage, L"GPU-instanced mesh nodes are not supported yet.");

        if (node.mesh)
        {
            NodeTransform transform;
            if (!BuildNodeTransform(node, transform, errorMessage))
                return false;

            for (cgltf_size primitiveIndex = 0;
                 primitiveIndex < node.mesh->primitives_count;
                 ++primitiveIndex)
            {
                if (!AppendPrimitive(
                    data,
                    node.mesh->primitives[primitiveIndex],
                    transform,
                    defaultMaterialIndex,
                    scene,
                    errorMessage))
                {
                    return false;
                }
            }
        }

        for (cgltf_size childIndex = 0; childIndex < node.children_count; ++childIndex)
        {
            if (!node.children[childIndex] ||
                !AppendNode(
                    data,
                    *node.children[childIndex],
                    defaultMaterialIndex,
                    scene,
                    errorMessage))
            {
                return false;
            }
        }
        return true;
    }
}

bool LoadGltfSceneData(
    const std::wstring& filePath,
    SceneData& scene,
    std::wstring& errorMessage)
{
    scene = {};
    errorMessage.clear();
    if (filePath.empty())
        return Fail(errorMessage, L"The glTF file path is empty.");

    std::string utf8Path;
    if (!WideToUtf8(filePath, utf8Path))
        return Fail(errorMessage, L"The glTF file path cannot be converted to UTF-8.");

    cgltf_options options = {};
    options.file.read = ReadFileUtf8;
    options.file.release = ReleaseFileMemory;

    cgltf_data* parsedData = nullptr;
    cgltf_result result = cgltf_parse_file(&options, utf8Path.c_str(), &parsedData);
    if (result != cgltf_result_success)
    {
        return Fail(
            errorMessage,
            L"cgltf_parse_file failed: " + std::wstring(ResultName(result)));
    }

    std::unique_ptr<cgltf_data, void(*)(cgltf_data*)> data(parsedData, cgltf_free);
    result = cgltf_load_buffers(&options, data.get(), utf8Path.c_str());
    if (result != cgltf_result_success)
    {
        return Fail(
            errorMessage,
            L"cgltf_load_buffers failed: " + std::wstring(ResultName(result)));
    }

    result = cgltf_validate(data.get());
    if (result != cgltf_result_success)
    {
        return Fail(
            errorMessage,
            L"cgltf_validate failed: " + std::wstring(ResultName(result)));
    }

    SceneData loadedScene;
    std::uint32_t defaultMaterialIndex = 0;
    if (!ConvertMaterials(
        *data,
        filePath,
        loadedScene,
        errorMessage,
        defaultMaterialIndex))
    {
        return false;
    }

    const cgltf_scene* activeScene = data->scene;
    if (!activeScene && data->scenes_count > 0)
        activeScene = &data->scenes[0];

    if (activeScene)
    {
        for (cgltf_size nodeIndex = 0;
             nodeIndex < activeScene->nodes_count;
             ++nodeIndex)
        {
            if (!activeScene->nodes[nodeIndex] ||
                !AppendNode(
                    *data,
                    *activeScene->nodes[nodeIndex],
                    defaultMaterialIndex,
                    loadedScene,
                    errorMessage))
            {
                return false;
            }
        }
    }
    else
    {
        for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex)
        {
            if (!data->nodes[nodeIndex].parent &&
                !AppendNode(
                    *data,
                    data->nodes[nodeIndex],
                    defaultMaterialIndex,
                    loadedScene,
                    errorMessage))
            {
                return false;
            }
        }
    }

    if (!loadedScene.IsValid())
        return Fail(errorMessage, L"The flattened glTF scene contains no valid triangle geometry.");

    scene = std::move(loadedScene);
    return true;
}
