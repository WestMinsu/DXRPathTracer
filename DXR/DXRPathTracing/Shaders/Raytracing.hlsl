struct Vertex
{
    float3 position;
};

struct RadiancePayload
{
    float3 color;
    uint depth;
};

static const uint c_maxBounce = 3;
static const float c_rayTMin = 0.001f;
static const float c_rayTMax = 1000.0f;
static const float c_rayOriginBias = 0.001f;
static const float3 c_cameraPosition = float3(0.0f, 0.15f, -1.2f);
static const float c_viewPlaneZ = 0.0f;
static const float c_viewHalfHeight = 0.85f;
static const float c_twoPi = 6.283185307f;
static const uint c_cubePrimitiveCount = 12;
static const float3 c_diffuseAlbedo = float3(1.0f, 0.0f, 1.0f);
static const float3 c_floorAlbedo = float3(0.75f, 0.75f, 0.75f);

RWTexture2D<float4> g_output : register(u0);
RaytracingAccelerationStructure g_scene : register(t0);
StructuredBuffer<Vertex> g_vertices : register(t1);
StructuredBuffer<uint> g_indices : register(t2);

cbuffer RenderSettings : register(b0)
{
    uint g_showNormalColor;
    uint g_frameIndex;
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

float3 SkyColor(float3 direction)
{
    float t = 0.5f * (normalize(direction).y + 1.0f);
    return lerp(float3(1.0f, 1.0f, 1.0f), float3(0.5f, 0.8f, 1.0f), t);
}

float3 SurfaceAlbedo(uint primitiveIndex)
{
    return primitiveIndex >= c_cubePrimitiveCount ? c_floorAlbedo : c_diffuseAlbedo;
}

[shader("raygeneration")]
void MyRaygenShader_RadianceRay()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float2 uv = (float2(launchIndex) + 0.5f) / float2(launchDim);
    float aspectRatio = float(launchDim.x) / float(launchDim.y);
    float2 screenPosition = float2(
        (uv.x * 2.0f - 1.0f) * aspectRatio * c_viewHalfHeight,
        (1.0f - uv.y * 2.0f) * c_viewHalfHeight);
    float3 viewPosition = float3(screenPosition, c_viewPlaneZ);

    RayDesc ray;
    ray.Origin = c_cameraPosition;
    ray.Direction = normalize(viewPosition - c_cameraPosition);
    ray.TMin = c_rayTMin;
    ray.TMax = c_rayTMax;

    RadiancePayload payload;
    payload.color = float3(0.0f, 0.0f, 0.0f);
    payload.depth = 0;

    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    g_output[launchIndex] = float4(payload.color, 1.0f);
}

[shader("closesthit")]
void MyClosestHitShader_RadianceRay(
    inout RadiancePayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    uint indexOffset = PrimitiveIndex() * 3;
    uint i0 = g_indices[indexOffset + 0];
    uint i1 = g_indices[indexOffset + 1];
    uint i2 = g_indices[indexOffset + 2];

    float3 p0 = g_vertices[i0].position;
    float3 p1 = g_vertices[i1].position;
    float3 p2 = g_vertices[i2].position;
    float3 normal = normalize(cross(p1 - p0, p2 - p0));
    if (dot(normal, WorldRayDirection()) > 0.0f)
    {
        normal = -normal;
    }

    float3 normalColor = normal * 0.5f + 0.5f;
    if (g_showNormalColor != 0)
    {
        payload.color = normalColor;
        return;
    }

    if (payload.depth >= c_maxBounce)
    {
        payload.color = float3(0.0f, 0.0f, 0.0f);
        return;
    }

    uint seed = CreateRandomSeed(payload.depth, PrimitiveIndex());
    float3 scatterDirection = normal + RandomUnitVector(seed);
    if (dot(scatterDirection, scatterDirection) < 0.000001f)
    {
        scatterDirection = normal;
    }
    scatterDirection = normalize(scatterDirection);

    float3 hitPosition = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    RayDesc bounceRay;
    bounceRay.Origin = hitPosition + normal * c_rayOriginBias;
    bounceRay.Direction = scatterDirection;
    bounceRay.TMin = c_rayTMin;
    bounceRay.TMax = c_rayTMax;

    RadiancePayload bouncePayload;
    bouncePayload.color = float3(0.0f, 0.0f, 0.0f);
    bouncePayload.depth = payload.depth + 1;

    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, bouncePayload);
    payload.color = SurfaceAlbedo(PrimitiveIndex()) * bouncePayload.color;
}

[shader("miss")]
void MyMissShader_RadianceRay(inout RadiancePayload payload)
{
    payload.color = SkyColor(WorldRayDirection());
}
