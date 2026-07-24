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

float UnpackGuideRoughness(float packedRoughness)
{
    return packedRoughness >= 1.5f
        ? packedRoughness - 2.0f
        : packedRoughness;
}

bool IsDynamicGuide(float4 materialGuide)
{
    return materialGuide.a >= 1.5f;
}

bool TemporalCameraIsMoving()
{
    return
        length(g_cameraPosition - g_previousCameraPosition) > 1.0e-5f ||
        length(g_cameraTarget - g_previousCameraTarget) > 1.0e-5f;
}

bool TemporalSceneIsMoving()
{
    return g_dynamicObjectMoved != 0u ||
        TemporalCameraIsMoving();
}

bool FindValidatedHistoryPixel(
    float3 worldPosition,
    float3 currentNormal,
    float4 currentMaterial,
    uint2 resolution,
    out int2 historyPixel)
{
    historyPixel = int2(0, 0);
    float3 previousForwardVector =
        g_previousCameraTarget - g_previousCameraPosition;
    float forwardLength = length(previousForwardVector);
    if (forwardLength <= 1.0e-5f)
        return false;

    float3 previousForward = previousForwardVector / forwardLength;
    float3 previousRightVector = cross(c_cameraUp, previousForward);
    float rightLength = length(previousRightVector);
    if (rightLength <= 1.0e-5f)
        return false;
    float3 previousRight = previousRightVector / rightLength;
    float3 previousUp = cross(previousForward, previousRight);

    float3 previousViewVector =
        worldPosition - g_previousCameraPosition;
    float previousViewDepth =
        dot(previousViewVector, previousForward);
    if (previousViewDepth <= c_rayTMin)
        return false;

    float tanHalfFov = tan(c_verticalFovRadians * 0.5f);
    float aspectRatio = float(resolution.x) / float(resolution.y);
    float2 screenPosition = float2(
        dot(previousViewVector, previousRight) /
            (previousViewDepth * tanHalfFov * aspectRatio),
        dot(previousViewVector, previousUp) /
            (previousViewDepth * tanHalfFov));
    float2 previousUv = float2(
        screenPosition.x * 0.5f + 0.5f,
        0.5f - screenPosition.y * 0.5f);
    if (any(previousUv < 0.0f) || any(previousUv >= 1.0f))
        return false;

    historyPixel = int2(previousUv * float2(resolution));
    float4 previousNormalDepth =
        g_previousNormalDepth.Load(int3(historyPixel, 0));
    float4 previousMaterial =
        g_previousMaterialGuide.Load(int3(historyPixel, 0));
    if (previousNormalDepth.w < 0.0f || previousMaterial.a < 0.0f)
        return false;

    float expectedPreviousDepth = length(previousViewVector);
    float depthTolerance = max(0.02f, expectedPreviousDepth * 0.02f);
    if (abs(previousNormalDepth.w - expectedPreviousDepth) > depthTolerance)
        return false;

    float normalAgreement = dot(
        normalize(currentNormal),
        normalize(previousNormalDepth.xyz));
    if (normalAgreement < 0.90f)
        return false;

    if (length(currentMaterial.rgb - previousMaterial.rgb) > 0.15f)
        return false;
    if (abs(
        UnpackGuideRoughness(currentMaterial.a) -
        UnpackGuideRoughness(previousMaterial.a)) > 0.15f)
    {
        return false;
    }

    if (g_dynamicObjectMoved != 0u &&
        (IsDynamicGuide(currentMaterial) ||
         IsDynamicGuide(previousMaterial)))
    {
        return false;
    }
    return true;
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
    bool guidesEnabled =
        g_enableAccumulation != 0u &&
        (g_enableAtrous != 0u ||
         g_enableTemporalReprojection != 0u);
    bool updatePrimaryGuides =
        guidesEnabled &&
        (g_enableTemporalReprojection != 0u ||
         g_sampleIndex == 0u ||
         g_dynamicObjectMoved != 0u ||
         TemporalCameraIsMoving());
    if (updatePrimaryGuides)
    {
        // A negative depth marks primary rays that missed geometry.
        g_normalDepth[launchIndex] =
            float4(0.0f, 0.0f, 0.0f, -1.0f);
        g_materialGuide[launchIndex] =
            float4(0.0f, 0.0f, 0.0f, -1.0f);
    }
    float3 sampleRadiance = float3(0.0f, 0.0f, 0.0f);
    float3 sampleDiffuseIndirectRadiance =
        float3(0.0f, 0.0f, 0.0f);
    float3 sampleSpecularIndirectRadiance =
        float3(0.0f, 0.0f, 0.0f);
    uint dynamicTouched = 0u;
    float3 primaryRayDirection = float3(0.0f, 0.0f, 0.0f);
    bool temporalHistoryAttempted = false;
    bool temporalHistoryAccepted = false;

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
        primaryRayDirection = ray.Direction;
        ray.TMin = c_rayTMin;
        ray.TMax = c_rayTMax;

        RadiancePayload payload;
        payload.color = float3(0.0f, 0.0f, 0.0f);
        payload.primaryDirectColor = float3(0.0f, 0.0f, 0.0f);
        payload.primaryDiffuseIndirectColor =
            float3(0.0f, 0.0f, 0.0f);
        payload.primarySpecularIndirectColor =
            float3(0.0f, 0.0f, 0.0f);
        payload.depth = 0;
        payload.dynamicTouched = 0u;
        payload.pathThroughput = float3(1.0f, 1.0f, 1.0f);
        payload.previousBsdfPdf = 0.0f;
        payload.previousWasDelta = 1u;

        RecordRadianceRay(0u);
        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
        sampleRadiance = payload.color;
        sampleDiffuseIndirectRadiance =
            payload.primaryDiffuseIndirectColor;
        sampleSpecularIndirectRadiance =
            payload.primarySpecularIndirectColor;
        dynamicTouched = payload.dynamicTouched;

        if (updatePrimaryGuides)
        {
            float2 guideUv =
                (float2(launchIndex) + float2(0.5f, 0.5f)) /
                float2(launchDim);
            float2 guideScreenPosition = float2(
                (guideUv.x * 2.0f - 1.0f) *
                    aspectRatio * tanHalfFov,
                (1.0f - guideUv.y * 2.0f) * tanHalfFov);

            RayDesc guideRay;
            guideRay.Origin = g_cameraPosition;
            guideRay.Direction = normalize(
                cameraForward +
                cameraRight * guideScreenPosition.x +
                cameraUp * guideScreenPosition.y);
            guideRay.TMin = c_rayTMin;
            guideRay.TMax = c_rayTMax;

            RadiancePayload guidePayload;
            guidePayload.color = float3(0.0f, 0.0f, 0.0f);
            guidePayload.primaryDirectColor =
                float3(0.0f, 0.0f, 0.0f);
            guidePayload.primaryDiffuseIndirectColor =
                float3(0.0f, 0.0f, 0.0f);
            guidePayload.primarySpecularIndirectColor =
                float3(0.0f, 0.0f, 0.0f);
            guidePayload.depth = c_primaryGuideRayDepth;
            guidePayload.dynamicTouched = 0u;
            guidePayload.pathThroughput =
                float3(1.0f, 1.0f, 1.0f);
            guidePayload.previousBsdfPdf = 0.0f;
            guidePayload.previousWasDelta = 1u;

            TraceRay(
                g_scene,
                RAY_FLAG_NONE,
                0xFF,
                0,
                1,
                0,
                guideRay,
                guidePayload);
            primaryRayDirection = guideRay.Direction;
        }
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
    float3 accumulatedDiffuseIndirect =
        sampleDiffuseIndirectRadiance;
    float3 accumulatedSpecularIndirect =
        sampleSpecularIndirectRadiance;
    float sampleDiffuseLuminance = dot(
        sampleDiffuseIndirectRadiance,
        float3(0.2126f, 0.7152f, 0.0722f));
    float sampleSpecularLuminance = dot(
        sampleSpecularIndirectRadiance,
        float3(0.2126f, 0.7152f, 0.0722f));
    float2 accumulatedDiffuseMoments = float2(
        sampleDiffuseLuminance,
        sampleDiffuseLuminance * sampleDiffuseLuminance);
    float2 accumulatedSpecularMoments = float2(
        sampleSpecularLuminance,
        sampleSpecularLuminance * sampleSpecularLuminance);
    float localSampleCount = 1.0f;
    bool dynamicInvalidation =
        g_dynamicObjectMoved != 0u &&
        dynamicTouched != 0u;
    if (g_sampleIndex > 0)
    {
        int2 historyPixel = int2(launchIndex);
        bool historyValid = true;
        if (g_enableTemporalReprojection != 0u)
        {
            temporalHistoryAttempted = true;
            float4 currentMaterial = g_materialGuide[launchIndex];
            if (TemporalCameraIsMoving())
            {
                float4 currentNormalDepth = g_normalDepth[launchIndex];
                historyValid =
                    currentNormalDepth.w >= 0.0f &&
                    currentMaterial.a >= 0.0f &&
                    FindValidatedHistoryPixel(
                        g_cameraPosition +
                            primaryRayDirection * currentNormalDepth.w,
                        currentNormalDepth.xyz,
                        currentMaterial,
                        launchDim,
                        historyPixel);
            }
            else if (g_dynamicObjectMoved != 0u)
            {
                float4 previousMaterial =
                    g_previousMaterialGuide.Load(
                        int3(historyPixel, 0));
                historyValid =
                    !IsDynamicGuide(currentMaterial) &&
                    !IsDynamicGuide(previousMaterial);
            }
            else
            {
                // With a static camera and static geometry, sub-pixel ray
                // jitter changes texture and normal-map samples even though
                // the same screen pixel is still valid. Reuse its history
                // directly instead of applying unstable geometric tests.
                historyValid = true;
            }
        }

        float4 previousAccumulation =
            g_enableTemporalReprojection != 0u
            ? g_previousAccumulation.Load(int3(historyPixel, 0))
            : g_accumulation[launchIndex];
        bool previousDynamicInvalidation =
            previousAccumulation.a < 0.0f;
        if (historyValid &&
            !dynamicInvalidation &&
            !previousDynamicInvalidation)
        {
            temporalHistoryAccepted =
                g_enableTemporalReprojection != 0u;
            float previousSampleCount =
                max(abs(previousAccumulation.a), 1.0f);
            float retainedHistoryCount = previousSampleCount;
            if (g_enableTemporalReprojection != 0u &&
                TemporalSceneIsMoving())
            {
                retainedHistoryCount = min(
                    previousSampleCount,
                    31.0f);
            }
            float historyScale =
                retainedHistoryCount / previousSampleCount;
            accumulatedColor +=
                previousAccumulation.rgb * historyScale;
            if (g_enableAtrous != 0u)
            {
                if (g_enableTemporalReprojection != 0u)
                {
                    accumulatedDiffuseIndirect +=
                        g_previousDiffuseIndirect.Load(
                            int3(historyPixel, 0)).rgb * historyScale;
                    accumulatedSpecularIndirect +=
                        g_previousSpecularIndirect.Load(
                            int3(historyPixel, 0)).rgb * historyScale;
                    accumulatedDiffuseMoments +=
                        g_previousDiffuseMoments.Load(
                            int3(historyPixel, 0)) * historyScale;
                    accumulatedSpecularMoments +=
                        g_previousSpecularMoments.Load(
                            int3(historyPixel, 0)) * historyScale;
                }
                else
                {
                    accumulatedDiffuseIndirect +=
                        g_diffuseIndirectAccumulation[launchIndex].rgb;
                    accumulatedSpecularIndirect +=
                        g_specularIndirectAccumulation[launchIndex].rgb;
                    accumulatedDiffuseMoments +=
                        g_diffuseLuminanceMoments[launchIndex];
                    accumulatedSpecularMoments +=
                        g_specularLuminanceMoments[launchIndex];
                }
            }
            localSampleCount = retainedHistoryCount + 1.0f;
        }
    }

    float signedSampleCount = dynamicInvalidation
        ? -localSampleCount
        : localSampleCount;
    g_accumulation[launchIndex] =
        float4(accumulatedColor, signedSampleCount);
    if (g_enableAtrous != 0u)
    {
        g_diffuseIndirectAccumulation[launchIndex] =
            float4(accumulatedDiffuseIndirect, signedSampleCount);
        g_specularIndirectAccumulation[launchIndex] =
            float4(accumulatedSpecularIndirect, signedSampleCount);
        g_diffuseLuminanceMoments[launchIndex] =
            accumulatedDiffuseMoments;
        g_specularLuminanceMoments[launchIndex] =
            accumulatedSpecularMoments;
    }
    if (g_temporalDebugView == 1u)
    {
        float normalizedHistory = saturate(
            (localSampleCount - 1.0f) / 31.0f);
        g_output[launchIndex] = float4(
            normalizedHistory.xxx,
            1.0f);
        return;
    }
    if (g_temporalDebugView == 2u)
    {
        float3 debugColor = temporalHistoryAttempted
            ? (temporalHistoryAccepted
                ? float3(0.0f, 1.0f, 0.0f)
                : float3(1.0f, 0.0f, 0.0f))
            : float3(0.0f, 0.0f, 1.0f);
        g_output[launchIndex] = float4(debugColor, 1.0f);
        return;
    }

    float3 averageRadiance = accumulatedColor / localSampleCount;
    g_output[launchIndex] = float4(ToneMapForDisplay(averageRadiance), 1.0f);
}

