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

float3 EvaluateGpuBrdfValidationSample(uint2 launchIndex, uint2 launchDim)
{
    uint caseIndex = min(
        launchIndex.y * 4u / max(launchDim.y, 1u),
        3u);

    PbrMaterial material;
    material.baseColor = float3(1.0f, 0.766f, 0.336f);
    material.metallic = caseIndex < 2u ? 1.0f : 0.0f;
    material.roughness = caseIndex == 0u || caseIndex == 2u
        ? 0.35f
        : (caseIndex == 1u ? 0.1f : 0.8f);
    material.emission = float3(0.0f, 0.0f, 0.0f);

    float nDotV = caseIndex == 3u ? 0.5f : 1.0f;
    float3 normal = float3(0.0f, 0.0f, 1.0f);
    float3 viewDirection = float3(
        sqrt(max(0.0f, 1.0f - nDotV * nDotV)),
        0.0f,
        nDotV);

    uint seed = CreateRandomSeed(0u, 0xA511E9B3u + caseIndex * 0x9E3779B9u);
    float3 sampleDirection;
    float3 weightedBrdf;
    float samplePdf;
    if (!SamplePbrBrdfWithMixtureSampling(
        material,
        normal,
        viewDirection,
        seed,
        sampleDirection,
        weightedBrdf,
        samplePdf))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }
    return weightedBrdf;
}

[shader("raygeneration")]
void MyRaygenShader_RadianceRay()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    // A negative depth marks pixels where the primary ray missed geometry.
    g_normalDepth[launchIndex] = float4(0.0f, 0.0f, 0.0f, -1.0f);
    float3 sampleRadiance = float3(0.0f, 0.0f, 0.0f);
    uint dynamicTouched = 0u;

    if (g_sceneType == c_scenePbrGpuValidation)
    {
        sampleRadiance = EvaluateGpuBrdfValidationSample(
            launchIndex,
            launchDim);
    }
    else
    {
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

        float3 cameraForward = normalize(g_cameraTarget - g_cameraPosition);
        float3 cameraRight = normalize(cross(c_cameraUp, cameraForward));
        float3 cameraUp = cross(cameraForward, cameraRight);

        RayDesc ray;
        ray.Origin = g_cameraPosition;
        ray.Direction = normalize(
            cameraForward + cameraRight * screenPosition.x + cameraUp * screenPosition.y);
        ray.TMin = c_rayTMin;
        ray.TMax = c_rayTMax;

        RadiancePayload payload;
        payload.color = float3(0.0f, 0.0f, 0.0f);
        payload.depth = 0;
        payload.dynamicTouched = 0u;
        payload.pathThroughput = float3(1.0f, 1.0f, 1.0f);
        payload.previousBsdfPdf = 0.0f;
        payload.previousWasDelta = 1u;

        RecordRadianceRay(0u);
        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
        sampleRadiance = payload.color;
        dynamicTouched = payload.dynamicTouched;
    }

    if (g_enableAccumulation == 0)
    {
        float3 displayColor = IsLinearDebugView()
            ? saturate(sampleRadiance)
            : ToneMapForDisplay(sampleRadiance);
        g_output[launchIndex] = float4(displayColor, 1.0f);
        return;
    }

    float3 accumulatedColor = sampleRadiance;
    float localSampleCount = 1.0f;
    bool dynamicInvalidation =
        g_dynamicObjectMoved != 0u &&
        dynamicTouched != 0u;
    if (g_sampleIndex > 0)
    {
        float4 previousAccumulation = g_accumulation[launchIndex];
        bool previousDynamicInvalidation =
            previousAccumulation.a < 0.0f;
        if (!dynamicInvalidation && !previousDynamicInvalidation)
        {
            accumulatedColor += previousAccumulation.rgb;
            localSampleCount =
                max(abs(previousAccumulation.a), 1.0f) + 1.0f;
        }
    }

    float signedSampleCount = dynamicInvalidation
        ? -localSampleCount
        : localSampleCount;
    g_accumulation[launchIndex] =
        float4(accumulatedColor, signedSampleCount);
    float3 averageRadiance = accumulatedColor / localSampleCount;
    g_output[launchIndex] = float4(ToneMapForDisplay(averageRadiance), 1.0f);
}

