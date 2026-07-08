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

float PbrRoughnessFromColumn(uint column)
{
    if (column == 0)
        return 0.06f;
    if (column == 1)
        return 0.18f;
    if (column == 2)
        return 0.35f;
    if (column == 3)
        return 0.60f;
    return 0.85f;
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
        uint sphereIndex = primitiveIndex / c_pbrSpherePrimitiveCount;
        uint row = sphereIndex / c_pbrSphereColumns;
        uint column = sphereIndex - row * c_pbrSphereColumns;
        material.roughness = PbrRoughnessFromColumn(column);

        if (row == 0)
        {
            material.baseColor = float3(0.82f, 0.08f, 0.035f);
            material.metallic = 0.0f;
        }
        else
        {
            material.baseColor = float3(1.0f, 0.766f, 0.336f);
            material.metallic = 1.0f;
        }
    }
    else if (primitiveIndex >= c_pbrBackWallPrimitiveStart)
    {
        material.baseColor = float3(0.48f, 0.50f, 0.55f);
        material.roughness = 0.75f;
    }

    return material;
}

#endif
