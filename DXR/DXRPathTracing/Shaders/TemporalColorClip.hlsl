Texture2D<float4> g_currentTotal : register(t0);
Texture2D<float4> g_currentDiffuse : register(t1);
Texture2D<float4> g_currentSpecular : register(t2);
Texture2D<float4> g_normalHitDistance : register(t3);
Texture2D<float4> g_materialGuide : register(t4);
Texture2D<float4> g_previousMaterialGuide : register(t5);

RWTexture2D<float4> g_totalAccumulation : register(u0);
RWTexture2D<float4> g_diffuseAccumulation : register(u1);
RWTexture2D<float4> g_specularAccumulation : register(u2);
RWTexture2D<float2> g_diffuseMoments : register(u3);
RWTexture2D<float2> g_specularMoments : register(u4);
RWTexture2D<float4> g_debugOutput : register(u5);

cbuffer TemporalColorClipSettings : register(b0)
{
    uint2 g_resolution;
    float g_clipGamma;
    uint g_minNeighborhoodSamples;
    uint g_debugView;
    uint g_useCurrentFrameVisibleResidual;
};

static const uint c_groupSize = 8u;
static const uint c_filterRadius = 2u;
static const uint c_tileSize = c_groupSize + c_filterRadius * 2u;
static const uint c_tileElementCount = c_tileSize * c_tileSize;
static const float c_spatialKernel[5] =
{
    1.0f / 16.0f,
    4.0f / 16.0f,
    6.0f / 16.0f,
    4.0f / 16.0f,
    1.0f / 16.0f
};
groupshared float4 s_currentTotal[c_tileElementCount];
groupshared float4 s_currentDiffuse[c_tileElementCount];
groupshared float4 s_currentSpecular[c_tileElementCount];
groupshared float4 s_normalDepth[c_tileElementCount];
groupshared float4 s_materialGuide[c_tileElementCount];
groupshared float4 s_previousMaterialGuide[c_tileElementCount];

float GuideRoughness(float packedRoughness)
{
    if (packedRoughness < 1.5f)
        return packedRoughness;
    return packedRoughness -
        floor(packedRoughness * 0.5f) * 2.0f;
}

uint DynamicGuideInstance(float packedRoughness)
{
    if (packedRoughness < 1.5f)
        return 0u;
    return uint(floor(packedRoughness * 0.5f)) - 1u;
}

bool IsDynamicMaterial(float4 materialGuide)
{
    return materialGuide.a >= 1.5f;
}

bool PixelNeedsClipping(uint2 groupThreadId)
{
    // Restrict clipping to pixels occupied by a dynamic object now or in the
    // previous frame, plus a one-pixel border for its revealed background.
    // Camera-only motion deliberately bypasses this pass so the full frame
    // does not follow noisy 1 spp clipping bounds and become darker.
    [unroll]
    for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        [unroll]
        for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            uint sampleIndex = uint(
                int(groupThreadId.y) + int(c_filterRadius) + offsetY) *
                c_tileSize +
                uint(
                    int(groupThreadId.x) +
                    int(c_filterRadius) +
                    offsetX);
            if (IsDynamicMaterial(s_materialGuide[sampleIndex]) ||
                IsDynamicMaterial(s_previousMaterialGuide[sampleIndex]))
            {
                return true;
            }
        }
    }
    return false;
}

float3 RgbToYCoCg(float3 rgb)
{
    return float3(
        dot(rgb, float3(0.25f, 0.50f, 0.25f)),
        0.50f * rgb.r - 0.50f * rgb.b,
        -0.25f * rgb.r + 0.50f * rgb.g - 0.25f * rgb.b);
}