[shader("anyhit")]
void MyAnyHitShader_AlphaMask(
    inout RadiancePayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    SceneInstanceMetadata instanceMetadata =
        g_instanceMetadata[InstanceID()];
    uint globalPrimitiveIndex =
        instanceMetadata.primitiveOffset + PrimitiveIndex();
    SceneMaterial material = GetSceneMaterial(globalPrimitiveIndex);
    if (material.alphaCutoff < 0.0f)
    {
        return;
    }

    uint indexOffset =
        instanceMetadata.indexOffset + PrimitiveIndex() * 3u;
    uint i0 = instanceMetadata.vertexOffset + g_indices[indexOffset + 0u];
    uint i1 = instanceMetadata.vertexOffset + g_indices[indexOffset + 1u];
    uint i2 = instanceMetadata.vertexOffset + g_indices[indexOffset + 2u];
    float2 texCoord = InterpolateTexCoord(i0, i1, i2, attributes);
    if (!PassesSceneAlphaMask(globalPrimitiveIndex, texCoord))
    {
        IgnoreHit();
    }
}

[shader("closesthit")]
void MyClosestHitShader_RadianceRay(
    inout RadiancePayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    bool guideOnly = payload.depth == c_primaryGuideRayDepth;
    if (!guideOnly)
        RecordSurfaceHit();
    SceneInstanceMetadata instanceMetadata =
        g_instanceMetadata[InstanceID()];
    bool dynamicInstance =
        (instanceMetadata.flags & c_instanceFlagDynamic) != 0u;
    if (!guideOnly && dynamicInstance)
        payload.dynamicTouched = 1u;
    uint globalPrimitiveIndex =
        instanceMetadata.primitiveOffset + PrimitiveIndex();
    uint indexOffset =
        instanceMetadata.indexOffset + PrimitiveIndex() * 3u;
    uint i0 = instanceMetadata.vertexOffset + g_indices[indexOffset + 0u];
    uint i1 = instanceMetadata.vertexOffset + g_indices[indexOffset + 1u];
    uint i2 = instanceMetadata.vertexOffset + g_indices[indexOffset + 2u];

    float3 normal = InterpolateNormal(i0, i1, i2, attributes);
    float3x3 objectToWorld = (float3x3)ObjectToWorld3x4();
    normal = normalize(mul(objectToWorld, normal));
    bool frontFace = dot(normal, WorldRayDirection()) < 0.0f;
    if (!frontFace)
    {
        normal = -normal;
    }

    float2 texCoord = InterpolateTexCoord(i0, i1, i2, attributes);
    float4 tangent = InterpolateTangent(i0, i1, i2, attributes);
    tangent.xyz = normalize(mul(objectToWorld, tangent.xyz));
    if (g_sceneType == c_scenePbrGgx)
    {
        normal = ApplySceneNormalMap(
            globalPrimitiveIndex,
            texCoord,
            tangent,
            normal);
    }

    PbrMaterial surfaceMaterial;
    if (g_sceneType == c_scenePbrGgx)
    {
        surfaceMaterial =
            GetPbrMaterial(globalPrimitiveIndex, texCoord);
    }
    else
    {
        surfaceMaterial.baseColor =
            CornellSurfaceAlbedo(globalPrimitiveIndex);
        surfaceMaterial.metallic = 0.0f;
        surfaceMaterial.roughness = 1.0f;
        surfaceMaterial.emission =
            SurfaceEmission(globalPrimitiveIndex);
    }

    if (guideOnly)
    {
        g_normalDepth[DispatchRaysIndex().xy] =
            float4(normal, RayTCurrent());
        g_materialGuide[DispatchRaysIndex().xy] =
            float4(
                surfaceMaterial.baseColor,
                surfaceMaterial.roughness +
                    (dynamicInstance ? 2.0f : 0.0f));
        payload.color = float3(0.0f, 0.0f, 0.0f);
        return;
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
        if (payload.depth == 0u)
            payload.primaryDirectColor = payload.color;
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

    float3 hitPosition =
        WorldRayOrigin() + WorldRayDirection() * RayTCurrent();

    if (g_sceneType == c_scenePbrGgx)
    {
        float3 localDiffuseIndirectLighting;
        float3 localSpecularIndirectLighting;
        float3 localDirectLighting;
        payload.color = TracePbrBrdfWithMixtureSampling(
            surfaceMaterial,
            normal,
            hitPosition,
            payload.depth,
            globalPrimitiveIndex,
            payload.dynamicTouched,
            payload.pathThroughput,
            localDiffuseIndirectLighting,
            localSpecularIndirectLighting,
            localDirectLighting);
        if (payload.depth == 0u)
        {
            payload.primaryDirectColor = localDirectLighting;
            payload.primaryDiffuseIndirectColor =
                localDiffuseIndirectLighting;
            payload.primarySpecularIndirectColor =
                localSpecularIndirectLighting;
        }
        return;
    }

    float3 localDirectLighting;
    payload.color = TraceLambertianBounce(
        normal,
        hitPosition,
        surfaceMaterial.baseColor,
        payload.depth,
        globalPrimitiveIndex,
        payload.dynamicTouched,
        payload.pathThroughput,
        localDirectLighting);
    if (payload.depth == 0u)
    {
        payload.primaryDirectColor = localDirectLighting;
        payload.primaryDiffuseIndirectColor =
            max(payload.color - localDirectLighting, 0.0f);
        payload.primarySpecularIndirectColor =
            float3(0.0f, 0.0f, 0.0f);
    }
}

[shader("miss")]
void MyMissShader_RadianceRay(inout RadiancePayload payload)
{
    if (payload.depth == c_primaryGuideRayDepth)
    {
        payload.color = float3(0.0f, 0.0f, 0.0f);
        return;
    }

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
    if (payload.depth == 0u)
        payload.primaryDirectColor = payload.color;
}

[shader("miss")]
void MyMissShader_ShadowRay(inout ShadowPayload payload)
{
    payload.occluded = 0u;
}
