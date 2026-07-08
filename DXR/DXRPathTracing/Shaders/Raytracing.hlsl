#include "RaytracingCommon.hlsli"
#include "RaytracingScene.hlsli"
#include "RaytracingPbr.hlsli"

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

    if (g_enableAccumulation == 0)
    {
        g_output[launchIndex] = float4(payload.color, 1.0f);
        return;
    }

    float3 accumulatedColor = payload.color;
    if (g_sampleIndex > 0)
    {
        accumulatedColor += g_accumulation[launchIndex].rgb;
    }

    g_accumulation[launchIndex] = float4(accumulatedColor, 1.0f);
    g_output[launchIndex] = float4(accumulatedColor / float(g_sampleIndex + 1), 1.0f);
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

    float3 normal = InterpolateNormal(i0, i1, i2, attributes);
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

    if (g_sceneType == c_scenePbrGgx && g_pbrDebugView != c_pbrDebugBeauty)
    {
        payload.color = PbrMaterialDebugColor(PrimitiveIndex());
        return;
    }

    float3 hitPosition = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 emission = SurfaceEmission(PrimitiveIndex());
    if (any(emission > 0.0f))
    {
        payload.color = emission;
        return;
    }

    if (payload.depth >= g_maxBounce)
    {
        payload.color = float3(0.0f, 0.0f, 0.0f);
        return;
    }

    if (g_sceneType == c_scenePbrGgx)
    {
        PbrMaterial material = GetPbrMaterial(PrimitiveIndex());
        payload.color = TracePbrBrdfWithGgxImportanceSampling(material, normal, hitPosition, payload.depth, PrimitiveIndex());
        return;
    }

    payload.color = TraceLambertianBounce(
        normal,
        hitPosition,
        CornellSurfaceAlbedo(PrimitiveIndex()),
        payload.depth,
        PrimitiveIndex());
}

[shader("miss")]
void MyMissShader_RadianceRay(inout RadiancePayload payload)
{
    payload.color = float3(0.0f, 0.0f, 0.0f);
}