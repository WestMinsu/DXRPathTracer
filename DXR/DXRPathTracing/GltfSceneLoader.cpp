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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>

namespace
{
    constexpr float c_normalLengthEpsilon = 1.0e-20f;
    constexpr float c_transformDeterminantEpsilon = 1.0e-12f;

    struct NodeTransform
    {
        float world[16] = {};
        float normal[9] = {};
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

    SceneMaterial MakeDefaultMaterial()
    {
        SceneMaterial material = {};
        material.baseColor[0] = 1.0f;
        material.baseColor[1] = 1.0f;
        material.baseColor[2] = 1.0f;
        material.metallic = 1.0f;
        material.roughness = 1.0f;
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

        return material.normal_texture.texture ||
            material.occlusion_texture.texture ||
            material.emissive_texture.texture ||
            (material.has_pbr_metallic_roughness &&
             (material.pbr_metallic_roughness.base_color_texture.texture ||
              material.pbr_metallic_roughness.metallic_roughness_texture.texture));
    }

    bool ConvertMaterials(
        const cgltf_data& data,
        SceneData& scene,
        std::wstring& errorMessage,
        std::uint32_t& defaultMaterialIndex)
    {
        scene.materials.reserve(data.materials_count + 1);
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
            }

            const float emissiveStrength = source.has_emissive_strength
                ? std::max(source.emissive_strength.emissive_strength, 0.0f)
                : 1.0f;
            material.emission[0] = std::max(source.emissive_factor[0], 0.0f) * emissiveStrength;
            material.emission[1] = std::max(source.emissive_factor[1], 0.0f) * emissiveStrength;
            material.emission[2] = std::max(source.emissive_factor[2], 0.0f) * emissiveStrength;
            material.useGlobalPbrParameters = 0;
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
        return vertex;
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

        const cgltf_accessor* positions = cgltf_find_accessor(
            &primitive,
            cgltf_attribute_type_position,
            0);
        const cgltf_accessor* normals = cgltf_find_accessor(
            &primitive,
            cgltf_attribute_type_normal,
            0);
        if (!positions || !normals)
            return Fail(errorMessage, L"Each primitive must provide POSITION and NORMAL attributes.");
        if (positions->type != cgltf_type_vec3 ||
            normals->type != cgltf_type_vec3 ||
            positions->count != normals->count)
        {
            return Fail(errorMessage, L"POSITION and NORMAL must be matching VEC3 accessors.");
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
            if (!cgltf_accessor_read_float(positions, vertexIndex, position, 3) ||
                !cgltf_accessor_read_float(normals, vertexIndex, normal, 3))
            {
                return Fail(errorMessage, L"Failed to read a POSITION or NORMAL accessor.");
            }

            const SceneVertex vertex = TransformVertex(position, normal, transform);
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
