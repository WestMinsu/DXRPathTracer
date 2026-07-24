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

float DistributionGGX(float3 normal, float3 halfVector, float roughness)
{
    float alpha = GgxAlpha(roughness);
    float alphaSquared = alpha * alpha;
    float nDotH = saturate(dot(normal, halfVector));
    float nDotHSquared = nDotH * nDotH;
    float denominator = nDotHSquared * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / (c_pi * denominator * denominator);
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

    float alpha = GgxAlpha(roughness);
    float alphaSquared = alpha * alpha;
    float smithV = nDotL * sqrt(max(
        nDotV * nDotV * (1.0f - alphaSquared) + alphaSquared,
        0.0f));
    float smithL = nDotV * sqrt(max(
        nDotL * nDotL * (1.0f - alphaSquared) + alphaSquared,
        0.0f));
    return (2.0f * nDotV * nDotL) / max(smithV + smithL, 0.000001f);
}

float GeometrySmithG1GGX(
    float3 normal,
    float3 direction,
    float roughness)
{
    float nDotDirection = saturate(dot(normal, direction));
    if (nDotDirection <= 0.0f)
    {
        return 0.0f;
    }

    float alpha = GgxAlpha(roughness);
    float alphaSquared = alpha * alpha;
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

void EvaluateBrdfComponents(
    PbrMaterial material,
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    out float3 diffuseContribution,
    out float3 specularContribution)
{
    diffuseContribution = float3(0.0f, 0.0f, 0.0f);
    specularContribution = float3(0.0f, 0.0f, 0.0f);
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotV <= 0.0f || nDotL <= 0.0f)
    {
        return;
    }

    float3 halfVectorSum = viewDirection + lightDirection;
    float halfVectorLengthSquared = dot(halfVectorSum, halfVectorSum);
    float3 halfVector = halfVectorLengthSquared > 0.00000001f
        ? halfVectorSum * rsqrt(halfVectorLengthSquared)
        : normal;
    float vDotH = saturate(dot(viewDirection, halfVector));

    float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), material.baseColor, material.metallic);
    float d = DistributionGGX(normal, halfVector, material.roughness);
    float g = GeometrySmithHeightCorrelatedGGX(
        normal,
        viewDirection,
        lightDirection,
        material.roughness);
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
}

