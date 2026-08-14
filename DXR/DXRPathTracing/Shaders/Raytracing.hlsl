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
    if (packedRoughness < 1.5f)
        return packedRoughness;
    return packedRoughness -
        floor(packedRoughness * 0.5f) * 2.0f;
}

bool IsDynamicGuide(float4 materialGuide)
{
    return materialGuide.a >= 1.5f;
}

uint GetDynamicGuideInstance(float4 materialGuide)
{
    if (!IsDynamicGuide(materialGuide))
        return 0u;
    return uint(floor(materialGuide.a * 0.5f)) - 1u;
}

bool TemporalCameraIsMoving()
{
    return
        length(g_cameraPosition - g_previousCameraPosition) > 1.0e-5f ||
        length(g_cameraTarget - g_previousCameraTarget) > 1.0e-5f;
}

static const uint c_historyAccepted = 0u;
static const uint c_historyRejectedProjection = 1u;
static const uint c_historyRejectedOutsideScreen = 2u;
static const uint c_historyRejectedHitMiss = 3u;
static const uint c_historyRejectedInstance = 4u;
static const uint c_historyRejectedHitDistance = 5u;
static const uint c_historyRejectedNormal = 6u;
static const uint c_historyRejectedMaterial = 7u;
static const uint c_historyRejectedNoSamples = 8u;
static const uint c_historyRejectedCoverage = 9u;
static const uint c_historyNotAttempted = 10u;
static const uint c_historyRejectedDirectionalShadow = 11u;
static const uint c_primaryVisibilitySurface = 0u;
static const uint c_primaryVisibilityEnvironment = 1u;
static const uint c_primaryVisibilityEmitter = 2u;

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

bool LoadTemporalHistoryTap(
    int2 historyPixel,
    float confidence,
    out TemporalHistorySample history)
{
    ResetTemporalHistorySample(history);
    float4 previousAccumulation =
        g_previousAccumulation.Load(int3(historyPixel, 0));
    if (previousAccumulation.a <= 0.0f)
        return false;

    float previousSampleCount =
        max(previousAccumulation.a, 1.0f);
    history.radianceAverage =
        previousAccumulation.rgb / previousSampleCount;
    history.sampleCount = previousSampleCount * confidence;

    if (TemporalLobeHistoryEnabled())
    {
        float4 previousDiffuse =
            g_previousDiffuseIndirect.Load(int3(historyPixel, 0));
        float4 previousSpecular =
            g_previousSpecularIndirect.Load(int3(historyPixel, 0));
        float diffuseSampleCount =
            max(abs(previousDiffuse.a), 1.0f);
        float specularSampleCount =
            max(abs(previousSpecular.a), 1.0f);
        history.diffuseAverage =
            previousDiffuse.rgb / diffuseSampleCount;
        history.specularAverage =
            previousSpecular.rgb / specularSampleCount;
        history.diffuseMomentAverage =
            g_previousDiffuseMoments.Load(int3(historyPixel, 0)) /
            diffuseSampleCount;
        history.specularMomentAverage =
            g_previousSpecularMoments.Load(int3(historyPixel, 0)) /
            specularSampleCount;
    }
    return true;
}

bool SampleTemporalHistoryBilinear(
    float2 historyUv,
    out TemporalHistorySample history)
{
    ResetTemporalHistorySample(history);
    float4 previousAccumulation =
        g_previousAccumulation.SampleLevel(
            g_environmentSampler,
            historyUv,
            0.0f);
    if (previousAccumulation.a <= 0.0f)
        return false;

    float previousSampleCount =
        max(previousAccumulation.a, 1.0f);
    history.radianceAverage =
        previousAccumulation.rgb / previousSampleCount;
    history.sampleCount = previousSampleCount;

    if (TemporalLobeHistoryEnabled())
    {
        float4 previousDiffuse =
            g_previousDiffuseIndirect.SampleLevel(
                g_environmentSampler,
                historyUv,
                0.0f);
        float4 previousSpecular =
            g_previousSpecularIndirect.SampleLevel(
                g_environmentSampler,
                historyUv,
                0.0f);
        float diffuseSampleCount =
            max(abs(previousDiffuse.a), 1.0f);
        float specularSampleCount =
            max(abs(previousSpecular.a), 1.0f);
        history.diffuseAverage =
            previousDiffuse.rgb / diffuseSampleCount;
        history.specularAverage =
            previousSpecular.rgb / specularSampleCount;
        history.diffuseMomentAverage =
            g_previousDiffuseMoments.SampleLevel(
                g_environmentSampler,
                historyUv,
                0.0f) / diffuseSampleCount;
        history.specularMomentAverage =
            g_previousSpecularMoments.SampleLevel(
                g_environmentSampler,
                historyUv,
                0.0f) / specularSampleCount;
    }
    return true;
}

bool ProjectToPreviousFrame(
    float3 worldPosition,
    float3 currentRayDirection,
    bool currentHit,
    uint2 resolution,
    out float2 historyPosition,
    out float expectedPreviousDepth,
    out uint rejectionReason)
{
    historyPosition = float2(0.0f, 0.0f);
    expectedPreviousDepth = -1.0f;
    rejectionReason = c_historyRejectedProjection;
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
    // Keep the projected position available for the motion-vector debug view
    // even when the previous location lies outside the image.
    historyPosition = previousUv * float2(resolution) - 0.5f;
    if (any(previousUv < 0.0f) || any(previousUv >= 1.0f))
    {
        rejectionReason = c_historyRejectedOutsideScreen;
        return false;
    }

    rejectionReason = c_historyAccepted;
    return true;
}

