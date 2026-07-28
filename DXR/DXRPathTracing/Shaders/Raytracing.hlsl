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

struct TemporalHistorySample
{
    float3 radianceAverage;
    float3 diffuseAverage;
    float3 specularAverage;
    float2 diffuseMomentAverage;
    float2 specularMomentAverage;
    float sampleCount;
};

void ResetTemporalHistorySample(out TemporalHistorySample history)
{
    history.radianceAverage = float3(0.0f, 0.0f, 0.0f);
    history.diffuseAverage = float3(0.0f, 0.0f, 0.0f);
    history.specularAverage = float3(0.0f, 0.0f, 0.0f);
    history.diffuseMomentAverage = float2(0.0f, 0.0f);
    history.specularMomentAverage = float2(0.0f, 0.0f);
    history.sampleCount = 0.0f;
}

bool ProjectToPreviousFrame(
    float3 worldPosition,
    float3 currentRayDirection,
    bool currentHit,
    uint2 resolution,
    out float2 historyPosition,
    out float expectedPreviousDepth)
{
    historyPosition = float2(0.0f, 0.0f);
    expectedPreviousDepth = -1.0f;
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

    float3 previousViewVector = currentHit
        ? worldPosition - g_previousCameraPosition
        : currentRayDirection;
    float previousViewDepth = dot(previousViewVector, previousForward);
    if (previousViewDepth <= c_rayTMin)
        return false;

    if (currentHit)
        expectedPreviousDepth = length(previousViewVector);

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

    // Texture texel centers are at integer + 0.5 in UV-scaled coordinates.
    historyPosition = previousUv * float2(resolution) - 0.5f;
    return true;
}

bool IsValidHistoryTap(
    int2 historyPixel,
    bool currentHit,
    float expectedPreviousDepth,
    float3 currentNormal,
    float4 currentMaterial,
    uint2 resolution)
{
    if (any(historyPixel < int2(0, 0)) ||
        any(historyPixel >= int2(resolution)))
    {
        return false;
    }

    float4 previousNormalDepth =
        g_previousNormalDepth.Load(int3(historyPixel, 0));
    float4 previousMaterial =
        g_previousMaterialGuide.Load(int3(historyPixel, 0));
    bool previousHit =
        previousNormalDepth.w >= 0.0f && previousMaterial.a >= 0.0f;
    if (currentHit != previousHit)
        return false;
    if (!currentHit)
        return true;

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

    bool currentDynamic = IsDynamicGuide(currentMaterial);
    bool previousDynamic = IsDynamicGuide(previousMaterial);
    if (g_dynamicObjectMoved != 0u &&
        !DynamicObjectReprojectionEnabled())
    {
        if (currentDynamic || previousDynamic)
            return false;
    }
    else if (currentDynamic != previousDynamic)
    {
        return false;
    }
    return true;
}

