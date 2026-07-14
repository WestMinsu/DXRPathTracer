#include "RaytracingCommon.hlsli"
#include "RaytracingScene.hlsli"
#include "RaytracingPbr.hlsli"

float3 LinearToSrgb(float3 linearColor)
{
    float3 low = linearColor * 12.92f;
    float3 high = 1.055f * pow(max(linearColor, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(high, low, linearColor <= 0.0031308f);
}

float3 ToneMapForDisplay(float3 linearRadiance)
{
    float3 exposed = max(linearRadiance, 0.0f) * exp2(g_exposure);
    float3 mapped = exposed / (1.0f + exposed);
    return LinearToSrgb(saturate(mapped));
}

bool IsLinearDebugView()
{
    return g_showNormalColor != 0 ||
        (g_sceneType == c_scenePbrGgx && g_pbrDebugView != c_pbrDebugBeauty);
}

[shader("raygeneration")]
void MyRaygenShader_RadianceRay()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float2 pixelOffset = float2(0.5f, 0.5f);
    if (g_enableAccumulation != 0)
    {
        uint cameraSeed = CreateRandomSeed(0u, 0x9E3779B9u);
        pixelOffset = float2(RandomFloat01(cameraSeed), RandomFloat01(cameraSeed));
    }

    float2 uv = (float2(launchIndex) + pixelOffset) / float2(launchDim);
    float aspectRatio = float(launchDim.x) / float(launchDim.y);
    float tanHalfFov = tan(c_verticalFovRadians * 0.5f);
    float2 screenPosition = float2(
        (uv.x * 2.0f - 1.0f) * aspectRatio * tanHalfFov,
        (1.0f - uv.y * 2.0f) * tanHalfFov);

    float3 cameraForward = normalize(c_cameraTarget - c_cameraPosition);
    float3 cameraRight = normalize(cross(c_cameraUp, cameraForward));
    float3 cameraUp = cross(cameraForward, cameraRight);

    RayDesc ray;
    ray.Origin = c_cameraPosition;
    ray.Direction = normalize(
        cameraForward + cameraRight * screenPosition.x + cameraUp * screenPosition.y);
    ray.TMin = c_rayTMin;
    ray.TMax = c_rayTMax;

    RadiancePayload payload;
    payload.color = float3(0.0f, 0.0f, 0.0f);
    payload.depth = 0;

    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    if (g_enableAccumulation == 0)
    {
        float3 displayColor = IsLinearDebugView()
            ? saturate(payload.color)
            : ToneMapForDisplay(payload.color);
        g_output[launchIndex] = float4(displayColor, 1.0f);
        return;
    }

    float3 accumulatedColor = payload.color;
    if (g_sampleIndex > 0)
    {
        accumulatedColor += g_accumulation[launchIndex].rgb;
    }

    g_accumulation[launchIndex] = float4(accumulatedColor, 1.0f);
    float3 averageRadiance = accumulatedColor / float(g_sampleIndex + 1);
    g_output[launchIndex] = float4(ToneMapForDisplay(averageRadiance), 1.0f);
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
    bool frontFace = dot(normal, WorldRayDirection()) < 0.0f;
    if (!frontFace)
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
    if (frontFace && any(emission > 0.0f))
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
        payload.color = TracePbrBrdfWithMixtureSampling(material, normal, hitPosition, payload.depth, PrimitiveIndex());
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
    payload.color = g_sceneType == c_scenePbrGgx && g_enableIbl != 0
        ? SampleEnvironmentMap(WorldRayDirection())
        : float3(0.0f, 0.0f, 0.0f);
}