bool PreviousCameraRayDirection(
    int2 pixel,
    uint2 resolution,
    out float3 rayDirection)
{
    rayDirection = float3(0.0f, 0.0f, 0.0f);
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

    float2 uv =
        (float2(pixel) + 0.5f) / float2(resolution);
    float aspectRatio = float(resolution.x) / float(resolution.y);
    float tanHalfFov = tan(c_verticalFovRadians * 0.5f);
    float2 screenPosition = float2(
        (uv.x * 2.0f - 1.0f) * aspectRatio * tanHalfFov,
        (1.0f - uv.y * 2.0f) * tanHalfFov);
    rayDirection = normalize(
        previousForward +
        previousRight * screenPosition.x +
        previousUp * screenPosition.y);
    return true;
}

uint ValidateHistoryTap(
    int2 historyPixel,
    bool currentHit,
    float expectedPreviousDepth,
    float3 currentNormal,
    float4 currentMaterial,
    float currentDirectionalShadow,
    uint2 resolution)
{
    if (any(historyPixel < int2(0, 0)) ||
        any(historyPixel >= int2(resolution)))
    {
        return c_historyRejectedOutsideScreen;
    }

    float4 previousNormalDepth =
        g_previousNormalDepth.Load(int3(historyPixel, 0));
    float4 previousMaterial =
        g_previousMaterialGuide.Load(int3(historyPixel, 0));
    bool previousHit =
        previousNormalDepth.w >= 0.0f && previousMaterial.a >= 0.0f;
    if (currentHit != previousHit)
        return c_historyRejectedHitMiss;
    if (!currentHit)
        return c_historyAccepted;

    uint currentDynamicInstance =
        GetDynamicGuideInstance(currentMaterial);
    uint previousDynamicInstance =
        GetDynamicGuideInstance(previousMaterial);
    bool currentDynamic = currentDynamicInstance != 0u;
    bool previousDynamic = previousDynamicInstance != 0u;
    if (g_dynamicObjectMoved != 0u &&
        !DynamicObjectReprojectionEnabled())
    {
        if (currentDynamic || previousDynamic)
            return c_historyRejectedInstance;
    }
    else if (currentDynamicInstance != previousDynamicInstance)
    {
        return c_historyRejectedInstance;
    }

    float depthTolerance = max(0.02f, expectedPreviousDepth * 0.02f);
    if (abs(previousNormalDepth.w - expectedPreviousDepth) > depthTolerance)
        return c_historyRejectedHitDistance;

    float normalAgreement = dot(
        normalize(currentNormal),
        normalize(previousNormalDepth.xyz));
    if (normalAgreement < 0.90f)
        return c_historyRejectedNormal;

    if (length(currentMaterial.rgb - previousMaterial.rgb) > 0.15f)
        return c_historyRejectedMaterial;
    if (abs(
        UnpackGuideRoughness(currentMaterial.a) -
        UnpackGuideRoughness(previousMaterial.a)) > 0.15f)
    {
        return c_historyRejectedMaterial;
    }
    if (DynamicShadowHistoryValidationEnabled() &&
        g_dynamicObjectMoved != 0u &&
        currentDirectionalShadow >= 0.0f)
    {
        float previousDirectionalShadow =
            g_previousDirectionalShadowGuide.Load(
                int3(historyPixel, 0));
        if (previousDirectionalShadow >= 0.0f &&
            abs(
                currentDirectionalShadow -
                previousDirectionalShadow) > 0.5f)
        {
            return c_historyRejectedDirectionalShadow;
        }
    }
    return c_historyAccepted;
}

