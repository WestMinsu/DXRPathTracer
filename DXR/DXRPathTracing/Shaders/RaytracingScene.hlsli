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

PbrMaterial GetPbrMaterial(uint primitiveIndex)
{
    SceneMaterial sceneMaterial = GetSceneMaterial(primitiveIndex);
    PbrMaterial material;
    material.baseColor = sceneMaterial.baseColor;
    material.metallic = sceneMaterial.metallic;
    material.roughness = sceneMaterial.roughness;
    material.emission = sceneMaterial.emission;
    if (sceneMaterial.useGlobalPbrParameters != 0)
    {
        material.metallic = g_pbrMetallic;
        material.roughness = g_pbrRoughness;
    }
    return material;
}

float3 PbrMaterialDebugColor(uint primitiveIndex)
{
    PbrMaterial material = GetPbrMaterial(primitiveIndex);

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
#endif
