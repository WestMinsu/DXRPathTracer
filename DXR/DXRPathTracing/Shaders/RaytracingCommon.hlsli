#ifndef RAYTRACING_COMMON_HLSLI
#define RAYTRACING_COMMON_HLSLI

struct Vertex
{
    float3 position;
    float3 normal;
    float2 texCoord;
    float4 tangent;
};

struct RadiancePayload
{
    float3 color;
    uint depth;
    uint dynamicTouched;
    float3 pathThroughput;
};

struct ShadowPayload
{
    uint occluded;
};

struct PbrMaterial
{
    float3 baseColor;
    float metallic;
    float roughness;
    float3 emission;
};

// Mirrored by SceneMaterial in SceneData.h.
struct SceneMaterial
{
    float3 baseColor;
    float metallic;
    float roughness;
    float3 emission;
    uint pbrParameterMode;
    uint baseColorTextureIndex;
    uint metallicRoughnessTextureIndex;
    uint normalTextureIndex;
    float normalTextureScale;
};

struct SceneInstanceMetadata
{
    uint vertexOffset;
    uint indexOffset;
    uint primitiveOffset;
    uint reserved;
};

// Mirrored by GpuEmissiveTriangle in RayTracingManager.cpp.
struct EmissiveTriangle
{
    float3 vertex0;
    float area;
    float3 edge1;
    float selectionPdf;
    float3 edge2;
    float selectionCdf;
    float3 emission;
    float padding;
};

static const uint c_sceneCornellBox = 0;
static const uint c_scenePbrGgx = 1;
static const uint c_scenePbrGpuValidation = 2;
static const uint c_sceneIndirectBounceStress = 3;
static const uint c_lightingModeBsdf = 0u;
static const uint c_lightingModeNee = 1u;
static const uint c_pbrDebugBeauty = 0;
static const uint c_pbrDebugAlbedo = 1;
static const uint c_pbrDebugMetallic = 2;
static const uint c_pbrDebugRoughness = 3;
static const uint c_pbrDebugDepth = 4;
static const uint c_pbrDebugMaterialId = 5;
static const uint c_pbrDebugNormal = 6;
static const uint c_invalidSceneTextureIndex = 0xFFFFFFFFu;
static const uint c_pbrParameterModeFixed = 0u;
static const uint c_pbrParameterModeGlobal = 1u;
static const uint c_pbrParameterModeFixedNoOverride = 2u;
static const uint c_maxMaterialTextures = 256u;
static const uint c_statisticsRayDepthCount = 9u;
static const uint c_statisticsShadowRayIndex = 9u;
static const uint c_statisticsHitIndex = 10u;
static const uint c_statisticsMissIndex = 11u;
static const uint c_russianRouletteStartBounce = 3u;
static const float c_rayTMin = 0.001f;
static const float c_rayTMax = 1000.0f;
static const float c_rayOriginBias = 0.001f;
static const float3 c_cameraUp = float3(0.0f, 1.0f, 0.0f);
static const float c_verticalFovRadians = 1.221730476f; // 70 degrees.
static const float c_pi = 3.141592654f;
static const float c_invPi = 0.318309886f;
static const float c_twoPi = 6.283185307f;

RWTexture2D<float4> g_output : register(u0);
RWTexture2D<float4> g_accumulation : register(u1);
RWStructuredBuffer<uint> g_statistics : register(u2);
RaytracingAccelerationStructure g_scene : register(t0);
StructuredBuffer<Vertex> g_vertices : register(t1);
StructuredBuffer<uint> g_indices : register(t2);
TextureCube<float4> g_environmentMap : register(t3);
StructuredBuffer<SceneMaterial> g_sceneMaterials : register(t4);
StructuredBuffer<uint> g_primitiveMaterialIndices : register(t5);
Texture2D<float4> g_materialTextures[c_maxMaterialTextures] : register(t6);
StructuredBuffer<SceneInstanceMetadata> g_instanceMetadata : register(t262);
StructuredBuffer<EmissiveTriangle> g_emissiveTriangles : register(t263);
SamplerState g_environmentSampler : register(s0);
SamplerState g_materialSampler : register(s1);

