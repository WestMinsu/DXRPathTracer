#ifndef RAYTRACING_PBR_HLSLI
#define RAYTRACING_PBR_HLSLI

#include "RaytracingCommon.hlsli"
#include "RaytracingScene.hlsli"

// Keep alpha above float cancellation range so the evaluated GGX NDF and
// the sampled distribution remain identical. sqrt(0.001) ~= 0.0316.
float GgxAlpha(float roughness)
{
    float clampedRoughness = saturate(roughness);
    return max(clampedRoughness * clampedRoughness, 0.001f);
}

float DistributionGGX(float nDotH, float alphaSquared)
{
    float nDotHSquared = nDotH * nDotH;
    float denominator = nDotHSquared * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / (c_pi * denominator * denominator);
}

float GeometrySmithHeightCorrelatedGGX(
    float nDotV,
    float nDotL,
    float alphaSquared)
{
    if (nDotV <= 0.0f || nDotL <= 0.0f)
    {
        return 0.0f;
    }

    float smithV = nDotL * sqrt(max(
        nDotV * nDotV * (1.0f - alphaSquared) + alphaSquared,
        0.0f));
    float smithL = nDotV * sqrt(max(
        nDotL * nDotL * (1.0f - alphaSquared) + alphaSquared,
        0.0f));
    return (2.0f * nDotV * nDotL) / max(smithV + smithL, 0.000001f);
}

float GeometrySmithG1GGX(
    float nDotDirection,
    float alphaSquared)
{
    if (nDotDirection <= 0.0f)
    {
        return 0.0f;
    }

    float root = sqrt(max(
        nDotDirection * nDotDirection * (1.0f - alphaSquared) +
            alphaSquared,
        0.0f));
    return (2.0f * nDotDirection) /
        max(nDotDirection + root, 0.000001f);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0f - f0) * pow(1.0f - saturate(cosTheta), 5.0f);
}

void EvaluateBrdfComponentsAndPdf(
    PbrMaterial material,
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    out float3 diffuseContribution,
    out float3 specularContribution,
    out float samplingPdf)
{
    diffuseContribution = float3(0.0f, 0.0f, 0.0f);
    specularContribution = float3(0.0f, 0.0f, 0.0f);
    samplingPdf = 0.0f;
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotV <= 0.0f || nDotL <= 0.0f)
    {
        return;
    }

    float3 halfVectorSum = viewDirection + lightDirection;
    float halfVectorLengthSquared = dot(halfVectorSum, halfVectorSum);
    bool hasValidHalfVector = halfVectorLengthSquared > 0.00000001f;
    float3 halfVector = hasValidHalfVector
        ? halfVectorSum * rsqrt(halfVectorLengthSquared)
        : normal;
    float vDotH = saturate(dot(viewDirection, halfVector));
    float nDotH = saturate(dot(normal, halfVector));
    float alpha = GgxAlpha(material.roughness);
    float alphaSquared = alpha * alpha;

    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), material.baseColor, material.metallic);
    float d = DistributionGGX(nDotH, alphaSquared);
    float g = GeometrySmithHeightCorrelatedGGX(
        nDotV,
        nDotL,
        alphaSquared);
    float3 f = FresnelSchlick(vDotH, f0);

    float3 specular = (d * g * f) / max(4.0f * nDotV * nDotL, 0.00000001f);

    // Diffuse light crosses the dielectric boundary on both entry and exit.
    // The symmetric factors preserve reciprocity and prevent the diffuse and
    // specular lobes from independently claiming the same grazing energy.
    float3 fresnelView = FresnelSchlick(nDotV, f0);
    float3 fresnelLight = FresnelSchlick(nDotL, f0);
    float3 diffuseTransmission = (1.0f - fresnelView) * (1.0f - fresnelLight);
    float3 diffuse = diffuseTransmission * (1.0f - material.metallic) * material.baseColor * c_invPi;
    diffuseContribution = diffuse * nDotL;
    specularContribution = specular * nDotL;

    // Preserve the original PDF validity rules. BRDF evaluation still uses
    // the normal as a fallback for a degenerate half vector, while that
    // direction has no valid sampling PDF.
    if (!hasValidHalfVector || vDotH <= 0.0f || nDotH <= 0.0f)
        return;

    float visibleNormalPdf =
        d *
        GeometrySmithG1GGX(
            nDotV,
            alphaSquared) *
        vDotH /
        max(nDotV, 0.00000001f);
    float specularPdf =
        visibleNormalPdf /
        max(4.0f * vDotH, 0.00000001f);
    float diffusePdf = nDotL * c_invPi;
    float specularProbability =
        lerp(0.5f, 1.0f, saturate(material.metallic));
    samplingPdf =
        specularProbability * specularPdf +
        (1.0f - specularProbability) * diffusePdf;
}

