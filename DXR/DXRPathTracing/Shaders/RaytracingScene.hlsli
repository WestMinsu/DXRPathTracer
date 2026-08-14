#ifndef RAYTRACING_SCENE_HLSLI
#define RAYTRACING_SCENE_HLSLI

#include "RaytracingCommon.hlsli"

SceneMaterial GetSceneMaterial(uint primitiveIndex)
{
    return g_sceneMaterials[g_primitiveMaterialIndices[primitiveIndex]];
}

static const uint c_sceneTextureIndexMask = 0x000000FFu;
static const uint c_sceneTextureMaxDimensionMask = 0x0000FFFFu;
static const uint c_sceneTextureMaxDimensionShift = 8u;
static const uint c_sceneTextureLastMipShift = 24u;

uint SceneTextureDescriptorIndex(uint metadata)
{
    return metadata & c_sceneTextureIndexMask;
}

float TextureMipLevel(uint textureMetadata, float uvFootprint)
{
    if (g_textureLodBiasOrDisabled < -16.0f)
        return 0.0f;

    uint maxDimension =
        (textureMetadata >> c_sceneTextureMaxDimensionShift) &
        c_sceneTextureMaxDimensionMask;
    uint lastMip = textureMetadata >> c_sceneTextureLastMipShift;
    float texelFootprint = uvFootprint *
        float(maxDimension);
    float mipLevel = log2(max(texelFootprint, 1.0f)) +
        g_textureLodBiasOrDisabled;
    return clamp(mipLevel, 0.0f, float(lastMip));
}

float EdgeUvPerWorld(
    float3 worldPosition0,
    float3 worldPosition1,
    float2 texCoord0,
    float2 texCoord1)
{
    float worldLength = length(worldPosition1 - worldPosition0);
    return length(texCoord1 - texCoord0) /
        max(worldLength, 1.0e-6f);
}

float EstimateTriangleUvFootprint(
    uint i0,
    uint i1,
    uint i2,
    float3x4 objectToWorldTransform,
    float rayConeWidthAtOrigin,
    float rayConeSpread,
    float3 worldNormal,
    bool useGeometricNormal)
{
    if (g_textureLodBiasOrDisabled < -16.0f)
        return 0.0f;

    float3 objectPosition0 = g_vertices[i0].position;
    float3 worldPosition0 = float3(0.0f, 0.0f, 0.0f);
    float3 worldPosition1 = mul(
        objectToWorldTransform,
        float4(g_vertices[i1].position - objectPosition0, 0.0f));
    float3 worldPosition2 = mul(
        objectToWorldTransform,
        float4(g_vertices[i2].position - objectPosition0, 0.0f));
    float2 texCoord0 = g_vertices[i0].texCoord;
    float2 texCoord1 = g_vertices[i1].texCoord;
    float2 texCoord2 = g_vertices[i2].texCoord;

    if (useGeometricNormal)
        worldNormal = cross(worldPosition1, worldPosition2);

    float uvPerWorld = max(
        EdgeUvPerWorld(
            worldPosition0,
            worldPosition1,
            texCoord0,
            texCoord1),
        max(
            EdgeUvPerWorld(
                worldPosition0,
                worldPosition2,
                texCoord0,
                texCoord2),
            EdgeUvPerWorld(
                worldPosition1,
                worldPosition2,
                texCoord1,
                texCoord2)));

    float rayConeWidth =
        rayConeWidthAtOrigin + rayConeSpread * RayTCurrent();
    float viewAgreement = abs(dot(
        normalize(worldNormal),
        normalize(-WorldRayDirection())));
    float grazingScale = min(
        1.0f / max(viewAgreement, 0.25f),
        4.0f);
    return max(rayConeWidth, 0.0f) *
        uvPerWorld * grazingScale;
}

bool PassesSceneAlphaMask(
    SceneMaterial material,
    float2 texCoord,
    float uvFootprint)
{
    if (material.alphaCutoff < 0.0f)
    {
        return true;
    }

    float alpha = material.baseColorAlpha;
    if (material.baseColorTextureIndex != c_invalidSceneTextureIndex)
    {
        uint textureMetadata = material.baseColorTextureIndex;
        uint textureIndex = NonUniformResourceIndex(
            SceneTextureDescriptorIndex(textureMetadata));
        float mipLevel = TextureMipLevel(textureMetadata, uvFootprint);
        alpha *= g_materialTextures[textureIndex].SampleLevel(
                g_materialSampler, texCoord, mipLevel).a;
    }
    return alpha >= material.alphaCutoff;
}

float3 SurfaceEmission(uint primitiveIndex)
{
    return GetSceneMaterial(primitiveIndex).emission;
}

float3 CornellSurfaceAlbedo(uint primitiveIndex)
{
    return GetSceneMaterial(primitiveIndex).baseColor;
}

