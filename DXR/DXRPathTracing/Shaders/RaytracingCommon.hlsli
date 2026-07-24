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
    float3 primaryDirectColor;
    float3 primaryDiffuseIndirectColor;
    float3 primarySpecularIndirectColor;
    uint depth;
    uint dynamicTouched;
    float3 pathThroughput;
    float previousBsdfPdf;
    uint previousWasDelta;
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
    float baseColorAlpha;
    float alphaCutoff;
};

struct SceneInstanceMetadata
{
    uint vertexOffset;
    uint indexOffset;
    uint primitiveOffset;
    uint flags;
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
    uint primitiveIndex;
};

// Mirrored by GpuEnvironmentAliasEntry in RayTracingManager.cpp.
struct EnvironmentAliasEntry
{
    float acceptProbability;
    uint aliasIndex;
    float selectionPdf;
    uint padding;
};

static const uint c_sceneCornellBox = 0;
static const uint c_scenePbrGgx = 1;
static const uint c_scenePbrGpuValidation = 2;
static const uint c_sceneIndirectBounceStress = 3;
static const uint c_lightingModeBsdf = 0u;
static const uint c_lightingModeNee = 1u;
static const uint c_lightingModeMis = 2u;
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
static const uint c_instanceFlagDynamic = 1u;
static const uint c_primaryGuideRayDepth = 0xFFFFFFFFu;
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
RWTexture2D<float4> g_normalDepth : register(u3);
RWTexture2D<float4> g_materialGuide : register(u4);
RWTexture2D<float4> g_diffuseIndirectAccumulation : register(u5);
RWTexture2D<float4> g_specularIndirectAccumulation : register(u6);
RWTexture2D<float2> g_diffuseLuminanceMoments : register(u7);
RWTexture2D<float2> g_specularLuminanceMoments : register(u8);
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
StructuredBuffer<EnvironmentAliasEntry>
    g_environmentDistribution : register(t264);
Texture2D<float4> g_previousAccumulation : register(t265);
Texture2D<float4> g_previousNormalDepth : register(t266);
Texture2D<float4> g_previousMaterialGuide : register(t267);
Texture2D<float4> g_previousDiffuseIndirect : register(t268);
Texture2D<float4> g_previousSpecularIndirect : register(t269);
Texture2D<float2> g_previousDiffuseMoments : register(t270);
Texture2D<float2> g_previousSpecularMoments : register(t271);
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
    uint g_environmentResolution;
    uint g_environmentTexelCount;
    float g_areaLightPower;
    float g_environmentPower;
    uint g_enableAtrous;
    uint2 g_temporalPadding0;
    float3 g_previousCameraPosition;
    uint g_temporalPadding1;
    float3 g_previousCameraTarget;
    uint g_enableTemporalReprojection;
    uint g_temporalDebugView;
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

float PowerHeuristic(float sampledPdf, float otherPdf)
{
    float sampledPdfSquared = sampledPdf * sampledPdf;
    float otherPdfSquared = otherPdf * otherPdf;
    return sampledPdfSquared /
        max(sampledPdfSquared + otherPdfSquared, 0.000000000001f);
}

void EmitterSelectionProbabilities(
    out float areaEmitterPdf,
    out float environmentEmitterPdf)
{
    float areaWeight = g_emissiveTriangleCount > 0u
        ? max(g_areaLightPower, 0.0f)
        : 0.0f;
    float environmentWeight =
        g_sceneType == c_scenePbrGgx &&
        g_enableIbl != 0u &&
        g_environmentTexelCount > 0u
        ? max(g_environmentPower * g_iblIntensity, 0.0f)
        : 0.0f;
    float totalWeight = areaWeight + environmentWeight;
    if (totalWeight <= 0.0f)
    {
        areaEmitterPdf = 0.0f;
        environmentEmitterPdf = 0.0f;
        return;
    }

    areaEmitterPdf = areaWeight / totalWeight;
    environmentEmitterPdf = environmentWeight / totalWeight;
}

