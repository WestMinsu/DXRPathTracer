#include "SceneData.h"

#include <cmath>
#include <limits>

namespace
{
    constexpr float c_pi = 3.141592654f;
    constexpr float c_twoPi = 6.283185307f;

    constexpr float c_shortBlockHalfWidth = 0.42f;
    constexpr float c_shortBlockHeight = 0.58f;
    constexpr float c_shortBlockHalfDepth = 0.42f;
    constexpr float c_shortBlockCenterX = -0.68f;
    constexpr float c_shortBlockCenterZ = 1.28f;
    constexpr float c_shortBlockCosY = 0.951056516f;
    constexpr float c_shortBlockSinY = -0.309016994f;
    constexpr float c_tallBlockHalfWidth = 0.42f;
    constexpr float c_tallBlockHeight = 1.15f;
    constexpr float c_tallBlockHalfDepth = 0.42f;
    constexpr float c_tallBlockCenterX = 0.62f;
    constexpr float c_tallBlockCenterZ = 2.35f;
    constexpr float c_tallBlockCosY = 0.965925826f;
    constexpr float c_tallBlockSinY = 0.258819045f;
    constexpr float c_boxFloorY = -0.85f;
    constexpr float c_boxCeilingY = 1.25f;
    constexpr float c_boxHalfWidth = 2.25f;
    constexpr float c_boxNearZ = 0.0f;
    constexpr float c_boxFarZ = 4.0f;
    constexpr float c_cornellLightY = c_boxCeilingY - 0.002f;
    constexpr float c_cornellLightHalfWidth = 0.55f;
    constexpr float c_cornellLightNearZ = 1.10f;
    constexpr float c_cornellLightFarZ = 2.25f;

    constexpr std::uint32_t c_sphereCount = 3;
    constexpr std::uint32_t c_sphereSlices = 24;
    constexpr std::uint32_t c_sphereStacks = 12;
    constexpr float c_sphereRadius = 0.42f;
    constexpr float c_sphereStartX = -0.92f;
    constexpr float c_sphereSpacingX = 0.92f;
    constexpr float c_sphereCenterZ = 1.80f;
    constexpr float c_pbrFloorY = -0.85f;
    constexpr float c_pbrSceneHalfWidth = 2.05f;
    constexpr float c_pbrSceneNearZ = 0.0f;
    constexpr float c_pbrSceneBackZ = 4.25f;

    struct Float3
    {
        float x;
        float y;
        float z;
    };

    Float3 MakeFloat3(float x, float y, float z)
    {
        return { x, y, z };
    }

    SceneVertex MakeVertex(Float3 position, Float3 normal)
    {
        return
        {
            { position.x, position.y, position.z },
            { normal.x, normal.y, normal.z }
        };
    }

    SceneMaterial MakeMaterial(
        Float3 baseColor,
        float metallic,
        float roughness,
        Float3 emission = { 0.0f, 0.0f, 0.0f },
        bool useGlobalPbrParameters = false)
    {
        return
        {
            { baseColor.x, baseColor.y, baseColor.z },
            metallic,
            roughness,
            { emission.x, emission.y, emission.z },
            useGlobalPbrParameters ? 1u : 0u
        };
    }

    Float3 RotateY(Float3 value, float cosY, float sinY)
    {
        return MakeFloat3(
            value.x * cosY + value.z * sinY,
            value.y,
            -value.x * sinY + value.z * cosY);
    }

    Float3 MakeBlockPoint(
        float x,
        float y,
        float z,
        float centerX,
        float centerZ,
        float cosY,
        float sinY)
    {
        const Float3 rotated = RotateY(MakeFloat3(x, y, z), cosY, sinY);
        return MakeFloat3(centerX + rotated.x, c_boxFloorY + rotated.y, centerZ + rotated.z);
    }

    Float3 MakeBlockNormal(float x, float y, float z, float cosY, float sinY)
    {
        return RotateY(MakeFloat3(x, y, z), cosY, sinY);
    }

    void AddTriangleMaterialIndices(
        SceneData& scene,
        std::size_t previousIndexCount,
        std::uint32_t materialIndex)
    {
        const std::size_t addedIndexCount = scene.indices.size() - previousIndexCount;
        const std::size_t addedTriangleCount = addedIndexCount / 3;
        scene.primitiveMaterialIndices.insert(
            scene.primitiveMaterialIndices.end(),
            addedTriangleCount,
            materialIndex);
    }

