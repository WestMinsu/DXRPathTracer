Texture2D<float4> g_source : register(t0);
Texture2D<float4> g_normalDepth : register(t1);
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
};

static const float c_kernel[5] =
{
    1.0f / 16.0f,
    4.0f / 16.0f,
    6.0f / 16.0f,
    4.0f / 16.0f,
    1.0f / 16.0f
};

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 LoadLinearRadiance(int2 pixel)
{
    float4 value = g_source.Load(int3(pixel, 0));
    if (g_inputIsAccumulation != 0u)
        return value.rgb / max(abs(value.a), 1.0f);
    return value.rgb;
}

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

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThreadId.xy;
    if (any(pixel >= g_resolution))
        return;

    int2 centerPixel = int2(pixel);
    float3 centerColor = LoadLinearRadiance(centerPixel);
    float4 centerGuide = g_normalDepth.Load(int3(centerPixel, 0));

    // Environment misses have no surface edge information and need no
    // path-space denoising. Keeping them untouched also protects silhouettes.
    if (centerGuide.w < 0.0f)
    {
        float3 result = g_finalPass != 0u
            ? ToneMapForDisplay(centerColor)
            : centerColor;
        g_destination[pixel] = float4(result, 1.0f);
        return;
    }

    float3 centerNormal = normalize(centerGuide.xyz);
    float centerDepth = centerGuide.w;
    float centerLuminance = Luminance(centerColor);
    float depthScale = max(centerDepth, 0.01f) * max(g_depthSigma, 1.0e-5f);
    float colorScale = max(
        g_colorSigma * sqrt(max(centerLuminance, 0.01f)),
        1.0e-4f);

    float3 filteredColor = 0.0f;
    float totalWeight = 0.0f;

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            int2 samplePixel = centerPixel + int2(x, y) * int(g_stepWidth);
            samplePixel = clamp(
                samplePixel,
                int2(0, 0),
                int2(g_resolution) - int2(1, 1));

            float4 sampleGuide = g_normalDepth.Load(int3(samplePixel, 0));
            if (sampleGuide.w < 0.0f)
                continue;

            float3 sampleColor = LoadLinearRadiance(samplePixel);
            float3 sampleNormal = normalize(sampleGuide.xyz);
            float normalWeight = pow(
                saturate(dot(centerNormal, sampleNormal)),
                g_normalExponent);
            float depthWeight = exp(
                -abs(sampleGuide.w - centerDepth) / depthScale);
            float colorWeight = exp(
                -abs(Luminance(sampleColor) - centerLuminance) / colorScale);
            float spatialWeight = c_kernel[x + 2] * c_kernel[y + 2];
            float weight = spatialWeight * normalWeight * depthWeight * colorWeight;

            filteredColor += sampleColor * weight;
            totalWeight += weight;
        }
    }

    filteredColor = totalWeight > 1.0e-6f
        ? filteredColor / totalWeight
        : centerColor;
    float3 result = g_finalPass != 0u
        ? ToneMapForDisplay(filteredColor)
        : filteredColor;
    g_destination[pixel] = float4(result, 1.0f);
}