bool GatherValidatedHistory(
    float3 worldPosition,
    float3 currentRayDirection,
    float4 currentNormalDepth,
    float4 currentMaterial,
    uint2 resolution,
    out TemporalHistorySample history)
{
    ResetTemporalHistorySample(history);
    bool currentHit =
        currentNormalDepth.w >= 0.0f && currentMaterial.a >= 0.0f;
    float2 historyPosition;
    float expectedPreviousDepth;
    if (!ProjectToPreviousFrame(
        worldPosition,
        currentRayDirection,
        currentHit,
        resolution,
        historyPosition,
        expectedPreviousDepth))
    {
        return false;
    }

    int2 basePixel = int2(floor(historyPosition));
    float2 fraction = frac(historyPosition);
    int2 tapOffsets[4] =
    {
        int2(0, 0),
        int2(1, 0),
        int2(0, 1),
        int2(1, 1)
    };
    float tapWeights[4] =
    {
        (1.0f - fraction.x) * (1.0f - fraction.y),
        fraction.x * (1.0f - fraction.y),
        (1.0f - fraction.x) * fraction.y,
        fraction.x * fraction.y
    };

    float validWeight = 0.0f;
    float weightedSampleCount = 0.0f;
    [unroll]
    for (uint tapIndex = 0u; tapIndex < 4u; ++tapIndex)
    {
        float tapWeight = tapWeights[tapIndex];
        if (tapWeight <= 0.0f)
            continue;

        int2 historyPixel = basePixel + tapOffsets[tapIndex];
        if (!IsValidHistoryTap(
            historyPixel,
            currentHit,
            expectedPreviousDepth,
            currentNormalDepth.xyz,
            currentMaterial,
            resolution))
        {
            continue;
        }

        float4 previousAccumulation =
            g_previousAccumulation.Load(int3(historyPixel, 0));
        if (previousAccumulation.a <= 0.0f)
            continue;

        float tapSampleCount = max(previousAccumulation.a, 1.0f);
        history.radianceAverage += tapWeight *
            previousAccumulation.rgb / tapSampleCount;
        weightedSampleCount += tapWeight * tapSampleCount;

        if (g_enableAtrous != 0u)
        {
            float4 previousDiffuse =
                g_previousDiffuseIndirect.Load(int3(historyPixel, 0));
            float4 previousSpecular =
                g_previousSpecularIndirect.Load(int3(historyPixel, 0));
            float diffuseSampleCount = max(abs(previousDiffuse.a), 1.0f);
            float specularSampleCount = max(abs(previousSpecular.a), 1.0f);
            history.diffuseAverage += tapWeight *
                previousDiffuse.rgb / diffuseSampleCount;
            history.specularAverage += tapWeight *
                previousSpecular.rgb / specularSampleCount;
            history.diffuseMomentAverage += tapWeight *
                g_previousDiffuseMoments.Load(int3(historyPixel, 0)) /
                diffuseSampleCount;
            history.specularMomentAverage += tapWeight *
                g_previousSpecularMoments.Load(int3(historyPixel, 0)) /
                specularSampleCount;
        }
        validWeight += tapWeight;
    }

    // Avoid stretching a tiny surviving bilinear tap across a disocclusion.
    if (validWeight < 0.10f)
        return false;

    float inverseValidWeight = 1.0f / validWeight;
    history.radianceAverage *= inverseValidWeight;
    history.diffuseAverage *= inverseValidWeight;
    history.specularAverage *= inverseValidWeight;
    history.diffuseMomentAverage *= inverseValidWeight;
    history.specularMomentAverage *= inverseValidWeight;
    history.sampleCount = max(
        weightedSampleCount * inverseValidWeight,
        1.0f);
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

    uint seed = CreateRandomSeed(
        0u,
        0xA511E9B3u + caseIndex * 0x9E3779B9u,
        0u);
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

void ResetSurfaceQueryPayload(out SurfaceQueryPayload payload)
{
    payload.normal = float3(0.0f, 0.0f, 0.0f);
    payload.hitT = 0.0f;
    payload.baseColor = float3(0.0f, 0.0f, 0.0f);
    payload.metallic = 0.0f;
    payload.emission = float3(0.0f, 0.0f, 0.0f);
    payload.roughness = 1.0f;
    payload.primitiveIndex = 0u;
    payload.dynamicInstance = 0u;
    payload.frontFace = 0u;
    payload.hit = 0u;
}

void TracePrimaryGuide(
    RayDesc ray,
    out float3 previousWorldPosition,
    out uint dynamicInstance)
{
    previousWorldPosition = ray.Origin;
    dynamicInstance = 0u;
    SurfaceQueryPayload payload;
    ResetSurfaceQueryPayload(payload);
    // The guide query does not consume emission. Mark it before TraceRay so
    // closest-hit can return the previous rigid-body position in that slot
    // without increasing the payload carried by every radiance bounce.
    payload.hit = DynamicObjectReprojectionEnabled() ? 2u : 0u;
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    if (payload.hit == 0u)
        return;

    previousWorldPosition = payload.emission;
    dynamicInstance = payload.dynamicInstance;
    uint2 launchIndex = DispatchRaysIndex().xy;
    g_normalDepth[launchIndex] = float4(payload.normal, payload.hitT);
    g_materialGuide[launchIndex] = float4(
        payload.baseColor,
        payload.roughness + (payload.dynamicInstance != 0u ? 2.0f : 0.0f));
}

void TracePath(
    RayDesc ray,
    uint subSampleIndex,
    out float3 sampleRadiance,
    out float3 primaryDiffuseDenoisingRadiance,
    out float3 primarySpecularDenoisingRadiance)
{
    sampleRadiance = float3(0.0f, 0.0f, 0.0f);
    primaryDiffuseDenoisingRadiance = float3(0.0f, 0.0f, 0.0f);
    primarySpecularDenoisingRadiance = float3(0.0f, 0.0f, 0.0f);

    float3 pathThroughput = float3(1.0f, 1.0f, 1.0f);
    float3 tailThroughput = float3(1.0f, 1.0f, 1.0f);
    float3 tailRadiance = float3(0.0f, 0.0f, 0.0f);
    float3 firstDiffuseWeight = float3(0.0f, 0.0f, 0.0f);
    float3 firstSpecularWeight = float3(0.0f, 0.0f, 0.0f);
    bool hasFirstPbrSplit = false;
    bool primarySurfaceHit = false;
    float previousBsdfPdf = 0.0f;
    uint previousWasDelta = 1u;

    for (uint depth = 0u; depth <= g_maxBounce; ++depth)
    {
        SurfaceQueryPayload payload;
        ResetSurfaceQueryPayload(payload);
        RecordRadianceRay(depth);
        TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);

        if (payload.hit == 0u)
        {
            RecordRadianceMiss();
            float3 localRadiance = float3(0.0f, 0.0f, 0.0f);
            if (g_showNormalColor == 0u &&
                (g_sceneType != c_scenePbrGgx ||
                 g_pbrDebugView == c_pbrDebugBeauty) &&
                g_sceneType == c_scenePbrGgx &&
                g_enableIbl != 0u)
            {
                float environmentWeight = 1.0f;
                if (depth > 0u &&
                    previousWasDelta == 0u &&
                    g_lightingMode != c_lightingModeBsdf)
                {
                    float lightPdf = EvaluateEnvironmentLightPdf(ray.Direction);
                    if (lightPdf > 0.0f)
                    {
                        if (g_lightingMode == c_lightingModeNee)
                            environmentWeight = 0.0f;
                        else if (g_lightingMode == c_lightingModeMis)
                            environmentWeight = PowerHeuristic(
                                previousBsdfPdf,
                                lightPdf);
                    }
                }
                localRadiance =
                    SampleEnvironmentMap(ray.Direction) * environmentWeight;
            }

            sampleRadiance += pathThroughput * localRadiance;
            if (depth > 0u)
                tailRadiance += tailThroughput * localRadiance;
            break;
        }

        RecordSurfaceHit();
        float3 normalColor = payload.normal * 0.5f + 0.5f;
        if (g_showNormalColor != 0u)
        {
            sampleRadiance = normalColor;
            break;
        }
        if (g_sceneType == c_scenePbrGgx &&
            g_pbrDebugView != c_pbrDebugBeauty)
        {
            if (g_pbrDebugView == c_pbrDebugNormal)
                sampleRadiance = normalColor;
            else if (g_pbrDebugView == c_pbrDebugDepth)
                sampleRadiance = DepthDebugColor(payload.hitT);
            else if (g_pbrDebugView == c_pbrDebugMaterialId)
                sampleRadiance = MaterialIdDebugColor(payload.primitiveIndex);
            else if (g_pbrDebugView == c_pbrDebugAlbedo)
                sampleRadiance = payload.baseColor;
            else if (g_pbrDebugView == c_pbrDebugMetallic)
                sampleRadiance = payload.metallic.xxx;
            else if (g_pbrDebugView == c_pbrDebugRoughness)
                sampleRadiance = payload.roughness.xxx;
            break;
        }

        if (payload.frontFace != 0u && any(payload.emission > 0.0f))
        {
            float emissionWeight = 1.0f;
            if (depth > 0u &&
                previousWasDelta == 0u &&
                g_lightingMode != c_lightingModeBsdf)
            {
                float lightPdf = EvaluateAreaLightPdf(
                    payload.primitiveIndex,
                    payload.hitT * payload.hitT,
                    normalize(ray.Direction));
                if (lightPdf > 0.0f)
                {
                    if (g_lightingMode == c_lightingModeNee)
                        emissionWeight = 0.0f;
                    else if (g_lightingMode == c_lightingModeMis)
                        emissionWeight = PowerHeuristic(
                            previousBsdfPdf,
                            lightPdf);
                }
            }
            float3 localRadiance = payload.emission * emissionWeight;
            sampleRadiance += pathThroughput * localRadiance;
            if (depth > 0u)
                tailRadiance += tailThroughput * localRadiance;
            break;
        }

        if (depth == 0u)
            primarySurfaceHit = true;

        if (depth >= g_maxBounce)
            break;

        float3 hitPosition = ray.Origin + ray.Direction * payload.hitT;
        PbrMaterial material;
        material.baseColor = payload.baseColor;
        material.metallic = payload.metallic;
        material.roughness = payload.roughness;
        material.emission = payload.emission;
        float3 localDirectDiffuseLighting = float3(0.0f, 0.0f, 0.0f);
        float3 localDirectSpecularLighting = float3(0.0f, 0.0f, 0.0f);
        uint directSeed =
            CreateRandomSeed(
                depth,
                payload.primitiveIndex,
                subSampleIndex) ^ 0xA511E9B3u;
        float3 directLightDirection;
        float3 radianceOverPdf;
        float lightPdf;
        bool directLightVisible = SampleDirectLight(
            payload.normal,
            hitPosition,
            directSeed,
            directLightDirection,
            radianceOverPdf,
            lightPdf);
        if (directLightVisible)
        {
            if (g_sceneType == c_scenePbrGgx)
            {
                float3 viewDirection = normalize(-ray.Direction);
                float bsdfPdf = PbrBrdfSamplingPdf(
                    material,
                    payload.normal,
                    viewDirection,
                    directLightDirection);
                float misWeight = g_lightingMode == c_lightingModeMis
                    ? PowerHeuristic(lightPdf, bsdfPdf)
                    : 1.0f;
                float3 diffuseBrdf;
                float3 specularBrdf;
                EvaluateBrdfComponents(
                    material,
                    payload.normal,
                    viewDirection,
                    directLightDirection,
                    diffuseBrdf,
                    specularBrdf);
                localDirectDiffuseLighting =
                    diffuseBrdf * radianceOverPdf * misWeight;
                localDirectSpecularLighting =
                    specularBrdf * radianceOverPdf * misWeight;
            }
            else
            {
                float nDotL = saturate(
                    dot(payload.normal, directLightDirection));
                float bsdfPdf = nDotL * c_invPi;
                float misWeight = g_lightingMode == c_lightingModeMis
                    ? PowerHeuristic(lightPdf, bsdfPdf)
                    : 1.0f;
                localDirectDiffuseLighting = payload.baseColor * c_invPi *
                    nDotL * radianceOverPdf * misWeight;
            }
        }

        float3 localDirectLighting =
            localDirectDiffuseLighting + localDirectSpecularLighting;
        sampleRadiance += pathThroughput * localDirectLighting;
        if (depth == 0u)
        {
            primaryDiffuseDenoisingRadiance +=
                localDirectDiffuseLighting;
            primarySpecularDenoisingRadiance +=
                localDirectSpecularLighting;
        }
        else
            tailRadiance += tailThroughput * localDirectLighting;

        uint seed = CreateRandomSeed(
            depth,
            payload.primitiveIndex,
            subSampleIndex);
        float3 sampleDirection;
        float3 bounceWeight;
        float samplePdf;
        float3 diffuseContribution = float3(0.0f, 0.0f, 0.0f);
        float3 specularContribution = float3(0.0f, 0.0f, 0.0f);
        if (g_sceneType == c_scenePbrGgx)
        {
            float3 viewDirection = normalize(-ray.Direction);
            if (!SamplePbrBrdfWithMixtureSampling(
                material,
                payload.normal,
                viewDirection,
                seed,
                sampleDirection,
                bounceWeight,
                samplePdf))
            {
                break;
            }
            if (depth == 0u && g_enableAtrous != 0u)
            {
                EvaluateBrdfComponents(
                    material,
                    payload.normal,
                    viewDirection,
                    sampleDirection,
                    diffuseContribution,
                    specularContribution);
            }
        }
        else
        {
            sampleDirection = RandomCosineHemisphereDirection(
                payload.normal,
                seed);
            bounceWeight = payload.baseColor;
            samplePdf = saturate(dot(payload.normal, sampleDirection)) * c_invPi;
        }

        uint nextDepth = depth + 1u;
        float3 nextThroughput = pathThroughput * bounceWeight;
        float survivalProbability = 1.0f;
        if (!SurvivesRussianRoulette(
            nextThroughput,
            nextDepth,
            seed,
            survivalProbability))
        {
            break;
        }
        float inverseSurvivalProbability = 1.0f / survivalProbability;
        float3 effectiveBounceWeight =
            bounceWeight * inverseSurvivalProbability;
        if (depth == 0u)
        {
            if (g_sceneType == c_scenePbrGgx && g_enableAtrous != 0u)
            {
                firstDiffuseWeight = diffuseContribution *
                    (inverseSurvivalProbability / samplePdf);
                firstSpecularWeight = specularContribution *
                    (inverseSurvivalProbability / samplePdf);
                hasFirstPbrSplit = true;
            }
            tailThroughput = float3(1.0f, 1.0f, 1.0f);
        }
        else
        {
            tailThroughput *= effectiveBounceWeight;
        }

        pathThroughput = nextThroughput * inverseSurvivalProbability;
        previousBsdfPdf = samplePdf;
        previousWasDelta = 0u;
        ray.Origin = hitPosition + payload.normal * c_rayOriginBias;
        ray.Direction = sampleDirection;
        ray.TMin = c_rayTMin;
        ray.TMax = c_rayTMax;
    }

    if (g_sceneType == c_scenePbrGgx)
    {
        if (hasFirstPbrSplit)
        {
            primaryDiffuseDenoisingRadiance +=
                firstDiffuseWeight * tailRadiance;
            primarySpecularDenoisingRadiance +=
                firstSpecularWeight * tailRadiance;
        }
    }
    else if (primarySurfaceHit)
    {
        // Lambert scenes have a single diffuse lobe. Keep visible emitters and
        // the environment in the unfiltered residual, but filter all lighting
        // received at a primary surface, including direct lighting.
        primaryDiffuseDenoisingRadiance = max(sampleRadiance, 0.0f);
    }
}