float EvaluateAreaLightPdf(
    uint primitiveIndex,
    float distanceSquared,
    float3 lightDirection)
{
    float areaEmitterPdf;
    float environmentEmitterPdf;
    EmitterSelectionProbabilities(
        areaEmitterPdf,
        environmentEmitterPdf);
    if (areaEmitterPdf <= 0.0f ||
        g_emissiveTriangleCount == 0u ||
        distanceSquared <= 0.0f)
    {
        return 0.0f;
    }

    for (uint lightIndex = 0u;
         lightIndex < g_emissiveTriangleCount;
         ++lightIndex)
    {
        EmissiveTriangle light = g_emissiveTriangles[lightIndex];
        if (light.primitiveIndex != primitiveIndex)
        {
            continue;
        }

        float3 lightNormal = normalize(cross(light.edge1, light.edge2));
        float lightCosine = saturate(dot(lightNormal, -lightDirection));
        if (light.area <= 0.0f ||
            light.selectionPdf <= 0.0f ||
            lightCosine <= 0.0f)
        {
            return 0.0f;
        }

        float areaPdf = light.selectionPdf / light.area;
        return areaEmitterPdf * areaPdf * distanceSquared /
            max(lightCosine, 0.000001f);
    }

    return 0.0f;
}

bool SampleDirectAreaLight(
    float3 normal,
    float3 hitPosition,
    float areaEmitterPdf,
    inout uint seed,
    out float3 lightDirection,
    out float3 radianceOverPdf,
    out float lightPdf)
{
    lightDirection = normal;
    radianceOverPdf = float3(0.0f, 0.0f, 0.0f);
    lightPdf = 0.0f;
    if (areaEmitterPdf <= 0.0f ||
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
    lightPdf =
        areaEmitterPdf * areaPdf * distanceSquared /
        max(lightCosine, 0.000001f);
    if (lightPdf <= 0.0f)
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
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
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

    radianceOverPdf = light.emission / lightPdf;
    return true;
}

float3 CubemapFaceDirection(
    uint face,
    float faceU,
    float faceV)
{
    if (face == 0u)
        return normalize(float3(1.0f, -faceV, -faceU));
    if (face == 1u)
        return normalize(float3(-1.0f, -faceV, faceU));
    if (face == 2u)
        return normalize(float3(faceU, 1.0f, faceV));
    if (face == 3u)
        return normalize(float3(faceU, -1.0f, -faceV));
    if (face == 4u)
        return normalize(float3(faceU, -faceV, 1.0f));
    return normalize(float3(-faceU, -faceV, -1.0f));
}

void DirectionToCubemapFace(
    float3 direction,
    out uint face,
    out float faceU,
    out float faceV)
{
    float3 unitDirection = normalize(direction);
    float3 absoluteDirection = abs(unitDirection);
    if (absoluteDirection.x >= absoluteDirection.y &&
        absoluteDirection.x >= absoluteDirection.z)
    {
        if (unitDirection.x >= 0.0f)
        {
            face = 0u;
            faceU = -unitDirection.z / absoluteDirection.x;
            faceV = -unitDirection.y / absoluteDirection.x;
        }
        else
        {
            face = 1u;
            faceU = unitDirection.z / absoluteDirection.x;
            faceV = -unitDirection.y / absoluteDirection.x;
        }
    }
    else if (absoluteDirection.y >= absoluteDirection.z)
    {
        if (unitDirection.y >= 0.0f)
        {
            face = 2u;
            faceU = unitDirection.x / absoluteDirection.y;
            faceV = unitDirection.z / absoluteDirection.y;
        }
        else
        {
            face = 3u;
            faceU = unitDirection.x / absoluteDirection.y;
            faceV = -unitDirection.z / absoluteDirection.y;
        }
    }
    else
    {
        if (unitDirection.z >= 0.0f)
        {
            face = 4u;
            faceU = unitDirection.x / absoluteDirection.z;
            faceV = -unitDirection.y / absoluteDirection.z;
        }
        else
        {
            face = 5u;
            faceU = -unitDirection.x / absoluteDirection.z;
            faceV = -unitDirection.y / absoluteDirection.z;
        }
    }
}

float EnvironmentConditionalPdf(
    float selectionPdf,
    float faceU,
    float faceV)
{
    float texelUvPdf =
        float(g_environmentResolution * g_environmentResolution) * 0.25f;
    float solidAngleJacobianInverse = pow(
        1.0f + faceU * faceU + faceV * faceV,
        1.5f);
    return selectionPdf * texelUvPdf * solidAngleJacobianInverse;
}

float EvaluateEnvironmentLightPdf(float3 direction)
{
    float areaEmitterPdf;
    float environmentEmitterPdf;
    EmitterSelectionProbabilities(
        areaEmitterPdf,
        environmentEmitterPdf);
    if (environmentEmitterPdf <= 0.0f ||
        g_environmentResolution == 0u ||
        g_environmentTexelCount == 0u)
    {
        return 0.0f;
    }

    uint face;
    float faceU;
    float faceV;
    DirectionToCubemapFace(direction, face, faceU, faceV);
    float2 texturePosition = saturate(
        float2(faceU, faceV) * 0.5f + 0.5f);
    uint2 texel = min(
        uint2(texturePosition * float(g_environmentResolution)),
        uint2(
            g_environmentResolution - 1u,
            g_environmentResolution - 1u));
    uint faceTexelCount =
        g_environmentResolution * g_environmentResolution;
    uint texelIndex =
        face * faceTexelCount +
        texel.y * g_environmentResolution +
        texel.x;
    float selectionPdf =
        g_environmentDistribution[texelIndex].selectionPdf;
    return environmentEmitterPdf * EnvironmentConditionalPdf(
        selectionPdf,
        faceU,
        faceV);
}

bool SampleDirectEnvironmentLight(
    float3 normal,
    float3 hitPosition,
    float environmentEmitterPdf,
    inout uint seed,
    out float3 lightDirection,
    out float3 radianceOverPdf,
    out float lightPdf)
{
    lightDirection = normal;
    radianceOverPdf = float3(0.0f, 0.0f, 0.0f);
    lightPdf = 0.0f;
    if (environmentEmitterPdf <= 0.0f ||
        g_environmentResolution == 0u ||
        g_environmentTexelCount == 0u)
    {
        return false;
    }

    float aliasSample =
        RandomFloat01(seed) * float(g_environmentTexelCount);
    uint bucketIndex = min(
        uint(aliasSample),
        g_environmentTexelCount - 1u);
    EnvironmentAliasEntry bucket =
        g_environmentDistribution[bucketIndex];
    uint selectedIndex = frac(aliasSample) <
        bucket.acceptProbability
        ? bucketIndex
        : min(bucket.aliasIndex, g_environmentTexelCount - 1u);
    EnvironmentAliasEntry selected =
        g_environmentDistribution[selectedIndex];

    uint faceTexelCount =
        g_environmentResolution * g_environmentResolution;
    uint face = selectedIndex / faceTexelCount;
    uint faceIndex = selectedIndex - face * faceTexelCount;
    uint texelY = faceIndex / g_environmentResolution;
    uint texelX = faceIndex - texelY * g_environmentResolution;
    float faceU =
        -1.0f +
        2.0f *
        (float(texelX) + RandomFloat01(seed)) /
        float(g_environmentResolution);
    float faceV =
        -1.0f +
        2.0f *
        (float(texelY) + RandomFloat01(seed)) /
        float(g_environmentResolution);
    lightDirection = CubemapFaceDirection(face, faceU, faceV);

    float surfaceCosine = saturate(dot(normal, lightDirection));
    if (surfaceCosine <= 0.0f)
    {
        return false;
    }

    lightPdf = environmentEmitterPdf *
        EnvironmentConditionalPdf(
            selected.selectionPdf,
            faceU,
            faceV);
    if (lightPdf <= 0.0f)
    {
        return false;
    }

    RayDesc shadowRay;
    shadowRay.Origin = hitPosition + normal * c_rayOriginBias;
    shadowRay.Direction = lightDirection;
    shadowRay.TMin = c_rayTMin;
    shadowRay.TMax = c_rayTMax;

    ShadowPayload shadowPayload;
    shadowPayload.occluded = 1u;
    RecordShadowRay();
    TraceRay(
        g_scene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
            RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
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

    radianceOverPdf =
        SampleEnvironmentMap(lightDirection) / lightPdf;
    return true;
}

bool SampleDirectLight(
    float3 normal,
    float3 hitPosition,
    inout uint seed,
    out float3 lightDirection,
    out float3 radianceOverPdf,
    out float lightPdf)
{
    lightDirection = normal;
    radianceOverPdf = float3(0.0f, 0.0f, 0.0f);
    lightPdf = 0.0f;
    if (g_lightingMode != c_lightingModeNee &&
        g_lightingMode != c_lightingModeMis)
    {
        return false;
    }

    float areaEmitterPdf;
    float environmentEmitterPdf;
    EmitterSelectionProbabilities(
        areaEmitterPdf,
        environmentEmitterPdf);
    if (areaEmitterPdf + environmentEmitterPdf <= 0.0f)
    {
        return false;
    }

    if (RandomFloat01(seed) < environmentEmitterPdf)
    {
        return SampleDirectEnvironmentLight(
            normal,
            hitPosition,
            environmentEmitterPdf,
            seed,
            lightDirection,
            radianceOverPdf,
            lightPdf);
    }

    return SampleDirectAreaLight(
        normal,
        hitPosition,
        areaEmitterPdf,
        seed,
        lightDirection,
        radianceOverPdf,
        lightPdf);
}

float3 TraceLambertianBounce(
    float3 normal,
    float3 hitPosition,
    float3 albedo,
    uint depth,
    uint primitiveIndex,
    inout uint dynamicTouched,
    float3 pathThroughput,
    out float3 localDirectLighting)
{
    float3 directLighting = float3(0.0f, 0.0f, 0.0f);
    uint directSeed =
        CreateRandomSeed(depth, primitiveIndex) ^ 0xA511E9B3u;
    float3 directLightDirection;
    float3 radianceOverPdf;
    float lightPdf;
    if (SampleDirectLight(
        normal,
        hitPosition,
        directSeed,
        directLightDirection,
        radianceOverPdf,
        lightPdf))
    {
        float nDotL = saturate(dot(normal, directLightDirection));
        float bsdfPdf = nDotL * c_invPi;
        float misWeight = g_lightingMode == c_lightingModeMis
            ? PowerHeuristic(lightPdf, bsdfPdf)
            : 1.0f;
        directLighting =
            albedo * c_invPi * nDotL * radianceOverPdf * misWeight;
    }
    localDirectLighting = directLighting;

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
    bouncePayload.primaryDirectColor = float3(0.0f, 0.0f, 0.0f);
    bouncePayload.primaryDiffuseIndirectColor =
        float3(0.0f, 0.0f, 0.0f);
    bouncePayload.primarySpecularIndirectColor =
        float3(0.0f, 0.0f, 0.0f);
    bouncePayload.depth = nextDepth;
    bouncePayload.dynamicTouched = 0u;
    bouncePayload.pathThroughput =
        nextThroughput * inverseSurvivalProbability;
    bouncePayload.previousBsdfPdf =
        saturate(dot(normal, scatterDirection)) * c_invPi;
    bouncePayload.previousWasDelta = 0u;

    RecordRadianceRay(bouncePayload.depth);
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, bouncePayload);
    dynamicTouched |= bouncePayload.dynamicTouched;
    return directLighting +
        albedo * inverseSurvivalProbability * bouncePayload.color;
}
#endif
