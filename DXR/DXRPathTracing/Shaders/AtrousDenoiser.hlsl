Texture2D<float4> g_source : register(t0);
Texture2D<float4> g_normalHitDistance : register(t1);
Texture2D<float4> g_materialGuide : register(t2);
Texture2D<float2> g_luminanceMoments : register(t3);
Texture2D<float4> g_diffuseIndirectAccumulation : register(t4);
Texture2D<float4> g_specularIndirectAccumulation : register(t5);
Texture2D<float4> g_totalAccumulation : register(t6);
Texture2D<float4> g_filteredDiffuse : register(t7);
Texture2D<float> g_metallicGuide : register(t8);
RWTexture2D<float4> g_destination : register(u0);

cbuffer AtrousSettings : register(b0)
{
    uint2 g_resolution;
    uint g_stepWidth;
    uint g_inputIsAccumulation;
    uint g_finalPass;
    float g_normalExponent;
    float g_depthSigma;
    float g_colorSigma;
    float g_exposure;
    uint g_demodulateDiffuse;
    uint g_filterChannel;
    uint g_specularMaterialWeightMode;
    uint g_specularRoughnessWeightMode;
    uint g_kernelMode;
    uint g_adaptiveEdgeWeights;
    float g_adaptiveDynamicNormalExponent;
    float g_adaptiveDynamicDepthSigma;
    float g_adaptiveLowHistoryNormalExponent;
    float g_adaptiveLowHistoryDepthSigma;
    float g_adaptiveStableNormalExponent;
    float g_adaptiveStableDepthSigma;
    uint g_passIndex;
    uint g_adaptiveIterations;
    uint g_debugView;
};

static const uint c_filterChannelDiffuse = 0u;
static const uint c_filterChannelSpecular = 1u;
static const uint c_specularMaterialWeightNone = 0u;
static const uint c_specularMaterialWeightAlbedo = 1u;
static const uint c_specularMaterialWeightF0 = 2u;
static const uint c_specularRoughnessWeightNone = 0u;
static const uint c_specularRoughnessWeightRoughness = 1u;
static const uint c_atrousKernel3x3 = 0u;
static const uint c_atrousKernel5x5 = 1u;
static const uint c_atrousDebugNone = 0u;
static const uint c_atrousDebugIterationCount = 1u;

static const float c_kernel3x3[3] =
{
    1.0f / 4.0f,
    2.0f / 4.0f,
    1.0f / 4.0f
};

// Standard separable B3 spline kernel used by A-Trous filtering.
static const float c_kernel5x5[5] =
{
    1.0f / 16.0f,
    4.0f / 16.0f,
    6.0f / 16.0f,
    4.0f / 16.0f,
    1.0f / 16.0f
};

int KernelRadius()
{
    return g_kernelMode == c_atrousKernel5x5 ? 2 : 1;
}