float3 EvaluateBrdf(PbrMaterial material, float3 normal, float3 viewDirection, float3 lightDirection)
{
    float3 diffuseContribution;
    float3 specularContribution;
    EvaluateBrdfComponents(
        material,
        normal,
        viewDirection,
        lightDirection,
        diffuseContribution,
        specularContribution);
    return diffuseContribution + specularContribution;
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

float PbrBrdfSamplingPdf(
    PbrMaterial material,
    float3 normal,
    float3 viewDirection,
    float3 lightDirection)
{
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotL = saturate(dot(normal, lightDirection));
    if (nDotV <= 0.0f || nDotL <= 0.0f)
    {
        return 0.0f;
    }

    float3 halfVectorSum = viewDirection + lightDirection;
    float halfVectorLengthSquared = dot(halfVectorSum, halfVectorSum);
    if (halfVectorLengthSquared <= 0.00000001f)
    {
        return 0.0f;
    }

    float3 halfVector =
        halfVectorSum * rsqrt(halfVectorLengthSquared);
    float vDotH = saturate(dot(viewDirection, halfVector));
    float nDotH = saturate(dot(normal, halfVector));
    if (vDotH <= 0.0f || nDotH <= 0.0f)
    {
        return 0.0f;
    }

    float visibleNormalPdf =
        DistributionGGX(
            normal,
            halfVector,
            material.roughness) *
        GeometrySmithG1GGX(
            normal,
            viewDirection,
            material.roughness) *
        vDotH /
        max(nDotV, 0.00000001f);
    float specularPdf =
        visibleNormalPdf /
        max(4.0f * vDotH, 0.00000001f);
    float diffusePdf = nDotL * c_invPi;
    float specularProbability =
        PbrSpecularSamplingProbability(material);
    return specularProbability * specularPdf +
        (1.0f - specularProbability) * diffusePdf;
}

bool SamplePbrBrdfWithMixtureSampling(
    PbrMaterial material,
    float3 normal,
    float3 viewDirection,
    inout uint seed,
    out float3 sampleDirection,
    out float3 weightedBrdf,
    out float samplePdf)
{
    sampleDirection = normal;
    weightedBrdf = float3(0.0f, 0.0f, 0.0f);
    samplePdf = 0.0f;
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

    samplePdf = PbrBrdfSamplingPdf(
        material,
        normal,
        viewDirection,
        sampleDirection);
    if (samplePdf <= 0.0f)
    {
        return false;
    }

    weightedBrdf = EvaluateBrdf(
        material,
        normal,
        viewDirection,
        sampleDirection) / samplePdf;
    return true;
}

float3 TracePbrBrdfWithMixtureSampling(
    PbrMaterial material,
    float3 normal,
    float3 hitPosition,
    uint depth,
    uint primitiveIndex,
    inout uint dynamicTouched,
    float3 pathThroughput,
    out float3 localDiffuseIndirectLighting,
    out float3 localSpecularIndirectLighting,
    out float3 localDirectLighting)
{
    localDiffuseIndirectLighting = float3(0.0f, 0.0f, 0.0f);
    localSpecularIndirectLighting = float3(0.0f, 0.0f, 0.0f);
    float3 viewDirection = normalize(-WorldRayDirection());
    float3 directLighting = float3(0.0f, 0.0f, 0.0f);
    uint directSeed =
        CreateRandomSeed(depth, primitiveIndex) ^ 0xA511E9B3u;
    float3 directLightDirection;
    float3 radianceOverPdf;
    float lightPdf;
    if (SampleDirectLight(
        normal,
        hitPosition,
        directSeed,
        directLightDirection,
        radianceOverPdf,
        lightPdf))
    {
        float bsdfPdf = PbrBrdfSamplingPdf(
            material,
            normal,
            viewDirection,
            directLightDirection);
        float misWeight = g_lightingMode == c_lightingModeMis
            ? PowerHeuristic(lightPdf, bsdfPdf)
            : 1.0f;
        directLighting =
            EvaluateBrdf(
                material,
                normal,
                viewDirection,
                directLightDirection) *
            radianceOverPdf *
            misWeight;
    }
    localDirectLighting = directLighting;

    if (depth >= g_maxBounce)
    {
        return directLighting;
    }

    uint seed = CreateRandomSeed(depth, primitiveIndex);
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
        return directLighting;
    }
    uint nextDepth = depth + 1u;
    float3 nextThroughput = pathThroughput * weightedBrdf;
    float survivalProbability = 1.0f;
    if (!SurvivesRussianRoulette(
        nextThroughput,
        nextDepth,
        seed,
        survivalProbability))
    {
        return directLighting;
    }
    float inverseSurvivalProbability = 1.0f / survivalProbability;

    RayDesc bounceRay;
    bounceRay.Origin = hitPosition + normal * c_rayOriginBias;
    bounceRay.Direction = sampleDirection;
    bounceRay.TMin = c_rayTMin;
    bounceRay.TMax = c_rayTMax;

    RadiancePayload bouncePayload;
    bouncePayload.color = float3(0.0f, 0.0f, 0.0f);
    bouncePayload.primaryDirectColor = float3(0.0f, 0.0f, 0.0f);
    // When the indirect AOVs are disabled, preserve the already evaluated
    // BRDF*cos/pdf weight in an otherwise unused payload slot. This avoids
    // evaluating GGX, Fresnel, and Smith a second time after TraceRay without
    // keeping another continuation value live across the recursive call.
    bouncePayload.primaryDiffuseIndirectColor =
        g_enableAtrous == 0u
        ? weightedBrdf
        : float3(0.0f, 0.0f, 0.0f);
    bouncePayload.primarySpecularIndirectColor =
        float3(0.0f, 0.0f, 0.0f);
    bouncePayload.depth = nextDepth;
    bouncePayload.dynamicTouched = 0u;
    bouncePayload.pathThroughput =
        nextThroughput * inverseSurvivalProbability;
    bouncePayload.previousBsdfPdf = samplePdf;
    bouncePayload.previousWasDelta = 0u;

    RecordRadianceRay(bouncePayload.depth);
    TraceRay(g_scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, bounceRay, bouncePayload);
    dynamicTouched |= bouncePayload.dynamicTouched;

    if (g_enableAtrous == 0u)
    {
        return directLighting +
            bouncePayload.primaryDiffuseIndirectColor *
            bouncePayload.color * inverseSurvivalProbability;
    }

    float3 diffuseContribution;
    float3 specularContribution;
    EvaluateBrdfComponents(
        material,
        normal,
        viewDirection,
        sampleDirection,
        diffuseContribution,
        specularContribution);
    float3 incomingRadianceOverPdf =
        bouncePayload.color *
        (inverseSurvivalProbability / samplePdf);
    localDiffuseIndirectLighting =
        diffuseContribution * incomingRadianceOverPdf;
    localSpecularIndirectLighting =
        specularContribution * incomingRadianceOverPdf;
    return directLighting +
        localDiffuseIndirectLighting +
        localSpecularIndirectLighting;
}
#endif
