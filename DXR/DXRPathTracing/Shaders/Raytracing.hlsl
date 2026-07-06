struct Vertex
{
    float3 position;
};

struct RadiancePayload
{
    float3 color;
};

RWTexture2D<float4> g_output : register(u0);
RaytracingAccelerationStructure g_scene : register(t0);
StructuredBuffer<Vertex> g_vertices : register(t1);
StructuredBuffer<uint> g_indices : register(t2);

cbuffer RenderSettings : register(b0)
{
    uint g_showNormalColor;
};

[shader("raygeneration")]
void MyRaygenShader_RadianceRay()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float2 uv = (float2(launchIndex) + 0.5f) / float2(launchDim);
    float2 screenPosition = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);

    RayDesc ray;
    ray.Origin = float3(screenPosition, 0.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.001f;
    ray.TMax = 1000.0f;

    RadiancePayload payload;
    payload.color = float3(0.5f, 0.8f, 1.0f);

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

    float3 baseColor = float3(1.0f, 0.0f, 1.0f);
    float3 normalColor = normal * 0.5f + 0.5f;
    payload.color = g_showNormalColor != 0 ? normalColor : baseColor;
}

[shader("miss")]
void MyMissShader_RadianceRay(inout RadiancePayload payload)
{
    payload.color = float3(0.5f, 0.8f, 1.0f);
}