float3 YCoCgToRgb(float3 ycocg)
{
    return float3(
        ycocg.x + ycocg.y - ycocg.z,
        ycocg.x + ycocg.z,
        ycocg.x - ycocg.y - ycocg.z);
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 RadianceHistoryDifferenceColor(
    float4 accumulatedSignal,
    float3 currentSignal,
    float3 currentEstimateYCoCg,
    float effectiveSampleCount)
{
    float totalCount = max(abs(accumulatedSignal.a), 1.0f);
    float historyCount = totalCount - 1.0f;
    if (historyCount < 1.0f ||
        effectiveSampleCount < float(g_minNeighborhoodSamples))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 historyAverage = max(
        (accumulatedSignal.rgb - currentSignal) / historyCount,
        0.0f);
    float3 currentEstimate = max(
        YCoCgToRgb(currentEstimateYCoCg),
        0.0f);
    float historyLuminance = Luminance(historyAverage);
    float currentLuminance = Luminance(currentEstimate);
    float colorScale = max(
        max(length(historyAverage), length(currentEstimate)),
        0.01f);
    float relativeColorDifference =
        length(historyAverage - currentEstimate) / colorScale;
    float differenceStrength = saturate(
        relativeColorDifference / 0.50f);

    // Green means the stable current 5x5 estimate agrees with history.
    // Red means history is brighter (bright smear), blue means history is
    // darker (dark smear). Saturation is reached at 50% RGB difference.
    float3 mismatchColor = historyLuminance >= currentLuminance
        ? float3(1.0f, 0.0f, 0.0f)
        : float3(0.0f, 0.25f, 1.0f);
    return lerp(
        float3(0.0f, 1.0f, 0.0f),
        mismatchColor,
        differenceStrength);
}

float GuideWeight(
    float4 centerNormalDepth,
    float4 centerMaterial,
    float4 sampleNormalDepth,
    float4 sampleMaterial,
    int offsetX,
    int offsetY)
{
    bool centerHit =
        centerNormalDepth.w >= 0.0f && centerMaterial.a >= 0.0f;
    bool sampleHit =
        sampleNormalDepth.w >= 0.0f && sampleMaterial.a >= 0.0f;
    if (centerHit != sampleHit)
        return 0.0f;

    float spatialWeight =
        c_spatialKernel[offsetX + int(c_filterRadius)] *
        c_spatialKernel[offsetY + int(c_filterRadius)];
    if (!centerHit)
        return spatialWeight;

    if (DynamicGuideInstance(centerMaterial.a) !=
        DynamicGuideInstance(sampleMaterial.a))
    {
        return 0.0f;
    }

    float3 centerNormal = normalize(centerNormalDepth.xyz);
    float3 sampleNormal = normalize(sampleNormalDepth.xyz);
    float normalWeight = pow(
        saturate(dot(centerNormal, sampleNormal)),
        32.0f);
    float depthScale = max(centerNormalDepth.w * 0.03f, 0.02f);
    float depthWeight = exp(
        -abs(centerNormalDepth.w - sampleNormalDepth.w) / depthScale);
    float albedoWeight = exp(
        -length(centerMaterial.rgb - sampleMaterial.rgb) / 0.20f);
    float roughnessWeight = exp(
        -abs(
            GuideRoughness(centerMaterial.a) -
            GuideRoughness(sampleMaterial.a)) / 0.18f);
    return spatialWeight *
        normalWeight *
        depthWeight *
        albedoWeight *
        roughnessWeight;
}

void AccumulateNeighborhoodSample(
    float3 rgb,
    float weight,
    inout float3 valueSum,
    inout float3 squareSum)
{
    float3 value = RgbToYCoCg(max(rgb, 0.0f));
    valueSum += value * weight;
    squareSum += value * value * weight;
}

void FinalizeClipBounds(
    float3 valueSum,
    float3 squareSum,
    float weightSum,
    float weightSquareSum,
    out float3 lowerBound,
    out float3 upperBound,
    out float effectiveSampleCount)
{
    float inverseWeight = 1.0f / max(weightSum, 1.0e-6f);
    float3 mean = valueSum * inverseWeight;
    float3 variance = max(
        squareSum * inverseWeight - mean * mean,
        0.0f);
    float3 deviation = sqrt(variance);
    // Preserve a small multiplicative interval even when the 5x5 estimate is
    // locally uniform. This prevents sub-pixel numerical changes from turning
    // a stable interval into a single exact HDR value.
    deviation.x = max(
        deviation.x,
        abs(mean.x) * 0.025f + 0.001f);
    lowerBound = mean - g_clipGamma * deviation;
    upperBound = mean + g_clipGamma * deviation;
    effectiveSampleCount =
        weightSum * weightSum / max(weightSquareSum, 1.0e-6f);
}

void BuildClipBounds(
    uint2 groupThreadId,
    out float3 totalLower,
    out float3 totalUpper,
    out float3 diffuseLower,
    out float3 diffuseUpper,
    out float3 specularLower,
    out float3 specularUpper,
    out float effectiveSampleCount)
{
    uint centerIndex =
        (groupThreadId.y + c_filterRadius) * c_tileSize +
        groupThreadId.x + c_filterRadius;
    float4 centerNormalDepth = s_normalDepth[centerIndex];
    float4 centerMaterial = s_materialGuide[centerIndex];
    float3 totalSum = 0.0f;
    float3 totalSquareSum = 0.0f;
    float3 diffuseSum = 0.0f;
    float3 diffuseSquareSum = 0.0f;
    float3 specularSum = 0.0f;
    float3 specularSquareSum = 0.0f;
    float weightSum = 0.0f;
    float weightSquareSum = 0.0f;

    [unroll]
    for (int offsetY = -int(c_filterRadius);
         offsetY <= int(c_filterRadius);
         ++offsetY)
    {
        [unroll]
        for (int offsetX = -int(c_filterRadius);
             offsetX <= int(c_filterRadius);
             ++offsetX)
        {
            uint sampleIndex = uint(
                int(groupThreadId.y) + int(c_filterRadius) + offsetY) *
                c_tileSize +
                uint(
                    int(groupThreadId.x) +
                    int(c_filterRadius) +
                    offsetX);
            float4 sampleNormalDepth = s_normalDepth[sampleIndex];
            float4 sampleMaterial = s_materialGuide[sampleIndex];
            float weight = GuideWeight(
                centerNormalDepth,
                centerMaterial,
                sampleNormalDepth,
                sampleMaterial,
                offsetX,
                offsetY);
            if (weight <= 1.0e-6f)
                continue;

            AccumulateNeighborhoodSample(
                s_currentTotal[sampleIndex].rgb,
                weight,
                totalSum,
                totalSquareSum);
            AccumulateNeighborhoodSample(
                s_currentDiffuse[sampleIndex].rgb,
                weight,
                diffuseSum,
                diffuseSquareSum);
            AccumulateNeighborhoodSample(
                s_currentSpecular[sampleIndex].rgb,
                weight,
                specularSum,
                specularSquareSum);
            weightSum += weight;
            weightSquareSum += weight * weight;
        }
    }

    float ignoredEffectiveSampleCount;
    FinalizeClipBounds(
        totalSum,
        totalSquareSum,
        weightSum,
        weightSquareSum,
        totalLower,
        totalUpper,
        effectiveSampleCount);
    FinalizeClipBounds(
        diffuseSum,
        diffuseSquareSum,
        weightSum,
        weightSquareSum,
        diffuseLower,
        diffuseUpper,
        ignoredEffectiveSampleCount);
    FinalizeClipBounds(
        specularSum,
        specularSquareSum,
        weightSum,
        weightSquareSum,
        specularLower,
        specularUpper,
        ignoredEffectiveSampleCount);
}

float4 ClipReprojectedHistory(
    float4 accumulatedSignal,
    float3 currentSignal,
    float3 lowerBound,
    float3 upperBound,
    float effectiveSampleCount)
{
    float totalCount = max(abs(accumulatedSignal.a), 1.0f);
    float historyCount = totalCount - 1.0f;
    if (historyCount < 1.0f ||
        effectiveSampleCount < float(g_minNeighborhoodSamples))
    {
        return accumulatedSignal;
    }

    // RayGen has already added the current observation. Recover only the
    // reprojected history mean, clip that value, and then rebuild the sum.
    // This leaves the unbiased current sample untouched.
    float3 historyAverage =
        (accumulatedSignal.rgb - currentSignal) / historyCount;
    float3 historyYCoCg = RgbToYCoCg(max(historyAverage, 0.0f));
    float3 boundedHistoryYCoCg = clamp(
        historyYCoCg,
        lowerBound,
        upperBound);

    // The bounds come from a 5x5 edge-aware current-frame estimate.
    float3 clippedHistoryYCoCg = boundedHistoryYCoCg;

    float targetLuminance = max(clippedHistoryYCoCg.x, 0.0f);
    float3 clippedHistory = max(YCoCgToRgb(clippedHistoryYCoCg), 0.0f);
    float reconstructedLuminance =
        max(RgbToYCoCg(clippedHistory).x, 1.0e-6f);
    clippedHistory *= targetLuminance / reconstructedLuminance;
    return float4(
        currentSignal + clippedHistory * historyCount,
        totalCount);
}

void RecenterMoments(
    float4 clippedAccumulation,
    inout float2 momentSums)
{
    float sampleCount = max(abs(clippedAccumulation.a), 1.0f);
    float oldMean = momentSums.x / sampleCount;
    float oldMeanSquare = momentSums.y / sampleCount;
    float preservedVariance = max(
        oldMeanSquare - oldMean * oldMean,
        0.0f);
    float newMean = Luminance(
        clippedAccumulation.rgb / sampleCount);
    momentSums = float2(
        newMean * sampleCount,
        (preservedVariance + newMean * newMean) * sampleCount);
}

[numthreads(8, 8, 1)]
void CSMain(
    uint3 dispatchThreadId : SV_DispatchThreadID,
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID)
{
    uint flattenedThreadIndex =
        groupThreadId.y * c_groupSize + groupThreadId.x;
    int2 tileOrigin =
        int2(groupId.xy * c_groupSize) - int2(c_filterRadius, c_filterRadius);
    for (uint tileIndex = flattenedThreadIndex;
         tileIndex < c_tileElementCount;
         tileIndex += c_groupSize * c_groupSize)
    {
        uint tileY = tileIndex / c_tileSize;
        uint tileX = tileIndex - tileY * c_tileSize;
        int2 sourcePixel = clamp(
            tileOrigin + int2(tileX, tileY),
            int2(0, 0),
            int2(g_resolution) - int2(1, 1));
        s_currentTotal[tileIndex] =
            g_currentTotal.Load(int3(sourcePixel, 0));
        s_currentDiffuse[tileIndex] =
            g_currentDiffuse.Load(int3(sourcePixel, 0));
        s_currentSpecular[tileIndex] =
            g_currentSpecular.Load(int3(sourcePixel, 0));
        s_normalDepth[tileIndex] =
            g_normalHitDistance.Load(int3(sourcePixel, 0));
        s_materialGuide[tileIndex] =
            g_materialGuide.Load(int3(sourcePixel, 0));
        s_previousMaterialGuide[tileIndex] =
            g_previousMaterialGuide.Load(int3(sourcePixel, 0));
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_resolution))
        return;
    bool showRadianceHistoryDifference = g_debugView == 6u;
    if (!showRadianceHistoryDifference &&
        !PixelNeedsClipping(groupThreadId.xy))
        return;

    int2 centerPixel = int2(pixel);
    float3 totalLower;
    float3 totalUpper;
    float3 diffuseLower;
    float3 diffuseUpper;
    float3 specularLower;
    float3 specularUpper;
    float effectiveSampleCount;
    BuildClipBounds(
        groupThreadId.xy,
        totalLower,
        totalUpper,
        diffuseLower,
        diffuseUpper,
        specularLower,
        specularUpper,
        effectiveSampleCount);

    uint centerIndex =
        (groupThreadId.y + c_filterRadius) * c_tileSize +
        groupThreadId.x + c_filterRadius;
    float3 currentTotal = s_currentTotal[centerIndex].rgb;
    float3 currentDiffuse = s_currentDiffuse[centerIndex].rgb;
    float3 currentSpecular = s_currentSpecular[centerIndex].rgb;
    if (showRadianceHistoryDifference)
    {
        float3 currentEstimateYCoCg =
            (totalLower + totalUpper) * 0.5f;
        g_debugOutput[centerPixel] = float4(
            RadianceHistoryDifferenceColor(
                g_totalAccumulation[centerPixel],
                currentTotal,
                currentEstimateYCoCg,
                effectiveSampleCount),
            1.0f);
        return;
    }

    float4 clippedDiffuse = ClipReprojectedHistory(
        g_diffuseAccumulation[centerPixel],
        currentDiffuse,
        diffuseLower,
        diffuseUpper,
        effectiveSampleCount);
    float4 clippedSpecular = ClipReprojectedHistory(
        g_specularAccumulation[centerPixel],
        currentSpecular,
        specularLower,
        specularUpper,
        effectiveSampleCount);
    float4 clippedTotal;
    if (g_useCurrentFrameVisibleResidual != 0u)
    {
        // RayGen intentionally keeps camera-visible emission/environment out
        // of temporal history. Rebuild total from that current-frame residual
        // and the independently clipped diffuse/specular histories so this
        // pass does not reintroduce stale visible-light history.
        float3 currentVisibleResidual = max(
            currentTotal - currentDiffuse - currentSpecular,
            0.0f);
        float totalCount = max(abs(clippedDiffuse.a), 1.0f);
        clippedTotal = float4(
            currentVisibleResidual * totalCount +
            clippedDiffuse.rgb +
            clippedSpecular.rgb,
            totalCount);
    }
    else
    {
        clippedTotal = ClipReprojectedHistory(
            g_totalAccumulation[centerPixel],
            currentTotal,
            totalLower,
            totalUpper,
            effectiveSampleCount);
    }
    float2 diffuseMoments = g_diffuseMoments[centerPixel];
    float2 specularMoments = g_specularMoments[centerPixel];
    RecenterMoments(clippedDiffuse, diffuseMoments);
    RecenterMoments(clippedSpecular, specularMoments);
    g_totalAccumulation[centerPixel] = clippedTotal;
    g_diffuseAccumulation[centerPixel] = clippedDiffuse;
    g_specularAccumulation[centerPixel] = clippedSpecular;
    g_diffuseMoments[centerPixel] = diffuseMoments;
    g_specularMoments[centerPixel] = specularMoments;
}