cbuffer RenderSettings : register(b0)
{
    uint g_showNormalColor;
    uint g_frameIndex;
    uint g_maxBounce;
    uint g_sampleIndex;
    uint g_enableAccumulation;
    uint g_sceneType;
    uint g_pbrDebugView;
    uint g_enableIbl;
    float g_pbrMetallic;
    float g_pbrRoughness;
    float g_iblIntensity;
    float g_exposure;
    uint g_validationSeed;
    float3 g_cameraPosition;
    float3 g_cameraTarget;
    uint g_overridePbrMaterial;
    uint g_enableStatistics;
    uint g_dynamicObjectMoved;
    uint g_enableRussianRoulette;
    uint g_lightingMode;
    uint g_emissiveTriangleCount;
};

void RecordRadianceRay(uint depth)
{
    if (g_enableStatistics != 0)
    {
        uint ignored;
        InterlockedAdd(
            g_statistics[min(depth, c_statisticsRayDepthCount - 1u)],
            1u,
            ignored);
    }
}

void RecordShadowRay()
{
    if (g_enableStatistics != 0)
    {
        uint ignored;
        InterlockedAdd(g_statistics[c_statisticsShadowRayIndex], 1u, ignored);
    }
}

void RecordSurfaceHit()
{
    if (g_enableStatistics != 0)
    {
        uint ignored;
        InterlockedAdd(g_statistics[c_statisticsHitIndex], 1u, ignored);
    }
}

void RecordRadianceMiss()
{
    if (g_enableStatistics != 0)
    {
        uint ignored;
        InterlockedAdd(g_statistics[c_statisticsMissIndex], 1u, ignored);
    }
}

uint CreateRandomSeed(uint depth, uint primitiveIndex)
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    uint seed = launchIndex.x + launchIndex.y * launchDim.x;
    uint sequenceIndex = g_enableAccumulation != 0 ? g_sampleIndex : g_frameIndex;
    seed = seed * 1973u + sequenceIndex * 9277u + depth * 26699u + primitiveIndex * 911u + 1u;
    seed ^= g_validationSeed * 0x85EBCA6Bu;
    seed ^= seed >> 16;
    seed *= 2246822519u;
    seed ^= seed >> 13;
    seed *= 3266489917u;
    seed ^= seed >> 16;
    return seed;
}

uint NextRandom(inout uint seed)
{
    seed = seed * 1664525u + 1013904223u;
    return seed;
}

float RandomFloat01(inout uint seed)
{
    return float(NextRandom(seed) & 0x00FFFFFFu) / 16777216.0f;
}

bool SurvivesRussianRoulette(
    float3 nextThroughput,
    uint nextDepth,
    inout uint seed,
    out float survivalProbability)
{
    survivalProbability = 1.0f;
    if (g_enableRussianRoulette == 0u ||
        nextDepth < c_russianRouletteStartBounce)
    {
        return true;
    }

    float3 nonNegativeThroughput = max(
        nextThroughput,
        float3(0.0f, 0.0f, 0.0f));
    float continuationWeight = max(
        nonNegativeThroughput.r,
        max(nonNegativeThroughput.g, nonNegativeThroughput.b));
    if (continuationWeight <= 0.0f)
    {
        survivalProbability = 0.0f;
        return false;
    }

    survivalProbability = clamp(continuationWeight, 0.05f, 0.95f);
    return RandomFloat01(seed) < survivalProbability;
}

float3 RandomUnitVector(inout uint seed)
{
    float z = RandomFloat01(seed) * 2.0f - 1.0f;
    float phi = RandomFloat01(seed) * c_twoPi;
    float radius = sqrt(max(0.0f, 1.0f - z * z));
    float sinPhi;
    float cosPhi;
    sincos(phi, sinPhi, cosPhi);
    return float3(radius * cosPhi, radius * sinPhi, z);
}


