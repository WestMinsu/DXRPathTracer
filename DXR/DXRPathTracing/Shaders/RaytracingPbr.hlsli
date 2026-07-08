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

float GeometrySchlickGGX(float nDotV, float roughness)
{
    float remappedRoughness = roughness + 1.0f;
    float k = (remappedRoughness * remappedRoughness) / 8.0f;
    return nDotV / max(nDotV * (1.0f - k) + k, 0.000001f);
}

float GeometrySmith(float3 normal, float3 viewDirection, float3 lightDirection, float roughness)
{
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotL = saturate(dot(normal, lightDirection));
    return GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
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
    float g = GeometrySmith(normal, viewDirection, lightDirection, material.roughness);
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

#endif
