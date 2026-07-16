#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct SceneVertex
{
    float position[3];
    float normal[3];
};

// This layout is mirrored by SceneMaterial in RaytracingCommon.hlsli.
struct SceneMaterial
{
    float baseColor[3];
    float metallic;
    float roughness;
    float emission[3];
    std::uint32_t useGlobalPbrParameters;
};

static_assert(sizeof(SceneVertex) == 24);
static_assert(sizeof(SceneMaterial) == 36);
static_assert(offsetof(SceneMaterial, metallic) == 12);
static_assert(offsetof(SceneMaterial, roughness) == 16);
static_assert(offsetof(SceneMaterial, emission) == 20);
static_assert(offsetof(SceneMaterial, useGlobalPbrParameters) == 32);

// Flat GPU-ready scene data. A model loader can populate the same structure
// after applying node transforms and flattening mesh primitives.
struct SceneData
{
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SceneMaterial> materials;
    std::vector<std::uint32_t> primitiveMaterialIndices;

    bool IsValid() const;
};

SceneData CreateCornellBoxSceneData();
SceneData CreatePbrGgxSceneData();
