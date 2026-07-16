#ifndef RAYTRACING_SCENE_HLSLI
#define RAYTRACING_SCENE_HLSLI

#include "RaytracingCommon.hlsli"

SceneMaterial GetSceneMaterial(uint primitiveIndex)
{
    return g_sceneMaterials[g_primitiveMaterialIndices[primitiveIndex]];
}

float3 SurfaceEmission(uint primitiveIndex)
{
    return GetSceneMaterial(primitiveIndex).emission;
}

float3 CornellSurfaceAlbedo(uint primitiveIndex)
{
    return GetSceneMaterial(primitiveIndex).baseColor;
}

PbrMaterial GetPbrMaterial(uint primitiveIndex, float2 texCoord)
{
    SceneMaterial sceneMaterial = GetSceneMaterial(primitiveIndex);
    PbrMaterial material;
    material.baseColor = sceneMaterial.baseColor;
    material.metallic = sceneMaterial.metallic;
    material.roughness = sceneMaterial.roughness;
    material.emission = sceneMaterial.emission;
    if (sceneMaterial.baseColorTextureIndex != c_invalidSceneTextureIndex)
    {
        uint textureIndex = NonUniformResourceIndex(
            sceneMaterial.baseColorTextureIndex);
        material.baseColor *= g_materialTextures[textureIndex].SampleLevel(
                g_materialSampler, texCoord, 0.0f).rgb;
    }
    if (sceneMaterial.metallicRoughnessTextureIndex != c_invalidSceneTextureIndex)
    {
        uint textureIndex = NonUniformResourceIndex(
            sceneMaterial.metallicRoughnessTextureIndex);
        float4 metallicRoughness = g_materialTextures[textureIndex].SampleLevel(
                g_materialSampler, texCoord, 0.0f);
        material.roughness *= metallicRoughness.g;
        material.metallic *= metallicRoughness.b;
    }
    if (sceneMaterial.useGlobalPbrParameters != 0)
    {
        material.metallic = g_pbrMetallic;
        material.roughness = g_pbrRoughness;
    }
    material.metallic = saturate(material.metallic);
    material.roughness = saturate(material.roughness);
    return material;
}

float3 ApplySceneNormalMap(
    uint primitiveIndex,
    float2 texCoord,
    float4 interpolatedTangent,
    float3 normal)
{
    SceneMaterial sceneMaterial = GetSceneMaterial(primitiveIndex);
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

    uint textureIndex = NonUniformResourceIndex(
        sceneMaterial.normalTextureIndex);
    float3 tangentNormal = g_materialTextures[textureIndex].SampleLevel(
            g_materialSampler, texCoord, 0.0f).xyz * 2.0f - 1.0f;
    tangentNormal.xy *= sceneMaterial.normalTextureScale;
    return normalize(
        tangent * tangentNormal.x +
        bitangent * tangentNormal.y +
        normal * tangentNormal.z);
}

float3 PbrMaterialDebugColor(uint primitiveIndex, float2 texCoord)
{
    PbrMaterial material = GetPbrMaterial(primitiveIndex, texCoord);

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
