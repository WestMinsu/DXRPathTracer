#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

constexpr std::uint32_t c_invalidSceneTextureIndex = 0xFFFFFFFFu;
constexpr std::uint32_t c_sceneTextureIndexMask = 0x000000FFu;
constexpr std::uint32_t c_sceneTextureMaxDimensionMask = 0x0000FFFFu;
constexpr std::uint32_t c_sceneTextureMaxDimensionShift = 8u;
constexpr std::uint32_t c_sceneTextureLastMipShift = 24u;
constexpr std::uint32_t c_invalidSceneNodeIndex = 0xFFFFFFFFu;
constexpr std::uint32_t c_invalidSceneMeshIndex = 0xFFFFFFFFu;
constexpr std::uint32_t c_invalidSceneSkinIndex = 0xFFFFFFFFu;
constexpr std::uint32_t c_pbrParameterModeFixed = 0u;
constexpr std::uint32_t c_pbrParameterModeGlobal = 1u;
constexpr std::uint32_t c_pbrParameterModeFixedNoOverride = 2u;

constexpr std::uint32_t SceneTextureDescriptorIndex(std::uint32_t metadata)
{
    return metadata & c_sceneTextureIndexMask;
}

constexpr std::uint32_t SceneTextureMaxDimension(std::uint32_t metadata)
{
    return (metadata >> c_sceneTextureMaxDimensionShift) &
        c_sceneTextureMaxDimensionMask;
}

constexpr std::uint32_t SceneTextureLastMip(std::uint32_t metadata)
{
    return metadata >> c_sceneTextureLastMipShift;
}

struct SceneVertex
{
    float position[3];
    float normal[3];
    float texCoord[2];
    float tangent[4];
};

// JOINTS_0 values are indices into the SceneSkin::jointNodeIndices array,
// not global scene-node indices. Weights are normalized by the loader.
struct SceneVertexSkinInfluence
{
    std::uint32_t jointIndices[4] = {};
    float jointWeights[4] = {};
};

// This layout is mirrored by SceneMaterial in RaytracingCommon.hlsli.
struct SceneMaterial
{
    float baseColor[3];
    float metallic;
    float roughness;
    float emission[3];
    std::uint32_t pbrParameterMode;
    // Texture fields pack descriptor index [7:0], maximum dimension [23:8],
    // and last mip level [31:24]. All ones still marks an unused texture.
    std::uint32_t baseColorTextureIndex;
    std::uint32_t metallicRoughnessTextureIndex;
    std::uint32_t normalTextureIndex;
    float normalTextureScale;
    float baseColorAlpha;
    // A negative cutoff marks an opaque material. Non-negative values use
    // glTF MASK semantics in the any-hit shader.
    float alphaCutoff;
};

struct SceneTextureMip
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8;
};

struct SceneTexture
{
    std::uint32_t isSrgb = 0;
    std::vector<SceneTextureMip> mips;
};

// This 16-byte record is mirrored by SceneMetadataEntry in
// RaytracingCommon.hlsli. Instance headers use data0/data1 as geometry
// metadata offset/flags; geometry records use data0/data1/data2 as the
// vertex/index/primitive offsets.
struct SceneMetadataEntry
{
    std::uint32_t data0 = 0;
    std::uint32_t data1 = 0;
    std::uint32_t data2 = 0;
    std::uint32_t data3 = 0;
};

enum class SceneAnimationInterpolation : std::uint32_t
{
    Linear,
    Step,
    CubicSpline
};

enum class SceneAnimationPath : std::uint32_t
{
    Translation,
    Rotation,
    Scale,
    Weights
};