float KernelWeight1D(int offset)
{
    return g_kernelMode == c_atrousKernel5x5
        ? c_kernel5x5[offset + 2]
        : c_kernel3x3[offset + 1];
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float GuideRoughness(float packedRoughness)
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

uint DynamicGuideInstance(float4 materialGuide)
{
    return IsDynamicGuide(materialGuide)
        ? uint(floor(materialGuide.a * 0.5f)) - 1u
        : 0u;
}

float3 LoadAverageRadiance(Texture2D<float4> textureResource, int2 pixel)
{
    float4 value = textureResource.Load(int3(pixel, 0));
    return value.rgb / max(abs(value.a), 1.0f);
}

float3 DemodulateDiffuse(float3 indirectRadiance, int2 pixel)
{
    if (g_demodulateDiffuse == 0u)
        return indirectRadiance;

    float3 albedo = g_materialGuide.Load(int3(pixel, 0)).rgb;
    return indirectRadiance / max(albedo, 0.05f);
}

float3 RemodulateDiffuse(float3 filteredValue, int2 pixel)
{
    if (g_demodulateDiffuse == 0u)
        return filteredValue;
    return filteredValue * g_materialGuide.Load(int3(pixel, 0)).rgb;
}

float3 LoadFilterValue(int2 pixel)
{
    float3 value = g_source.Load(int3(pixel, 0)).rgb;
    if (g_inputIsAccumulation != 0u)
    {
        float4 accumulation = g_source.Load(int3(pixel, 0));
        value = accumulation.rgb / max(abs(accumulation.a), 1.0f);
        value = DemodulateDiffuse(value, pixel);
    }
    return value;
}

float3 GuideF0(float3 baseColor, float metallic)
{
    // Metallic-roughness workflow: dielectrics use about 4% normal-incidence
    // reflectance, while metals use their base color as colored F0.
    return lerp(0.04f.xxx, baseColor, saturate(metallic));
}

float MaterialEdgeWeight(
    float4 centerMaterial,
    float centerMetallic,
    float4 sampleMaterial,
    float sampleMetallic)
{
    if (g_filterChannel == c_filterChannelDiffuse ||
        g_specularMaterialWeightMode == c_specularMaterialWeightAlbedo)
    {
        return exp(
            -length(sampleMaterial.rgb - centerMaterial.rgb) / 0.12f);
    }
    if (g_specularMaterialWeightMode == c_specularMaterialWeightF0)
    {
        float3 centerF0 = GuideF0(centerMaterial.rgb, centerMetallic);
        float3 sampleF0 = GuideF0(sampleMaterial.rgb, sampleMetallic);
        return exp(-length(sampleF0 - centerF0) / 0.12f);
    }
    return 1.0f;
}

float RoughnessEdgeWeight(float centerRoughness, float sampleRoughness)
{
    return g_filterChannel == c_filterChannelSpecular &&
        g_specularRoughnessWeightMode ==
            c_specularRoughnessWeightRoughness
        ? exp(-abs(sampleRoughness - centerRoughness) / 0.10f)
        : 1.0f;
}

float ChannelNormalExponent(float roughness, float baseNormalExponent)
{
    if (g_filterChannel != c_filterChannelSpecular)
        return baseNormalExponent;

    // Glossy reflections vary rapidly for small normal changes. Tighten the
    // normal guide at low roughness and relax it for broad rough reflections.
    return lerp(
        baseNormalExponent * 4.0f,
        baseNormalExponent * 0.5f,
        saturate(roughness));
}

float SurfaceIdentityWeight(float4 centerMaterial, float4 sampleMaterial)
{
    if (g_adaptiveEdgeWeights == 0u &&
        g_adaptiveIterations == 0u)
        return 1.0f;

    bool centerDynamic = IsDynamicGuide(centerMaterial);
    bool sampleDynamic = IsDynamicGuide(sampleMaterial);
    if (!centerDynamic && !sampleDynamic)
        return 1.0f;
    if (centerDynamic != sampleDynamic)
        return 0.0f;
    return DynamicGuideInstance(centerMaterial) ==
        DynamicGuideInstance(sampleMaterial)
        ? 1.0f
        : 0.0f;
}

void AdaptiveEdgeParameters(
    int2 pixel,
    float4 materialGuide,
    float roughness,
    out float normalExponent,
    out float depthSigma)
{
    if (g_adaptiveEdgeWeights == 0u)
    {
        normalExponent = ChannelNormalExponent(
            roughness,
            g_normalExponent);
        depthSigma = g_depthSigma;
        return;
    }

    if (IsDynamicGuide(materialGuide))
    {
        // A dynamic instance is filtered aggressively only within the same
        // instance. SurfaceIdentityWeight prevents background color leakage.
        normalExponent = g_adaptiveDynamicNormalExponent;
        depthSigma = g_adaptiveDynamicDepthSigma;
        return;
    }

    float historyLength = max(
        abs(g_totalAccumulation.Load(int3(pixel, 0)).a),
        1.0f);
    float historyConfidence = saturate(
        (historyLength - 1.0f) / 15.0f);
    float stableNormalExponent = ChannelNormalExponent(
        roughness,
        g_adaptiveStableNormalExponent);
    normalExponent = lerp(
        g_adaptiveLowHistoryNormalExponent,
        stableNormalExponent,
        historyConfidence);
    depthSigma = lerp(
        g_adaptiveLowHistoryDepthSigma,
        g_adaptiveStableDepthSigma,
        historyConfidence);
}

uint AdaptiveIterationCount(
    int2 pixel,
    float4 materialGuide,
    float roughness)
{
    if (IsDynamicGuide(materialGuide))
        return 5u;

    uint stableIterationCount = 4u;
    if (g_filterChannel == c_filterChannelSpecular)
    {
        stableIterationCount = roughness < 0.2f
            ? 2u
            : (roughness < 0.6f ? 3u : 4u);
    }

    float historyLength = max(
        abs(g_totalAccumulation.Load(int3(pixel, 0)).a),
        1.0f);
    float historyConfidence = saturate(
        (historyLength - 1.0f) / 15.0f);
    float adaptiveCount = lerp(
        5.0f,
        float(stableIterationCount),
        historyConfidence);
    return (uint)clamp(ceil(adaptiveCount), 1.0f, 5.0f);
}

float CompatibleNeighborCount(
    int2 centerPixel,
    float4 centerMaterial,
    float3 centerNormal,
    float centerDepth,
    float centerRoughness,
    uint stepWidth)
{
    float normalExponent;
    float edgeDepthSigma;
    AdaptiveEdgeParameters(
        centerPixel,
        centerMaterial,
        centerRoughness,
        normalExponent,
        edgeDepthSigma);
    float depthScale =
        max(centerDepth, 0.01f) * max(edgeDepthSigma, 1.0e-5f);
    float validNeighborCount = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 samplePixel = clamp(
                centerPixel + int2(x, y) * int(stepWidth),
                int2(0, 0),
                int2(g_resolution) - int2(1, 1));
            float4 sampleNormalDepth =
                g_normalHitDistance.Load(int3(samplePixel, 0));
            float4 sampleMaterial =
                g_materialGuide.Load(int3(samplePixel, 0));
            if (sampleNormalDepth.w < 0.0f || sampleMaterial.a < 0.0f)
                continue;

            float normalWeight = pow(
                saturate(dot(
                    centerNormal,
                    normalize(sampleNormalDepth.xyz))),
                normalExponent);
            float depthWeight = exp(
                -abs(sampleNormalDepth.w - centerDepth) / depthScale);
            float identityWeight = SurfaceIdentityWeight(
                centerMaterial,
                sampleMaterial);
            if (identityWeight > 0.0f &&
                normalWeight > 0.05f &&
                depthWeight > 0.05f)
            {
                validNeighborCount += 1.0f;
            }
        }
    }
    return validNeighborCount;
}

