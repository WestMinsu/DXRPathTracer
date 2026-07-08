#ifndef RAYTRACING_SCENE_HLSLI
#define RAYTRACING_SCENE_HLSLI

#include "RaytracingCommon.hlsli"

bool IsCornellLightPrimitive(uint primitiveIndex)
{
    return primitiveIndex >= c_lightPrimitiveStart &&
        primitiveIndex < c_lightPrimitiveStart + c_lightPrimitiveCount;
}

bool IsPbrLightPrimitive(uint primitiveIndex)
{
    return primitiveIndex >= c_pbrLightPrimitiveStart &&
        primitiveIndex < c_pbrLightPrimitiveStart + c_pbrLightPrimitiveCount;
}

bool IsLightPrimitive(uint primitiveIndex)
{
    return g_sceneType == c_scenePbrGgx
        ? IsPbrLightPrimitive(primitiveIndex)
        : IsCornellLightPrimitive(primitiveIndex);
}

float3 SurfaceEmission(uint primitiveIndex)
{
    if (g_sceneType == c_scenePbrGgx)
    {
        return IsPbrLightPrimitive(primitiveIndex) ? c_pbrLightEmission : float3(0.0f, 0.0f, 0.0f);
    }

    return IsCornellLightPrimitive(primitiveIndex) ? c_cornellLightEmission : float3(0.0f, 0.0f, 0.0f);
}

float3 CornellSurfaceAlbedo(uint primitiveIndex)
{
    if (IsCornellLightPrimitive(primitiveIndex))
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    if (primitiveIndex >= c_rightWallPrimitiveStart)
    {
        return c_rightWallAlbedo;
    }

    if (primitiveIndex >= c_leftWallPrimitiveStart)
    {
        return c_leftWallAlbedo;
    }

    if (primitiveIndex >= c_backWallPrimitiveStart)
    {
        return c_backWallAlbedo;
    }

    if (primitiveIndex >= c_ceilingPrimitiveStart)
    {
        return c_ceilingAlbedo;
    }

    if (primitiveIndex >= c_floorPrimitiveStart)
    {
        return c_floorAlbedo;
    }

    return c_blockAlbedo;
}


PbrMaterial GetPbrMaterial(uint primitiveIndex)
{
    PbrMaterial material;
    material.baseColor = float3(0.55f, 0.55f, 0.55f);
    material.metallic = 0.0f;
    material.roughness = 0.65f;
    material.emission = float3(0.0f, 0.0f, 0.0f);

    if (IsPbrLightPrimitive(primitiveIndex))
    {
        material.baseColor = float3(0.0f, 0.0f, 0.0f);
        material.emission = c_pbrLightEmission;
        return material;
    }

    if (primitiveIndex < c_pbrSpherePrimitiveCount * c_pbrSphereCount)
    {
        material.baseColor = float3(1.0f, 0.766f, 0.336f);
        material.metallic = g_pbrMetallic;
        material.roughness = g_pbrRoughness;
    }
    else if (primitiveIndex >= c_pbrBackWallPrimitiveStart)
    {
        material.baseColor = float3(0.48f, 0.50f, 0.55f);
        material.roughness = 0.75f;
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
