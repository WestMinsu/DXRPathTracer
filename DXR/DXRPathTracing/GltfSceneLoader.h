#pragma once

#include <string>

#include "SceneData.h"

struct GltfLoadOptions
{
    bool skipNonOpaquePrimitives = false;
};

struct GltfLoadReport
{
    std::uint32_t sourcePrimitiveCount = 0;
    std::uint32_t loadedPrimitiveCount = 0;
    std::uint32_t skippedNonOpaquePrimitiveCount = 0;
    std::uint32_t sourceMaterialCount = 0;
    std::uint32_t ignoredOcclusionTextureCount = 0;
    std::uint32_t loadedMaterialCount = 0;
    std::uint32_t loadedTextureCount = 0;
};

// Loads a static glTF 2.0 scene, converts its right-handed coordinates to the
// renderer's left-handed coordinates, and flattens node transforms into SceneData.
// The first stage supports triangle meshes with POSITION, NORMAL, indices,
// and constant metallic-roughness material factors.
bool LoadGltfSceneData(
    const std::wstring& filePath,
    SceneData& scene,
    std::wstring& errorMessage,
    const GltfLoadOptions& options = {},
    GltfLoadReport* report = nullptr);