uint EffectiveAdaptiveIterationCount(int2 pixel)
{
    float4 normalDepth = g_normalHitDistance.Load(int3(pixel, 0));
    float4 materialGuide = g_materialGuide.Load(int3(pixel, 0));
    if (normalDepth.w < 0.0f || materialGuide.a < 0.0f)
        return 0u;

    float3 normal = normalize(normalDepth.xyz);
    float roughness = GuideRoughness(materialGuide.a);
    uint desiredCount = AdaptiveIterationCount(
        pixel,
        materialGuide,
        roughness);
    [loop]
    for (uint passIndex = 2u; passIndex < desiredCount; ++passIndex)
    {
        if (CompatibleNeighborCount(
                pixel,
                materialGuide,
                normal,
                normalDepth.w,
                roughness,
                1u << passIndex) < 3.0f)
        {
            return passIndex;
        }
    }
    return desiredCount;
}

float3 IterationCountDebugColor(uint count)
{
    if (count == 1u)
        return float3(0.1f, 0.2f, 1.0f);
    if (count == 2u)
        return float3(0.0f, 1.0f, 1.0f);
    if (count == 3u)
        return float3(0.0f, 1.0f, 0.0f);
    if (count == 4u)
        return float3(1.0f, 1.0f, 0.0f);
    if (count >= 5u)
        return float3(1.0f, 0.0f, 0.0f);
    return 0.0f;
}

float StandardError(int2 pixel, float centerLuminance)
{
    float sampleCount = max(
        abs(g_totalAccumulation.Load(int3(pixel, 0)).a),
        1.0f);
    if (sampleCount < 2.0f)
        return max(abs(centerLuminance), 0.25f);

    float2 momentSums = g_luminanceMoments.Load(int3(pixel, 0));
    float mean = momentSums.x / sampleCount;
    float meanSquare = momentSums.y / sampleCount;
    float sampleVariance =
        max(meanSquare - mean * mean, 0.0f) *
        sampleCount / max(sampleCount - 1.0f, 1.0f);
    float standardError = sqrt(sampleVariance / sampleCount);

    if (g_demodulateDiffuse != 0u)
    {
        float albedoLuminance = max(
            Luminance(g_materialGuide.Load(int3(pixel, 0)).rgb),
            0.05f);
        standardError /= albedoLuminance;
    }
    return standardError;
}

