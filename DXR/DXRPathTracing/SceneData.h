#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

constexpr std::uint32_t c_invalidSceneTextureIndex = 0xFFFFFFFFu;

struct SceneVertex
{
    float position[3];
    float normal[3];
    float texCoord[2];
    float tangent[4];
};

// This layout is mirrored by SceneMaterial in RaytracingCommon.hlsli.
struct SceneMaterial
{
    float baseColor[3];
    float metallic;
    float roughness;
    float emission[3];
    std::uint32_t useGlobalPbrParameters;
    std::uint32_t baseColorTextureIndex;
    std::uint32_t metallicRoughnessTextureIndex;
    std::uint32_t normalTextureIndex;
    float normalTextureScale;
};

struct SceneTexture
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t isSrgb = 0;
    std::vector<std::uint8_t> rgba8;
};

static_assert(sizeof(SceneVertex) == 48);
static_assert(offsetof(SceneVertex, texCoord) == 24);
static_assert(offsetof(SceneVertex, tangent) == 32);
static_assert(sizeof(SceneMaterial) == 52);
static_assert(offsetof(SceneMaterial, metallic) == 12);
static_assert(offsetof(SceneMaterial, roughness) == 16);
static_assert(offsetof(SceneMaterial, emission) == 20);
static_assert(offsetof(SceneMaterial, useGlobalPbrParameters) == 32);
static_assert(offsetof(SceneMaterial, baseColorTextureIndex) == 36);
static_assert(offsetof(SceneMaterial, metallicRoughnessTextureIndex) == 40);
static_assert(offsetof(SceneMaterial, normalTextureIndex) == 44);
static_assert(offsetof(SceneMaterial, normalTextureScale) == 48);

// Flat GPU-ready scene data. A model loader can populate the same structure
// after applying node transforms and flattening mesh primitives.
struct SceneData
{
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SceneMaterial> materials;
    std::vector<std::uint32_t> primitiveMaterialIndices;
    std::vector<SceneTexture> textures;

    bool IsValid() const;
};

SceneData CreateCornellBoxSceneData();
SceneData CreatePbrGgxSceneData();