bool GatherValidatedHistory(
    float3 worldPosition,
    float3 currentRayDirection,
    float4 currentNormalHitDistance,
    float4 currentMaterial,
    uint2 currentPixel,
    uint2 resolution,
    out TemporalHistorySample history,
    out float2 motionVectorPixels,
    out uint rejectionReason,
    out float relativeSurfaceError)
{
    ResetTemporalHistorySample(history);
    motionVectorPixels = float2(0.0f, 0.0f);
    rejectionReason = c_historyRejectedProjection;
    relativeSurfaceError = -1.0f;
    bool currentHit =
        currentNormalHitDistance.w >= 0.0f && currentMaterial.a >= 0.0f;
    float2 historyPosition;
    float expectedPreviousDepth;
    uint projectionReason;
    if (!ProjectToPreviousFrame(
        worldPosition,
        currentRayDirection,
        currentHit,
        resolution,
        historyPosition,
        expectedPreviousDepth,
        projectionReason))
    {
        motionVectorPixels =
            historyPosition - float2(currentPixel);
        rejectionReason = projectionReason;
        return false;
    }
    motionVectorPixels = historyPosition - float2(currentPixel);

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

    // Normal rendering uses smooth bilinear history only when every tap
    // participating in the footprint belongs to the validated surface.
    // At an edge or disocclusion, choose the valid tap closest to the
    // fractional reprojection position instead of blending across surfaces.
    // The surface-error debug view retains the original weighted path below.
    if (BestTapHistoryGatherEnabled() &&
        g_temporalDebugView != 7u)
    {
        uint validMask = 0u;
        uint validCount = 0u;
        uint weightedTapCount = 0u;
        float currentDirectionalShadow =
            g_directionalShadowGuide[currentPixel];
        rejectionReason = c_historyRejectedOutsideScreen;

        [unroll]
        for (uint tapIndex = 0u; tapIndex < 4u; ++tapIndex)
        {
            float tapWeight = tapWeights[tapIndex];
            if (tapWeight <= 0.0f)
                continue;

            ++weightedTapCount;
            int2 historyPixel = basePixel + tapOffsets[tapIndex];
            uint tapRejectionReason = ValidateHistoryTap(
                historyPixel,
                currentHit,
                expectedPreviousDepth,
                currentNormalHitDistance.xyz,
                currentMaterial,
                currentDirectionalShadow,
                resolution);
            if (tapRejectionReason == c_historyAccepted)
            {
                validMask |= 1u << tapIndex;
                ++validCount;
            }
            else if (
                rejectionReason == c_historyRejectedOutsideScreen ||
                tapRejectionReason != c_historyRejectedOutsideScreen)
            {
                rejectionReason = tapRejectionReason;
            }
        }

        if (validCount == 0u)
            return false;

        if (validCount == weightedTapCount)
        {
            float2 historyUv =
                (historyPosition + 0.5f) / float2(resolution);
            if (!SampleTemporalHistoryBilinear(historyUv, history))
            {
                rejectionReason = c_historyRejectedNoSamples;
                return false;
            }
            rejectionReason = c_historyAccepted;
            return true;
        }

        uint remainingMask = validMask;
        [unroll]
        for (uint attempt = 0u; attempt < 4u; ++attempt)
        {
            uint bestTapIndex = 0u;
            float bestTapWeight = -1.0f;
            [unroll]
            for (uint tapIndex = 0u; tapIndex < 4u; ++tapIndex)
            {
                if ((remainingMask & (1u << tapIndex)) != 0u &&
                    tapWeights[tapIndex] > bestTapWeight)
                {
                    bestTapIndex = tapIndex;
                    bestTapWeight = tapWeights[tapIndex];
                }
            }

            if (bestTapWeight < 0.0f)
                break;

            remainingMask &= ~(1u << bestTapIndex);
            int2 historyPixel =
                basePixel + tapOffsets[bestTapIndex];
            if (LoadTemporalHistoryTap(
                    historyPixel,
                    bestTapWeight,
                    history))
            {
                rejectionReason = c_historyAccepted;
                return true;
            }
        }

        rejectionReason = c_historyRejectedNoSamples;
        return false;
    }

    float validWeight = 0.0f;
    float weightedSampleCount = 0.0f;
    float surfaceErrorSum = 0.0f;
    float surfaceErrorWeight = 0.0f;
    rejectionReason = c_historyRejectedOutsideScreen;
    float currentDirectionalShadow =
        g_directionalShadowGuide[currentPixel];
    [unroll]
    for (uint tapIndex = 0u; tapIndex < 4u; ++tapIndex)
    {
        float tapWeight = tapWeights[tapIndex];
        if (tapWeight <= 0.0f)
            continue;

        int2 historyPixel = basePixel + tapOffsets[tapIndex];
        uint tapRejectionReason = ValidateHistoryTap(
            historyPixel,
            currentHit,
            expectedPreviousDepth,
            currentNormalHitDistance.xyz,
            currentMaterial,
            currentDirectionalShadow,
            resolution);

        if (g_temporalDebugView == 7u &&
            currentHit &&
            all(historyPixel >= int2(0, 0)) &&
            all(historyPixel < int2(resolution)))
        {
            float4 previousNormalDepth =
                g_previousNormalDepth.Load(int3(historyPixel, 0));
            float4 previousMaterial =
                g_previousMaterialGuide.Load(int3(historyPixel, 0));
            if (previousNormalDepth.w >= 0.0f &&
                previousMaterial.a >= 0.0f)
            {
                float3 previousRayDirection;
                if (PreviousCameraRayDirection(
                    historyPixel,
                    resolution,
                    previousRayDirection))
                {
                    float3 reconstructedPreviousWorldPosition =
                        g_previousCameraPosition +
                        previousRayDirection * previousNormalDepth.w;
                    surfaceErrorSum += tapWeight * length(
                        reconstructedPreviousWorldPosition -
                        worldPosition);
                    surfaceErrorWeight += tapWeight;
                }
            }
        }

        if (tapRejectionReason != c_historyAccepted)
        {
            if (rejectionReason == c_historyRejectedOutsideScreen ||
                tapRejectionReason != c_historyRejectedOutsideScreen)
            {
                rejectionReason = tapRejectionReason;
            }
            continue;
        }

        float4 previousAccumulation =
            g_previousAccumulation.Load(int3(historyPixel, 0));
        if (previousAccumulation.a <= 0.0f)
        {
            rejectionReason = c_historyRejectedNoSamples;
            continue;
        }

        float tapSampleCount = max(previousAccumulation.a, 1.0f);
        history.radianceAverage += tapWeight *
            previousAccumulation.rgb / tapSampleCount;
        weightedSampleCount += tapWeight * tapSampleCount;

        if (TemporalLobeHistoryEnabled())
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

    if (surfaceErrorWeight > 1.0e-6f)
    {
        relativeSurfaceError =
            (surfaceErrorSum / surfaceErrorWeight) /
            max(expectedPreviousDepth, 1.0e-3f);
    }

    // Avoid stretching a tiny surviving bilinear tap across a disocclusion.
    if (validWeight < 0.10f)
    {
        if (validWeight > 0.0f)
            rejectionReason = c_historyRejectedCoverage;
        return false;
    }

    float inverseValidWeight = 1.0f / validWeight;
    history.radianceAverage *= inverseValidWeight;
    history.diffuseAverage *= inverseValidWeight;
    history.specularAverage *= inverseValidWeight;
    history.diffuseMomentAverage *= inverseValidWeight;
    history.specularMomentAverage *= inverseValidWeight;
    // Keep the radiance unbiased by renormalizing the surviving taps, but do
    // not promote a small surviving bilinear footprint back to full temporal
    // confidence. At a disocclusion edge, one weak valid tap must contribute
    // proportionally less history instead of inheriting its entire count.
    history.sampleCount = max(weightedSampleCount, 1.0f);
    rejectionReason = c_historyAccepted;
    return true;
}

float3 HistoryRejectionColor(uint rejectionReason)
{
    switch (rejectionReason)
    {
    case c_historyAccepted:
        return float3(0.0f, 1.0f, 0.0f);
    case c_historyRejectedProjection:
        return float3(1.0f, 0.0f, 1.0f);
    case c_historyRejectedOutsideScreen:
        return float3(0.0f, 0.25f, 1.0f);
    case c_historyRejectedHitMiss:
        return float3(1.0f, 0.0f, 0.0f);
    case c_historyRejectedInstance:
        return float3(1.0f, 0.45f, 0.0f);
    case c_historyRejectedHitDistance:
        return float3(1.0f, 1.0f, 0.0f);
    case c_historyRejectedNormal:
        return float3(0.0f, 1.0f, 1.0f);
    case c_historyRejectedMaterial:
        return float3(0.55f, 0.0f, 1.0f);
    case c_historyRejectedNoSamples:
        return float3(0.45f, 0.45f, 0.45f);
    case c_historyRejectedCoverage:
        return float3(1.0f, 1.0f, 1.0f);
    case c_historyRejectedDirectionalShadow:
        return float3(1.0f, 0.0f, 0.35f);
    default:
        return float3(0.0f, 0.0f, 0.25f);
    }
}

float3 ReprojectionSurfaceErrorColor(float relativeError)
{
    if (relativeError < 0.0f)
        return float3(0.0f, 0.0f, 0.0f);

    // Green = 0%, yellow = 2.5%, red = 5% or larger relative
    // world-space reconstruction error.
    float normalizedError = saturate(relativeError / 0.05f);
    if (normalizedError < 0.5f)
    {
        return lerp(
            float3(0.0f, 1.0f, 0.0f),
            float3(1.0f, 1.0f, 0.0f),
            normalizedError * 2.0f);
    }
    return lerp(
        float3(1.0f, 1.0f, 0.0f),
        float3(1.0f, 0.0f, 0.0f),
        (normalizedError - 0.5f) * 2.0f);
}

bool IsLinearDebugView()
{
    return g_showNormalColor != 0 ||
        (IsPbrRenderingScene() &&
         g_pbrDebugView != c_pbrDebugBeauty);
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
    float3 ignoredDiffuseContribution;
    float3 ignoredSpecularContribution;
    if (!SamplePbrBrdfWithMixtureSampling(
        material,
        normal,
        viewDirection,
        seed,
        sampleDirection,
        weightedBrdf,
        samplePdf,
        ignoredDiffuseContribution,
        ignoredSpecularContribution))
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

float EvaluateDirectionalShadowGuide(
    float3 surfaceNormal,
    float3 surfacePosition)
{
    if (!DynamicShadowHistoryValidationEnabled() ||
        g_dynamicObjectMoved == 0u ||
        !DirectionalLightEnabled() ||
        g_lightingMode == c_lightingModeBsdf)
    {
        return -1.0f;
    }

    DirectionalLight light = g_directionalLights[0];
    if (light.enabled == 0u ||
        dot(light.direction, light.direction) <= 1.0e-8f)
    {
        return -1.0f;
    }

    float3 lightDirection = normalize(-light.direction);
    if (dot(surfaceNormal, lightDirection) <= 0.0f)
        return -1.0f;

    RayDesc shadowRay;
    shadowRay.Origin =
        surfacePosition + surfaceNormal * c_rayOriginBias;
    shadowRay.Direction = lightDirection;
    shadowRay.TMin = c_rayTMin;
    shadowRay.TMax = c_rayTMax;

    ShadowPayload shadowPayload;
    shadowPayload.occluded = 1u;
    RecordHistoryValidationShadowRay();
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
    return shadowPayload.occluded != 0u ? 0.0f : 1.0f;
}

void TracePrimaryGuide(
    RayDesc ray,
    out float3 previousWorldPosition,
    out uint dynamicInstance,
    out float3 visibleResidual,
    out uint visibilityClass)
{
    previousWorldPosition = ray.Origin;
    dynamicInstance = 0u;
    visibleResidual = float3(0.0f, 0.0f, 0.0f);
    visibilityClass = c_primaryVisibilitySurface;
    SurfaceQueryPayload payload;
    ResetSurfaceQueryPayload(payload);
    // The guide query does not consume emission. Mark it before TraceRay so
    // closest-hit can return the previous rigid-body position in that slot
    // without increasing the payload carried by every radiance bounce.
    payload.hit = DynamicObjectReprojectionEnabled() ? 2u : 0u;
    RecordPrimaryGuideRay();
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    if (payload.hit == 0u)
    {
        visibilityClass = c_primaryVisibilityEnvironment;
        if (g_showNormalColor == 0u &&
            IsPbrRenderingScene() &&
            g_pbrDebugView == c_pbrDebugBeauty &&
            g_enableIbl != 0u)
        {
            visibleResidual = SampleEnvironmentMap(ray.Direction);
        }
        return;
    }

    previousWorldPosition = payload.emission;
    dynamicInstance = payload.dynamicInstance;
    float3 surfaceEmission = SurfaceEmission(payload.primitiveIndex);
    if (payload.frontFace != 0u && any(surfaceEmission > 0.0f))
    {
        visibilityClass = c_primaryVisibilityEmitter;
        visibleResidual = surfaceEmission;
    }
    uint2 launchIndex = DispatchRaysIndex().xy;
    g_normalHitDistance[launchIndex] = float4(payload.normal, payload.hitT);
    g_materialGuide[launchIndex] = float4(
        payload.baseColor,
        payload.roughness +
            (payload.dynamicInstance != 0u
                ? float(payload.dynamicInstance) * 2.0f
                : 0.0f));
    g_metallicGuide[launchIndex] = payload.metallic;
    float3 hitPosition =
        ray.Origin + ray.Direction * payload.hitT;
    g_directionalShadowGuide[launchIndex] =
        EvaluateDirectionalShadowGuide(
            payload.normal,
            hitPosition);
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
                (!IsPbrRenderingScene() ||
                 g_pbrDebugView == c_pbrDebugBeauty) &&
                IsPbrRenderingScene() &&
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
        if (IsPbrRenderingScene() &&
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
        bool directLightIsDelta;
        bool directLightVisible = SampleDirectLight(
            payload.normal,
            hitPosition,
            directSeed,
            directLightDirection,
            radianceOverPdf,
            lightPdf,
            directLightIsDelta);
        if (directLightVisible)
        {
            if (IsPbrRenderingScene())
            {
                float3 viewDirection = normalize(-ray.Direction);
                float3 diffuseBrdf;
                float3 specularBrdf;
                float bsdfPdf;
                EvaluateBrdfComponentsAndPdf(
                    material,
                    payload.normal,
                    viewDirection,
                    directLightDirection,
                    diffuseBrdf,
                    specularBrdf,
                    bsdfPdf);
                float misWeight =
                    g_lightingMode == c_lightingModeMis &&
                    !directLightIsDelta
                    ? PowerHeuristic(lightPdf, bsdfPdf)
                    : 1.0f;
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
                float misWeight =
                    g_lightingMode == c_lightingModeMis &&
                    !directLightIsDelta
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
        if (IsPbrRenderingScene())
        {
            float3 viewDirection = normalize(-ray.Direction);
            if (!SamplePbrBrdfWithMixtureSampling(
                material,
                payload.normal,
                viewDirection,
                seed,
                sampleDirection,
                bounceWeight,
                samplePdf,
                diffuseContribution,
                specularContribution))
            {
                break;
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
            if (IsPbrRenderingScene() && TemporalLobeHistoryEnabled())
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

    if (IsPbrRenderingScene())
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

void RunDisocclusionRepairRaygen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float4 currentMaterial = g_materialGuide[launchIndex];
    float4 previousMaterial =
        g_previousMaterialGuide.Load(int3(launchIndex, 0));
    if (currentMaterial.a < 0.0f ||
        IsDynamicGuide(currentMaterial) ||
        !IsDynamicGuide(previousMaterial))
    {
        return;
    }

    // If temporal reprojection recovered valid history, more rays are not
    // needed. A count close to one identifies the newly revealed pixels whose
    // history was rejected by the main pass.
    float4 currentAccumulation = g_accumulation[launchIndex];
    float currentFrameCount = max(abs(currentAccumulation.a), 1.0f);
    if (currentFrameCount > 1.5f)
        return;

    uint mainSamplesPerPixel = clamp(g_samplesPerPixel, 1u, 8u);
    uint repairSamplesPerPixel = DisocclusionRepairSamplesPerPixel();
    float3 repairRadianceSum = float3(0.0f, 0.0f, 0.0f);
    float3 repairDiffuseSum = float3(0.0f, 0.0f, 0.0f);
    float3 repairSpecularSum = float3(0.0f, 0.0f, 0.0f);

    float aspectRatio = float(launchDim.x) / float(launchDim.y);
    float tanHalfFov = tan(c_verticalFovRadians * 0.5f);
    float3 cameraForward = normalize(g_cameraTarget - g_cameraPosition);
    float3 cameraRight = normalize(cross(c_cameraUp, cameraForward));
    float3 cameraUp = cross(cameraForward, cameraRight);

    for (uint repairIndex = 0u;
         repairIndex < repairSamplesPerPixel;
         ++repairIndex)
    {
        uint subSampleIndex = mainSamplesPerPixel + repairIndex;
        uint cameraSeed = CreateRandomSeed(
            0u,
            0xD15C0C1u,
            subSampleIndex);
        float2 pixelOffset = float2(
            RandomFloat01(cameraSeed),
            RandomFloat01(cameraSeed));
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
        ray.TMin = c_rayTMin;
        ray.TMax = c_rayTMax;

        float3 repairRadiance;
        float3 repairDiffuse;
        float3 repairSpecular;
        TracePath(
            ray,
            subSampleIndex,
            repairRadiance,
            repairDiffuse,
            repairSpecular);
        repairRadianceSum += repairRadiance;
        repairDiffuseSum += repairDiffuse;
        repairSpecularSum += repairSpecular;
    }

    float mainWeight = float(mainSamplesPerPixel);
    float totalWeight =
        mainWeight + float(repairSamplesPerPixel);
    float4 currentDiffuse =
        g_diffuseIndirectAccumulation[launchIndex];
    float4 currentSpecular =
        g_specularIndirectAccumulation[launchIndex];
    float diffuseFrameCount = max(abs(currentDiffuse.a), 1.0f);
    float specularFrameCount = max(abs(currentSpecular.a), 1.0f);
    float3 mainRadiance =
        currentAccumulation.rgb / currentFrameCount;
    float3 mainDiffuse =
        currentDiffuse.rgb / diffuseFrameCount;
    float3 mainSpecular =
        currentSpecular.rgb / specularFrameCount;
    float3 combinedDiffuse =
        (mainDiffuse * mainWeight + repairDiffuseSum) / totalWeight;
    float3 combinedSpecular =
        (mainSpecular * mainWeight + repairSpecularSum) / totalWeight;
    float3 combinedRadiance;
    if (CurrentFrameVisibleResidualEnabled())
    {
        float3 visibleResidual = max(
            mainRadiance - mainDiffuse - mainSpecular,
            0.0f);
        combinedRadiance =
            visibleResidual + combinedDiffuse + combinedSpecular;
    }
    else
    {
        combinedRadiance =
            (mainRadiance * mainWeight + repairRadianceSum) /
            totalWeight;
    }

    float diffuseLuminance = dot(
        combinedDiffuse,
        float3(0.2126f, 0.7152f, 0.0722f));
    float specularLuminance = dot(
        combinedSpecular,
        float3(0.2126f, 0.7152f, 0.0722f));
    g_accumulation[launchIndex] =
        float4(combinedRadiance, 1.0f);
    g_diffuseIndirectAccumulation[launchIndex] =
        float4(combinedDiffuse, 1.0f);
    g_specularIndirectAccumulation[launchIndex] =
        float4(combinedSpecular, 1.0f);
    g_diffuseLuminanceMoments[launchIndex] =
        float2(diffuseLuminance, diffuseLuminance * diffuseLuminance);
    g_specularLuminanceMoments[launchIndex] =
        float2(specularLuminance, specularLuminance * specularLuminance);
    g_currentTotalRadiance[launchIndex] =
        float4(combinedRadiance, 1.0f);
    g_currentDiffuseRadiance[launchIndex] =
        float4(combinedDiffuse, 1.0f);
    g_currentSpecularRadiance[launchIndex] =
        float4(combinedSpecular, 1.0f);
    g_output[launchIndex] =
        float4(ToneMapForDisplay(combinedRadiance), 1.0f);
}

void RunRaygen()
{
    if (DisocclusionRepairPassEnabled())
    {
        RunDisocclusionRepairRaygen();
        return;
    }

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
        g_normalHitDistance[launchIndex] =
            float4(0.0f, 0.0f, 0.0f, -1.0f);
        g_materialGuide[launchIndex] =
            float4(0.0f, 0.0f, 0.0f, -1.0f);
        g_metallicGuide[launchIndex] = -1.0f;
        g_directionalShadowGuide[launchIndex] = -1.0f;
    }
    float3 sampleRadiance = float3(0.0f, 0.0f, 0.0f);
    float3 sampleDiffuseDenoisingRadiance =
        float3(0.0f, 0.0f, 0.0f);
    float3 sampleSpecularDenoisingRadiance =
        float3(0.0f, 0.0f, 0.0f);
    float3 previousPrimaryWorldPosition =
        float3(0.0f, 0.0f, 0.0f);
    uint primaryDynamicInstance = 0u;
    float3 primaryGuideVisibleResidual =
        float3(0.0f, 0.0f, 0.0f);
    uint primaryVisibilityClass = c_primaryVisibilitySurface;
    float3 primaryRayDirection = float3(0.0f, 0.0f, 0.0f);
    bool temporalHistoryAttempted = false;
    bool temporalHistoryAccepted = false;
    float2 temporalMotionVectorPixels = float2(0.0f, 0.0f);
    uint temporalRejectionReason = c_historyNotAttempted;
    float temporalRelativeSurfaceError = -1.0f;
    uint samplesPerPixel = clamp(g_samplesPerPixel, 1u, 8u);
    bool useCurrentFrameVisibleResidual =
        g_enableTemporalReprojection != 0u &&
        CurrentFrameVisibleResidualEnabled() &&
        g_sceneType != c_scenePbrGpuValidation &&
        g_showNormalColor == 0u &&
        (!IsPbrRenderingScene() ||
         g_pbrDebugView == c_pbrDebugBeauty);

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
                primaryDynamicInstance,
                primaryGuideVisibleResidual,
                primaryVisibilityClass);
            primaryRayDirection = guideRay.Direction;
        }

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

        if (useCurrentFrameVisibleResidual &&
            primaryVisibilityClass != c_primaryVisibilitySurface)
        {
            // The unjittered guide owns primary visibility. Do not mix a
            // jittered surface sample with guide-visible emission/environment
            // at a silhouette pixel.
            sampleDiffuseDenoisingRadiance =
                float3(0.0f, 0.0f, 0.0f);
            sampleSpecularDenoisingRadiance =
                float3(0.0f, 0.0f, 0.0f);
        }

        if (g_enableTemporalReprojection != 0u &&
            (g_enableAtrous != 0u ||
             g_temporalDebugView == 8u))
        {
            // Preserve the unaccumulated current-frame observations. A later
            // compute pass uses their 5x5 neighborhood to constrain only the
            // reprojected history before A-Trous consumes these scratch maps.
            float3 currentTotalRadiance = useCurrentFrameVisibleResidual
                ? primaryGuideVisibleResidual +
                    sampleDiffuseDenoisingRadiance +
                    sampleSpecularDenoisingRadiance
                : sampleRadiance;
            g_currentTotalRadiance[launchIndex] =
                float4(max(currentTotalRadiance, 0.0f), 1.0f);
            g_currentDiffuseRadiance[launchIndex] =
                float4(max(sampleDiffuseDenoisingRadiance, 0.0f), 1.0f);
            g_currentSpecularRadiance[launchIndex] =
                float4(max(sampleSpecularDenoisingRadiance, 0.0f), 1.0f);
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
    // The lobe split contains all radiance received at a primary surface.
    // What remains is camera-visible emission/environment. Keeping that
    // residual current-frame-only prevents an old visible area light from
    // being reprojected as an unfiltered bright trail onto another surface.
    float3 currentVisibleResidual = useCurrentFrameVisibleResidual
        ? primaryGuideVisibleResidual
        : max(
            sampleRadiance -
            sampleDiffuseDenoisingRadiance -
            sampleSpecularDenoisingRadiance,
            0.0f);
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
    bool cameraTemporalMotion = TemporalCameraIsMoving();
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
            float4 currentNormalHitDistance =
                g_normalHitDistance[launchIndex];
            bool currentHit =
                currentNormalHitDistance.w >= 0.0f &&
                currentMaterial.a >= 0.0f;
            bool currentDynamic =
                currentHit &&
                primaryDynamicInstance != 0u &&
                IsDynamicGuide(currentMaterial);
            if (cameraTemporalMotion ||
                (g_dynamicObjectMoved != 0u &&
                 DynamicObjectReprojectionEnabled() &&
                 (!StaticBackgroundHistoryFastPathEnabled() ||
                  currentDynamic)))
            {
                float3 currentWorldPosition = currentHit
                    ? g_cameraPosition +
                        primaryRayDirection * currentNormalHitDistance.w
                    : float3(0.0f, 0.0f, 0.0f);
                float3 reprojectionWorldPosition = currentDynamic
                    ? previousPrimaryWorldPosition
                    : currentWorldPosition;
                useBilinearHistory = true;
                historyValid = GatherValidatedHistory(
                    reprojectionWorldPosition,
                    primaryRayDirection,
                    currentNormalHitDistance,
                    currentMaterial,
                    launchIndex,
                    launchDim,
                    history,
                    temporalMotionVectorPixels,
                    temporalRejectionReason,
                    temporalRelativeSurfaceError);
            }
            else if (g_dynamicObjectMoved != 0u)
            {
                float4 previousMaterial =
                    g_previousMaterialGuide.Load(
                        int3(historyPixel, 0));
                bool materialHistoryValid =
                    !IsDynamicGuide(currentMaterial) &&
                    !IsDynamicGuide(previousMaterial);
                bool shadowHistoryValid = true;
                if (DynamicShadowHistoryValidationEnabled())
                {
                    float currentDirectionalShadow =
                        g_directionalShadowGuide[launchIndex];
                    float previousDirectionalShadow =
                        g_previousDirectionalShadowGuide.Load(
                            int3(historyPixel, 0));
                    shadowHistoryValid =
                        currentDirectionalShadow < 0.0f ||
                        previousDirectionalShadow < 0.0f ||
                        abs(
                            currentDirectionalShadow -
                            previousDirectionalShadow) <= 0.5f;
                }
                historyValid =
                    materialHistoryValid && shadowHistoryValid;
                temporalRejectionReason =
                    !materialHistoryValid
                    ? c_historyRejectedInstance
                    : (!shadowHistoryValid
                        ? c_historyRejectedDirectionalShadow
                        : c_historyAccepted);
            }
            else
            {
                // With a static camera and static geometry, sub-pixel ray
                // jitter changes texture and normal-map samples even though
                // the same screen pixel is still valid. Reuse its history
                // directly instead of applying unstable geometric tests.
                historyValid = true;
                temporalRejectionReason = c_historyAccepted;
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
            if (TemporalLobeHistoryEnabled())
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
                if (cameraTemporalMotion)
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
            if (!useCurrentFrameVisibleResidual)
            {
                accumulatedColor +=
                    history.radianceAverage * retainedHistoryCount;
            }
            if (TemporalLobeHistoryEnabled())
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

    if (useCurrentFrameVisibleResidual)
    {
        accumulatedColor =
            currentVisibleResidual * localSampleCount +
            accumulatedDiffuseRadiance +
            accumulatedSpecularRadiance;
    }

    g_accumulation[launchIndex] =
        float4(accumulatedColor, localSampleCount);
    if (TemporalLobeHistoryEnabled())
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
            cameraTemporalMotion ? 31.0f : 255.0f;
        historyDisplayRange = max(historyDisplayRange, 1.0f);
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
    if (g_temporalDebugView == 3u)
    {
        // Current-to-previous motion magnitude in pixels.
        float motionMagnitude = saturate(
            length(temporalMotionVectorPixels) / 16.0f);
        g_output[launchIndex] = float4(
            motionMagnitude.xxx,
            1.0f);
        return;
    }
    if (g_temporalDebugView == 4u)
    {
        float normalizedMotionX = clamp(
            temporalMotionVectorPixels.x / 16.0f,
            -1.0f,
            1.0f);
        float displayValue = 0.5f + 0.5f * normalizedMotionX;
        g_output[launchIndex] = float4(displayValue.xxx, 1.0f);
        return;
    }
    if (g_temporalDebugView == 5u)
    {
        float normalizedMotionY = clamp(
            temporalMotionVectorPixels.y / 16.0f,
            -1.0f,
            1.0f);
        float displayValue = 0.5f + 0.5f * normalizedMotionY;
        g_output[launchIndex] = float4(displayValue.xxx, 1.0f);
        return;
    }
    if (g_temporalDebugView == 6u)
    {
        g_output[launchIndex] = float4(
            HistoryRejectionColor(temporalRejectionReason),
            1.0f);
        return;
    }
    if (g_temporalDebugView == 7u)
    {
        g_output[launchIndex] = float4(
            ReprojectionSurfaceErrorColor(
                temporalRelativeSurfaceError),
            1.0f);
        return;
    }
    if (g_temporalDebugView == 8u)
    {
        // The temporal compute pass replaces this placeholder with the
        // edge-aware current-vs-history radiance diagnostic.
        g_output[launchIndex] =
            float4(0.0f, 0.0f, 0.0f, 1.0f);
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

bool PassesCurrentAlphaMask(
    in BuiltInTriangleIntersectionAttributes attributes)
{
    uint ignoredInstanceFlags;
    HitGeometryMetadata hitGeometry =
        GetHitGeometryMetadata(ignoredInstanceFlags);
    uint globalPrimitiveIndex =
        hitGeometry.primitiveOffset + PrimitiveIndex();
    SceneMaterial material = GetSceneMaterial(globalPrimitiveIndex);
    if (material.alphaCutoff < 0.0f)
        return true;

    uint indexOffset =
        hitGeometry.indexOffset + PrimitiveIndex() * 3u;
    uint i0 = hitGeometry.vertexOffset + g_indices[indexOffset + 0u];
    uint i1 = hitGeometry.vertexOffset + g_indices[indexOffset + 1u];
    uint i2 = hitGeometry.vertexOffset + g_indices[indexOffset + 2u];
    float2 texCoord = InterpolateTexCoord(i0, i1, i2, attributes);
    float3x4 objectToWorldTransform = ObjectToWorld3x4();
    float3 worldNormal = normalize(mul(
        (float3x3)objectToWorldTransform,
        InterpolateNormal(i0, i1, i2, attributes)));
    float uvFootprint = EstimateTriangleUvFootprint(
        i0,
        i1,
        i2,
        objectToWorldTransform,
        worldNormal);
    return PassesSceneAlphaMask(
        material,
        texCoord,
        uvFootprint);
}

[shader("anyhit")]
void MyAnyHitShader_AlphaMask(
    inout SurfaceQueryPayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    if (!PassesCurrentAlphaMask(attributes))
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
    float uvFootprint = EstimateTriangleUvFootprint(
        i0,
        i1,
        i2,
        objectToWorldTransform,
        normal);
    if (IsPbrRenderingScene())
    {
        normal = ApplySceneNormalMap(
            globalPrimitiveIndex,
            texCoord,
            uvFootprint,
            tangent,
            normal);
    }

    PbrMaterial material;
    if (IsPbrRenderingScene())
    {
        material = GetPbrMaterial(
            globalPrimitiveIndex,
            texCoord,
            uvFootprint);
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
        motionGuideQuery &&
        (instanceFlags & c_instanceFlagDynamic) != 0u
            ? InstanceID() + 1u
            : 0u;
    if (motionGuideQuery)
    {
        float3 previousWorldPosition =
            WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
        uint instanceIndex = InstanceID();
        if ((instanceFlags & c_instanceFlagSkinned) != 0u &&
            SkinnedDeformationMotionEnabled())
        {
            float barycentric0 =
                1.0f - attributes.barycentrics.x -
                attributes.barycentrics.y;
            previousWorldPosition =
                barycentric0 * g_previousSkinnedPositions[i0].xyz +
                attributes.barycentrics.x *
                    g_previousSkinnedPositions[i1].xyz +
                attributes.barycentrics.y *
                    g_previousSkinnedPositions[i2].xyz;
        }
        else if (instanceIndex < g_previousInstanceTransformCount)
        {
            float4 objectPosition = float4(
                ObjectRayOrigin() + ObjectRayDirection() * RayTCurrent(),
                1.0f);
            row_major float3x4 previousObjectToWorld =
                g_previousInstanceTransforms[instanceIndex].objectToWorld;
            previousWorldPosition =
                mul(previousObjectToWorld, objectPosition);
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
