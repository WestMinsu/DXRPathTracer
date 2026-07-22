Texture2D<float4> g_source : register(t0);
Texture2D<float4> g_normalDepth : register(t1);
Texture2D<float4> g_materialGuide : register(t2);
Texture2D<float2> g_luminanceMoments : register(t3);
Texture2D<float4> g_diffuseIndirectAccumulation : register(t4);
Texture2D<float4> g_specularIndirectAccumulation : register(t5);
Texture2D<float4> g_totalAccumulation : register(t6);
Texture2D<float4> g_filteredDiffuse : register(t7);
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
};

static const uint c_filterChannelDiffuse = 0u;
static const uint c_filterChannelSpecular = 1u;

// A compact B3-style kernel. Two 3x3 passes require 18 taps per pixel,
// compared with the previous three 5x5 passes (75 taps per pixel).
static const float c_kernel[3] =
{
    1.0f / 4.0f,
    2.0f / 4.0f,
    1.0f / 4.0f
};

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
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
    float3 unfilteredDirect =
        max(
            totalRadiance - originalDiffuse - originalSpecular,
            0.0f);
    float3 filteredDiffuse =
        max(g_filteredDiffuse.Load(int3(pixel, 0)).rgb, 0.0f);
    return unfilteredDirect +
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
            result = ToneMapForDisplay(
                ReconstructRadiance(filteredValue, pixel));
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
        g_normalDepth.Load(int3(centerPixel, 0));
    float4 centerMaterial =
        g_materialGuide.Load(int3(centerPixel, 0));

    // Miss pixels have no surface guides. Their primary environment value is
    // classified as direct and is therefore reconstructed without filtering.
    if (centerNormalDepth.w < 0.0f || centerMaterial.a < 0.0f)
    {
        StoreResult(centerColor, centerPixel);
        return;
    }

    float centerLuminance = Luminance(centerColor);
    float standardError = StandardError(centerPixel, centerLuminance);
    float3 centerNormal = normalize(centerNormalDepth.xyz);
    float centerDepth = centerNormalDepth.w;
    float depthScale =
        max(centerDepth, 0.01f) * max(g_depthSigma, 1.0e-5f);
    float localLuminanceSum = 0.0f;
    float localLuminanceSquareSum = 0.0f;
    float localWeightSum = 0.0f;

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
                g_normalDepth.Load(int3(localPixel, 0));
            float4 localMaterial =
                g_materialGuide.Load(int3(localPixel, 0));
            if (localNormalDepth.w < 0.0f || localMaterial.a < 0.0f)
                continue;

            float normalWeight = pow(
                saturate(dot(
                    centerNormal,
                    normalize(localNormalDepth.xyz))),
                g_normalExponent);
            float depthWeight = exp(
                -abs(localNormalDepth.w - centerDepth) / depthScale);
            float albedoWeight = exp(
                -length(localMaterial.rgb - centerMaterial.rgb) / 0.12f);
            float roughnessWeight = exp(
                -abs(localMaterial.a - centerMaterial.a) / 0.10f);
            float spatialWeight =
                c_kernel[localX + 1] * c_kernel[localY + 1];
            float guideWeight =
                spatialWeight *
                normalWeight *
                depthWeight *
                albedoWeight *
                roughnessWeight;
            float localLuminance =
                Luminance(LoadFilterValue(localPixel));
            localLuminanceSum += localLuminance * guideWeight;
            localLuminanceSquareSum +=
                localLuminance * localLuminance * guideWeight;
            localWeightSum += guideWeight;
        }
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

    // Progressive accumulation eventually becomes more reliable than a
    // spatial estimate. Skip filtering once both temporal and local spatial
    // variation are below 1.5%.
    if (relativeError < 0.015f)
    {
        StoreResult(centerColor, centerPixel);
        return;
    }

    float colorScale = max(
        g_colorSigma * max(
            effectiveError,
            0.01f * sqrt(abs(centerLuminance) + 0.01f)),
        1.0e-4f);

    float3 filteredColor = 0.0f;
    float totalWeight = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 samplePixel =
                centerPixel + int2(x, y) * int(g_stepWidth);
            samplePixel = clamp(
                samplePixel,
                int2(0, 0),
                int2(g_resolution) - int2(1, 1));

            float4 sampleNormalDepth =
                g_normalDepth.Load(int3(samplePixel, 0));
            float4 sampleMaterial =
                g_materialGuide.Load(int3(samplePixel, 0));
            if (sampleNormalDepth.w < 0.0f || sampleMaterial.a < 0.0f)
                continue;

            float3 sampleColor = LoadFilterValue(samplePixel);
            float3 sampleNormal = normalize(sampleNormalDepth.xyz);
            float normalWeight = pow(
                saturate(dot(centerNormal, sampleNormal)),
                g_normalExponent);
            float depthWeight = exp(
                -abs(sampleNormalDepth.w - centerDepth) / depthScale);
            float albedoWeight = exp(
                -length(sampleMaterial.rgb - centerMaterial.rgb) / 0.12f);
            float roughnessWeight = exp(
                -abs(sampleMaterial.a - centerMaterial.a) / 0.10f);
            float colorWeight = exp(
                -abs(Luminance(sampleColor) - centerLuminance) /
                colorScale);
            float spatialWeight =
                c_kernel[x + 1] * c_kernel[y + 1];
            float weight =
                spatialWeight *
                normalWeight *
                depthWeight *
                albedoWeight *
                roughnessWeight *
                colorWeight;

            filteredColor += sampleColor * weight;
            totalWeight += weight;
        }
    }

    filteredColor = totalWeight > 1.0e-6f
        ? filteredColor / totalWeight
        : centerColor;
    StoreResult(filteredColor, centerPixel);
}
