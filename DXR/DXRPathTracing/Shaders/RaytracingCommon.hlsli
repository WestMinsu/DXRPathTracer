#ifndef RAYTRACING_COMMON_HLSLI
#define RAYTRACING_COMMON_HLSLI

struct Vertex
{
    float3 position;
    float3 normal;
};

struct RadiancePayload
{
    float3 color;
    uint depth;
};

struct PbrMaterial
{
    float3 baseColor;
    float metallic;
    float roughness;
    float3 emission;
};

static const uint c_sceneCornellBox = 0;
static const uint c_scenePbrGgx = 1;
static const uint c_pbrDebugBeauty = 0;
static const uint c_pbrDebugAlbedo = 1;
static const uint c_pbrDebugMetallic = 2;
static const uint c_pbrDebugRoughness = 3;
static const float c_rayTMin = 0.001f;
static const float c_rayTMax = 1000.0f;
static const float c_rayOriginBias = 0.001f;
static const float3 c_cameraPosition = float3(0.0f, 0.15f, -1.2f);
static const float c_viewPlaneZ = 0.0f;
static const float c_viewHalfHeight = 0.85f;
static const float c_pi = 3.141592654f;
static const float c_invPi = 0.318309886f;
static const float c_twoPi = 6.283185307f;
static const uint c_blockPrimitiveCount = 24;
static const uint c_floorPrimitiveStart = c_blockPrimitiveCount;
static const uint c_ceilingPrimitiveStart = 26;
static const uint c_backWallPrimitiveStart = 28;
static const uint c_leftWallPrimitiveStart = 30;
static const uint c_rightWallPrimitiveStart = 32;
static const uint c_lightPrimitiveStart = 34;
static const uint c_lightPrimitiveCount = 2;
static const uint c_pbrSpherePrimitiveCount = 528;
static const uint c_pbrSphereCount = 3;
static const uint c_pbrFloorPrimitiveStart = c_pbrSpherePrimitiveCount * c_pbrSphereCount;
static const uint c_pbrBackWallPrimitiveStart = c_pbrFloorPrimitiveStart + 2;
static const uint c_pbrLightPrimitiveStart = c_pbrBackWallPrimitiveStart + 2;
static const uint c_pbrLightPrimitiveCount = 2;
static const float3 c_pbrLightCenter = float3(0.0f, 1.55f, 2.05f);
static const float3 c_blockAlbedo = float3(0.74f, 0.74f, 0.74f);
static const float3 c_floorAlbedo = float3(0.75f, 0.75f, 0.75f);
static const float3 c_ceilingAlbedo = float3(0.75f, 0.75f, 0.75f);
static const float3 c_backWallAlbedo = float3(0.75f, 0.75f, 0.75f);
static const float3 c_leftWallAlbedo = float3(0.65f, 0.08f, 0.05f);
static const float3 c_rightWallAlbedo = float3(0.12f, 0.45f, 0.10f);
static const float3 c_cornellLightEmission = float3(12.0f, 10.0f, 8.0f);
static const float3 c_pbrLightEmission = float3(1.0f, 1.0f, 1.0f);

RWTexture2D<float4> g_output : register(u0);
RWTexture2D<float4> g_accumulation : register(u1);
RaytracingAccelerationStructure g_scene : register(t0);
StructuredBuffer<Vertex> g_vertices : register(t1);
StructuredBuffer<uint> g_indices : register(t2);
TextureCube<float4> g_environmentMap : register(t3);
SamplerState g_environmentSampler : register(s0);

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
};

uint CreateRandomSeed(uint depth, uint primitiveIndex)
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    uint seed = launchIndex.x + launchIndex.y * launchDim.x;
    seed = seed * 1973u + g_frameIndex * 9277u + depth * 26699u + primitiveIndex * 911u + 1u;
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


float3 SampleEnvironmentMap(float3 direction)
{
    return max(
        g_environmentMap.SampleLevel(g_environmentSampler, normalize(direction), 0.0f).rgb,
        float3(0.0f, 0.0f, 0.0f)) * g_iblIntensity;
}
float3 TraceLambertianBounce(float3 normal, float3 hitPosition, float3 albedo, uint depth, uint primitiveIndex)
{
    uint seed = CreateRandomSeed(depth, primitiveIndex);

    float3 scatterDirection = RandomCosineHemisphereDirection(normal, seed);

    RayDesc bounceRay;
    bounceRay.Origin = hitPosition + normal * c_rayOriginBias;
    bounceRay.Direction = scatterDirection;
    bounceRay.TMin = c_rayTMin;
    bounceRay.TMax = c_rayTMax;

    RadiancePayload bouncePayload;
    bouncePayload.color = float3(0.0f, 0.0f, 0.0f);
    bouncePayload.depth = depth + 1;

    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, bouncePayload);
    return albedo * bouncePayload.color;
}
#endif
