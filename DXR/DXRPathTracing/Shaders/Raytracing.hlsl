struct RadiancePayload
{
    float3 color;
};

RWTexture2D<float4> g_output : register(u0);
RaytracingAccelerationStructure g_scene : register(t0);

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
    payload.color = float3(0.0f, 0.0f, 0.0f);

    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    g_output[launchIndex] = float4(payload.color, 1.0f);
}

[shader("closesthit")]
void MyClosestHitShader_RadianceRay(
    inout RadiancePayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    payload.color = float3(1.0f, 0.0f, 1.0f);
}

[shader("miss")]
void MyMissShader_RadianceRay(inout RadiancePayload payload)
{
    payload.color = float3(0.0f, 0.0f, 0.0f);
}