    void AddQuad(
        SceneData& scene,
        Float3 p0,
        Float3 p1,
        Float3 p2,
        Float3 p3,
        Float3 normal,
        std::uint32_t materialIndex)
    {
        const std::uint32_t baseIndex = static_cast<std::uint32_t>(scene.vertices.size());
        scene.vertices.push_back(MakeVertex(p0, normal));
        scene.vertices.push_back(MakeVertex(p1, normal));
        scene.vertices.push_back(MakeVertex(p2, normal));
        scene.vertices.push_back(MakeVertex(p3, normal));

        scene.indices.push_back(baseIndex + 0);
        scene.indices.push_back(baseIndex + 1);
        scene.indices.push_back(baseIndex + 2);
        scene.indices.push_back(baseIndex + 0);
        scene.indices.push_back(baseIndex + 2);
        scene.indices.push_back(baseIndex + 3);
        scene.primitiveMaterialIndices.push_back(materialIndex);
        scene.primitiveMaterialIndices.push_back(materialIndex);
    }

    void AddCornellBlock(
        SceneData& scene,
        float halfWidth,
        float height,
        float halfDepth,
        float centerX,
        float centerZ,
        float cosY,
        float sinY,
        std::uint32_t materialIndex)
    {
        const auto point = [=](float x, float y, float z)
        {
            return MakeBlockPoint(x, y, z, centerX, centerZ, cosY, sinY);
        };
        const auto normal = [=](float x, float y, float z)
        {
            return MakeBlockNormal(x, y, z, cosY, sinY);
        };

        AddQuad(scene, point(-halfWidth, height, -halfDepth), point( halfWidth, height, -halfDepth), point( halfWidth, 0.0f, -halfDepth), point(-halfWidth, 0.0f, -halfDepth), normal( 0.0f,  0.0f, -1.0f), materialIndex);
        AddQuad(scene, point(-halfWidth, height,  halfDepth), point(-halfWidth, 0.0f,  halfDepth), point( halfWidth, 0.0f,  halfDepth), point( halfWidth, height,  halfDepth), normal( 0.0f,  0.0f,  1.0f), materialIndex);
        AddQuad(scene, point(-halfWidth, height,  halfDepth), point(-halfWidth, height, -halfDepth), point(-halfWidth, 0.0f, -halfDepth), point(-halfWidth, 0.0f,  halfDepth), normal(-1.0f,  0.0f,  0.0f), materialIndex);
        AddQuad(scene, point( halfWidth, height, -halfDepth), point( halfWidth, height,  halfDepth), point( halfWidth, 0.0f,  halfDepth), point( halfWidth, 0.0f, -halfDepth), normal( 1.0f,  0.0f,  0.0f), materialIndex);
        AddQuad(scene, point(-halfWidth, height,  halfDepth), point( halfWidth, height,  halfDepth), point( halfWidth, height, -halfDepth), point(-halfWidth, height, -halfDepth), normal( 0.0f,  1.0f,  0.0f), materialIndex);
        AddQuad(scene, point(-halfWidth, 0.0f, -halfDepth), point( halfWidth, 0.0f, -halfDepth), point( halfWidth, 0.0f,  halfDepth), point(-halfWidth, 0.0f,  halfDepth), normal( 0.0f, -1.0f,  0.0f), materialIndex);
    }

