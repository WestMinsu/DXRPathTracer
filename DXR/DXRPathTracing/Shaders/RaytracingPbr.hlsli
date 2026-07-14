#ifndef RAYTRACING_PBR_HLSLI
#define RAYTRACING_PBR_HLSLI

#include "RaytracingCommon.hlsli"
#include "RaytracingScene.hlsli"

float DistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float nDotH = saturate(dot(normal, halfVector));
    float nDotHSquared = nDotH * nDotH;
    float denominator = nDotHSquared * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / max(c_pi * denominator * denominator, 0.000001f);
}

float GeometrySmithHeightCorrelatedGGX(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float roughness)
{
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotV <= 0.0f || nDotL <= 0.0f)
    {
        return 0.0f;
    }

    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float smithV = nDotL * sqrt(max(
        nDotV * nDotV * (1.0f - alphaSquared) + alphaSquared,
        0.0f));
    float smithL = nDotV * sqrt(max(
        nDotL * nDotL * (1.0f - alphaSquared) + alphaSquared,
        0.0f));
    return (2.0f * nDotV * nDotL) / max(smithV + smithL, 0.000001f);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

float3 EvaluateBrdf(PbrMaterial material, float3 normal, float3 viewDirection, float3 lightDirection)
{
    float3 halfVector = normalize(viewDirection + lightDirection);
    float nDotV = max(saturate(dot(normal, viewDirection)), 0.0001f);
    float nDotL = saturate(dot(normal, lightDirection));
    float vDotH = saturate(dot(viewDirection, halfVector));

    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), material.baseColor, material.metallic);
    float d = DistributionGGX(normal, halfVector, material.roughness);
    float g = GeometrySmithHeightCorrelatedGGX(
        normal,
        viewDirection,
        lightDirection,
        material.roughness);
    float3 f = FresnelSchlick(vDotH, f0);

    float3 specular = (d * g * f) / max(4.0f * nDotV * nDotL, 0.0001f);
    float3 diffuse = (1.0f - f) * (1.0f - material.metallic) * material.baseColor * c_invPi;
    return (diffuse + specular) * nDotL;
}

float3 ShadePbrGgx(uint primitiveIndex, float3 normal, float3 hitPosition)
{
    PbrMaterial material = GetPbrMaterial(primitiveIndex);
    if (any(material.emission > 0.0f))
    {
        return material.emission;
    }

    float3 toLight = c_pbrLightCenter - hitPosition;
    float lightDistance = length(toLight);
    float3 lightDirection = toLight / max(lightDistance, 0.0001f);

    RayDesc shadowRay;
    shadowRay.Origin = hitPosition + normal * c_rayOriginBias;
    shadowRay.Direction = lightDirection;
    shadowRay.TMin = c_rayTMin;
    shadowRay.TMax = max(c_rayTMin, lightDistance - c_rayOriginBias);

    RadiancePayload shadowPayload;
    shadowPayload.color = float3(0.0f, 0.0f, 0.0f);
    shadowPayload.depth = g_maxBounce;
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, shadowRay, shadowPayload);

    float3 viewDirection = normalize(-WorldRayDirection());
    return EvaluateBrdf(material, normal, viewDirection, lightDirection) * shadowPayload.color;
}

float3 ImportanceSampleGGX(float2 sampleValue, float3 normal, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float phi = sampleValue.x * c_twoPi;
    float cosTheta = sqrt((1.0f - sampleValue.y) / max(1.0f + (alphaSquared - 1.0f) * sampleValue.y, 0.000001f));
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));

    float sinPhi;
    float cosPhi;
    sincos(phi, sinPhi, cosPhi);

    float3 tangent = abs(normal.z) < 0.999f
        ? normalize(cross(float3(0.0f, 0.0f, 1.0f), normal))
        : normalize(cross(float3(0.0f, 1.0f, 0.0f), normal));
    float3 bitangent = cross(normal, tangent);
    float3 localHalfVector = float3(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
    return normalize(tangent * localHalfVector.x + bitangent * localHalfVector.y + normal * localHalfVector.z);
}

float3 TracePbrBrdfWithGgxImportanceSampling(PbrMaterial material, float3 normal, float3 hitPosition, uint depth, uint primitiveIndex)
{
    uint seed = CreateRandomSeed(depth, primitiveIndex);
    float3 viewDirection = normalize(-WorldRayDirection());
    float2 sampleValue = float2(RandomFloat01(seed), RandomFloat01(seed));
    float3 halfVector = ImportanceSampleGGX(sampleValue, normal, material.roughness);
    float vDotH = saturate(dot(viewDirection, halfVector));
    if (vDotH <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float3 sampleDirection = normalize(2.0f * vDotH * halfVector - viewDirection);
    float nDotL = saturate(dot(normal, sampleDirection));
    if (nDotL <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    float nDotH = saturate(dot(normal, halfVector));
    float pdf = DistributionGGX(normal, halfVector, material.roughness) * nDotH / max(4.0f * vDotH, 0.000001f);
    pdf = max(pdf, 0.000001f);

    RayDesc bounceRay;
    bounceRay.Origin = hitPosition + normal * c_rayOriginBias;
    bounceRay.Direction = sampleDirection;
    bounceRay.TMin = c_rayTMin;
    bounceRay.TMax = c_rayTMax;

    RadiancePayload bouncePayload;
    bouncePayload.color = float3(0.0f, 0.0f, 0.0f);
    bouncePayload.depth = depth + 1;

    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, bouncePayload);

    float3 weightedBrdf = EvaluateBrdf(material, normal, viewDirection, sampleDirection) / pdf;
    return weightedBrdf * bouncePayload.color;
}
#endif