float3 RandomCosineHemisphereDirection(float3 normal, inout uint seed)
{
    float u0 = RandomFloat01(seed);
    float u1 = RandomFloat01(seed);
    float radius = sqrt(u0);
    float phi = u1 * c_twoPi;
    float sinPhi;
    float cosPhi;
    sincos(phi, sinPhi, cosPhi);

    float3 tangent = abs(normal.z) < 0.999f
        ? normalize(cross(float3(0.0f, 0.0f, 1.0f), normal))
        : normalize(cross(float3(0.0f, 1.0f, 0.0f), normal));
    float3 bitangent = cross(normal, tangent);
    float3 localDirection = float3(radius * cosPhi, radius * sinPhi, sqrt(max(0.0f, 1.0f - u0)));
    return normalize(tangent * localDirection.x + bitangent * localDirection.y + normal * localDirection.z);
}
float3 InterpolateNormal(uint i0, uint i1, uint i2, BuiltInTriangleIntersectionAttributes attributes)
{
    float3 barycentrics = float3(
        1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);
    float3 n0 = g_vertices[i0].normal;
    float3 n1 = g_vertices[i1].normal;
    float3 n2 = g_vertices[i2].normal;
    return normalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z);
}

float2 InterpolateTexCoord(uint i0, uint i1, uint i2, BuiltInTriangleIntersectionAttributes attributes)
{
    float3 barycentrics = float3(
        1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);
    return
        g_vertices[i0].texCoord * barycentrics.x +
        g_vertices[i1].texCoord * barycentrics.y +
        g_vertices[i2].texCoord * barycentrics.z;
}

float4 InterpolateTangent(uint i0, uint i1, uint i2, BuiltInTriangleIntersectionAttributes attributes)
{
    float3 barycentrics = float3(
        1.0f - attributes.barycentrics.x - attributes.barycentrics.y,
        attributes.barycentrics.x,
        attributes.barycentrics.y);
    float4 tangent =
        g_vertices[i0].tangent * barycentrics.x +
        g_vertices[i1].tangent * barycentrics.y +
        g_vertices[i2].tangent * barycentrics.z;
    tangent.w = tangent.w < 0.0f ? -1.0f : 1.0f;
    return tangent;
}


float3 SampleEnvironmentMap(float3 direction)
{
    return max(
        g_environmentMap.SampleLevel(g_environmentSampler, normalize(direction), 0.0f).rgb,
        float3(0.0f, 0.0f, 0.0f)) * g_iblIntensity;
}