// Node transforms are stored in glTF's original right-handed coordinate
// system. Geometry remains flattened into the renderer's left-handed system
// until node animation playback is connected to GPU instances.
struct SceneNode
{
    std::string name;
    std::uint32_t parentIndex = c_invalidSceneNodeIndex;
    std::vector<std::uint32_t> childIndices;
    std::uint32_t meshIndex = c_invalidSceneMeshIndex;
    std::uint32_t skinIndex = c_invalidSceneSkinIndex;
    bool activeInScene = false;
    bool hasMatrix = false;
    float translation[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
    float localTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    float worldTransform[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
};

// Matrices retain glTF's right-handed, column-major representation. They are
// converted only when GPU skinning is connected in the following stage.
struct SceneSkin
{
    std::string name;
    std::uint32_t skeletonRootNodeIndex = c_invalidSceneNodeIndex;
    std::vector<std::uint32_t> jointNodeIndices;
    std::vector<std::array<float, 16>> inverseBindMatrices;
};

// Geometry produced for one glTF primitive while the legacy loader still
// flattens node transforms into the shared vertex/index buffers. These ranges
// let the renderer build one representative BLAS per unique glTF mesh and
// instantiate it for every mesh node through a TLAS transform.
struct ScenePrimitiveRange
{
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t primitiveOffset = 0;
    bool containsAlphaMask = false;
};

struct SceneMeshNodeInstance
{
    std::uint32_t nodeIndex = c_invalidSceneNodeIndex;
    std::uint32_t meshIndex = c_invalidSceneMeshIndex;
    std::vector<ScenePrimitiveRange> primitives;
};

struct SceneAnimationSampler
{
    SceneAnimationInterpolation interpolation =
        SceneAnimationInterpolation::Linear;
    // Logical scalar count for one keyframe value: 3 for translation/scale,
    // 4 for rotation, and the morph-target count for weights.
    std::uint32_t outputComponentCount = 0;
    std::vector<float> inputTimes;
    // CUBICSPLINE stores in-tangent, value, and out-tangent for every key.
    std::vector<float> outputValues;
};

struct SceneAnimationChannel
{
    std::uint32_t samplerIndex = 0;
    std::uint32_t targetNodeIndex = c_invalidSceneNodeIndex;
    SceneAnimationPath targetPath = SceneAnimationPath::Translation;
};

struct SceneAnimation
{
    std::string name;
    std::vector<SceneAnimationSampler> samplers;
    std::vector<SceneAnimationChannel> channels;
    float startTime = 0.0f;
    float endTime = 0.0f;
};

static_assert(sizeof(SceneVertex) == 48);
static_assert(sizeof(SceneVertexSkinInfluence) == 32);
static_assert(offsetof(SceneVertex, texCoord) == 24);
static_assert(offsetof(SceneVertex, tangent) == 32);
static_assert(sizeof(SceneMaterial) == 60);
static_assert(sizeof(SceneMetadataEntry) == 16);
static_assert(offsetof(SceneMaterial, metallic) == 12);
static_assert(offsetof(SceneMaterial, roughness) == 16);
static_assert(offsetof(SceneMaterial, emission) == 20);
static_assert(offsetof(SceneMaterial, pbrParameterMode) == 32);
static_assert(offsetof(SceneMaterial, baseColorTextureIndex) == 36);
static_assert(offsetof(SceneMaterial, metallicRoughnessTextureIndex) == 40);
static_assert(offsetof(SceneMaterial, normalTextureIndex) == 44);
static_assert(offsetof(SceneMaterial, normalTextureScale) == 48);
static_assert(offsetof(SceneMaterial, baseColorAlpha) == 52);
static_assert(offsetof(SceneMaterial, alphaCutoff) == 56);

// Flat GPU-ready scene data. A model loader can populate the same structure
// after applying node transforms and flattening mesh primitives.
struct SceneData
{
    std::vector<SceneVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SceneMaterial> materials;
    std::vector<std::uint32_t> primitiveMaterialIndices;
    std::vector<SceneTexture> textures;
    std::vector<SceneNode> nodes;
    std::vector<SceneSkin> skins;
    std::vector<SceneVertexSkinInfluence> vertexSkinInfluences;
    std::vector<SceneMeshNodeInstance> meshNodeInstances;
    std::vector<std::uint32_t> rootNodeIndices;
    std::vector<SceneAnimation> animations;

    bool IsValid() const;
};

struct SceneBounds
{
    float minimum[3];
    float maximum[3];
};

struct SceneAreaLight
{
    float position[3];
    float right[3];
    float up[3];
    float radiance[3];
    float width = 0.0f;
    float height = 0.0f;
};

SceneData CreateCornellBoxSceneData();
SceneData CreateIndirectBounceStressSceneData();
SceneData CreatePbrGgxSceneData();
SceneData CreateRollingMetalSphereSceneData(float radius);
SceneData CreateDynamicTransformTestRoomSceneData();
SceneData CreateDynamicTransformTestSphereSceneData(
    float radius,
    std::uint32_t materialPreset);
SceneData CreateDynamicTransformTestCubeSceneData(
    float halfExtent,
    std::uint32_t materialPreset);
bool ComputeSceneBounds(const SceneData& scene, SceneBounds& bounds);
bool FindWalkableSurfaceHeight(
    const SceneData& scene,
    float x,
    float z,
    float maximumHeight,
    float& height);
bool AppendPbrModelRoom(SceneData& scene, const SceneBounds& modelBounds);
bool AppendAreaLights(
    SceneData& scene,
    const std::vector<SceneAreaLight>& lights);