PbrMaterial GetPbrMaterial(
    SceneMaterial sceneMaterial,
    float2 texCoord,
    float uvFootprint)
{
    PbrMaterial material;
    material.baseColor = sceneMaterial.baseColor;
    material.metallic = sceneMaterial.metallic;
    material.roughness = sceneMaterial.roughness;
    material.emission = sceneMaterial.emission;
    if (sceneMaterial.baseColorTextureIndex != c_invalidSceneTextureIndex)
    {
        uint textureMetadata = sceneMaterial.baseColorTextureIndex;
        uint textureIndex = NonUniformResourceIndex(
            SceneTextureDescriptorIndex(textureMetadata));
        float mipLevel = TextureMipLevel(textureMetadata, uvFootprint);
        material.baseColor *= g_materialTextures[textureIndex].SampleLevel(
                g_materialSampler, texCoord, mipLevel).rgb;
    }
    if (sceneMaterial.metallicRoughnessTextureIndex != c_invalidSceneTextureIndex)
    {
        uint textureMetadata = sceneMaterial.metallicRoughnessTextureIndex;
        uint textureIndex = NonUniformResourceIndex(
            SceneTextureDescriptorIndex(textureMetadata));
        float mipLevel = TextureMipLevel(textureMetadata, uvFootprint);
        float4 metallicRoughness = g_materialTextures[textureIndex].SampleLevel(
                g_materialSampler, texCoord, mipLevel);
        material.roughness *= metallicRoughness.g;
        material.metallic *= metallicRoughness.b;
    }
    if (sceneMaterial.pbrParameterMode == c_pbrParameterModeGlobal ||
        (sceneMaterial.pbrParameterMode == c_pbrParameterModeFixed &&
         g_overridePbrMaterial != 0))
    {
        material.metallic = g_pbrMetallic;
        material.roughness = g_pbrRoughness;
    }
    material.metallic = saturate(material.metallic);
    material.roughness = saturate(material.roughness);
    return material;
}

float3 ApplySceneNormalMap(
    SceneMaterial sceneMaterial,
    float2 texCoord,
    float uvFootprint,
    float4 interpolatedTangent,
    float3 normal)
{
    if (sceneMaterial.normalTextureIndex == c_invalidSceneTextureIndex)
    {
        return normal;
    }

    float3 tangent = interpolatedTangent.xyz -
        normal * dot(normal, interpolatedTangent.xyz);
    float tangentLengthSquared = dot(tangent, tangent);
    if (tangentLengthSquared <= 1.0e-12f)
    {
        return normal;
    }
    tangent *= rsqrt(tangentLengthSquared);
    float3 bitangent = cross(normal, tangent) * interpolatedTangent.w;

    uint textureMetadata = sceneMaterial.normalTextureIndex;
    uint textureIndex = NonUniformResourceIndex(
        SceneTextureDescriptorIndex(textureMetadata));
    float mipLevel = TextureMipLevel(textureMetadata, uvFootprint);
    float3 tangentNormal = g_materialTextures[textureIndex].SampleLevel(
            g_materialSampler, texCoord, mipLevel).xyz * 2.0f - 1.0f;
    tangentNormal.xy *= sceneMaterial.normalTextureScale;
    return normalize(
        tangent * tangentNormal.x +
        bitangent * tangentNormal.y +
        normal * tangentNormal.z);
}

float3 PbrMaterialDebugColor(
    uint primitiveIndex,
    float2 texCoord,
    float uvFootprint)
{
    PbrMaterial material = GetPbrMaterial(
        GetSceneMaterial(primitiveIndex),
        texCoord,
        uvFootprint);

    if (g_pbrDebugView == c_pbrDebugAlbedo)
    {
        return material.baseColor;
    }

    if (g_pbrDebugView == c_pbrDebugMetallic)
    {
        return float3(material.metallic, material.metallic, material.metallic);
    }

    if (g_pbrDebugView == c_pbrDebugRoughness)
    {
        return float3(material.roughness, material.roughness, material.roughness);
    }

    return float3(0.0f, 0.0f, 0.0f);
}

float3 DepthDebugColor(float rayDistance)
{
    float normalizedDepth = max(rayDistance, 0.0f) /
        (1.0f + max(rayDistance, 0.0f));
    return float3(normalizedDepth, normalizedDepth, normalizedDepth);
}

float3 MaterialIdDebugColor(uint primitiveIndex)
{
    uint value = g_primitiveMaterialIndices[primitiveIndex] + 1u;
    value ^= value >> 16;
    value *= 0x7FEB352Du;
    value ^= value >> 15;
    value *= 0x846CA68Bu;
    value ^= value >> 16;
    return float3(
        float((value >> 0) & 0xFFu),
        float((value >> 8) & 0xFFu),
        float((value >> 16) & 0xFFu)) / 255.0f;
}
#endif