void RunRaygen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    bool guidesEnabled =
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
    float3 sampleDiffuseDenoisingRadiance =
        float3(0.0f, 0.0f, 0.0f);
    float3 sampleSpecularDenoisingRadiance =
        float3(0.0f, 0.0f, 0.0f);
    float3 previousPrimaryWorldPosition =
        float3(0.0f, 0.0f, 0.0f);
    uint primaryDynamicInstance = 0u;
    float3 primaryRayDirection = float3(0.0f, 0.0f, 0.0f);
    bool temporalHistoryAttempted = false;
    bool temporalHistoryAccepted = false;
    uint samplesPerPixel = clamp(g_samplesPerPixel, 1u, 8u);

    if (g_sceneType == c_scenePbrGpuValidation)
    {
        sampleRadiance = EvaluateGpuBrdfValidationSample(
            launchIndex,
            launchDim);
    }
    else
    {
        float aspectRatio = float(launchDim.x) / float(launchDim.y);
        float tanHalfFov = tan(c_verticalFovRadians * 0.5f);
        float3 cameraForward = normalize(g_cameraTarget - g_cameraPosition);
        float3 cameraRight = normalize(cross(c_cameraUp, cameraForward));
        float3 cameraUp = cross(cameraForward, cameraRight);

        for (uint subSampleIndex = 0u;
             subSampleIndex < samplesPerPixel;
             ++subSampleIndex)
        {
            float2 pixelOffset = float2(0.5f, 0.5f);
            if (g_enableAccumulation != 0u ||
                g_enableTemporalReprojection != 0u ||
                samplesPerPixel > 1u)
            {
                uint cameraSeed = CreateRandomSeed(
                    0u,
                    0x9E3779B9u,
                    subSampleIndex);
                pixelOffset = float2(
                    RandomFloat01(cameraSeed),
                    RandomFloat01(cameraSeed));
            }

            float2 uv =
                (float2(launchIndex) + pixelOffset) / float2(launchDim);
            float2 screenPosition = float2(
                (uv.x * 2.0f - 1.0f) * aspectRatio * tanHalfFov,
                (1.0f - uv.y * 2.0f) * tanHalfFov);

            RayDesc ray;
            ray.Origin = g_cameraPosition;
            ray.Direction = normalize(
                cameraForward +
                cameraRight * screenPosition.x +
                cameraUp * screenPosition.y);
            primaryRayDirection = ray.Direction;
            ray.TMin = c_rayTMin;
            ray.TMax = c_rayTMax;

            float3 subSampleRadiance;
            float3 subSampleDiffuseRadiance;
            float3 subSampleSpecularRadiance;
            TracePath(
                ray,
                subSampleIndex,
                subSampleRadiance,
                subSampleDiffuseRadiance,
                subSampleSpecularRadiance);
            sampleRadiance += subSampleRadiance;
            sampleDiffuseDenoisingRadiance +=
                subSampleDiffuseRadiance;
            sampleSpecularDenoisingRadiance +=
                subSampleSpecularRadiance;
        }

        float inverseSamplesPerPixel = 1.0f / float(samplesPerPixel);
        sampleRadiance *= inverseSamplesPerPixel;
        sampleDiffuseDenoisingRadiance *= inverseSamplesPerPixel;
        sampleSpecularDenoisingRadiance *= inverseSamplesPerPixel;

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

            TracePrimaryGuide(
                guideRay,
                previousPrimaryWorldPosition,
                primaryDynamicInstance);
            primaryRayDirection = guideRay.Direction;
        }
    }

    if (g_enableAccumulation == 0u &&
        g_enableTemporalReprojection == 0u &&
        g_enableAtrous == 0u)
    {
        float3 displayColor = IsLinearDebugView()
            ? saturate(sampleRadiance)
            : ToneMapForDisplay(sampleRadiance);
        g_output[launchIndex] = float4(displayColor, 1.0f);
        return;
    }

    float3 accumulatedColor = sampleRadiance;
    float3 accumulatedDiffuseRadiance =
        sampleDiffuseDenoisingRadiance;
    float3 accumulatedSpecularRadiance =
        sampleSpecularDenoisingRadiance;
    float sampleDiffuseLuminance = dot(
        sampleDiffuseDenoisingRadiance,
        float3(0.2126f, 0.7152f, 0.0722f));
    float sampleSpecularLuminance = dot(
        sampleSpecularDenoisingRadiance,
        float3(0.2126f, 0.7152f, 0.0722f));
    float2 accumulatedDiffuseMoments = float2(
        sampleDiffuseLuminance,
        sampleDiffuseLuminance * sampleDiffuseLuminance);
    float2 accumulatedSpecularMoments = float2(
        sampleSpecularLuminance,
        sampleSpecularLuminance * sampleSpecularLuminance);
    float localSampleCount = 1.0f;
    bool pixelTemporalMotion =
        TemporalCameraIsMoving() ||
        (g_dynamicObjectMoved != 0u &&
         primaryDynamicInstance != 0u);
    if (g_sampleIndex > 0)
    {
        int2 historyPixel = int2(launchIndex);
        bool historyValid = true;
        bool useBilinearHistory = false;
        TemporalHistorySample history;
        ResetTemporalHistorySample(history);
        if (g_enableTemporalReprojection != 0u)
        {
            temporalHistoryAttempted = true;
            float4 currentMaterial = g_materialGuide[launchIndex];
            if (TemporalCameraIsMoving() ||
                (g_dynamicObjectMoved != 0u &&
                 DynamicObjectReprojectionEnabled()))
            {
                float4 currentNormalDepth = g_normalDepth[launchIndex];
                bool currentHit =
                    currentNormalDepth.w >= 0.0f &&
                    currentMaterial.a >= 0.0f;
                float3 currentWorldPosition = currentHit
                    ? g_cameraPosition +
                        primaryRayDirection * currentNormalDepth.w
                    : float3(0.0f, 0.0f, 0.0f);
                bool currentDynamic =
                    currentHit &&
                    primaryDynamicInstance != 0u &&
                    IsDynamicGuide(currentMaterial);
                float3 reprojectionWorldPosition = currentDynamic
                    ? previousPrimaryWorldPosition
                    : currentWorldPosition;
                useBilinearHistory = true;
                historyValid = GatherValidatedHistory(
                    reprojectionWorldPosition,
                    primaryRayDirection,
                    currentNormalDepth,
                    currentMaterial,
                    launchDim,
                    history);
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

        if (historyValid && !useBilinearHistory)
        {
            float4 previousAccumulation =
                g_enableTemporalReprojection != 0u
                ? g_previousAccumulation.Load(int3(historyPixel, 0))
                : g_accumulation[launchIndex];
            float previousSampleCount =
                max(abs(previousAccumulation.a), 1.0f);
            history.radianceAverage =
                previousAccumulation.rgb / previousSampleCount;
            history.sampleCount = previousSampleCount;

            if (g_enableAtrous != 0u)
            {
                float4 previousDiffuse;
                float4 previousSpecular;
                float2 previousDiffuseMoments;
                float2 previousSpecularMoments;
                if (g_enableTemporalReprojection != 0u)
                {
                    previousDiffuse =
                        g_previousDiffuseIndirect.Load(
                            int3(historyPixel, 0));
                    previousSpecular =
                        g_previousSpecularIndirect.Load(
                            int3(historyPixel, 0));
                    previousDiffuseMoments =
                        g_previousDiffuseMoments.Load(
                            int3(historyPixel, 0));
                    previousSpecularMoments =
                        g_previousSpecularMoments.Load(
                            int3(historyPixel, 0));
                }
                else
                {
                    previousDiffuse =
                        g_diffuseIndirectAccumulation[launchIndex];
                    previousSpecular =
                        g_specularIndirectAccumulation[launchIndex];
                    previousDiffuseMoments =
                        g_diffuseLuminanceMoments[launchIndex];
                    previousSpecularMoments =
                        g_specularLuminanceMoments[launchIndex];
                }

                float diffuseSampleCount =
                    max(abs(previousDiffuse.a), 1.0f);
                float specularSampleCount =
                    max(abs(previousSpecular.a), 1.0f);
                history.diffuseAverage =
                    previousDiffuse.rgb / diffuseSampleCount;
                history.specularAverage =
                    previousSpecular.rgb / specularSampleCount;
                history.diffuseMomentAverage =
                    previousDiffuseMoments / diffuseSampleCount;
                history.specularMomentAverage =
                    previousSpecularMoments / specularSampleCount;
            }
        }

        if (historyValid)
        {
            temporalHistoryAccepted =
                g_enableTemporalReprojection != 0u;
            float retainedHistoryCount = history.sampleCount;
            if (g_enableTemporalReprojection != 0u)
            {
                if (pixelTemporalMotion)
                {
                    retainedHistoryCount = min(
                        retainedHistoryCount,
                        31.0f);
                }
                else if (g_enableAccumulation == 0u ||
                         g_dynamicObjectMoved != 0u)
                {
                    retainedHistoryCount = min(
                        retainedHistoryCount,
                        255.0f);
                }
            }
            accumulatedColor +=
                history.radianceAverage * retainedHistoryCount;
            if (g_enableAtrous != 0u)
            {
                accumulatedDiffuseRadiance +=
                    history.diffuseAverage * retainedHistoryCount;
                accumulatedSpecularRadiance +=
                    history.specularAverage * retainedHistoryCount;
                accumulatedDiffuseMoments +=
                    history.diffuseMomentAverage * retainedHistoryCount;
                accumulatedSpecularMoments +=
                    history.specularMomentAverage * retainedHistoryCount;
            }
            localSampleCount = retainedHistoryCount + 1.0f;
        }
    }

    g_accumulation[launchIndex] =
        float4(accumulatedColor, localSampleCount);
    if (g_enableAtrous != 0u)
    {
        g_diffuseIndirectAccumulation[launchIndex] =
            float4(accumulatedDiffuseRadiance, localSampleCount);
        g_specularIndirectAccumulation[launchIndex] =
            float4(accumulatedSpecularRadiance, localSampleCount);
        g_diffuseLuminanceMoments[launchIndex] =
            accumulatedDiffuseMoments;
        g_specularLuminanceMoments[launchIndex] =
            accumulatedSpecularMoments;
    }
    if (g_temporalDebugView == 1u)
    {
        float historyDisplayRange =
            pixelTemporalMotion ? 31.0f : 255.0f;
        float normalizedHistory = saturate(
            (localSampleCount - 1.0f) / historyDisplayRange);
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

[shader("raygeneration")]
void MyRaygenShader_PathTrace()
{
    RunRaygen();
}

[shader("miss")]
void MyMissShader_ShadowRay(inout ShadowPayload payload)
{
    payload.occluded = 0u;
}

[shader("anyhit")]
void MyAnyHitShader_AlphaMask(
    inout SurfaceQueryPayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    uint ignoredInstanceFlags;
    HitGeometryMetadata hitGeometry =
        GetHitGeometryMetadata(ignoredInstanceFlags);
    uint globalPrimitiveIndex =
        hitGeometry.primitiveOffset + PrimitiveIndex();
    SceneMaterial material = GetSceneMaterial(globalPrimitiveIndex);
    if (material.alphaCutoff < 0.0f)
        return;

    uint indexOffset =
        hitGeometry.indexOffset + PrimitiveIndex() * 3u;
    uint i0 = hitGeometry.vertexOffset + g_indices[indexOffset + 0u];
    uint i1 = hitGeometry.vertexOffset + g_indices[indexOffset + 1u];
    uint i2 = hitGeometry.vertexOffset + g_indices[indexOffset + 2u];
    float2 texCoord = InterpolateTexCoord(i0, i1, i2, attributes);
    if (!PassesSceneAlphaMask(globalPrimitiveIndex, texCoord))
        IgnoreHit();
}

[shader("closesthit")]
void MyClosestHitShader_SurfaceQuery(
    inout SurfaceQueryPayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    bool motionGuideQuery = payload.hit == 2u;
    uint instanceFlags;
    HitGeometryMetadata hitGeometry =
        GetHitGeometryMetadata(instanceFlags);
    uint globalPrimitiveIndex =
        hitGeometry.primitiveOffset + PrimitiveIndex();
    uint indexOffset =
        hitGeometry.indexOffset + PrimitiveIndex() * 3u;
    uint i0 = hitGeometry.vertexOffset + g_indices[indexOffset + 0u];
    uint i1 = hitGeometry.vertexOffset + g_indices[indexOffset + 1u];
    uint i2 = hitGeometry.vertexOffset + g_indices[indexOffset + 2u];

    float3 normal = InterpolateNormal(i0, i1, i2, attributes);
    float3x4 objectToWorldTransform = ObjectToWorld3x4();
    float3x3 objectToWorld = (float3x3)objectToWorldTransform;
    normal = normalize(mul(objectToWorld, normal));
    bool frontFace = dot(normal, WorldRayDirection()) < 0.0f;
    if (!frontFace)
        normal = -normal;

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

    PbrMaterial material;
    if (g_sceneType == c_scenePbrGgx)
    {
        material = GetPbrMaterial(globalPrimitiveIndex, texCoord);
    }
    else
    {
        material.baseColor = CornellSurfaceAlbedo(globalPrimitiveIndex);
        material.metallic = 0.0f;
        material.roughness = 1.0f;
        material.emission = SurfaceEmission(globalPrimitiveIndex);
    }

    payload.normal = normal;
    payload.hitT = RayTCurrent();
    payload.baseColor = material.baseColor;
    payload.metallic = material.metallic;
    payload.emission = material.emission;
    payload.roughness = material.roughness;
    payload.primitiveIndex = globalPrimitiveIndex;
    payload.dynamicInstance =
        (instanceFlags & c_instanceFlagDynamic) != 0u ? 1u : 0u;
    if (motionGuideQuery)
    {
        float3 previousWorldPosition =
            WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
        uint instanceIndex = InstanceID();
        if (instanceIndex < g_previousInstanceTransformCount)
        {
            float3 objectPosition =
                ObjectRayOrigin() + ObjectRayDirection() * RayTCurrent();
            InstanceTransform previousTransform =
                g_previousInstanceTransforms[instanceIndex];
            previousWorldPosition = float3(
                dot(previousTransform.row0.xyz, objectPosition) +
                    previousTransform.row0.w,
                dot(previousTransform.row1.xyz, objectPosition) +
                    previousTransform.row1.w,
                dot(previousTransform.row2.xyz, objectPosition) +
                    previousTransform.row2.w);
        }
        payload.emission = previousWorldPosition;
    }
    payload.frontFace = frontFace ? 1u : 0u;
    payload.hit = 1u;
}

[shader("miss")]
void MyMissShader_SurfaceQuery(inout SurfaceQueryPayload payload)
{
    payload.hit = 0u;
}