    void AddPbrSphere(
        SceneData& scene,
        Float3 center,
        float radius,
        std::uint32_t materialIndex)
    {
        const std::uint32_t baseIndex = static_cast<std::uint32_t>(scene.vertices.size());
        for (std::uint32_t stack = 0; stack <= c_sphereStacks; ++stack)
        {
            const float theta = c_pi * static_cast<float>(stack) / static_cast<float>(c_sphereStacks);
            const float sinTheta = std::sin(theta);
            const float cosTheta = std::cos(theta);

            for (std::uint32_t slice = 0; slice <= c_sphereSlices; ++slice)
            {
                const float phi = c_twoPi * static_cast<float>(slice) / static_cast<float>(c_sphereSlices);
                const float sinPhi = std::sin(phi);
                const float cosPhi = std::cos(phi);
                const Float3 normal = MakeFloat3(sinTheta * cosPhi, cosTheta, sinTheta * sinPhi);
                const Float3 position = MakeFloat3(
                    center.x + normal.x * radius,
                    center.y + normal.y * radius,
                    center.z + normal.z * radius);
                scene.vertices.push_back(MakeVertex(position, normal));
            }
        }

        const auto vertexIndex = [baseIndex](std::uint32_t stack, std::uint32_t slice)
        {
            return baseIndex + stack * (c_sphereSlices + 1) + slice;
        };

        const std::size_t previousIndexCount = scene.indices.size();
        for (std::uint32_t stack = 0; stack < c_sphereStacks; ++stack)
        {
            for (std::uint32_t slice = 0; slice < c_sphereSlices; ++slice)
            {
                const std::uint32_t i0 = vertexIndex(stack, slice);
                const std::uint32_t i1 = vertexIndex(stack + 1, slice);
                const std::uint32_t i2 = vertexIndex(stack + 1, slice + 1);
                const std::uint32_t i3 = vertexIndex(stack, slice + 1);

                if (stack == 0)
                {
                    scene.indices.push_back(i0);
                    scene.indices.push_back(i1);
                    scene.indices.push_back(i2);
                }
                else if (stack == c_sphereStacks - 1)
                {
                    scene.indices.push_back(i0);
                    scene.indices.push_back(i1);
                    scene.indices.push_back(i3);
                }
                else
                {
                    scene.indices.push_back(i0);
                    scene.indices.push_back(i1);
                    scene.indices.push_back(i2);
                    scene.indices.push_back(i0);
                    scene.indices.push_back(i2);
                    scene.indices.push_back(i3);
                }
            }
        }
        AddTriangleMaterialIndices(scene, previousIndexCount, materialIndex);
    }
}

bool SceneData::IsValid() const
{
    if (vertices.empty() || indices.empty() || materials.empty() || indices.size() % 3 != 0)
        return false;

    if (primitiveMaterialIndices.size() != indices.size() / 3)
        return false;

    for (const std::uint32_t index : indices)
    {
        if (index >= vertices.size())
            return false;
    }

    for (const std::uint32_t materialIndex : primitiveMaterialIndices)
    {
        if (materialIndex >= materials.size())
            return false;
    }

    return vertices.size() <= std::numeric_limits<std::uint32_t>::max() &&
        indices.size() <= std::numeric_limits<std::uint32_t>::max();
}

