#include "SceneData.h"

#include <algorithm>
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
            { normal.x, normal.y, normal.z },
            { 0.0f, 0.0f },
            { 1.0f, 0.0f, 0.0f, 1.0f }
        };
    }

    SceneMaterial MakeMaterial(
        Float3 baseColor,
        float metallic,
        float roughness,
        Float3 emission = { 0.0f, 0.0f, 0.0f },
        std::uint32_t pbrParameterMode = c_pbrParameterModeFixed)
    {
        return
        {
            { baseColor.x, baseColor.y, baseColor.z },
            metallic,
            roughness,
            { emission.x, emission.y, emission.z },
            pbrParameterMode,
            c_invalidSceneTextureIndex,
            c_invalidSceneTextureIndex,
            c_invalidSceneTextureIndex,
            1.0f,
            1.0f,
            -1.0f
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

    void AddAxisAlignedBox(
        SceneData& scene,
        Float3 minimum,
        Float3 maximum,
        std::uint32_t materialIndex)
    {
        AddQuad(scene,
            MakeFloat3(minimum.x, minimum.y, minimum.z),
            MakeFloat3(minimum.x, maximum.y, minimum.z),
            MakeFloat3(maximum.x, maximum.y, minimum.z),
            MakeFloat3(maximum.x, minimum.y, minimum.z),
            MakeFloat3(0.0f, 0.0f, -1.0f), materialIndex);
        AddQuad(scene,
            MakeFloat3(maximum.x, minimum.y, maximum.z),
            MakeFloat3(maximum.x, maximum.y, maximum.z),
            MakeFloat3(minimum.x, maximum.y, maximum.z),
            MakeFloat3(minimum.x, minimum.y, maximum.z),
            MakeFloat3(0.0f, 0.0f, 1.0f), materialIndex);
        AddQuad(scene,
            MakeFloat3(minimum.x, minimum.y, maximum.z),
            MakeFloat3(minimum.x, maximum.y, maximum.z),
            MakeFloat3(minimum.x, maximum.y, minimum.z),
            MakeFloat3(minimum.x, minimum.y, minimum.z),
            MakeFloat3(-1.0f, 0.0f, 0.0f), materialIndex);
        AddQuad(scene,
            MakeFloat3(maximum.x, minimum.y, minimum.z),
            MakeFloat3(maximum.x, maximum.y, minimum.z),
            MakeFloat3(maximum.x, maximum.y, maximum.z),
            MakeFloat3(maximum.x, minimum.y, maximum.z),
            MakeFloat3(1.0f, 0.0f, 0.0f), materialIndex);
        AddQuad(scene,
            MakeFloat3(minimum.x, maximum.y, minimum.z),
            MakeFloat3(minimum.x, maximum.y, maximum.z),
            MakeFloat3(maximum.x, maximum.y, maximum.z),
            MakeFloat3(maximum.x, maximum.y, minimum.z),
            MakeFloat3(0.0f, 1.0f, 0.0f), materialIndex);
        AddQuad(scene,
            MakeFloat3(minimum.x, minimum.y, maximum.z),
            MakeFloat3(minimum.x, minimum.y, minimum.z),
            MakeFloat3(maximum.x, minimum.y, minimum.z),
            MakeFloat3(maximum.x, minimum.y, maximum.z),
            MakeFloat3(0.0f, -1.0f, 0.0f), materialIndex);
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

    for (const SceneTexture& texture : textures)
    {
        if (texture.mips.empty())
            return false;

        std::uint32_t expectedWidth = texture.mips.front().width;
        std::uint32_t expectedHeight = texture.mips.front().height;
        for (const SceneTextureMip& mip : texture.mips)
        {
            const std::uint64_t requiredSize =
                static_cast<std::uint64_t>(mip.width) * mip.height * 4u;
            if (mip.width == 0 ||
                mip.height == 0 ||
                mip.width != expectedWidth ||
                mip.height != expectedHeight ||
                requiredSize != mip.rgba8.size())
            {
                return false;
            }

            expectedWidth = (std::max)(expectedWidth / 2u, 1u);
            expectedHeight = (std::max)(expectedHeight / 2u, 1u);
        }
    }

    const auto validTextureIndex = [this](std::uint32_t textureIndex)
    {
        return textureIndex == c_invalidSceneTextureIndex ||
            textureIndex < textures.size();
    };
    for (const SceneMaterial& material : materials)
    {
        if (!validTextureIndex(material.baseColorTextureIndex) ||
            !validTextureIndex(material.metallicRoughnessTextureIndex) ||
            !validTextureIndex(material.normalTextureIndex) ||
            !std::isfinite(material.baseColorAlpha) ||
            !std::isfinite(material.alphaCutoff) ||
            material.baseColorAlpha < 0.0f ||
            material.baseColorAlpha > 1.0f ||
            material.alphaCutoff > 1.0f)
        {
            return false;
        }
    }

    return vertices.size() <= std::numeric_limits<std::uint32_t>::max() &&
        indices.size() <= std::numeric_limits<std::uint32_t>::max();
}

bool ComputeSceneBounds(const SceneData& scene, SceneBounds& bounds)
{
    if (scene.vertices.empty())
        return false;

    for (std::size_t component = 0; component < 3; ++component)
    {
        bounds.minimum[component] = std::numeric_limits<float>::max();
        bounds.maximum[component] = std::numeric_limits<float>::lowest();
    }

    for (const SceneVertex& vertex : scene.vertices)
    {
        for (std::size_t component = 0; component < 3; ++component)
        {
            const float position = vertex.position[component];
            if (!std::isfinite(position))
                return false;
            bounds.minimum[component] = std::min(
                bounds.minimum[component],
                position);
            bounds.maximum[component] = std::max(
                bounds.maximum[component],
                position);
        }
    }
    return true;
}

bool FindWalkableSurfaceHeight(
    const SceneData& scene,
    float x,
    float z,
    float maximumHeight,
    float& height)
{
    bool found = false;
    float bestHeight = std::numeric_limits<float>::lowest();
    constexpr float epsilon = 1.0e-6f;
    for (std::size_t index = 0; index + 2 < scene.indices.size(); index += 3)
    {
        const SceneVertex& v0 = scene.vertices[scene.indices[index + 0]];
        const SceneVertex& v1 = scene.vertices[scene.indices[index + 1]];
        const SceneVertex& v2 = scene.vertices[scene.indices[index + 2]];
        const float x0 = v0.position[0];
        const float z0 = v0.position[2];
        const float x1 = v1.position[0];
        const float z1 = v1.position[2];
        const float x2 = v2.position[0];
        const float z2 = v2.position[2];
        const float denominator =
            (z1 - z2) * (x0 - x2) +
            (x2 - x1) * (z0 - z2);
        if (std::abs(denominator) <= epsilon)
            continue;

        const float w0 =
            ((z1 - z2) * (x - x2) +
             (x2 - x1) * (z - z2)) / denominator;
        const float w1 =
            ((z2 - z0) * (x - x2) +
             (x0 - x2) * (z - z2)) / denominator;
        const float w2 = 1.0f - w0 - w1;
        if (w0 < -epsilon || w1 < -epsilon || w2 < -epsilon)
            continue;

        const float candidateHeight =
            w0 * v0.position[1] +
            w1 * v1.position[1] +
            w2 * v2.position[1];
        if (candidateHeight > maximumHeight + epsilon ||
            candidateHeight <= bestHeight)
        {
            continue;
        }

        const Float3 edge0 = MakeFloat3(
            v1.position[0] - v0.position[0],
            v1.position[1] - v0.position[1],
            v1.position[2] - v0.position[2]);
        const Float3 edge1 = MakeFloat3(
            v2.position[0] - v0.position[0],
            v2.position[1] - v0.position[1],
            v2.position[2] - v0.position[2]);
        const Float3 normal = MakeFloat3(
            edge0.y * edge1.z - edge0.z * edge1.y,
            edge0.z * edge1.x - edge0.x * edge1.z,
            edge0.x * edge1.y - edge0.y * edge1.x);
        const float normalLength = std::sqrt(
            normal.x * normal.x +
            normal.y * normal.y +
            normal.z * normal.z);
        if (normalLength <= epsilon ||
            std::abs(normal.y) / normalLength < 0.5f)
        {
            continue;
        }

        bestHeight = candidateHeight;
        found = true;
    }

    if (found)
        height = bestHeight;
    return found;
}

bool AppendPbrModelRoom(SceneData& scene, const SceneBounds& modelBounds)
{
    Float3 center = {};
    Float3 halfExtent = {};
    float* centerComponents[] = { &center.x, &center.y, &center.z };
    float* halfExtentComponents[] =
    {
        &halfExtent.x,
        &halfExtent.y,
        &halfExtent.z
    };
    for (std::size_t component = 0; component < 3; ++component)
    {
        const float minimum = modelBounds.minimum[component];
        const float maximum = modelBounds.maximum[component];
        if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
            maximum < minimum)
        {
            return false;
        }
        *centerComponents[component] = (minimum + maximum) * 0.5f;
        *halfExtentComponents[component] = (maximum - minimum) * 0.5f;
    }

    const float scale = std::max(
        std::max(halfExtent.x, halfExtent.y),
        std::max(halfExtent.z, 0.01f));
    const float roomHalfWidth = std::max(halfExtent.x * 1.7f, scale * 0.9f);
    const float roomNearZ = center.z - std::max(halfExtent.z * 2.0f, scale * 1.8f);
    const float roomFarZ = center.z + std::max(halfExtent.z * 1.6f, scale * 1.2f);
    const float floorY = modelBounds.minimum[1] - std::max(scale * 0.01f, 0.001f);
    const float ceilingY = modelBounds.maximum[1] + scale * 0.55f;
    const float leftX = center.x - roomHalfWidth;
    const float rightX = center.x + roomHalfWidth;

    const std::uint32_t floorMaterial =
        static_cast<std::uint32_t>(scene.materials.size());
    scene.materials.push_back(MakeMaterial(
        MakeFloat3(0.32f, 0.34f, 0.37f),
        0.0f,
        0.75f,
        MakeFloat3(0.0f, 0.0f, 0.0f),
        c_pbrParameterModeFixedNoOverride));
    const std::uint32_t wallMaterial =
        static_cast<std::uint32_t>(scene.materials.size());
    scene.materials.push_back(MakeMaterial(
        MakeFloat3(0.62f, 0.64f, 0.68f),
        0.0f,
        0.85f,
        MakeFloat3(0.0f, 0.0f, 0.0f),
        c_pbrParameterModeFixedNoOverride));
    const std::uint32_t lightMaterial =
        static_cast<std::uint32_t>(scene.materials.size());
    scene.materials.push_back(MakeMaterial(
        MakeFloat3(0.0f, 0.0f, 0.0f),
        0.0f,
        1.0f,
        MakeFloat3(12.0f, 10.0f, 8.0f),
        c_pbrParameterModeFixedNoOverride));

    AddQuad(
        scene,
        MakeFloat3(leftX, floorY, roomNearZ),
        MakeFloat3(leftX, floorY, roomFarZ),
        MakeFloat3(rightX, floorY, roomFarZ),
        MakeFloat3(rightX, floorY, roomNearZ),
        MakeFloat3(0.0f, 1.0f, 0.0f),
        floorMaterial);
    AddQuad(
        scene,
        MakeFloat3(leftX, floorY, roomFarZ),
        MakeFloat3(leftX, ceilingY, roomFarZ),
        MakeFloat3(rightX, ceilingY, roomFarZ),
        MakeFloat3(rightX, floorY, roomFarZ),
        MakeFloat3(0.0f, 0.0f, -1.0f),
        wallMaterial);
    AddQuad(
        scene,
        MakeFloat3(leftX, floorY, roomNearZ),
        MakeFloat3(leftX, ceilingY, roomNearZ),
        MakeFloat3(leftX, ceilingY, roomFarZ),
        MakeFloat3(leftX, floorY, roomFarZ),
        MakeFloat3(1.0f, 0.0f, 0.0f),
        wallMaterial);
    AddQuad(
        scene,
        MakeFloat3(rightX, floorY, roomNearZ),
        MakeFloat3(rightX, floorY, roomFarZ),
        MakeFloat3(rightX, ceilingY, roomFarZ),
        MakeFloat3(rightX, ceilingY, roomNearZ),
        MakeFloat3(-1.0f, 0.0f, 0.0f),
        wallMaterial);

    const float lightHalfWidth = std::max(halfExtent.x * 0.65f, scale * 0.22f);
    const float lightHalfDepth = std::max(halfExtent.z * 0.45f, scale * 0.16f);
    const float lightY = ceilingY - std::max(scale * 0.002f, 0.0002f);
    const float lightCenterZ = center.z - halfExtent.z * 0.25f;
    AddQuad(
        scene,
        MakeFloat3(center.x - lightHalfWidth, lightY, lightCenterZ - lightHalfDepth),
        MakeFloat3(center.x + lightHalfWidth, lightY, lightCenterZ - lightHalfDepth),
        MakeFloat3(center.x + lightHalfWidth, lightY, lightCenterZ + lightHalfDepth),
        MakeFloat3(center.x - lightHalfWidth, lightY, lightCenterZ + lightHalfDepth),
        MakeFloat3(0.0f, -1.0f, 0.0f),
        lightMaterial);

    return scene.IsValid();
}

bool AppendAreaLights(
    SceneData& scene,
    const std::vector<SceneAreaLight>& lights)
{
    for (const SceneAreaLight& light : lights)
    {
        const Float3 right = MakeFloat3(
            light.right[0], light.right[1], light.right[2]);
        const Float3 up = MakeFloat3(
            light.up[0], light.up[1], light.up[2]);
        const float rightLength = std::sqrt(
            right.x * right.x + right.y * right.y + right.z * right.z);
        const float upLength = std::sqrt(
            up.x * up.x + up.y * up.y + up.z * up.z);
        if (!(light.width > 0.0f) || !(light.height > 0.0f) ||
            rightLength <= 1.0e-6f || upLength <= 1.0e-6f)
        {
            return false;
        }

        const Float3 rightUnit = MakeFloat3(
            right.x / rightLength,
            right.y / rightLength,
            right.z / rightLength);
        const Float3 upUnit = MakeFloat3(
            up.x / upLength,
            up.y / upLength,
            up.z / upLength);
        Float3 normal = MakeFloat3(
            rightUnit.y * upUnit.z - rightUnit.z * upUnit.y,
            rightUnit.z * upUnit.x - rightUnit.x * upUnit.z,
            rightUnit.x * upUnit.y - rightUnit.y * upUnit.x);
        const float normalLength = std::sqrt(
            normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
        if (normalLength <= 1.0e-6f)
            return false;
        normal = MakeFloat3(
            normal.x / normalLength,
            normal.y / normalLength,
            normal.z / normalLength);

        const Float3 center = MakeFloat3(
            light.position[0], light.position[1], light.position[2]);
        const Float3 rightHalf = MakeFloat3(
            rightUnit.x * light.width * 0.5f,
            rightUnit.y * light.width * 0.5f,
            rightUnit.z * light.width * 0.5f);
        const Float3 upHalf = MakeFloat3(
            upUnit.x * light.height * 0.5f,
            upUnit.y * light.height * 0.5f,
            upUnit.z * light.height * 0.5f);
        const auto corner = [&center](const Float3& a, const Float3& b)
        {
            return MakeFloat3(
                center.x + a.x + b.x,
                center.y + a.y + b.y,
                center.z + a.z + b.z);
        };

        const std::uint32_t materialIndex =
            static_cast<std::uint32_t>(scene.materials.size());
        scene.materials.push_back(MakeMaterial(
            MakeFloat3(0.0f, 0.0f, 0.0f),
            0.0f,
            1.0f,
            MakeFloat3(
                light.radiance[0],
                light.radiance[1],
                light.radiance[2]),
            c_pbrParameterModeFixedNoOverride));
        AddQuad(
            scene,
            corner(MakeFloat3(-rightHalf.x, -rightHalf.y, -rightHalf.z),
                   MakeFloat3(-upHalf.x, -upHalf.y, -upHalf.z)),
            corner(rightHalf,
                   MakeFloat3(-upHalf.x, -upHalf.y, -upHalf.z)),
            corner(rightHalf, upHalf),
            corner(MakeFloat3(-rightHalf.x, -rightHalf.y, -rightHalf.z), upHalf),
            normal,
            materialIndex);
    }
    return scene.IsValid();
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

SceneData CreateIndirectBounceStressSceneData()
{
    SceneData scene;

    constexpr float floorY = -1.20f;
    constexpr float ceilingY = 2.20f;
    constexpr float minimumX = -3.20f;
    constexpr float maximumX = 3.20f;
    constexpr float nearZ = -5.00f;
    constexpr float farZ = 5.00f;
    constexpr float leftGapMaximumX = -1.00f;
    constexpr float rightGapMinimumX = 1.00f;

    constexpr std::uint32_t neutralMaterial = 0;
    constexpr std::uint32_t floorMaterial = 1;
    constexpr std::uint32_t redMaterial = 2;
    constexpr std::uint32_t greenMaterial = 3;
    constexpr std::uint32_t blueMaterial = 4;
    constexpr std::uint32_t columnMaterial = 5;
    constexpr std::uint32_t lightMaterial = 6;
    scene.materials =
    {
        MakeMaterial(MakeFloat3(0.82f, 0.82f, 0.82f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.72f, 0.72f, 0.72f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.78f, 0.10f, 0.07f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.09f, 0.68f, 0.16f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.08f, 0.18f, 0.76f), 0.0f, 1.0f),
        MakeMaterial(MakeFloat3(0.68f, 0.68f, 0.68f), 0.0f, 1.0f),
        MakeMaterial(
            MakeFloat3(0.0f, 0.0f, 0.0f),
            0.0f,
            1.0f,
            MakeFloat3(1800.0f, 1500.0f, 1100.0f))
    };

    // Closed outer room.
    AddQuad(scene,
        MakeFloat3(minimumX, floorY, nearZ),
        MakeFloat3(minimumX, floorY, farZ),
        MakeFloat3(maximumX, floorY, farZ),
        MakeFloat3(maximumX, floorY, nearZ),
        MakeFloat3(0.0f, 1.0f, 0.0f), floorMaterial);
    AddQuad(scene,
        MakeFloat3(minimumX, ceilingY, nearZ),
        MakeFloat3(maximumX, ceilingY, nearZ),
        MakeFloat3(maximumX, ceilingY, farZ),
        MakeFloat3(minimumX, ceilingY, farZ),
        MakeFloat3(0.0f, -1.0f, 0.0f), neutralMaterial);
    AddQuad(scene,
        MakeFloat3(minimumX, floorY, nearZ),
        MakeFloat3(maximumX, floorY, nearZ),
        MakeFloat3(maximumX, ceilingY, nearZ),
        MakeFloat3(minimumX, ceilingY, nearZ),
        MakeFloat3(0.0f, 0.0f, 1.0f), neutralMaterial);
    AddQuad(scene,
        MakeFloat3(minimumX, floorY, farZ),
        MakeFloat3(minimumX, floorY, nearZ),
        MakeFloat3(minimumX, ceilingY, nearZ),
        MakeFloat3(minimumX, ceilingY, farZ),
        MakeFloat3(1.0f, 0.0f, 0.0f), neutralMaterial);
    AddQuad(scene,
        MakeFloat3(maximumX, floorY, nearZ),
        MakeFloat3(maximumX, floorY, farZ),
        MakeFloat3(maximumX, ceilingY, farZ),
        MakeFloat3(maximumX, ceilingY, nearZ),
        MakeFloat3(-1.0f, 0.0f, 0.0f), neutralMaterial);
    AddQuad(scene,
        MakeFloat3(maximumX, floorY, farZ),
        MakeFloat3(minimumX, floorY, farZ),
        MakeFloat3(minimumX, ceilingY, farZ),
        MakeFloat3(maximumX, ceilingY, farZ),
        MakeFloat3(0.0f, 0.0f, -1.0f), neutralMaterial);

    // A separate ceiling area light illuminates only the final chamber. It is
    // hidden behind every baffle from the default camera.
    constexpr float lightY = ceilingY - 0.002f;
    AddQuad(scene,
        MakeFloat3(-2.40f, lightY, 3.35f),
        MakeFloat3(2.40f, lightY, 3.35f),
        MakeFloat3(2.40f, lightY, 4.65f),
        MakeFloat3(-2.40f, lightY, 4.65f),
        MakeFloat3(0.0f, -1.0f, 0.0f), lightMaterial);

    // Three full-height baffles form a right-left-right light path. A straight
    // camera ray cannot pass every opening, so the area-light contribution must
    // arrive through several diffuse reflections.
    AddQuad(scene,
        MakeFloat3(minimumX, floorY, -1.50f),
        MakeFloat3(minimumX, ceilingY, -1.50f),
        MakeFloat3(rightGapMinimumX, ceilingY, -1.50f),
        MakeFloat3(rightGapMinimumX, floorY, -1.50f),
        MakeFloat3(0.0f, 0.0f, -1.0f), redMaterial);
    AddQuad(scene,
        MakeFloat3(leftGapMaximumX, floorY, 0.80f),
        MakeFloat3(leftGapMaximumX, ceilingY, 0.80f),
        MakeFloat3(maximumX, ceilingY, 0.80f),
        MakeFloat3(maximumX, floorY, 0.80f),
        MakeFloat3(0.0f, 0.0f, -1.0f), greenMaterial);
    AddQuad(scene,
        MakeFloat3(minimumX, floorY, 3.00f),
        MakeFloat3(minimumX, ceilingY, 3.00f),
        MakeFloat3(rightGapMinimumX, ceilingY, 3.00f),
        MakeFloat3(rightGapMinimumX, floorY, 3.00f),
        MakeFloat3(0.0f, 0.0f, -1.0f), blueMaterial);

    // Repeated side columns keep the scene visually readable while making
    // every additional radiance ray traverse a non-trivial static BLAS.
    constexpr int columnPairCount = 15;
    constexpr float columnHalfWidth = 0.18f;
    constexpr float columnHalfDepth = 0.14f;
    constexpr float columnTopY = 1.45f;
    for (int columnIndex = 0; columnIndex < columnPairCount; ++columnIndex)
    {
        const float t = static_cast<float>(columnIndex) /
            static_cast<float>(columnPairCount - 1);
        const float centerZ = nearZ + 0.45f + t * (farZ - nearZ - 0.90f);
        for (int side = -1; side <= 1; side += 2)
        {
            const float centerX = static_cast<float>(side) * 2.78f;
            AddAxisAlignedBox(
                scene,
                MakeFloat3(
                    centerX - columnHalfWidth,
                    floorY,
                    centerZ - columnHalfDepth),
                MakeFloat3(
                    centerX + columnHalfWidth,
                    columnTopY,
                    centerZ + columnHalfDepth),
                columnMaterial);
        }
    }

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
        MakeMaterial(MakeFloat3(1.0f, 0.766f, 0.336f), 1.0f, 0.35f, MakeFloat3(0.0f, 0.0f, 0.0f), c_pbrParameterModeGlobal),
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

SceneData CreateRollingMetalSphereSceneData(float radius)
{
    SceneData scene;
    const float safeRadius = (std::max)(radius, 0.001f);
    scene.materials =
    {
        MakeMaterial(
            MakeFloat3(1.0f, 0.766f, 0.336f),
            1.0f,
            0.25f,
            MakeFloat3(0.0f, 0.0f, 0.0f),
            c_pbrParameterModeFixedNoOverride),
        MakeMaterial(
            MakeFloat3(0.06f, 0.07f, 0.08f),
            1.0f,
            0.58f,
            MakeFloat3(0.0f, 0.0f, 0.0f),
            c_pbrParameterModeFixedNoOverride)
    };

    AddPbrSphere(
        scene,
        MakeFloat3(0.0f, 0.0f, 0.0f),
        safeRadius,
        0u);

    for (std::size_t primitiveIndex = 0;
         primitiveIndex < scene.primitiveMaterialIndices.size();
         ++primitiveIndex)
    {
        const std::size_t indexOffset = primitiveIndex * 3u;
        const SceneVertex& vertex0 = scene.vertices[scene.indices[indexOffset]];
        const SceneVertex& vertex1 =
            scene.vertices[scene.indices[indexOffset + 1u]];
        const SceneVertex& vertex2 =
            scene.vertices[scene.indices[indexOffset + 2u]];
        const float centroidX =
            (vertex0.position[0] +
             vertex1.position[0] +
             vertex2.position[0]) / 3.0f;
        if (std::abs(centroidX) <= safeRadius * 0.18f)
            scene.primitiveMaterialIndices[primitiveIndex] = 1u;
    }

    return scene;
}