float3 ImportanceSampleGGXVisibleNormal(
    float2 sampleValue,
    float3 normal,
    float3 viewDirection,
    float roughness)
{
    float alpha = GgxAlpha(roughness);
    float3 tangent = abs(normal.z) < 0.999f
        ? normalize(cross(float3(0.0f, 0.0f, 1.0f), normal))
        : normalize(cross(float3(0.0f, 1.0f, 0.0f), normal));
    float3 bitangent = cross(normal, tangent);

    float3 localView = float3(
        dot(viewDirection, tangent),
        dot(viewDirection, bitangent),
        saturate(dot(viewDirection, normal)));

    // Heitz 2018: sample the GGX distribution of normals visible from V.
    // Stretching makes the ellipsoid isotropic in this intermediate space.
    float3 stretchedView = normalize(float3(
        alpha * localView.x,
        alpha * localView.y,
        localView.z));

    float lensq =
        stretchedView.x * stretchedView.x +
        stretchedView.y * stretchedView.y;
    float3 basisX = lensq > 0.0f
        ? float3(-stretchedView.y, stretchedView.x, 0.0f) *
            rsqrt(lensq)
        : float3(1.0f, 0.0f, 0.0f);
    float3 basisY = cross(stretchedView, basisX);

    float radius = sqrt(sampleValue.x);
    float phi = c_twoPi * sampleValue.y;
    float sinPhi;
    float cosPhi;
    sincos(phi, sinPhi, cosPhi);
    float projectedX = radius * cosPhi;
    float projectedY = radius * sinPhi;

    float interpolation = 0.5f * (1.0f + stretchedView.z);
    projectedY =
        (1.0f - interpolation) *
            sqrt(max(0.0f, 1.0f - projectedX * projectedX)) +
        interpolation * projectedY;

    float projectedZ = sqrt(max(
        0.0f,
        1.0f - projectedX * projectedX - projectedY * projectedY));
    float3 stretchedNormal =
        projectedX * basisX +
        projectedY * basisY +
        projectedZ * stretchedView;

    float3 localHalfVector = normalize(float3(
        alpha * stretchedNormal.x,
        alpha * stretchedNormal.y,
        max(0.0f, stretchedNormal.z)));
    return normalize(tangent * localHalfVector.x + bitangent * localHalfVector.y + normal * localHalfVector.z);
}

float PbrSpecularSamplingProbability(PbrMaterial material)
{
    // Metals have no diffuse lobe. Dielectrics use a balanced mixture so a
    // narrow GGX proposal does not leave the broad diffuse lobe undersampled.
    return lerp(0.5f, 1.0f, saturate(material.metallic));
}

bool SamplePbrBrdfWithMixtureSampling(
    PbrMaterial material,
    float3 normal,
    float3 viewDirection,
    inout uint seed,
    out float3 sampleDirection,
    out float3 weightedBrdf,
    out float samplePdf,
    out float3 diffuseContribution,
    out float3 specularContribution)
{
    sampleDirection = normal;
    weightedBrdf = float3(0.0f, 0.0f, 0.0f);
    samplePdf = 0.0f;
    diffuseContribution = float3(0.0f, 0.0f, 0.0f);
    specularContribution = float3(0.0f, 0.0f, 0.0f);
    float specularProbability = PbrSpecularSamplingProbability(material);
    bool sampleSpecular = RandomFloat01(seed) < specularProbability;

    if (sampleSpecular)
    {
        float2 sampleValue = float2(RandomFloat01(seed), RandomFloat01(seed));
        float3 sampledHalfVector = ImportanceSampleGGXVisibleNormal(
            sampleValue,
            normal,
            viewDirection,
            material.roughness);
        float sampledVDotH = saturate(dot(viewDirection, sampledHalfVector));
        if (sampledVDotH <= 0.0f)
        {
            return false;
        }
        sampleDirection = normalize(
            2.0f * sampledVDotH * sampledHalfVector - viewDirection);
    }
    else
    {
        sampleDirection = RandomCosineHemisphereDirection(normal, seed);
    }

    float nDotL = saturate(dot(normal, sampleDirection));
    if (nDotL <= 0.0f)
    {
        return false;
    }

    EvaluateBrdfComponentsAndPdf(
        material,
        normal,
        viewDirection,
        sampleDirection,
        diffuseContribution,
        specularContribution,
        samplePdf);
    if (samplePdf <= 0.0f)
    {
        return false;
    }

    weightedBrdf =
        (diffuseContribution + specularContribution) / samplePdf;
    return true;
}

#endif