SceneData CreateCornellBoxSceneData()
{
    SceneData scene;
    scene.vertices.reserve(72);
    scene.indices.reserve(108);
    scene.primitiveMaterialIndices.reserve(36);

    constexpr std::uint32_t blockMaterial = 0;
    constexpr std::uint32_t floorMaterial = 1;
    constexpr std::uint32_t ceilingMaterial = 2;
    constexpr std::uint32_t backWallMaterial = 3;
    constexpr std::uint32_t leftWallMaterial = 4;
    constexpr std::uint32_t rightWallMaterial = 5;
    constexpr std::uint32_t lightMaterial = 6;
    scene.materials =
    {
        MakeMaterial(MakeFloat3(0.74f, 0.74f, 0.74f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.75f, 0.75f, 0.75f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.75f, 0.75f, 0.75f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.75f, 0.75f, 0.75f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.65f, 0.08f, 0.05f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.12f, 0.45f, 0.10f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.0f, 0.0f, 0.0f), 0.0f, 1.0f, MakeFloat3(12.0f, 10.0f, 8.0f))
    };

    AddCornellBlock(scene, c_shortBlockHalfWidth, c_shortBlockHeight, c_shortBlockHalfDepth, c_shortBlockCenterX, c_shortBlockCenterZ, c_shortBlockCosY, c_shortBlockSinY, blockMaterial);
    AddCornellBlock(scene, c_tallBlockHalfWidth, c_tallBlockHeight, c_tallBlockHalfDepth, c_tallBlockCenterX, c_tallBlockCenterZ, c_tallBlockCosY, c_tallBlockSinY, blockMaterial);

    AddQuad(scene, MakeFloat3(-c_boxHalfWidth, c_boxFloorY, c_boxNearZ), MakeFloat3(-c_boxHalfWidth, c_boxFloorY, c_boxFarZ), MakeFloat3( c_boxHalfWidth, c_boxFloorY, c_boxFarZ), MakeFloat3( c_boxHalfWidth, c_boxFloorY, c_boxNearZ), MakeFloat3(0.0f, 1.0f, 0.0f), floorMaterial);
    AddQuad(scene, MakeFloat3(-c_boxHalfWidth, c_boxCeilingY, c_boxNearZ), MakeFloat3( c_boxHalfWidth, c_boxCeilingY, c_boxNearZ), MakeFloat3( c_boxHalfWidth, c_boxCeilingY, c_boxFarZ), MakeFloat3(-c_boxHalfWidth, c_boxCeilingY, c_boxFarZ), MakeFloat3(0.0f, -1.0f, 0.0f), ceilingMaterial);
    AddQuad(scene, MakeFloat3(-c_boxHalfWidth, c_boxFloorY, c_boxFarZ), MakeFloat3(-c_boxHalfWidth, c_boxCeilingY, c_boxFarZ), MakeFloat3( c_boxHalfWidth, c_boxCeilingY, c_boxFarZ), MakeFloat3( c_boxHalfWidth, c_boxFloorY, c_boxFarZ), MakeFloat3(0.0f, 0.0f, -1.0f), backWallMaterial);
    AddQuad(scene, MakeFloat3(-c_boxHalfWidth, c_boxFloorY, c_boxNearZ), MakeFloat3(-c_boxHalfWidth, c_boxCeilingY, c_boxNearZ), MakeFloat3(-c_boxHalfWidth, c_boxCeilingY, c_boxFarZ), MakeFloat3(-c_boxHalfWidth, c_boxFloorY, c_boxFarZ), MakeFloat3(1.0f, 0.0f, 0.0f), leftWallMaterial);
    AddQuad(scene, MakeFloat3( c_boxHalfWidth, c_boxFloorY, c_boxNearZ), MakeFloat3( c_boxHalfWidth, c_boxFloorY, c_boxFarZ), MakeFloat3( c_boxHalfWidth, c_boxCeilingY, c_boxFarZ), MakeFloat3( c_boxHalfWidth, c_boxCeilingY, c_boxNearZ), MakeFloat3(-1.0f, 0.0f, 0.0f), rightWallMaterial);
    AddQuad(scene, MakeFloat3(-c_cornellLightHalfWidth, c_cornellLightY, c_cornellLightNearZ), MakeFloat3( c_cornellLightHalfWidth, c_cornellLightY, c_cornellLightNearZ), MakeFloat3( c_cornellLightHalfWidth, c_cornellLightY, c_cornellLightFarZ), MakeFloat3(-c_cornellLightHalfWidth, c_cornellLightY, c_cornellLightFarZ), MakeFloat3(0.0f, -1.0f, 0.0f), lightMaterial);
    return scene;
}

SceneData CreatePbrGgxSceneData()
{
    SceneData scene;
    scene.vertices.reserve(c_sphereCount * (c_sphereStacks + 1) * (c_sphereSlices + 1) + 4);
    scene.indices.reserve(c_sphereCount * c_sphereSlices * (c_sphereStacks - 1) * 6 + 6);
    scene.primitiveMaterialIndices.reserve(c_sphereCount * c_sphereSlices * (c_sphereStacks - 1) * 2 + 2);

    constexpr std::uint32_t sphereMaterial = 0;
    constexpr std::uint32_t floorMaterial = 1;
    scene.materials =
    {
        MakeMaterial(MakeFloat3(1.0f, 0.766f, 0.336f), 1.0f, 0.35f, MakeFloat3(0.0f, 0.0f, 0.0f), true),
        MakeMaterial(MakeFloat3(0.55f, 0.55f, 0.55f), 0.0f, 0.65f)
    };

    for (std::uint32_t sphereIndex = 0; sphereIndex < c_sphereCount; ++sphereIndex)
    {
        const float sphereX = c_sphereStartX + c_sphereSpacingX * static_cast<float>(sphereIndex);
        AddPbrSphere(
            scene,
            MakeFloat3(sphereX, c_pbrFloorY + c_sphereRadius, c_sphereCenterZ),
            c_sphereRadius,
            sphereMaterial);
    }

    AddQuad(
        scene,
        MakeFloat3(-c_pbrSceneHalfWidth, c_pbrFloorY, c_pbrSceneNearZ),
        MakeFloat3(-c_pbrSceneHalfWidth, c_pbrFloorY, c_pbrSceneBackZ),
        MakeFloat3( c_pbrSceneHalfWidth, c_pbrFloorY, c_pbrSceneBackZ),
        MakeFloat3( c_pbrSceneHalfWidth, c_pbrFloorY, c_pbrSceneNearZ),
        MakeFloat3(0.0f, 1.0f, 0.0f),
        floorMaterial);
    return scene;
}