[shader("closesthit")]
void MyClosestHitShader_RadianceRay(
    inout RadiancePayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    RecordSurfaceHit();
    SceneInstanceMetadata instanceMetadata =
        g_instanceMetadata[InstanceID()];
    if (InstanceID() == 1u)
        payload.dynamicTouched = 1u;
    uint globalPrimitiveIndex =
        instanceMetadata.primitiveOffset + PrimitiveIndex();
    uint indexOffset =
        instanceMetadata.indexOffset + PrimitiveIndex() * 3u;
    uint i0 = instanceMetadata.vertexOffset + g_indices[indexOffset + 0u];
    uint i1 = instanceMetadata.vertexOffset + g_indices[indexOffset + 1u];
    uint i2 = instanceMetadata.vertexOffset + g_indices[indexOffset + 2u];

    float3 normal = InterpolateNormal(i0, i1, i2, attributes);
    float2 texCoord = InterpolateTexCoord(i0, i1, i2, attributes);
    float4 tangent = InterpolateTangent(i0, i1, i2, attributes);
    float3x3 objectToWorld = (float3x3)ObjectToWorld3x4();
    normal = normalize(mul(objectToWorld, normal));
    tangent.xyz = normalize(mul(objectToWorld, tangent.xyz));
    bool frontFace = dot(normal, WorldRayDirection()) < 0.0f;
    if (!frontFace)
    {
        normal = -normal;
    }
    if (g_sceneType == c_scenePbrGgx)
    {
        normal = ApplySceneNormalMap(
            globalPrimitiveIndex,
            texCoord,
            tangent,
            normal);
    }

    if (payload.depth == 0u)
    {
        g_normalDepth[DispatchRaysIndex().xy] =
            float4(normal, RayTCurrent());
    }

    float3 normalColor = normal * 0.5f + 0.5f;
    if (g_showNormalColor != 0)
    {
        payload.color = normalColor;
        return;
    }

    if (g_sceneType == c_scenePbrGgx && g_pbrDebugView != c_pbrDebugBeauty)
    {
        if (g_pbrDebugView == c_pbrDebugNormal)
        {
            payload.color = normalColor;
        }
        else if (g_pbrDebugView == c_pbrDebugDepth)
        {
            payload.color = DepthDebugColor(RayTCurrent());
        }
        else if (g_pbrDebugView == c_pbrDebugMaterialId)
        {
            payload.color = MaterialIdDebugColor(globalPrimitiveIndex);
        }
        else
        {
            payload.color =
                PbrMaterialDebugColor(globalPrimitiveIndex, texCoord);
        }
        return;
    }

    float3 hitPosition = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 emission = SurfaceEmission(globalPrimitiveIndex);
    if (frontFace && any(emission > 0.0f))
    {
        float emissionWeight = 1.0f;
        if (payload.depth > 0u &&
            payload.previousWasDelta == 0u &&
            g_lightingMode != c_lightingModeBsdf)
        {
            float hitDistance = RayTCurrent();
            float lightPdf = EvaluateAreaLightPdf(
                globalPrimitiveIndex,
                hitDistance * hitDistance,
                normalize(WorldRayDirection()));
            if (lightPdf > 0.0f)
            {
                if (g_lightingMode == c_lightingModeNee)
                {
                    emissionWeight = 0.0f;
                }
                else if (g_lightingMode == c_lightingModeMis)
                {
                    emissionWeight = PowerHeuristic(
                        payload.previousBsdfPdf,
                        lightPdf);
                }
            }
        }
        payload.color = emission * emissionWeight;
        return;
    }

    // Preserve the renderer's existing max-bounce definition: a non-emissive
    // vertex at the terminal depth must not launch either a bounce ray or an
    // NEE visibility ray.
    if (payload.depth >= g_maxBounce)
    {
        payload.color = float3(0.0f, 0.0f, 0.0f);
        return;
    }

    if (g_sceneType == c_scenePbrGgx)
    {
        PbrMaterial material =
            GetPbrMaterial(globalPrimitiveIndex, texCoord);
        payload.color = TracePbrBrdfWithMixtureSampling(
            material,
            normal,
            hitPosition,
            payload.depth,
            globalPrimitiveIndex,
            payload.dynamicTouched,
            payload.pathThroughput);
        return;
    }

    payload.color = TraceLambertianBounce(
        normal,
        hitPosition,
        CornellSurfaceAlbedo(globalPrimitiveIndex),
        payload.depth,
        globalPrimitiveIndex,
        payload.dynamicTouched,
        payload.pathThroughput);
}

[shader("miss")]
void MyMissShader_RadianceRay(inout RadiancePayload payload)
{
    RecordRadianceMiss();
    if (g_showNormalColor != 0 ||
        (g_sceneType == c_scenePbrGgx && g_pbrDebugView != c_pbrDebugBeauty))
    {
        payload.color = float3(0.0f, 0.0f, 0.0f);
        return;
    }

    if (g_sceneType != c_scenePbrGgx || g_enableIbl == 0u)
    {
        payload.color = float3(0.0f, 0.0f, 0.0f);
        return;
    }

    float environmentWeight = 1.0f;
    if (payload.depth > 0u &&
        payload.previousWasDelta == 0u &&
        g_lightingMode != c_lightingModeBsdf)
    {
        float lightPdf =
            EvaluateEnvironmentLightPdf(WorldRayDirection());
        if (lightPdf > 0.0f)
        {
            if (g_lightingMode == c_lightingModeNee)
            {
                environmentWeight = 0.0f;
            }
            else if (g_lightingMode == c_lightingModeMis)
            {
                environmentWeight = PowerHeuristic(
                    payload.previousBsdfPdf,
                    lightPdf);
            }
        }
    }
    payload.color =
        SampleEnvironmentMap(WorldRayDirection()) *
        environmentWeight;
}

[shader("miss")]
void MyMissShader_ShadowRay(inout ShadowPayload payload)
{
    payload.occluded = 0u;
}