float3 LinearToSrgb(float3 linearColor)
{
    float3 low = linearColor * 12.92f;
    float3 high =
        1.055f * pow(max(linearColor, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(high, low, linearColor <= 0.0031308f);
}

float3 ToneMapForDisplay(float3 linearRadiance)
{
    float3 exposed = max(linearRadiance, 0.0f) * exp2(g_exposure);
    float3 mapped = exposed / (1.0f + exposed);
    return LinearToSrgb(saturate(mapped));
}

float3 ReconstructRadiance(float3 filteredSpecular, int2 pixel)
{
    float3 totalRadiance =
        LoadAverageRadiance(g_totalAccumulation, pixel);
    float3 originalDiffuse =
        LoadAverageRadiance(
            g_diffuseIndirectAccumulation,
            pixel);
    float3 originalSpecular =
        LoadAverageRadiance(
            g_specularIndirectAccumulation,
            pixel);
    // Camera-visible environment/emission is not associated with a primary
    // surface lobe. Preserve that residual without spatial filtering.
    float3 unfilteredResidual = max(
        totalRadiance - originalDiffuse - originalSpecular,
        0.0f);
    float3 filteredDiffuse =
        max(g_filteredDiffuse.Load(int3(pixel, 0)).rgb, 0.0f);
    return unfilteredResidual +
        filteredDiffuse +
        max(filteredSpecular, 0.0f);
}

void StoreResult(float3 filteredValue, int2 pixel)
{
    float3 result = filteredValue;
    if (g_finalPass != 0u)
    {
        if (g_filterChannel == c_filterChannelDiffuse)
        {
            result = max(
                RemodulateDiffuse(filteredValue, pixel),
                0.0f);
        }
        else
        {
            if (g_debugView == c_atrousDebugIterationCount &&
                g_adaptiveIterations != 0u)
            {
                result = IterationCountDebugColor(
                    EffectiveAdaptiveIterationCount(pixel));
            }
            else
            {
                result = ToneMapForDisplay(
                    ReconstructRadiance(filteredValue, pixel));
            }
        }
    }
    g_destination[pixel] = float4(result, 1.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_resolution))
        return;

    int2 centerPixel = int2(pixel);
    float3 centerColor = LoadFilterValue(centerPixel);
    float4 centerNormalDepth =
        g_normalHitDistance.Load(int3(centerPixel, 0));
    float4 centerMaterial =
        g_materialGuide.Load(int3(centerPixel, 0));

    // Miss pixels have no surface guides. Their primary environment value is
    // classified as direct and is therefore reconstructed without filtering.
    if (centerNormalDepth.w < 0.0f || centerMaterial.a < 0.0f)
    {
        StoreResult(centerColor, centerPixel);
        return;
    }

    float3 centerNormal = normalize(centerNormalDepth.xyz);
    float centerDepth = centerNormalDepth.w;
    float centerRoughness = GuideRoughness(centerMaterial.a);
    float centerMetallic = saturate(
        g_metallicGuide.Load(int3(centerPixel, 0)));
    if (g_adaptiveIterations != 0u &&
        g_passIndex >= AdaptiveIterationCount(
            centerPixel,
            centerMaterial,
            centerRoughness))
    {
        // Keep the result produced by the last required pass. The final
        // global pass still performs remodulation and display reconstruction.
        StoreResult(centerColor, centerPixel);
        return;
    }

    float centerLuminance = Luminance(centerColor);
    float standardError = StandardError(centerPixel, centerLuminance);
    float normalExponent;
    float edgeDepthSigma;
    AdaptiveEdgeParameters(
        centerPixel,
        centerMaterial,
        centerRoughness,
        normalExponent,
        edgeDepthSigma);
    float depthScale =
        max(centerDepth, 0.01f) * max(edgeDepthSigma, 1.0e-5f);
    float localLuminanceSum = 0.0f;
    float localLuminanceSquareSum = 0.0f;
    float localWeightSum = 0.0f;
    float validNeighborCount = 0.0f;

    // A pixel that has not sampled a rare light path has zero temporal
    // variance even though it is not converged. Estimate local spatial
    // variance as a fallback so neighboring valid paths can fill those holes.
    [unroll]
    for (int localY = -1; localY <= 1; ++localY)
    {
        [unroll]
        for (int localX = -1; localX <= 1; ++localX)
        {
            int2 localPixel =
                centerPixel +
                int2(localX, localY) * int(g_stepWidth);
            localPixel = clamp(
                localPixel,
                int2(0, 0),
                int2(g_resolution) - int2(1, 1));
            float4 localNormalDepth =
                g_normalHitDistance.Load(int3(localPixel, 0));
            float4 localMaterial =
                g_materialGuide.Load(int3(localPixel, 0));
            if (localNormalDepth.w < 0.0f || localMaterial.a < 0.0f)
                continue;

            float localMetallic = saturate(
                g_metallicGuide.Load(int3(localPixel, 0)));
            float normalWeight = pow(
                saturate(dot(
                    centerNormal,
                    normalize(localNormalDepth.xyz))),
                normalExponent);
            float depthWeight = exp(
                -abs(localNormalDepth.w - centerDepth) / depthScale);
            float materialWeight = MaterialEdgeWeight(
                centerMaterial,
                centerMetallic,
                localMaterial,
                localMetallic);
            float roughnessWeight = RoughnessEdgeWeight(
                centerRoughness,
                GuideRoughness(localMaterial.a));
            float identityWeight = SurfaceIdentityWeight(
                centerMaterial,
                localMaterial);
            float spatialWeight =
                c_kernel3x3[localX + 1] *
                c_kernel3x3[localY + 1];
            float guideWeight =
                spatialWeight *
                normalWeight *
                depthWeight *
                materialWeight *
                roughnessWeight *
                identityWeight;
            if (identityWeight > 0.0f &&
                normalWeight > 0.05f &&
                depthWeight > 0.05f)
            {
                validNeighborCount += 1.0f;
            }
            float localLuminance =
                Luminance(LoadFilterValue(localPixel));
            localLuminanceSum += localLuminance * guideWeight;
            localLuminanceSquareSum +=
                localLuminance * localLuminance * guideWeight;
            localWeightSum += guideWeight;
        }
    }

    if (g_adaptiveIterations != 0u &&
        g_passIndex >= 2u &&
        validNeighborCount < 3.0f)
    {
        // A large A-Trous step has left too few compatible samples on this
        // surface. Preserve the previous pass instead of crossing a small
        // silhouette or spending work on an ineffective wide kernel.
        StoreResult(centerColor, centerPixel);
        return;
    }

    float localMean =
        localLuminanceSum / max(localWeightSum, 1.0e-6f);
    float localVariance = max(
        localLuminanceSquareSum / max(localWeightSum, 1.0e-6f) -
        localMean * localMean,
        0.0f);
    float effectiveError = max(
        standardError,
        sqrt(localVariance));
    float relativeError =
        effectiveError / max(abs(centerLuminance), 0.05f);

    // Avoid a binary filtered/unfiltered switch near convergence. Pixels
    // transition continuously from spatial filtering to their accumulated
    // center value as the relative error falls.
    float filterStrength = smoothstep(0.01f, 0.03f, relativeError);

    float colorScale = max(
        g_colorSigma * max(
            effectiveError,
            0.01f * sqrt(abs(centerLuminance) + 0.01f)),
        1.0e-4f);

    float3 filteredColor = 0.0f;
    float totalWeight = 0.0f;

    int kernelRadius = KernelRadius();
    [loop]
    for (int y = -kernelRadius; y <= kernelRadius; ++y)
    {
        [loop]
        for (int x = -kernelRadius; x <= kernelRadius; ++x)
        {
            int2 samplePixel =
                centerPixel + int2(x, y) * int(g_stepWidth);
            samplePixel = clamp(
                samplePixel,
                int2(0, 0),
                int2(g_resolution) - int2(1, 1));

            float4 sampleNormalDepth =
                g_normalHitDistance.Load(int3(samplePixel, 0));
            float4 sampleMaterial =
                g_materialGuide.Load(int3(samplePixel, 0));
            if (sampleNormalDepth.w < 0.0f || sampleMaterial.a < 0.0f)
                continue;

            float3 sampleColor = LoadFilterValue(samplePixel);
            float3 sampleNormal = normalize(sampleNormalDepth.xyz);
            float sampleMetallic = saturate(
                g_metallicGuide.Load(int3(samplePixel, 0)));
            float normalWeight = pow(
                saturate(dot(centerNormal, sampleNormal)),
                normalExponent);
            float depthWeight = exp(
                -abs(sampleNormalDepth.w - centerDepth) / depthScale);
            float materialWeight = MaterialEdgeWeight(
                centerMaterial,
                centerMetallic,
                sampleMaterial,
                sampleMetallic);
            float roughnessWeight = RoughnessEdgeWeight(
                centerRoughness,
                GuideRoughness(sampleMaterial.a));
            float identityWeight = SurfaceIdentityWeight(
                centerMaterial,
                sampleMaterial);
            float colorWeight = exp(
                -abs(Luminance(sampleColor) - centerLuminance) /
                colorScale);
            float spatialWeight =
                KernelWeight1D(x) * KernelWeight1D(y);
            float weight = spatialWeight * normalWeight * depthWeight *
                materialWeight * roughnessWeight * identityWeight *
                colorWeight;

            filteredColor += sampleColor * weight;
            totalWeight += weight;
        }
    }

    filteredColor = totalWeight > 1.0e-6f
        ? filteredColor / totalWeight
        : centerColor;
    filteredColor = lerp(
        centerColor,
        filteredColor,
        filterStrength);
    StoreResult(filteredColor, centerPixel);
}