bool SampleDirectAreaLight(
    float3 normal,
    float3 hitPosition,
    inout uint seed,
    out float3 lightDirection,
    out float3 radianceOverPdf)
{
    lightDirection = normal;
    radianceOverPdf = float3(0.0f, 0.0f, 0.0f);
    if (g_lightingMode != c_lightingModeNee ||
        g_emissiveTriangleCount == 0u)
    {
        return false;
    }

    float lightSelectionSample = RandomFloat01(seed);
    uint selectedIndex = g_emissiveTriangleCount - 1u;
    for (uint lightIndex = 0u;
         lightIndex < g_emissiveTriangleCount;
         ++lightIndex)
    {
        if (lightSelectionSample <
            g_emissiveTriangles[lightIndex].selectionCdf)
        {
            selectedIndex = lightIndex;
            break;
        }
    }

    EmissiveTriangle light = g_emissiveTriangles[selectedIndex];
    if (light.area <= 0.0f || light.selectionPdf <= 0.0f)
    {
        return false;
    }

    float squareRootSample = sqrt(RandomFloat01(seed));
    float secondSample = RandomFloat01(seed);
    float edge1Weight = squareRootSample * (1.0f - secondSample);
    float edge2Weight = squareRootSample * secondSample;
    float3 lightPosition =
        light.vertex0 +
        light.edge1 * edge1Weight +
        light.edge2 * edge2Weight;

    float3 toLight = lightPosition - hitPosition;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= c_rayTMin * c_rayTMin)
    {
        return false;
    }
    float distanceToLight = sqrt(distanceSquared);
    lightDirection = toLight / distanceToLight;

    float surfaceCosine = saturate(dot(normal, lightDirection));
    float3 lightNormal = normalize(cross(light.edge1, light.edge2));
    float lightCosine = saturate(dot(lightNormal, -lightDirection));
    if (surfaceCosine <= 0.0f || lightCosine <= 0.0f)
    {
        return false;
    }

    float areaPdf = light.selectionPdf / light.area;
    float solidAnglePdf =
        areaPdf * distanceSquared / max(lightCosine, 0.000001f);
    if (solidAnglePdf <= 0.0f)
    {
        return false;
    }

    RayDesc shadowRay;
    shadowRay.Origin = hitPosition + normal * c_rayOriginBias;
    shadowRay.Direction = lightDirection;
    shadowRay.TMin = c_rayTMin;
    shadowRay.TMax = max(
        distanceToLight - 4.0f * c_rayOriginBias,
        c_rayTMin);

    ShadowPayload shadowPayload;
    shadowPayload.occluded = 1u;
    RecordShadowRay();
    TraceRay(
        g_scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
            RAY_FLAG_FORCE_OPAQUE,
        0xFF,
        0,
        0,
        1,
        shadowRay,
        shadowPayload);
    if (shadowPayload.occluded != 0u)
    {
        return false;
    }

    radianceOverPdf = light.emission / solidAnglePdf;
    return true;
}

float3 TraceLambertianBounce(
    float3 normal,
    float3 hitPosition,
    float3 albedo,
    uint depth,
    uint primitiveIndex,
    inout uint dynamicTouched,
    float3 pathThroughput)
{
    float3 directLighting = float3(0.0f, 0.0f, 0.0f);
    uint directSeed =
        CreateRandomSeed(depth, primitiveIndex) ^ 0xA511E9B3u;
    float3 directLightDirection;
    float3 radianceOverPdf;
    if (SampleDirectAreaLight(
        normal,
        hitPosition,
        directSeed,
        directLightDirection,
        radianceOverPdf))
    {
        float nDotL = saturate(dot(normal, directLightDirection));
        directLighting =
            albedo * c_invPi * nDotL * radianceOverPdf;
    }

    if (depth >= g_maxBounce)
    {
        return directLighting;
    }

    uint seed = CreateRandomSeed(depth, primitiveIndex);

    float3 scatterDirection = RandomCosineHemisphereDirection(normal, seed);
    uint nextDepth = depth + 1u;
    float3 nextThroughput = pathThroughput * albedo;
    float survivalProbability = 1.0f;
    if (!SurvivesRussianRoulette(
        nextThroughput,
        nextDepth,
        seed,
        survivalProbability))
    {
        return directLighting;
    }
    float inverseSurvivalProbability = 1.0f / survivalProbability;

    RayDesc bounceRay;
    bounceRay.Origin = hitPosition + normal * c_rayOriginBias;
    bounceRay.Direction = scatterDirection;
    bounceRay.TMin = c_rayTMin;
    bounceRay.TMax = c_rayTMax;

    RadiancePayload bouncePayload;
    bouncePayload.color = float3(0.0f, 0.0f, 0.0f);
    bouncePayload.depth = nextDepth;
    bouncePayload.dynamicTouched = 0u;
    bouncePayload.pathThroughput =
        nextThroughput * inverseSurvivalProbability;

    RecordRadianceRay(bouncePayload.depth);
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, bouncePayload);
    dynamicTouched |= bouncePayload.dynamicTouched;
    return directLighting +
        albedo * inverseSurvivalProbability * bouncePayload.color;
}
#endif
