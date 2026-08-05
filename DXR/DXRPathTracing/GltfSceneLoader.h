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
    std::uint32_t loadedNodeCount = 0;
    std::uint32_t loadedAnimationCount = 0;
    std::uint32_t loadedAnimationSamplerCount = 0;
    std::uint32_t loadedAnimationChannelCount = 0;
};

// Loads a glTF 2.0 scene, preserves its node hierarchy and animation data,
// converts mesh coordinates to the renderer's left-handed coordinates, and
// currently flattens the default node transforms into SceneData geometry.
// Animation playback is connected in a later stage.
bool LoadGltfSceneData(
    const std::wstring& filePath,
    SceneData& scene,
    std::wstring& errorMessage,
    const GltfLoadOptions& options = {},
    GltfLoadReport* report = nullptr);
