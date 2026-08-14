#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "RayTracingManager.h"
#include "GltfSceneLoader.h"
#include "SceneManifest.h"
#include "SceneData.h"
#include "SponzaLightConfig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d12.lib")

namespace
{
    using Matrix4 = std::array<float, 16>;

    struct NodeAnimationPose
    {
        float translation[3] = { 0.0f, 0.0f, 0.0f };
        float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float scale[3] = { 1.0f, 1.0f, 1.0f };
        bool animatedTrs = false;
    };

    Matrix4 IdentityMatrix()
    {
        return {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };
    }

    Matrix4 MultiplyMatrix(const Matrix4& left, const Matrix4& right)
    {
        Matrix4 result = {};
        for (std::size_t column = 0; column < 4; ++column)
        {
            for (std::size_t row = 0; row < 4; ++row)
            {
                float value = 0.0f;
                for (std::size_t component = 0;
                     component < 4;
                     ++component)
                {
                    value += left[component * 4 + row] *
                        right[column * 4 + component];
                }
                result[column * 4 + row] = value;
            }
        }
        return result;
    }

    bool InvertAffineMatrix(const float source[16], Matrix4& inverse)
    {
        const float a00 = source[0];
        const float a01 = source[4];
        const float a02 = source[8];
        const float a10 = source[1];
        const float a11 = source[5];
        const float a12 = source[9];
        const float a20 = source[2];
        const float a21 = source[6];
        const float a22 = source[10];
        const float determinant =
            a00 * (a11 * a22 - a12 * a21) -
            a01 * (a10 * a22 - a12 * a20) +
            a02 * (a10 * a21 - a11 * a20);
        if (std::abs(determinant) <= 1.0e-12f)
            return false;

        const float inverseDeterminant = 1.0f / determinant;
        const float i00 = (a11 * a22 - a12 * a21) * inverseDeterminant;
        const float i01 = (a02 * a21 - a01 * a22) * inverseDeterminant;
        const float i02 = (a01 * a12 - a02 * a11) * inverseDeterminant;
        const float i10 = (a12 * a20 - a10 * a22) * inverseDeterminant;
        const float i11 = (a00 * a22 - a02 * a20) * inverseDeterminant;
        const float i12 = (a02 * a10 - a00 * a12) * inverseDeterminant;
        const float i20 = (a10 * a21 - a11 * a20) * inverseDeterminant;
        const float i21 = (a01 * a20 - a00 * a21) * inverseDeterminant;
        const float i22 = (a00 * a11 - a01 * a10) * inverseDeterminant;
        const float tx = source[12];
        const float ty = source[13];
        const float tz = source[14];
        inverse = {
            i00, i10, i20, 0.0f,
            i01, i11, i21, 0.0f,
            i02, i12, i22, 0.0f,
            -(i00 * tx + i01 * ty + i02 * tz),
            -(i10 * tx + i11 * ty + i12 * tz),
            -(i20 * tx + i21 * ty + i22 * tz),
            1.0f
        };
        return true;
    }

    void NormalizeQuaternion(float quaternion[4])
    {
        const float lengthSquared =
            quaternion[0] * quaternion[0] +
            quaternion[1] * quaternion[1] +
            quaternion[2] * quaternion[2] +
            quaternion[3] * quaternion[3];
        if (lengthSquared <= 1.0e-20f)
        {
            quaternion[0] = 0.0f;
            quaternion[1] = 0.0f;
            quaternion[2] = 0.0f;
            quaternion[3] = 1.0f;
            return;
        }
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        for (std::size_t component = 0; component < 4; ++component)
            quaternion[component] *= inverseLength;
    }

    void SlerpQuaternion(
        const float firstSource[4],
        const float secondSource[4],
        float amount,
        float result[4])
    {
        float first[4] = {
            firstSource[0], firstSource[1],
            firstSource[2], firstSource[3]
        };
        float second[4] = {
            secondSource[0], secondSource[1],
            secondSource[2], secondSource[3]
        };
        NormalizeQuaternion(first);
        NormalizeQuaternion(second);
        float cosine =
            first[0] * second[0] +
            first[1] * second[1] +
            first[2] * second[2] +
            first[3] * second[3];
        if (cosine < 0.0f)
        {
            cosine = -cosine;
            for (float& component : second)
                component = -component;
        }

        if (cosine > 0.9995f)
        {
            for (std::size_t component = 0; component < 4; ++component)
            {
                result[component] =
                    first[component] +
                    (second[component] - first[component]) * amount;
            }
            NormalizeQuaternion(result);
            return;
        }

        cosine = (std::max)(-1.0f, (std::min)(cosine, 1.0f));
        const float angle = std::acos(cosine);
        const float sine = std::sin(angle);
        const float firstWeight = std::sin((1.0f - amount) * angle) / sine;
        const float secondWeight = std::sin(amount * angle) / sine;
        for (std::size_t component = 0; component < 4; ++component)
        {
            result[component] =
                first[component] * firstWeight +
                second[component] * secondWeight;
        }
        NormalizeQuaternion(result);
    }

    Matrix4 ComposeTrsMatrix(const NodeAnimationPose& pose)
    {
        float quaternion[4] = {
            pose.rotation[0], pose.rotation[1],
            pose.rotation[2], pose.rotation[3]
        };
        NormalizeQuaternion(quaternion);
        const float x = quaternion[0];
        const float y = quaternion[1];
        const float z = quaternion[2];
        const float w = quaternion[3];
        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float xz = x * z;
        const float yz = y * z;
        const float xw = x * w;
        const float yw = y * w;
        const float zw = z * w;
        return {
            (1.0f - 2.0f * (yy + zz)) * pose.scale[0],
            (2.0f * (xy + zw)) * pose.scale[0],
            (2.0f * (xz - yw)) * pose.scale[0],
            0.0f,
            (2.0f * (xy - zw)) * pose.scale[1],
            (1.0f - 2.0f * (xx + zz)) * pose.scale[1],
            (2.0f * (yz + xw)) * pose.scale[1],
            0.0f,
            (2.0f * (xz + yw)) * pose.scale[2],
            (2.0f * (yz - xw)) * pose.scale[2],
            (1.0f - 2.0f * (xx + yy)) * pose.scale[2],
            0.0f,
            pose.translation[0],
            pose.translation[1],
            pose.translation[2],
            1.0f
        };
    }

    bool SampleAnimationSampler(
        const SceneAnimationSampler& sampler,
        SceneAnimationPath path,
        float time,
        float result[4])
    {
        const std::size_t componentCount =
            sampler.outputComponentCount;
        if (componentCount == 0 ||
            componentCount > 4 ||
            sampler.inputTimes.empty())
        {
            return false;
        }

        std::size_t firstKey = 0;
        std::size_t secondKey = 0;
        float amount = 0.0f;
        if (time <= sampler.inputTimes.front())
        {
            firstKey = secondKey = 0;
        }
        else if (time >= sampler.inputTimes.back())
        {
            firstKey = secondKey = sampler.inputTimes.size() - 1;
        }
        else
        {
            const auto secondIterator = std::upper_bound(
                sampler.inputTimes.begin(),
                sampler.inputTimes.end(),
                time);
            secondKey = static_cast<std::size_t>(
                secondIterator - sampler.inputTimes.begin());
            firstKey = secondKey - 1;
            const float interval =
                sampler.inputTimes[secondKey] -
                sampler.inputTimes[firstKey];
            amount = interval > 0.0f
                ? (time - sampler.inputTimes[firstKey]) / interval
                : 0.0f;
        }

        const bool cubic =
            sampler.interpolation ==
            SceneAnimationInterpolation::CubicSpline;
        const auto valueOffset = [componentCount, cubic](
            std::size_t key,
            std::size_t cubicPart)
        {
            return cubic
                ? (key * 3u + cubicPart) * componentCount
                : key * componentCount;
        };
        const std::size_t firstValueOffset =
            valueOffset(firstKey, cubic ? 1u : 0u);
        const std::size_t secondValueOffset =
            valueOffset(secondKey, cubic ? 1u : 0u);

        if (firstKey == secondKey ||
            sampler.interpolation ==
                SceneAnimationInterpolation::Step)
        {
            for (std::size_t component = 0;
                 component < componentCount;
                 ++component)
            {
                result[component] =
                    sampler.outputValues[firstValueOffset + component];
            }
        }
        else if (cubic)
        {
            const float interval =
                sampler.inputTimes[secondKey] -
                sampler.inputTimes[firstKey];
            const float amountSquared = amount * amount;
            const float amountCubed = amountSquared * amount;
            const float h00 = 2.0f * amountCubed -
                3.0f * amountSquared + 1.0f;
            const float h10 = amountCubed -
                2.0f * amountSquared + amount;
            const float h01 = -2.0f * amountCubed +
                3.0f * amountSquared;
            const float h11 = amountCubed - amountSquared;
            const std::size_t firstOutTangentOffset =
                valueOffset(firstKey, 2u);
            const std::size_t secondInTangentOffset =
                valueOffset(secondKey, 0u);
            for (std::size_t component = 0;
                 component < componentCount;
                 ++component)
            {
                result[component] =
                    h00 * sampler.outputValues[
                        firstValueOffset + component] +
                    h10 * interval * sampler.outputValues[
                        firstOutTangentOffset + component] +
                    h01 * sampler.outputValues[
                        secondValueOffset + component] +
                    h11 * interval * sampler.outputValues[
                        secondInTangentOffset + component];
            }
        }
        else if (path == SceneAnimationPath::Rotation)
        {
            SlerpQuaternion(
                &sampler.outputValues[firstValueOffset],
                &sampler.outputValues[secondValueOffset],
                amount,
                result);
        }
        else
        {
            for (std::size_t component = 0;
                 component < componentCount;
                 ++component)
            {
                const float first =
                    sampler.outputValues[firstValueOffset + component];
                const float second =
                    sampler.outputValues[secondValueOffset + component];
                result[component] =
                    first + (second - first) * amount;
            }
        }

        if (path == SceneAnimationPath::Rotation)
            NormalizeQuaternion(result);
        return true;
    }

    Matrix4 ConvertRightHandedDeltaToLeftHanded(
        const Matrix4& rightHanded)
    {
        constexpr float signs[4] = { 1.0f, 1.0f, -1.0f, 1.0f };
        Matrix4 leftHanded = {};
        for (std::size_t column = 0; column < 4; ++column)
        {
            for (std::size_t row = 0; row < 4; ++row)
            {
                leftHanded[column * 4 + row] =
                    signs[row] *
                    rightHanded[column * 4 + row] *
                    signs[column];
            }
        }
        return leftHanded;
    }

    void WriteInstanceTransform(
        const Matrix4& columnMajor,
        std::array<float, 12>& rowMajor)
    {
        for (std::size_t row = 0; row < 3; ++row)
        {
            for (std::size_t column = 0; column < 4; ++column)
            {
                rowMajor[row * 4 + column] =
                    columnMajor[column * 4 + row];
            }
        }
    }

    void WriteGpuTimestamp(
        ID3D12GraphicsCommandList4* commandList,
        const RayTracingManager::GpuProfileQueries* profileQueries,
        UINT queryIndex)
    {
        if (!commandList || !profileQueries || !profileQueries->heap)
            return;

        commandList->EndQuery(
            profileQueries->heap,
            D3D12_QUERY_TYPE_TIMESTAMP,
            queryIndex);
    }

    void WriteEmptyGpuProfile(
        ID3D12GraphicsCommandList4* commandList,
        const RayTracingManager::GpuProfileQueries* profileQueries)
    {
        if (!profileQueries)
            return;

        const UINT queryIndices[] =
        {
            profileQueries->tlasBegin,
            profileQueries->tlasEnd,
            profileQueries->pathTraceBegin,
            profileQueries->pathTraceEnd,
            profileQueries->mainPathBegin,
            profileQueries->mainPathEnd,
            profileQueries->disocclusionRepairBegin,
            profileQueries->disocclusionRepairEnd,
            profileQueries->temporalColorClipBegin,
            profileQueries->temporalColorClipEnd,
            profileQueries->atrousDiffuseBegin,
            profileQueries->atrousDiffuseEnd,
            profileQueries->atrousSpecularBegin,
            profileQueries->atrousSpecularEnd
        };
        for (UINT queryIndex : queryIndices)
            WriteGpuTimestamp(commandList, profileQueries, queryIndex);
    }

    constexpr wchar_t c_shadowMissShaderName[] = L"MyMissShader_ShadowRay";
    constexpr wchar_t c_rayGenShaderName[] = L"MyRaygenShader_PathTrace";
    constexpr wchar_t c_surfaceQueryClosestHitShaderName[] =
        L"MyClosestHitShader_SurfaceQuery";
    constexpr wchar_t c_alphaMaskAnyHitShaderName[] =
        L"MyAnyHitShader_AlphaMask";
    constexpr wchar_t c_surfaceQueryMissShaderName[] =
        L"MyMissShader_SurfaceQuery";
    constexpr wchar_t c_surfaceQueryHitGroupName[] =
        L"MyHitGroup_Triangle_SurfaceQuery";
    constexpr wchar_t c_compiledShaderRelativePath[] = L"Shaders\\Raytracing.dxil";
    constexpr wchar_t c_compiledAtrousShaderRelativePath[] =
        L"Shaders\\AtrousDenoiser.dxil";
    constexpr wchar_t c_compiledTemporalColorClipShaderRelativePath[] =
        L"Shaders\\TemporalColorClip.dxil";
    constexpr wchar_t c_environmentMapRelativePath[] = L"Assets\\Textures\\Cubemaps\\HDRI\\autumn_hill_view_4kSpecularHDR.dds";
    constexpr wchar_t c_compiledSkinningShaderRelativePath[] =
        L"Shaders/Skinning.dxil";
    constexpr UINT c_environmentDescriptorIndex = 8;
    constexpr UINT c_materialTextureDescriptorIndex = 9;
    constexpr UINT c_materialTextureDescriptorCount = 256;
    constexpr UINT c_atrousDiffuseIndirectSrvIndex =
        c_materialTextureDescriptorIndex + c_materialTextureDescriptorCount;
    constexpr UINT c_atrousSpecularIndirectSrvIndex =
        c_atrousDiffuseIndirectSrvIndex + 1;
    constexpr UINT c_atrousNormalDepthSrvIndex =
        c_atrousSpecularIndirectSrvIndex + 1;
    constexpr UINT c_atrousMaterialGuideSrvIndex =
        c_atrousNormalDepthSrvIndex + 1;
    constexpr UINT c_atrousDiffuseMomentsSrvIndex =
        c_atrousMaterialGuideSrvIndex + 1;
    constexpr UINT c_atrousSpecularMomentsSrvIndex =
        c_atrousDiffuseMomentsSrvIndex + 1;
    constexpr UINT c_atrousTotalSrvIndex =
        c_atrousSpecularMomentsSrvIndex + 1;
    constexpr UINT c_atrousFilteredDiffuseSrvIndex =
        c_atrousTotalSrvIndex + 1;
    constexpr UINT c_atrousMetallicGuideSrvIndex =
        c_atrousFilteredDiffuseSrvIndex + 1;
    constexpr UINT c_atrousFilterASrvIndex =
        c_atrousMetallicGuideSrvIndex + 1;
    constexpr UINT c_atrousFilterBSrvIndex =
        c_atrousFilterASrvIndex + 1;
    constexpr UINT c_atrousFilterAUavIndex =
        c_atrousFilterBSrvIndex + 1;
    constexpr UINT c_atrousFilterBUavIndex =
        c_atrousFilterAUavIndex + 1;
    constexpr UINT c_atrousFilteredDiffuseUavIndex =
        c_atrousFilterBUavIndex + 1;
    constexpr UINT c_atrousOutputUavIndex =
        c_atrousFilteredDiffuseUavIndex + 1;
    constexpr UINT c_metallicGuideUavIndex =
        c_atrousOutputUavIndex + 1;
    constexpr UINT c_directionalShadowGuideUavIndex =
        c_metallicGuideUavIndex + 1;
    constexpr UINT c_temporalPreviousAccumulationSrvIndex =
        c_directionalShadowGuideUavIndex + 1;
    constexpr UINT c_temporalPreviousNormalDepthSrvIndex =
        c_temporalPreviousAccumulationSrvIndex + 1;
    constexpr UINT c_temporalPreviousMaterialGuideSrvIndex =
        c_temporalPreviousNormalDepthSrvIndex + 1;
    constexpr UINT c_temporalPreviousDiffuseSrvIndex =
        c_temporalPreviousMaterialGuideSrvIndex + 1;
    constexpr UINT c_temporalPreviousSpecularSrvIndex =
        c_temporalPreviousDiffuseSrvIndex + 1;
    constexpr UINT c_temporalPreviousDiffuseMomentsSrvIndex =
        c_temporalPreviousSpecularSrvIndex + 1;
    constexpr UINT c_temporalPreviousSpecularMomentsSrvIndex =
        c_temporalPreviousDiffuseMomentsSrvIndex + 1;
    constexpr UINT c_temporalPreviousDirectionalShadowSrvIndex =
        c_temporalPreviousSpecularMomentsSrvIndex + 1;
    constexpr UINT c_previousInstanceTransformsSrvIndex =
        c_temporalPreviousDirectionalShadowSrvIndex + 1;
    constexpr UINT c_previousSkinnedPositionsSrvIndex =
        c_previousInstanceTransformsSrvIndex + 1;
    constexpr UINT c_directionalLightSrvIndex =
        c_previousSkinnedPositionsSrvIndex + 1;
    constexpr UINT c_temporalHistorySrvCount = 8;
    constexpr UINT c_temporalDescriptorSrvCount =
        c_temporalHistorySrvCount + 3;
    constexpr UINT c_descriptorCount =
        c_directionalLightSrvIndex + 1;
    constexpr UINT c_atrousFilterChannelDiffuse = 0;
    constexpr UINT c_atrousFilterChannelSpecular = 1;
    constexpr UINT c_statisticsPrimaryGuideRayIndex =
        RayTracingManager::c_statisticsRayDepthCount;
    constexpr UINT c_statisticsNeeShadowRayIndex =
        c_statisticsPrimaryGuideRayIndex + 1;
    constexpr UINT c_statisticsHistoryValidationShadowRayIndex =
        c_statisticsNeeShadowRayIndex + 1;
    constexpr UINT c_statisticsHitIndex =
        c_statisticsHistoryValidationShadowRayIndex + 1;
    constexpr UINT c_statisticsMissIndex = c_statisticsHitIndex + 1;
    constexpr UINT c_statisticsCounterCount = c_statisticsMissIndex + 1;
    constexpr UINT c_cubeFaceCount = 6;
    constexpr UINT c_ddsHeaderSize = 128;
    constexpr UINT c_ddsMagic = 0x20534444u;
    constexpr UINT c_d3dFormatA16B16G16R16F = 113;
    constexpr UINT c_bytesPerRgba16FloatPixel = 8;
    constexpr float c_verticalFovRadians = 1.221730476f;
    constexpr float c_cameraFrameMargin = 1.15f;
    struct DdsCubemapData
    {
        UINT width = 0;
        UINT height = 0;
        UINT mipCount = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT bytesPerPixel = 0;
        std::vector<std::uint8_t> texels;
    };

    struct RenderSettingsConstants
    {
        UINT showNormalColor;
        UINT frameIndex;
        UINT maxBounce;
        UINT sampleIndex;
        UINT enableAccumulation;
        UINT sceneType;
        UINT pbrDebugView;
        UINT enableIbl;
        float pbrMetallic;
        float pbrRoughness;
        float iblIntensity;
        float exposure;
        UINT validationSeed;
        float cameraPosition[3];
        float cameraTarget[3];
        UINT overridePbrMaterial;
        UINT enableStatistics;
        UINT dynamicObjectMoved;
        UINT enableRussianRoulette;
        UINT lightingMode;
        UINT emissiveTriangleCount;
        UINT environmentResolution;
        UINT environmentTexelCount;
        float areaLightPower;
        float environmentPower;
        UINT enableAtrous;
        UINT samplesPerPixel;
        UINT previousInstanceTransformCount;
        float previousCameraPosition[3];
        float textureLodBiasOrDisabled;
        float previousCameraTarget[3];
        UINT enableTemporalReprojection;
        UINT temporalDebugView;
        UINT enableDynamicObjectReprojection;
    };
    static_assert(sizeof(RenderSettingsConstants) == 42 * sizeof(std::uint32_t));

    struct GpuDirectionalLight
    {
        float direction[3];
        UINT enabled;
        float radiance[3];
        float samplingProbability;
    };
    static_assert(sizeof(GpuDirectionalLight) == 8 * sizeof(std::uint32_t));

    struct AtrousSettingsConstants
    {
        UINT resolution[2];
        UINT stepWidth;
        UINT inputIsAccumulation;
        UINT finalPass;
        float normalExponent;
        float depthSigma;
        float colorSigma;
        float exposure;
        UINT demodulateDiffuse;
        UINT filterChannel;
        UINT specularMaterialWeightMode;
        UINT specularRoughnessWeightMode;
        UINT kernelMode;
        UINT adaptiveEdgeWeights;
        float adaptiveDynamicNormalExponent;
        float adaptiveDynamicDepthSigma;
        float adaptiveLowHistoryNormalExponent;
        float adaptiveLowHistoryDepthSigma;
        float adaptiveStableNormalExponent;
        float adaptiveStableDepthSigma;
        UINT passIndex;
        UINT adaptiveIterations;
        UINT debugView;
        UINT logLuminanceEdgeStop;
        float logLuminanceSigma;
    };
    static_assert(
        sizeof(AtrousSettingsConstants) == 26 * sizeof(std::uint32_t));

    struct TemporalColorClipSettingsConstants
    {
        UINT resolution[2];
        float clipGamma;
        UINT minNeighborhoodSamples;
        UINT debugView;
        UINT useCurrentFrameVisibleResidual;
    };
    static_assert(
        sizeof(TemporalColorClipSettingsConstants) ==
        6 * sizeof(std::uint32_t));

    struct GpuEmissiveTriangle
    {
        float vertex0[3];
        float area;
        float edge1[3];
        float selectionPdf;
        float edge2[3];
        float selectionCdf;
        float emission[3];
        std::uint32_t primitiveIndex;
    };
    static_assert(sizeof(GpuEmissiveTriangle) == 64);

    struct GpuEnvironmentAliasEntry
    {
        float acceptProbability;
        std::uint32_t aliasIndex;
        float selectionPdf;
        std::uint32_t padding;
    };
    static_assert(sizeof(GpuEnvironmentAliasEntry) == 16);


    constexpr std::uint32_t c_sceneMetadataFlagDynamic = 1u;
    constexpr std::uint32_t c_sceneMetadataFlagSkinned = 2u;

    float HalfToFloat(std::uint16_t value)
    {
        const bool negative = (value & 0x8000u) != 0u;
        const std::uint32_t exponent =
            (static_cast<std::uint32_t>(value) >> 10u) & 0x1Fu;
        const std::uint32_t mantissa =
            static_cast<std::uint32_t>(value) & 0x03FFu;
        float result = 0.0f;
        if (exponent == 0u)
        {
            result = std::ldexp(
                static_cast<float>(mantissa),
                -24);
        }
        else if (exponent == 31u)
        {
            result = mantissa == 0u
                ? std::numeric_limits<float>::infinity()
                : std::numeric_limits<float>::quiet_NaN();
        }
        else
        {
            result = std::ldexp(
                1.0f +
                    static_cast<float>(mantissa) / 1024.0f,
                static_cast<int>(exponent) - 15);
        }
        return negative ? -result : result;
    }

    double CubemapAreaElement(double x, double y)
    {
        return std::atan2(
            x * y,
            std::sqrt(x * x + y * y + 1.0));
    }

    double CubemapTexelSolidAngle(
        UINT x,
        UINT y,
        UINT resolution)
    {
        const double inverseResolution =
            1.0 / static_cast<double>(resolution);
        const double x0 =
            2.0 * static_cast<double>(x) * inverseResolution - 1.0;
        const double y0 =
            2.0 * static_cast<double>(y) * inverseResolution - 1.0;
        const double x1 =
            2.0 * static_cast<double>(x + 1u) * inverseResolution - 1.0;
        const double y1 =
            2.0 * static_cast<double>(y + 1u) * inverseResolution - 1.0;
        return CubemapAreaElement(x1, y1) -
            CubemapAreaElement(x0, y1) -
            CubemapAreaElement(x1, y0) +
            CubemapAreaElement(x0, y0);
    }

    bool BuildEnvironmentAliasTable(
        const DdsCubemapData& cubemap,
        std::vector<GpuEnvironmentAliasEntry>& entries,
        float& environmentPower)
    {
        if (cubemap.width == 0u ||
            cubemap.width != cubemap.height ||
            cubemap.bytesPerPixel != c_bytesPerRgba16FloatPixel)
        {
            return false;
        }

        const UINT resolution = cubemap.width;
        const UINT faceTexelCount = resolution * resolution;
        const UINT texelCount = c_cubeFaceCount * faceTexelCount;
        std::size_t faceByteCount = 0u;
        for (UINT mip = 0u; mip < cubemap.mipCount; ++mip)
        {
            const UINT mipWidth =
                std::max(cubemap.width >> mip, 1u);
            const UINT mipHeight =
                std::max(cubemap.height >> mip, 1u);
            faceByteCount +=
                static_cast<std::size_t>(mipWidth) *
                mipHeight *
                cubemap.bytesPerPixel;
        }

        std::vector<double> weights(texelCount, 0.0);
        double totalWeight = 0.0;
        for (UINT face = 0u; face < c_cubeFaceCount; ++face)
        {
            const std::size_t faceOffset =
                static_cast<std::size_t>(face) * faceByteCount;
            for (UINT y = 0u; y < resolution; ++y)
            {
                for (UINT x = 0u; x < resolution; ++x)
                {
                    const UINT texelIndex =
                        face * faceTexelCount + y * resolution + x;
                    const std::size_t byteOffset =
                        faceOffset +
                        static_cast<std::size_t>(
                            y * resolution + x) *
                        cubemap.bytesPerPixel;
                    const auto readHalf = [&](std::size_t componentOffset)
                    {
                        const std::uint16_t halfValue =
                            static_cast<std::uint16_t>(
                                cubemap.texels[
                                    byteOffset + componentOffset]) |
                            static_cast<std::uint16_t>(
                                cubemap.texels[
                                    byteOffset + componentOffset + 1u]
                                << 8u);
                        return HalfToFloat(halfValue);
                    };
                    const float red = readHalf(0u);
                    const float green = readHalf(2u);
                    const float blue = readHalf(4u);
                    const double luminance = std::max(
                        0.0,
                        static_cast<double>(red) * 0.2126 +
                        static_cast<double>(green) * 0.7152 +
                        static_cast<double>(blue) * 0.0722);
                    const double weight = std::isfinite(luminance)
                        ? luminance * CubemapTexelSolidAngle(
                            x,
                            y,
                            resolution)
                        : 0.0;
                    weights[texelIndex] = weight;
                    totalWeight += weight;
                }
            }
        }

        environmentPower = totalWeight > 0.0
            ? static_cast<float>(totalWeight)
            : 0.0f;
        entries.assign(texelCount, {});
        std::vector<double> scaledProbabilities(texelCount, 1.0);
        std::vector<UINT> smallEntries;
        std::vector<UINT> largeEntries;
        smallEntries.reserve(texelCount);
        largeEntries.reserve(texelCount);

        for (UINT index = 0u; index < texelCount; ++index)
        {
            const double selectionPdf = totalWeight > 0.0
                ? weights[index] / totalWeight
                : 1.0 / static_cast<double>(texelCount);
            entries[index].selectionPdf =
                static_cast<float>(selectionPdf);
            scaledProbabilities[index] =
                selectionPdf * static_cast<double>(texelCount);
            if (scaledProbabilities[index] < 1.0)
            {
                smallEntries.push_back(index);
            }
            else
            {
                largeEntries.push_back(index);
            }
        }

        while (!smallEntries.empty() && !largeEntries.empty())
        {
            const UINT smallIndex = smallEntries.back();
            smallEntries.pop_back();
            const UINT largeIndex = largeEntries.back();
            largeEntries.pop_back();

            entries[smallIndex].acceptProbability =
                static_cast<float>(std::max(
                    0.0,
                    std::min(
                        1.0,
                        scaledProbabilities[smallIndex])));
            entries[smallIndex].aliasIndex = largeIndex;
            scaledProbabilities[largeIndex] =
                scaledProbabilities[largeIndex] +
                scaledProbabilities[smallIndex] -
                1.0;
            if (scaledProbabilities[largeIndex] < 1.0)
            {
                smallEntries.push_back(largeIndex);
            }
            else
            {
                largeEntries.push_back(largeIndex);
            }
        }

        for (UINT remainingIndex : smallEntries)
        {
            entries[remainingIndex].acceptProbability = 1.0f;
            entries[remainingIndex].aliasIndex = remainingIndex;
        }
        for (UINT remainingIndex : largeEntries)
        {
            entries[remainingIndex].acceptProbability = 1.0f;
            entries[remainingIndex].aliasIndex = remainingIndex;
        }
        return true;
    }

    std::vector<GpuEmissiveTriangle> BuildEmissiveTriangles(
        const SceneData& scene,
        UINT staticIndexCount,
        float& areaLightPower)
    {
        std::vector<GpuEmissiveTriangle> lights;
        const UINT primitiveCount = staticIndexCount / 3u;
        double totalWeight = 0.0;
        for (UINT primitiveIndex = 0;
             primitiveIndex < primitiveCount;
             ++primitiveIndex)
        {
            const std::uint32_t materialIndex =
                scene.primitiveMaterialIndices[primitiveIndex];
            const SceneMaterial& material = scene.materials[materialIndex];
            const float luminance =
                material.emission[0] * 0.2126f +
                material.emission[1] * 0.7152f +
                material.emission[2] * 0.0722f;
            if (luminance <= 0.0f)
                continue;

            const std::uint32_t i0 =
                scene.indices[primitiveIndex * 3u + 0u];
            const std::uint32_t i1 =
                scene.indices[primitiveIndex * 3u + 1u];
            const std::uint32_t i2 =
                scene.indices[primitiveIndex * 3u + 2u];
            const SceneVertex& v0 = scene.vertices[i0];
            const SceneVertex& v1 = scene.vertices[i1];
            const SceneVertex& v2 = scene.vertices[i2];

            GpuEmissiveTriangle light = {};
            for (UINT component = 0; component < 3u; ++component)
            {
                light.vertex0[component] = v0.position[component];
                light.edge1[component] =
                    v1.position[component] - v0.position[component];
                light.edge2[component] =
                    v2.position[component] - v0.position[component];
                light.emission[component] = material.emission[component];
            }
            const float crossX =
                light.edge1[1] * light.edge2[2] -
                light.edge1[2] * light.edge2[1];
            const float crossY =
                light.edge1[2] * light.edge2[0] -
                light.edge1[0] * light.edge2[2];
            const float crossZ =
                light.edge1[0] * light.edge2[1] -
                light.edge1[1] * light.edge2[0];
            light.area = 0.5f * std::sqrt(
                crossX * crossX +
                crossY * crossY +
                crossZ * crossZ);
            if (light.area <= 0.0000001f)
                continue;

            light.selectionPdf = light.area * luminance;
            light.primitiveIndex = primitiveIndex;
            totalWeight += static_cast<double>(light.selectionPdf);
            lights.push_back(light);
        }

        areaLightPower = totalWeight > 0.0
            ? static_cast<float>(totalWeight * 3.14159265358979323846)
            : 0.0f;
        if (totalWeight > 0.0)
        {
            double cumulativeProbability = 0.0;
            for (GpuEmissiveTriangle& light : lights)
            {
                light.selectionPdf = static_cast<float>(
                    static_cast<double>(light.selectionPdf) /
                    totalWeight);
                cumulativeProbability += light.selectionPdf;
                light.selectionCdf = static_cast<float>(
                    cumulativeProbability);
            }
            lights.back().selectionCdf = 1.0f;
        }
        return lights;
    }

    UINT AlignUp(UINT value, UINT alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    UINT64 AlignUp64(UINT64 value, UINT64 alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    D3D12_RESOURCE_DESC CreateBufferDesc(
        UINT64 sizeInBytes,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Alignment = 0;
        desc.Width = sizeInBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
        return desc;
    }

    D3D12_HEAP_PROPERTIES CreateHeapProperties(D3D12_HEAP_TYPE heapType)
    {
        D3D12_HEAP_PROPERTIES properties = {};
        properties.Type = heapType;
        properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        properties.CreationNodeMask = 1;
        properties.VisibleNodeMask = 1;
        return properties;
    }

    UINT ReadUint32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
    {
        return static_cast<UINT>(bytes[offset + 0]) |
            (static_cast<UINT>(bytes[offset + 1]) << 8) |
            (static_cast<UINT>(bytes[offset + 2]) << 16) |
            (static_cast<UINT>(bytes[offset + 3]) << 24);
    }

    UINT GetMipDimension(UINT baseDimension, UINT mipLevel)
    {
        const UINT dimension = baseDimension >> mipLevel;
        return dimension > 0 ? dimension : 1;
    }

    bool ParseLegacyRgba16FloatCubemapDds(const std::vector<std::uint8_t>& bytes, DdsCubemapData& cubemap)
    {
        if (bytes.size() < c_ddsHeaderSize || ReadUint32(bytes, 0) != c_ddsMagic)
            return false;

        const UINT height = ReadUint32(bytes, 12);
        const UINT width = ReadUint32(bytes, 16);
        UINT mipCount = ReadUint32(bytes, 28);
        const UINT fourCc = ReadUint32(bytes, 84);
        if (width == 0 || height == 0 || fourCc != c_d3dFormatA16B16G16R16F)
            return false;

        if (mipCount == 0)
            mipCount = 1;

        UINT64 requiredBytes = 0;
        for (UINT face = 0; face < c_cubeFaceCount; ++face)
        {
            for (UINT mip = 0; mip < mipCount; ++mip)
            {
                const UINT mipWidth = GetMipDimension(width, mip);
                const UINT mipHeight = GetMipDimension(height, mip);
                requiredBytes += static_cast<UINT64>(mipWidth) * mipHeight * c_bytesPerRgba16FloatPixel;
            }
        }

        if (bytes.size() < static_cast<std::size_t>(c_ddsHeaderSize + requiredBytes))
            return false;

        cubemap.width = width;
        cubemap.height = height;
        cubemap.mipCount = mipCount;
        cubemap.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cubemap.bytesPerPixel = c_bytesPerRgba16FloatPixel;
        cubemap.texels.assign(
            bytes.begin() + c_ddsHeaderSize,
            bytes.begin() + c_ddsHeaderSize + static_cast<std::ptrdiff_t>(requiredBytes));
        return true;
    }

    struct ScopedFileHandle
    {
        explicit ScopedFileHandle(HANDLE handle) : value(handle) {}
        ~ScopedFileHandle()
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                CloseHandle(value);
            }
        }

        bool IsValid() const { return value != INVALID_HANDLE_VALUE; }

        HANDLE value = INVALID_HANDLE_VALUE;
    };
}

RayTracingManager::~RayTracingManager()
{
    if (m_buildFenceEvent)
    {
        CloseHandle(m_buildFenceEvent);
        m_buildFenceEvent = nullptr;
    }
}

bool RayTracingManager::Initialize(HWND hWnd, ID3D12Device5* device, UINT width, UINT height)
{
    m_hWnd = hWnd;
    m_device = device;
    m_width = width > 0 ? width : 1;
    m_height = height > 0 ? height : 1;

    if (!m_device)
    {
        ReportMessage(L"D3D12 raytracing device is null.");
        return false;
    }

    if (!CreateOutputTexture())
        return false;

    if (!CreateStatisticsResources())
        return false;

    if (!CreateBuildCommandObjects())
        return false;

    if (!CreateEnvironmentMap())
        return false;

    if (!CreateGlobalRootSignature())
        return false;

    if (!CreateAtrousPipeline())
        return false;

    if (!CreateTemporalColorClipPipeline())
        return false;

    if (!CreateSkinningPipeline())
        return false;

    if (!CreateRaytracingPipelineState())
        return false;

    if (!CreateShaderTables())
        return false;

    if (!CreateAccelerationStructures())
        return false;

    return true;
}

void RayTracingManager::DispatchRays(
    ID3D12GraphicsCommandList4* commandList,
    const GpuProfileQueries* profileQueries)
{
    if (!commandList)
        return;

    if (!m_stateObject || !m_rayGenShaderTable || !m_missShaderTable ||
        !m_hitGroupShaderTable || !m_descriptorHeap || !m_topLevelAS || !m_accumulationTexture ||
        !m_environmentMap || !m_environmentDistributionBuffer ||
        !m_sceneMaterialBuffer || !m_primitiveMaterialIndexBuffer ||
        !m_sceneMetadataBuffer || !m_emissiveTriangleBuffer ||
        !m_diffuseIndirectAccumulationTexture ||
        !m_specularIndirectAccumulationTexture ||
        !m_diffuseLuminanceMomentsTexture ||
        !m_specularLuminanceMomentsTexture ||
        !m_previousAccumulationTexture ||
        !m_previousNormalDepthTexture ||
        !m_previousMaterialGuideTexture ||
        !m_metallicGuideTexture ||
        !m_directionalShadowGuideTexture ||
        !m_previousDirectionalShadowGuideTexture ||
        !m_previousDiffuseIndirectAccumulationTexture ||
        !m_previousSpecularIndirectAccumulationTexture ||
        !m_previousDiffuseLuminanceMomentsTexture ||
        !m_previousSpecularLuminanceMomentsTexture ||
        !m_statisticsBuffer ||
        !m_statisticsResetBuffer || !m_statisticsReadbackBuffer)
    {
        WriteEmptyGpuProfile(commandList, profileQueries);
        return;
    }

    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->tlasBegin : 0u);
    const bool tlasUpdated =
        UpdateTopLevelAccelerationStructure(commandList);
    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->tlasEnd : 0u);
    if (!tlasUpdated)
    {
        if (profileQueries)
        {
            const UINT remainingQueryIndices[] =
            {
                profileQueries->pathTraceBegin,
                profileQueries->pathTraceEnd,
                profileQueries->mainPathBegin,
                profileQueries->mainPathEnd,
                profileQueries->disocclusionRepairBegin,
                profileQueries->disocclusionRepairEnd,
                profileQueries->temporalColorClipBegin,
                profileQueries->temporalColorClipEnd,
                profileQueries->atrousDiffuseBegin,
                profileQueries->atrousDiffuseEnd,
                profileQueries->atrousSpecularBegin,
                profileQueries->atrousSpecularEnd
            };
            for (UINT queryIndex : remainingQueryIndices)
                WriteGpuTimestamp(commandList, profileQueries, queryIndex);
        }
        return;
    }

    const bool isBeautyFrame =
        !m_showNormalColor &&
        !((m_sceneType == c_scenePbrGgx ||
           m_sceneType == c_sceneDynamicTransformTest) &&
          m_pbrDebugView != c_pbrDebugBeauty);
    const bool shouldAccumulate =
        m_enableAccumulation &&
        isBeautyFrame;
    const bool useTemporalHistory =
        isBeautyFrame && m_enableTemporalReprojection;
    const bool useAtrousFilter =
        isBeautyFrame && m_enableAtrous;

    const UINT directionalLightFrame =
        static_cast<UINT>(m_frameIndex % c_tlasFrameCount);
    if (!UpdateDirectionalLightBuffer(directionalLightFrame))
    {
        WriteEmptyGpuProfile(commandList, profileQueries);
        return;
    }
    WriteTemporalHistoryDescriptors();
    ID3D12Resource* previousHistoryResources[c_temporalHistorySrvCount] =
    {
        m_previousAccumulationTexture.Get(),
        m_previousNormalDepthTexture.Get(),
        m_previousMaterialGuideTexture.Get(),
        m_previousDirectionalShadowGuideTexture.Get(),
        m_previousDiffuseIndirectAccumulationTexture.Get(),
        m_previousSpecularIndirectAccumulationTexture.Get(),
        m_previousDiffuseLuminanceMomentsTexture.Get(),
        m_previousSpecularLuminanceMomentsTexture.Get()
    };
    D3D12_RESOURCE_BARRIER previousHistoryTransitions
        [c_temporalHistorySrvCount] = {};
    if (useTemporalHistory)
    {
        for (UINT index = 0;
             index < c_temporalHistorySrvCount;
             ++index)
        {
            previousHistoryTransitions[index].Type =
                D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            previousHistoryTransitions[index].Transition.pResource =
                previousHistoryResources[index];
            previousHistoryTransitions[index].Transition.StateBefore =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            previousHistoryTransitions[index].Transition.StateAfter =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            previousHistoryTransitions[index].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        commandList->ResourceBarrier(
            c_temporalHistorySrvCount,
            previousHistoryTransitions);
    }

    if (m_enableStatistics)
    {
        D3D12_RESOURCE_BARRIER statisticsToCopy = {};
        statisticsToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        statisticsToCopy.Transition.pResource = m_statisticsBuffer.Get();
        statisticsToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        statisticsToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        statisticsToCopy.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &statisticsToCopy);
        commandList->CopyBufferRegion(
            m_statisticsBuffer.Get(),
            0,
            m_statisticsResetBuffer.Get(),
            0,
            sizeof(UINT) * c_statisticsCounterCount);
        std::swap(
            statisticsToCopy.Transition.StateBefore,
            statisticsToCopy.Transition.StateAfter);
        commandList->ResourceBarrier(1, &statisticsToCopy);
    }

    ID3D12DescriptorHeap* descriptorHeaps[] = { m_descriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, descriptorHeaps);
    commandList->SetComputeRootSignature(m_globalRootSignature.Get());
    commandList->SetComputeRootDescriptorTable(0, m_descriptorHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootShaderResourceView(1, m_topLevelAS->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(2, m_vertexBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(3, m_indexBuffer->GetGPUVirtualAddress());
    RenderSettingsConstants renderSettings = {};
    renderSettings.showNormalColor = m_showNormalColor ? 1u : 0u;
    renderSettings.frameIndex = m_frameIndex++;
    renderSettings.maxBounce = m_maxBounce;
    renderSettings.sampleIndex = shouldAccumulate
        ? m_accumulatedSampleCount
        : (useTemporalHistory ? m_temporalHistoryFrameCount : 0u);
    renderSettings.enableAccumulation = shouldAccumulate ? 1u : 0u;
    renderSettings.sceneType = m_sceneType;
    renderSettings.pbrDebugView = m_pbrDebugView;
    renderSettings.enableIbl = m_enableIbl ? 1u : 0u;
    renderSettings.pbrMetallic = m_pbrMetallic;
    renderSettings.pbrRoughness = m_pbrRoughness;
    renderSettings.iblIntensity = m_iblIntensity;
    renderSettings.exposure = m_exposure;
    renderSettings.validationSeed = m_validationSeed;
    std::copy(
        m_cameraPosition.begin(),
        m_cameraPosition.end(),
        renderSettings.cameraPosition);
    std::copy(
        m_cameraTarget.begin(),
        m_cameraTarget.end(),
        renderSettings.cameraTarget);
    renderSettings.overridePbrMaterial = m_overridePbrMaterial ? 1u : 0u;
    renderSettings.enableStatistics = m_enableStatistics ? 1u : 0u;
    renderSettings.dynamicObjectMoved =
        m_dynamicObjectMovedThisFrame ? 1u : 0u;
    renderSettings.enableRussianRoulette =
        m_enableRussianRoulette ? 1u : 0u;
    renderSettings.lightingMode = m_lightingMode;
    renderSettings.emissiveTriangleCount = m_emissiveTriangleCount;
    renderSettings.environmentResolution = m_environmentResolution;
    renderSettings.environmentTexelCount = m_environmentTexelCount;
    renderSettings.areaLightPower = m_areaLightPower;
    renderSettings.environmentPower = m_environmentPower;
    renderSettings.enableAtrous = useAtrousFilter ? 1u : 0u;
    renderSettings.samplesPerPixel = m_samplesPerPixel;
    renderSettings.previousInstanceTransformCount =
        static_cast<UINT>(m_previousInstanceTransforms.size());
    std::copy(
        m_previousCameraPosition.begin(),
        m_previousCameraPosition.end(),
        renderSettings.previousCameraPosition);
    renderSettings.textureLodBiasOrDisabled =
        m_enableTextureLod ? m_textureLodBias : -100.0f;
    std::copy(
        m_previousCameraTarget.begin(),
        m_previousCameraTarget.end(),
        renderSettings.previousCameraTarget);
    renderSettings.enableTemporalReprojection =
        useTemporalHistory ? 1u : 0u;
    renderSettings.temporalDebugView = m_temporalDebugView;
    const bool directionalLightActive =
        m_directionalLightAvailable &&
        m_directionalLightEnabled &&
        m_directionalLightIntensityScale > 0.0f;
    renderSettings.enableDynamicObjectReprojection =
        (m_enableDynamicObjectReprojection ? 1u : 0u) |
        (m_useCurrentFrameVisibleResidual ? 2u : 0u) |
        (m_enableSkinnedDeformationMotion ? 4u : 0u) |
        (directionalLightActive ? 8u : 0u) |
        (m_enableDynamicShadowHistoryValidation ? 32u : 0u) |
        (m_enableStaticBackgroundHistoryFastPath ? 64u : 0u) |
        (m_enableBestTapHistoryGather ? 128u : 0u);
    commandList->SetComputeRoot32BitConstants(4, 42, &renderSettings, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE environmentHandle = m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    environmentHandle.ptr += static_cast<SIZE_T>(c_environmentDescriptorIndex) * m_descriptorSize;
    commandList->SetComputeRootDescriptorTable(5, environmentHandle);
    commandList->SetComputeRootShaderResourceView(6, m_sceneMaterialBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(7, m_primitiveMaterialIndexBuffer->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE materialTextureHandle =
        m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    materialTextureHandle.ptr +=
        static_cast<SIZE_T>(c_materialTextureDescriptorIndex) * m_descriptorSize;
    commandList->SetComputeRootDescriptorTable(8, materialTextureHandle);
    commandList->SetComputeRootUnorderedAccessView(
        9,
        m_statisticsBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        10,
        m_sceneMetadataBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        11,
        m_emissiveTriangleBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        12,
        m_environmentDistributionBuffer->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE temporalHistoryHandle =
        m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    temporalHistoryHandle.ptr +=
        static_cast<SIZE_T>(c_temporalPreviousAccumulationSrvIndex) *
        m_descriptorSize;
    commandList->SetComputeRootDescriptorTable(
        13,
        temporalHistoryHandle);
    commandList->SetPipelineState1(m_stateObject.Get());

    D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
    dispatchDesc.RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
    dispatchDesc.RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderRecordSize;
    dispatchDesc.MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
    dispatchDesc.MissShaderTable.SizeInBytes =
        m_missShaderRecordSize * 2u;
    dispatchDesc.MissShaderTable.StrideInBytes = m_missShaderRecordSize;
    dispatchDesc.HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
    dispatchDesc.HitGroupTable.SizeInBytes = m_hitGroupShaderRecordSize;
    dispatchDesc.HitGroupTable.StrideInBytes = m_hitGroupShaderRecordSize;
    dispatchDesc.Width = m_width;
    dispatchDesc.Height = m_height;
    dispatchDesc.Depth = 1;

    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->pathTraceBegin : 0u);
    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->mainPathBegin : 0u);
    commandList->DispatchRays(&dispatchDesc);
    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->mainPathEnd : 0u);
    const bool runDisocclusionRepair =
        m_enableDisocclusionRepair &&
        useTemporalHistory &&
        useAtrousFilter &&
        m_dynamicObjectMovedThisFrame &&
        m_temporalHistoryFrameCount > 0u &&
        m_temporalDebugView == c_temporalDebugNone;
    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->disocclusionRepairBegin : 0u);
    if (runDisocclusionRepair)
    {
        D3D12_RESOURCE_BARRIER raygenUavBarrier = {};
        raygenUavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        raygenUavBarrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &raygenUavBarrier);

        RenderSettingsConstants repairSettings = renderSettings;
        repairSettings.enableDynamicObjectReprojection |=
            16u |
            ((m_disocclusionRepairSamplesPerPixel & 0xFu) << 8u);
        commandList->SetComputeRoot32BitConstants(
            4,
            42,
            &repairSettings,
            0);
        commandList->DispatchRays(&dispatchDesc);
        commandList->ResourceBarrier(1, &raygenUavBarrier);
    }
    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->disocclusionRepairEnd : 0u);
    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->pathTraceEnd : 0u);

    if (m_enableStatistics)
    {
        D3D12_RESOURCE_BARRIER statisticsUavBarrier = {};
        statisticsUavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        statisticsUavBarrier.UAV.pResource = m_statisticsBuffer.Get();
        commandList->ResourceBarrier(1, &statisticsUavBarrier);

        D3D12_RESOURCE_BARRIER statisticsToCopy = {};
        statisticsToCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        statisticsToCopy.Transition.pResource = m_statisticsBuffer.Get();
        statisticsToCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        statisticsToCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        statisticsToCopy.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &statisticsToCopy);
        commandList->CopyBufferRegion(
            m_statisticsReadbackBuffer.Get(),
            0,
            m_statisticsBuffer.Get(),
            0,
            sizeof(UINT) * c_statisticsCounterCount);
        std::swap(
            statisticsToCopy.Transition.StateBefore,
            statisticsToCopy.Transition.StateAfter);
        commandList->ResourceBarrier(1, &statisticsToCopy);
    }

    const bool showRadianceHistoryDifference =
        m_temporalDebugView ==
        c_temporalDebugRadianceHistoryDifference;
    const bool applyDynamicObjectColorClip =
        useAtrousFilter &&
        m_enableTemporalColorClip &&
        m_temporalDebugView == c_temporalDebugNone &&
        m_dynamicObjectMovedThisFrame;
    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->temporalColorClipBegin : 0u);
    if (useTemporalHistory &&
        m_temporalHistoryFrameCount > 0u &&
        (applyDynamicObjectColorClip ||
         showRadianceHistoryDifference))
    {
        DispatchTemporalColorClip(commandList);
    }
    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->temporalColorClipEnd : 0u);

    if (useAtrousFilter &&
        m_temporalDebugView == c_temporalDebugNone)
    {
        DispatchAtrousFilter(commandList, profileQueries);
    }
    else if (profileQueries)
    {
        const UINT atrousQueryIndices[] =
        {
            profileQueries->atrousDiffuseBegin,
            profileQueries->atrousDiffuseEnd,
            profileQueries->atrousSpecularBegin,
            profileQueries->atrousSpecularEnd
        };
        for (UINT queryIndex : atrousQueryIndices)
            WriteGpuTimestamp(commandList, profileQueries, queryIndex);
    }

    if (useTemporalHistory)
    {
        for (UINT index = 0;
             index < c_temporalHistorySrvCount;
             ++index)
        {
            std::swap(
                previousHistoryTransitions[index].Transition.StateBefore,
                previousHistoryTransitions[index].Transition.StateAfter);
        }
        commandList->ResourceBarrier(
            c_temporalHistorySrvCount,
            previousHistoryTransitions);

        std::swap(
            m_accumulationTexture,
            m_previousAccumulationTexture);
        std::swap(
            m_normalDepthTexture,
            m_previousNormalDepthTexture);
        std::swap(
            m_materialGuideTexture,
            m_previousMaterialGuideTexture);
        std::swap(
            m_directionalShadowGuideTexture,
            m_previousDirectionalShadowGuideTexture);
        std::swap(
            m_diffuseIndirectAccumulationTexture,
            m_previousDiffuseIndirectAccumulationTexture);
        std::swap(
            m_specularIndirectAccumulationTexture,
            m_previousSpecularIndirectAccumulationTexture);
        std::swap(
            m_diffuseLuminanceMomentsTexture,
            m_previousDiffuseLuminanceMomentsTexture);
        std::swap(
            m_specularLuminanceMomentsTexture,
            m_previousSpecularLuminanceMomentsTexture);
    }

    m_previousCameraPosition = m_cameraPosition;
    m_previousCameraTarget = m_cameraTarget;

    if (shouldAccumulate)
    {
        m_accumulatedSampleCount += m_samplesPerPixel;
    }
    if (useTemporalHistory)
    {
        ++m_temporalHistoryFrameCount;
    }
}

void RayTracingManager::DispatchTemporalColorClip(
    ID3D12GraphicsCommandList4* commandList)
{
    if (!commandList ||
        !m_temporalColorClipRootSignature ||
        !m_temporalColorClipPipelineState ||
        !m_accumulationTexture ||
        !m_diffuseIndirectAccumulationTexture ||
        !m_specularIndirectAccumulationTexture ||
        !m_diffuseLuminanceMomentsTexture ||
        !m_specularLuminanceMomentsTexture ||
        !m_outputTexture ||
        !m_normalDepthTexture ||
        !m_materialGuideTexture ||
        !m_atrousFilterTextureA ||
        !m_atrousFilterTextureB ||
        !m_atrousFilteredDiffuseTexture)
    {
        return;
    }

    ID3D12Resource* uavResources[6] =
    {
        m_accumulationTexture.Get(),
        m_diffuseIndirectAccumulationTexture.Get(),
        m_specularIndirectAccumulationTexture.Get(),
        m_diffuseLuminanceMomentsTexture.Get(),
        m_specularLuminanceMomentsTexture.Get(),
        m_outputTexture.Get()
    };
    D3D12_RESOURCE_BARRIER uavBarriers[_countof(uavResources)] = {};
    for (UINT index = 0; index < _countof(uavResources); ++index)
    {
        uavBarriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarriers[index].UAV.pResource = uavResources[index];
    }
    commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

    ID3D12Resource* inputResources[5] =
    {
        m_atrousFilteredDiffuseTexture.Get(),
        m_atrousFilterTextureA.Get(),
        m_atrousFilterTextureB.Get(),
        m_normalDepthTexture.Get(),
        m_materialGuideTexture.Get()
    };
    D3D12_RESOURCE_BARRIER inputTransitions[_countof(inputResources)] = {};
    for (UINT index = 0; index < _countof(inputResources); ++index)
    {
        inputTransitions[index].Type =
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        inputTransitions[index].Transition.pResource = inputResources[index];
        inputTransitions[index].Transition.StateBefore =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        inputTransitions[index].Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        inputTransitions[index].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(
        _countof(inputTransitions),
        inputTransitions);

    const auto gpuDescriptorHandle = [&](UINT descriptorIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr +=
            static_cast<UINT64>(descriptorIndex) * m_descriptorSize;
        return handle;
    };

    commandList->SetComputeRootSignature(
        m_temporalColorClipRootSignature.Get());
    commandList->SetPipelineState(m_temporalColorClipPipelineState.Get());
    commandList->SetComputeRootDescriptorTable(
        0,
        gpuDescriptorHandle(c_atrousFilteredDiffuseSrvIndex));
    commandList->SetComputeRootDescriptorTable(
        1,
        gpuDescriptorHandle(c_atrousFilterASrvIndex));
    commandList->SetComputeRootDescriptorTable(
        2,
        gpuDescriptorHandle(c_atrousFilterBSrvIndex));
    commandList->SetComputeRootDescriptorTable(
        3,
        gpuDescriptorHandle(c_atrousNormalDepthSrvIndex));
    commandList->SetComputeRootDescriptorTable(
        4,
        gpuDescriptorHandle(c_atrousMaterialGuideSrvIndex));
    commandList->SetComputeRootDescriptorTable(
        5,
        gpuDescriptorHandle(c_temporalPreviousMaterialGuideSrvIndex));
    commandList->SetComputeRootDescriptorTable(
        6,
        gpuDescriptorHandle(1u));
    commandList->SetComputeRootDescriptorTable(
        7,
        gpuDescriptorHandle(4u));
    commandList->SetComputeRootDescriptorTable(
        8,
        gpuDescriptorHandle(5u));
    commandList->SetComputeRootDescriptorTable(
        9,
        gpuDescriptorHandle(6u));
    commandList->SetComputeRootDescriptorTable(
        10,
        gpuDescriptorHandle(7u));
    commandList->SetComputeRootDescriptorTable(
        11,
        gpuDescriptorHandle(0u));

    TemporalColorClipSettingsConstants settings = {};
    settings.resolution[0] = m_width;
    settings.resolution[1] = m_height;
    // Two standard deviations keeps ordinary Monte Carlo variation while
    // rejecting history colors unsupported by the current local surface.
    settings.clipGamma = 2.0f;
    settings.minNeighborhoodSamples = 3u;
    settings.debugView = m_temporalDebugView;
    settings.useCurrentFrameVisibleResidual =
        m_useCurrentFrameVisibleResidual ? 1u : 0u;
    commandList->SetComputeRoot32BitConstants(
        12,
        6,
        &settings,
        0);
    commandList->Dispatch(
        (m_width + 7u) / 8u,
        (m_height + 7u) / 8u,
        1u);

    commandList->ResourceBarrier(_countof(uavBarriers), uavBarriers);
    for (UINT index = 0; index < _countof(inputTransitions); ++index)
    {
        std::swap(
            inputTransitions[index].Transition.StateBefore,
            inputTransitions[index].Transition.StateAfter);
    }
    commandList->ResourceBarrier(
        _countof(inputTransitions),
        inputTransitions);
}

void RayTracingManager::DispatchAtrousFilter(
    ID3D12GraphicsCommandList4* commandList,
    const GpuProfileQueries* profileQueries)
{
    if (!commandList ||
        !m_atrousRootSignature ||
        !m_atrousPipelineState ||
        !m_accumulationTexture ||
        !m_normalDepthTexture ||
        !m_materialGuideTexture ||
        !m_metallicGuideTexture ||
        !m_diffuseIndirectAccumulationTexture ||
        !m_specularIndirectAccumulationTexture ||
        !m_diffuseLuminanceMomentsTexture ||
        !m_specularLuminanceMomentsTexture ||
        !m_atrousFilterTextureA ||
        !m_atrousFilterTextureB ||
        !m_atrousFilteredDiffuseTexture ||
        !m_outputTexture)
    {
        if (commandList && profileQueries)
        {
            const UINT queryIndices[] =
            {
                profileQueries->atrousDiffuseBegin,
                profileQueries->atrousDiffuseEnd,
                profileQueries->atrousSpecularBegin,
                profileQueries->atrousSpecularEnd
            };
            for (UINT queryIndex : queryIndices)
                WriteGpuTimestamp(commandList, profileQueries, queryIndex);
        }
        return;
    }

    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->atrousDiffuseBegin : 0u);

    D3D12_RESOURCE_BARRIER outputUavBarrier = {};
    outputUavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    outputUavBarrier.UAV.pResource = m_outputTexture.Get();
    commandList->ResourceBarrier(1, &outputUavBarrier);

    D3D12_RESOURCE_BARRIER inputTransitions[8] = {};
    ID3D12Resource* inputResources[8] =
    {
        m_diffuseIndirectAccumulationTexture.Get(),
        m_specularIndirectAccumulationTexture.Get(),
        m_normalDepthTexture.Get(),
        m_materialGuideTexture.Get(),
        m_diffuseLuminanceMomentsTexture.Get(),
        m_specularLuminanceMomentsTexture.Get(),
        m_accumulationTexture.Get(),
        m_metallicGuideTexture.Get()
    };
    for (UINT index = 0; index < _countof(inputTransitions); ++index)
    {
        inputTransitions[index].Type =
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        inputTransitions[index].Transition.pResource =
            inputResources[index];
        inputTransitions[index].Transition.StateBefore =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        inputTransitions[index].Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        inputTransitions[index].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(
        _countof(inputTransitions),
        inputTransitions);

    commandList->SetComputeRootSignature(m_atrousRootSignature.Get());
    commandList->SetPipelineState(m_atrousPipelineState.Get());

    const auto gpuDescriptorHandle = [&](UINT descriptorIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            m_descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr +=
            static_cast<UINT64>(descriptorIndex) * m_descriptorSize;
        return handle;
    };

    const auto dispatchChannel =
        [&](UINT filterChannel,
            UINT accumulationSrvIndex,
            UINT momentsSrvIndex,
            UINT finalUavIndex,
            ID3D12Resource* finalResource,
            UINT iterationCount)
    {
        const UINT dispatchIterationCount =
            m_enableAtrousAdaptiveIterations ? 5u : iterationCount;
        for (UINT passIndex = 0;
             passIndex < dispatchIterationCount;
             ++passIndex)
        {
            const bool firstPass = passIndex == 0u;
            const bool finalPass =
                passIndex + 1u == dispatchIterationCount;
            const bool sourceIsA =
                !firstPass && ((passIndex - 1u) % 2u == 0u);
            const bool destinationIsA = passIndex % 2u == 0u;

            const UINT sourceDescriptorIndex = firstPass
                ? accumulationSrvIndex
                : (sourceIsA
                    ? c_atrousFilterASrvIndex
                    : c_atrousFilterBSrvIndex);
            const UINT destinationDescriptorIndex = finalPass
                ? finalUavIndex
                : (destinationIsA
                    ? c_atrousFilterAUavIndex
                    : c_atrousFilterBUavIndex);
            ID3D12Resource* destinationResource = finalPass
                ? finalResource
                : (destinationIsA
                    ? m_atrousFilterTextureA.Get()
                    : m_atrousFilterTextureB.Get());

            AtrousSettingsConstants settings = {};
            settings.resolution[0] = m_width;
            settings.resolution[1] = m_height;
            settings.stepWidth = 1u << passIndex;
            settings.inputIsAccumulation = firstPass ? 1u : 0u;
            settings.finalPass = finalPass ? 1u : 0u;
            settings.normalExponent = m_atrousNormalExponent;
            settings.depthSigma = m_atrousDepthSigma;
            settings.colorSigma = m_atrousColorSigma;
            settings.exposure = m_exposure;
            settings.demodulateDiffuse =
                filterChannel == c_atrousFilterChannelDiffuse ? 1u : 0u;
            settings.filterChannel = filterChannel;
            settings.specularMaterialWeightMode =
                m_atrousSpecularMaterialWeightMode;
            settings.specularRoughnessWeightMode =
                m_atrousSpecularRoughnessWeightMode;
            settings.kernelMode = m_atrousKernelMode;
            settings.adaptiveEdgeWeights =
                m_enableAtrousAdaptiveEdgeWeights ? 1u : 0u;
            settings.adaptiveDynamicNormalExponent =
                m_atrousAdaptiveDynamicNormalExponent;
            settings.adaptiveDynamicDepthSigma =
                m_atrousAdaptiveDynamicDepthSigma;
            settings.adaptiveLowHistoryNormalExponent =
                m_atrousAdaptiveLowHistoryNormalExponent;
            settings.adaptiveLowHistoryDepthSigma =
                m_atrousAdaptiveLowHistoryDepthSigma;
            settings.adaptiveStableNormalExponent =
                m_atrousAdaptiveStableNormalExponent;
            settings.adaptiveStableDepthSigma =
                m_atrousAdaptiveStableDepthSigma;
            settings.passIndex = passIndex;
            settings.adaptiveIterations =
                m_enableAtrousAdaptiveIterations ? 1u : 0u;
            settings.debugView = m_atrousDebugView;
            settings.logLuminanceEdgeStop =
                m_enableAtrousLogLuminanceEdgeStop ? 1u : 0u;
            settings.logLuminanceSigma = m_atrousLogLuminanceSigma;
            commandList->SetComputeRootDescriptorTable(
                0,
                gpuDescriptorHandle(sourceDescriptorIndex));
            commandList->SetComputeRootDescriptorTable(
                1,
                gpuDescriptorHandle(c_atrousNormalDepthSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                2,
                gpuDescriptorHandle(c_atrousMaterialGuideSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                3,
                gpuDescriptorHandle(momentsSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                4,
                gpuDescriptorHandle(c_atrousDiffuseIndirectSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                5,
                gpuDescriptorHandle(c_atrousSpecularIndirectSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                6,
                gpuDescriptorHandle(c_atrousTotalSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                7,
                gpuDescriptorHandle(
                    filterChannel == c_atrousFilterChannelSpecular
                    ? c_atrousFilteredDiffuseSrvIndex
                    : c_atrousDiffuseIndirectSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                8,
                gpuDescriptorHandle(c_atrousMetallicGuideSrvIndex));
            commandList->SetComputeRootDescriptorTable(
                9,
                gpuDescriptorHandle(destinationDescriptorIndex));
            commandList->SetComputeRoot32BitConstants(
                10,
                26,
                &settings,
                0);
            commandList->Dispatch(
                (m_width + 7u) / 8u,
                (m_height + 7u) / 8u,
                1u);

            D3D12_RESOURCE_BARRIER destinationUavBarrier = {};
            destinationUavBarrier.Type =
                D3D12_RESOURCE_BARRIER_TYPE_UAV;
            destinationUavBarrier.UAV.pResource = destinationResource;
            commandList->ResourceBarrier(1, &destinationUavBarrier);

            if (!finalPass)
            {
                D3D12_RESOURCE_BARRIER destinationToSrv = {};
                destinationToSrv.Type =
                    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                destinationToSrv.Transition.pResource =
                    destinationResource;
                destinationToSrv.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                destinationToSrv.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                destinationToSrv.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &destinationToSrv);
            }

            if (!firstPass)
            {
                ID3D12Resource* sourceResource = sourceIsA
                    ? m_atrousFilterTextureA.Get()
                    : m_atrousFilterTextureB.Get();
                D3D12_RESOURCE_BARRIER sourceToUav = {};
                sourceToUav.Type =
                    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                sourceToUav.Transition.pResource = sourceResource;
                sourceToUav.Transition.StateBefore =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                sourceToUav.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                sourceToUav.Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &sourceToUav);
            }
        }
    };

    dispatchChannel(
        c_atrousFilterChannelDiffuse,
        c_atrousDiffuseIndirectSrvIndex,
        c_atrousDiffuseMomentsSrvIndex,
        c_atrousFilteredDiffuseUavIndex,
        m_atrousFilteredDiffuseTexture.Get(),
        m_atrousIterationCount);

    D3D12_RESOURCE_BARRIER filteredDiffuseToSrv = {};
    filteredDiffuseToSrv.Type =
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    filteredDiffuseToSrv.Transition.pResource =
        m_atrousFilteredDiffuseTexture.Get();
    filteredDiffuseToSrv.Transition.StateBefore =
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    filteredDiffuseToSrv.Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    filteredDiffuseToSrv.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &filteredDiffuseToSrv);

    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->atrousDiffuseEnd : 0u);
    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->atrousSpecularBegin : 0u);

    dispatchChannel(
        c_atrousFilterChannelSpecular,
        c_atrousSpecularIndirectSrvIndex,
        c_atrousSpecularMomentsSrvIndex,
        c_atrousOutputUavIndex,
        m_outputTexture.Get(),
        m_atrousSpecularIterationCount);

    std::swap(
        filteredDiffuseToSrv.Transition.StateBefore,
        filteredDiffuseToSrv.Transition.StateAfter);
    commandList->ResourceBarrier(1, &filteredDiffuseToSrv);

    for (UINT index = 0; index < _countof(inputTransitions); ++index)
    {
        std::swap(
            inputTransitions[index].Transition.StateBefore,
            inputTransitions[index].Transition.StateAfter);
    }
    commandList->ResourceBarrier(
        _countof(inputTransitions),
        inputTransitions);

    WriteGpuTimestamp(
        commandList,
        profileQueries,
        profileQueries ? profileQueries->atrousSpecularEnd : 0u);
}

void RayTracingManager::ResetAccumulation()
{
    m_accumulatedSampleCount = 0;
    m_temporalHistoryFrameCount = 0;
    m_previousCameraPosition = m_cameraPosition;
    m_previousCameraTarget = m_cameraTarget;
}

bool RayTracingManager::Resize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return true;

    m_width = width;
    m_height = height;
    if (m_autoFrameCamera)
        UpdateCameraFromSceneBounds();
    ResetAccumulation();
    return CreateOutputTexture();
}

void RayTracingManager::SetShowNormalColor(bool showNormalColor)
{
    if (m_showNormalColor == showNormalColor)
        return;

    m_showNormalColor = showNormalColor;
    ResetAccumulation();
}

void RayTracingManager::SetMaxBounce(UINT maxBounce)
{
    UINT clampedMaxBounce = maxBounce;
    if (clampedMaxBounce < 1)
    {
        clampedMaxBounce = 1;
    }
    else if (clampedMaxBounce > c_maxBounce)
    {
        clampedMaxBounce = c_maxBounce;
    }

    if (m_maxBounce == clampedMaxBounce)
        return;

    m_maxBounce = clampedMaxBounce;
    ResetAccumulation();
}

void RayTracingManager::SetSamplesPerPixel(UINT samplesPerPixel)
{
    const UINT clampedSamplesPerPixel =
        samplesPerPixel < 1u ? 1u : (samplesPerPixel > 8u ? 8u : samplesPerPixel);
    if (m_samplesPerPixel == clampedSamplesPerPixel)
        return;

    m_samplesPerPixel = clampedSamplesPerPixel;
    ResetAccumulation();
}

void RayTracingManager::SetRussianRouletteEnabled(bool enabled)
{
    if (m_enableRussianRoulette == enabled)
        return;

    m_enableRussianRoulette = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetAtrousEnabled(bool enabled)
{
    if (m_enableAtrous == enabled)
        return;

    m_enableAtrous = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetTemporalReprojectionEnabled(bool enabled)
{
    if (m_enableTemporalReprojection == enabled)
        return;

    m_enableTemporalReprojection = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetBestTapHistoryGatherEnabled(bool enabled)
{
    if (m_enableBestTapHistoryGather == enabled)
        return;

    m_enableBestTapHistoryGather = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetDynamicObjectReprojectionEnabled(bool enabled)
{
    if (m_enableDynamicObjectReprojection == enabled)
        return;

    m_enableDynamicObjectReprojection = enabled;
    ResetAccumulation();
}
void RayTracingManager::SetStaticBackgroundHistoryFastPathEnabled(bool enabled)
{
    if (m_enableStaticBackgroundHistoryFastPath == enabled)
        return;

    m_enableStaticBackgroundHistoryFastPath = enabled;
    ResetAccumulation();
}


void RayTracingManager::SetDisocclusionRepairSettings(
    bool enabled,
    UINT samplesPerPixel)
{
    const UINT clampedSamplesPerPixel =
        samplesPerPixel < 1u
        ? 1u
        : (samplesPerPixel > 8u ? 8u : samplesPerPixel);
    if (m_enableDisocclusionRepair == enabled &&
        m_disocclusionRepairSamplesPerPixel == clampedSamplesPerPixel)
    {
        return;
    }

    m_enableDisocclusionRepair = enabled;
    m_disocclusionRepairSamplesPerPixel = clampedSamplesPerPixel;
    ResetAccumulation();
}

void RayTracingManager::SetDynamicShadowHistoryValidationEnabled(
    bool enabled)
{
    if (m_enableDynamicShadowHistoryValidation == enabled)
        return;

    m_enableDynamicShadowHistoryValidation = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetSkinnedDeformationMotionEnabled(bool enabled)
{
    if (m_enableSkinnedDeformationMotion == enabled)
        return;

    m_enableSkinnedDeformationMotion = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetCurrentFrameVisibleResidualEnabled(bool enabled)
{
    if (m_useCurrentFrameVisibleResidual == enabled)
        return;

    m_useCurrentFrameVisibleResidual = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetTemporalColorClipEnabled(bool enabled)
{
    if (m_enableTemporalColorClip == enabled)
        return;

    m_enableTemporalColorClip = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetTemporalDebugView(UINT debugView)
{
    m_temporalDebugView =
        debugView <= c_temporalDebugRadianceHistoryDifference
        ? debugView
        : c_temporalDebugNone;
}

void RayTracingManager::SetLightingMode(UINT lightingMode)
{
    const UINT clampedLightingMode =
        lightingMode <= c_lightingModeMis
        ? lightingMode
        : c_lightingModeBsdf;
    if (m_lightingMode == clampedLightingMode)
        return;

    m_lightingMode = clampedLightingMode;
    ResetAccumulation();
}

void RayTracingManager::SetDirectionalLightRuntimeSettings(
    bool enabled,
    float intensityScale)
{
    const float clampedIntensity =
        (std::max)(0.0f, (std::min)(intensityScale, 8.0f));
    if (m_directionalLightEnabled == enabled &&
        std::abs(
            m_directionalLightIntensityScale -
            clampedIntensity) <= 1.0e-6f)
    {
        return;
    }

    m_directionalLightEnabled = enabled;
    m_directionalLightIntensityScale = clampedIntensity;
    ResetAccumulation();
}

void RayTracingManager::SetDirectionalLightDirection(
    const std::array<float, 3>& propagationDirection)
{
    float lengthSquared = 0.0f;
    for (float component : propagationDirection)
        lengthSquared += component * component;
    if (lengthSquared <= 1.0e-8f)
        return;

    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    std::array<float, 3> normalizedDirection = {};
    bool changed = false;
    for (std::size_t component = 0;
         component < normalizedDirection.size();
         ++component)
    {
        normalizedDirection[component] =
            propagationDirection[component] * inverseLength;
        changed |= std::abs(
            normalizedDirection[component] -
            m_directionalLightDirection[component]) > 1.0e-6f;
    }
    if (!changed)
        return;

    m_directionalLightDirection = normalizedDirection;
    ResetAccumulation();
}

void RayTracingManager::SetEnableAccumulation(bool enableAccumulation)
{
    if (m_enableAccumulation == enableAccumulation)
        return;

    m_enableAccumulation = enableAccumulation;
    ResetAccumulation();
}

void RayTracingManager::SetPbrDebugView(UINT pbrDebugView)
{
    const UINT clampedPbrDebugView = pbrDebugView <= c_pbrDebugNormal
        ? pbrDebugView
        : c_pbrDebugBeauty;
    if (m_pbrDebugView == clampedPbrDebugView)
        return;

    m_pbrDebugView = clampedPbrDebugView;
    ResetAccumulation();
}

void RayTracingManager::SetPbrMaterial(float metallic, float roughness)
{
    const float clampedMetallic = metallic < 0.0f ? 0.0f : (metallic > 1.0f ? 1.0f : metallic);
    const float clampedRoughness = roughness < 0.03f ? 0.03f : (roughness > 1.0f ? 1.0f : roughness);
    if (m_pbrMetallic == clampedMetallic && m_pbrRoughness == clampedRoughness)
        return;

    m_pbrMetallic = clampedMetallic;
    m_pbrRoughness = clampedRoughness;
    ResetAccumulation();
}

void RayTracingManager::SetPbrMaterialOverride(bool enabled)
{
    if (m_overridePbrMaterial == enabled)
        return;

    m_overridePbrMaterial = enabled;
    ResetAccumulation();
}

void RayTracingManager::SetTextureLodSettings(bool enabled, float bias)
{
    const float clampedBias = bias < -4.0f
        ? -4.0f
        : (bias > 4.0f ? 4.0f : bias);
    if (m_enableTextureLod == enabled &&
        m_textureLodBias == clampedBias)
    {
        return;
    }

    m_enableTextureLod = enabled;
    m_textureLodBias = clampedBias;
    ResetAccumulation();
}

void RayTracingManager::SetIblSettings(bool enableIbl, float intensity)
{
    const float clampedIntensity = intensity < 0.0f ? 0.0f : (intensity > 8.0f ? 8.0f : intensity);
    if (m_enableIbl == enableIbl && m_iblIntensity == clampedIntensity)
        return;

    m_enableIbl = enableIbl;
    m_iblIntensity = clampedIntensity;
    ResetAccumulation();
}

bool RayTracingManager::SetCamera(
    const std::array<float, 3>& position,
    const std::array<float, 3>& target)
{
    float directionLengthSquared = 0.0f;
    bool changed = false;
    for (std::size_t component = 0; component < 3; ++component)
    {
        const float direction = target[component] - position[component];
        directionLengthSquared += direction * direction;
        changed |= std::abs(position[component] - m_cameraPosition[component]) >
            0.000001f;
        changed |= std::abs(target[component] - m_cameraTarget[component]) >
            0.000001f;
    }
    if (directionLengthSquared <= 0.00000001f || !changed)
        return false;

    m_cameraPosition = position;
    m_cameraTarget = target;
    m_autoFrameCamera = false;
    if (!m_enableTemporalReprojection)
        ResetAccumulation();
    return true;
}

void RayTracingManager::SetDynamicSphereAnimationEnabled(bool enabled)
{
    if (m_dynamicSphereAnimationEnabled == enabled)
        return;
    m_dynamicSphereAnimationEnabled = enabled;
}

void RayTracingManager::SetDynamicSphereVisible(bool visible)
{
    if (m_dynamicSphereVisible == visible)
        return;

    m_dynamicSphereVisible = visible;
    m_dynamicSphereVisibilityDirty = m_hasDynamicSphere;
    if (!visible)
    {
        m_dynamicObjectLinearSpeed = 0.0;
        m_dynamicObjectAngularSpeed = 0.0;
    }
    ResetAccumulation();
}

void RayTracingManager::SetDynamicCubeAnimationEnabled(bool enabled)
{
    if (m_dynamicCubeAnimationEnabled == enabled)
        return;
    m_dynamicCubeAnimationEnabled = enabled;
}

void RayTracingManager::SetDynamicCubeVisible(bool visible)
{
    if (m_dynamicCubeVisible == visible)
        return;
    m_dynamicCubeVisible = visible;
    m_dynamicCubeVisibilityDirty = m_hasDynamicCube;
    ResetAccumulation();
}

void RayTracingManager::SetDynamicTestSphereMaterialPreset(UINT preset)
{
    const UINT clampedPreset = preset % 4u;
    if (m_dynamicTestSphereMaterialPreset == clampedPreset)
        return;
    m_dynamicTestSphereMaterialPreset = clampedPreset;
    if (m_device && m_sceneType == c_sceneDynamicTransformTest)
        CreateAccelerationStructures();
    ResetAccumulation();
}

void RayTracingManager::SetDynamicTestCubeMaterialPreset(UINT preset)
{
    const UINT clampedPreset = preset % 4u;
    if (m_dynamicTestCubeMaterialPreset == clampedPreset)
        return;
    m_dynamicTestCubeMaterialPreset = clampedPreset;
    if (m_device && m_sceneType == c_sceneDynamicTransformTest)
        CreateAccelerationStructures();
    ResetAccumulation();
}

void RayTracingManager::SetDynamicSphereDeterministicTimeline(bool enabled)
{
    if (m_dynamicSphereDeterministicTimeline == enabled)
        return;
    m_dynamicSphereDeterministicTimeline = enabled;
}

void RayTracingManager::ResetDynamicSphereTimeline()
{
    m_dynamicSceneFrameIndex = 0;
    m_dynamicObjectLinearSpeed = 0.0;
    m_dynamicObjectAngularSpeed = 0.0;
}

bool RayTracingManager::ConfigureImportedMeshInstances(
    const SceneData& scene)
{
    if (scene.vertices.empty() || scene.nodes.empty() ||
        scene.meshNodeInstances.empty() ||
        scene.vertices.size() >
            static_cast<std::size_t>(std::numeric_limits<UINT>::max()))
        return false;

    m_sceneAnimationNodes = scene.nodes;
    m_sceneSkins = scene.skins;
    m_importedSkinCount = static_cast<UINT>(scene.skins.size());
    for (const SceneSkin& skin : scene.skins)
    {
        if (skin.jointNodeIndices.size() >
            static_cast<std::size_t>(
                std::numeric_limits<UINT>::max() -
                m_importedSkinJointCount))
        {
            return false;
        }
        m_importedSkinJointCount +=
            static_cast<UINT>(skin.jointNodeIndices.size());
        m_sceneSkinJointNodeIndices.insert(
            m_sceneSkinJointNodeIndices.end(),
            skin.jointNodeIndices.begin(),
            skin.jointNodeIndices.end());
    }
    for (const SceneVertexSkinInfluence& influence :
         scene.vertexSkinInfluences)
    {
        float weightSum = 0.0f;
        for (const float weight : influence.jointWeights)
            weightSum += weight;
        if (weightSum > 0.0f)
            ++m_importedSkinnedVertexCount;
    }
    for (const SceneMeshNodeInstance& source : scene.meshNodeInstances)
    {
        if (source.nodeIndex >= scene.nodes.size() ||
            source.primitives.empty())
            return false;

        const std::uint32_t skinIndex =
            scene.nodes[source.nodeIndex].skinIndex;
        const bool skinned = skinIndex != c_invalidSceneSkinIndex;
        if (skinned && skinIndex >= scene.skins.size())
            return false;

        std::uint32_t blasIndex = c_invalidSceneMeshIndex;
        for (std::size_t index = 0;
             !skinned && index < m_importedMeshBlases.size(); ++index)
        {
            if (!m_importedMeshBlases[index].skinned &&
                m_importedMeshBlases[index].meshIndex == source.meshIndex)
            {
                blasIndex = static_cast<std::uint32_t>(index);
                break;
            }
        }

        if (blasIndex == c_invalidSceneMeshIndex)
        {
            ImportedMeshBlas blas;
            blas.meshIndex = source.meshIndex;
            blas.nodeIndex = source.nodeIndex;
            blas.skinned = skinned;
            if (!InvertAffineMatrix(
                    scene.nodes[source.nodeIndex].worldTransform,
                    blas.referenceWorldInverse))
                return false;
            for (const ScenePrimitiveRange& primitive : source.primitives)
            {
                GeometryRange geometry;
                geometry.vertexCount = static_cast<UINT>(scene.vertices.size());
                geometry.indexOffset = primitive.indexOffset;
                geometry.indexCount = primitive.indexCount;
                geometry.primitiveOffset = primitive.primitiveOffset;
                geometry.containsAlphaMask = primitive.containsAlphaMask;
                blas.geometries.push_back(geometry);
            }
            blasIndex = static_cast<std::uint32_t>(
                m_importedMeshBlases.size());
            m_importedMeshBlases.push_back(std::move(blas));
        }

        const ImportedMeshBlas& blas = m_importedMeshBlases[blasIndex];
        if (source.primitives.size() != blas.geometries.size())
            return false;
        for (std::size_t primitiveIndex = 0;
             primitiveIndex < source.primitives.size(); ++primitiveIndex)
        {
            const ScenePrimitiveRange& primitive =
                source.primitives[primitiveIndex];
            const GeometryRange& geometry = blas.geometries[primitiveIndex];
            if (primitive.indexCount != geometry.indexCount ||
                primitive.containsAlphaMask != geometry.containsAlphaMask)
                return false;
        }
        ImportedMeshInstance instance;
        instance.nodeIndex = source.nodeIndex;
        instance.meshBlasIndex = blasIndex;
        instance.skinIndex = skinIndex;
        if (skinned)
        {
            const std::size_t jointCount =
                scene.skins[skinIndex].jointNodeIndices.size();
            if (jointCount >
                static_cast<std::size_t>(
                    std::numeric_limits<UINT>::max() -
                    m_skinJointMatrixCount))
            {
                return false;
            }
            instance.skinJointMatrixOffset = m_skinJointMatrixCount;
            m_skinJointMatrixCount += static_cast<UINT>(jointCount);
            instance.skinVertexRanges = source.primitives;
        }
        for (const ScenePrimitiveRange& primitive : source.primitives)
            instance.primitiveOffsets.push_back(primitive.primitiveOffset);
        m_importedMeshInstances.push_back(std::move(instance));
    }
    m_useImportedMeshInstances = !m_importedMeshInstances.empty();
    if (!m_useImportedMeshInstances)
        return false;
    if (!scene.animations.empty())
    {
        m_sceneAnimationClips = scene.animations;
        m_sceneAnimationClipIndex = 0u;
        m_sceneAnimationClip = m_sceneAnimationClips.front();
        m_sceneAnimationDuration = (std::max)(
            m_sceneAnimationClip.endTime - m_sceneAnimationClip.startTime,
            0.0f);
        m_sceneAnimationName = m_sceneAnimationClip.name.empty()
            ? "clip_0"
            : m_sceneAnimationClip.name;
        for (const SceneAnimation& clip : m_sceneAnimationClips)
        {
            for (const SceneAnimationChannel& channel : clip.channels)
            {
                if (channel.targetPath == SceneAnimationPath::Weights)
                    continue;
                if (channel.targetNodeIndex >= scene.nodes.size() ||
                    scene.nodes[channel.targetNodeIndex].hasMatrix)
                {
                    return false;
                }
                m_hasSceneAnimation = true;
            }
        }

        for (ImportedMeshInstance& instance : m_importedMeshInstances)
        {
            if (instance.skinIndex != c_invalidSceneSkinIndex)
                instance.animated = m_hasSceneAnimation;
            for (const SceneAnimation& clip : m_sceneAnimationClips)
            {
                for (const SceneAnimationChannel& channel : clip.channels)
                {
                    if (channel.targetPath == SceneAnimationPath::Weights)
                        continue;
                    std::uint32_t nodeIndex = instance.nodeIndex;
                    while (nodeIndex != c_invalidSceneNodeIndex)
                    {
                        if (nodeIndex == channel.targetNodeIndex)
                        {
                            instance.animated = true;
                            break;
                        }
                        nodeIndex = scene.nodes[nodeIndex].parentIndex;
                    }
                    if (instance.animated)
                        break;
                }
                if (instance.animated)
                    break;
            }
        }

        std::vector<std::uint8_t> animatedJointNodes(
            scene.nodes.size(), 0u);
        for (const std::uint32_t jointNodeIndex :
             m_sceneSkinJointNodeIndices)
        {
            std::uint32_t nodeIndex = jointNodeIndex;
            while (nodeIndex != c_invalidSceneNodeIndex)
            {
                bool channelTargetsNode = false;
                for (const SceneAnimation& clip : m_sceneAnimationClips)
                {
                    for (const SceneAnimationChannel& channel : clip.channels)
                    {
                        if (channel.targetPath !=
                                SceneAnimationPath::Weights &&
                            channel.targetNodeIndex == nodeIndex)
                        {
                            channelTargetsNode = true;
                            break;
                        }
                    }
                    if (channelTargetsNode)
                        break;
                }
                if (channelTargetsNode)
                {
                    if (!animatedJointNodes[jointNodeIndex])
                    {
                        animatedJointNodes[jointNodeIndex] = 1u;
                        ++m_animatedSkinJointCount;
                    }
                    break;
                }
                nodeIndex = scene.nodes[nodeIndex].parentIndex;
            }
        }
    }

    m_skinJointMatrices.assign(
        m_skinJointMatrixCount,
        IdentityMatrix());
    if (!EvaluateImportedSceneAnimation(0.0))
        return false;
    m_previousSkinJointMatrices = m_skinJointMatrices;
    return true;
}

namespace
{
    bool ApplyUniformScenePlacement(
        SceneData& scene,
        const std::array<float, 3>& translationLeftHanded,
        float uniformScale)
    {
        if (!std::isfinite(uniformScale) || uniformScale <= 0.0f)
            return false;
        for (const float component : translationLeftHanded)
        {
            if (!std::isfinite(component))
                return false;
        }

        for (SceneVertex& vertex : scene.vertices)
        {
            for (std::size_t component = 0; component < 3u; ++component)
            {
                vertex.position[component] =
                    vertex.position[component] * uniformScale +
                    translationLeftHanded[component];
            }
        }

        // Scene nodes retain glTF's right-handed transforms while flattened
        // vertices are already left-handed. Reflect only the placement Z.
        Matrix4 placement = IdentityMatrix();
        placement[0] = uniformScale;
        placement[5] = uniformScale;
        placement[10] = uniformScale;
        placement[12] = translationLeftHanded[0];
        placement[13] = translationLeftHanded[1];
        placement[14] = -translationLeftHanded[2];

        for (SceneNode& node : scene.nodes)
        {
            Matrix4 world = {};
            std::copy_n(node.worldTransform, 16u, world.begin());
            const Matrix4 placedWorld = MultiplyMatrix(placement, world);
            std::copy_n(
                placedWorld.begin(),
                16u,
                node.worldTransform);
        }

        for (const std::uint32_t rootNodeIndex : scene.rootNodeIndices)
        {
            if (rootNodeIndex >= scene.nodes.size())
                return false;
            SceneNode& root = scene.nodes[rootNodeIndex];
            Matrix4 local = {};
            std::copy_n(root.localTransform, 16u, local.begin());
            const Matrix4 placedLocal = MultiplyMatrix(placement, local);
            std::copy_n(
                placedLocal.begin(),
                16u,
                root.localTransform);

            root.translation[0] =
                root.translation[0] * uniformScale +
                translationLeftHanded[0];
            root.translation[1] =
                root.translation[1] * uniformScale +
                translationLeftHanded[1];
            root.translation[2] =
                root.translation[2] * uniformScale -
                translationLeftHanded[2];
            for (float& component : root.scale)
                component *= uniformScale;
        }
        return true;
    }

    bool NextMeshIndex(
        const SceneData& scene,
        std::uint32_t& nextMeshIndex)
    {
        std::uint32_t maximumMeshIndex = 0u;
        bool hasMesh = false;
        for (const SceneNode& node : scene.nodes)
        {
            if (node.meshIndex == c_invalidSceneMeshIndex)
                continue;
            maximumMeshIndex = hasMesh
                ? (std::max)(maximumMeshIndex, node.meshIndex)
                : node.meshIndex;
            hasMesh = true;
        }
        if (!hasMesh)
        {
            nextMeshIndex = 0u;
            return true;
        }
        if (maximumMeshIndex >= c_invalidSceneMeshIndex - 1u)
            return false;
        nextMeshIndex = maximumMeshIndex + 1u;
        return true;
    }

    bool AppendStaticGeometryInstance(
        SceneData& scene,
        const char* name,
        std::uint32_t vertexOffset,
        std::uint32_t vertexCount,
        std::uint32_t indexOffset,
        std::uint32_t indexCount,
        std::uint32_t primitiveOffset)
    {
        if (vertexCount == 0u || indexCount == 0u ||
            indexCount % 3u != 0u)
        {
            return false;
        }
        std::uint32_t meshIndex = 0u;
        if (!NextMeshIndex(scene, meshIndex) ||
            scene.nodes.size() >= c_invalidSceneNodeIndex)
        {
            return false;
        }

        SceneNode node;
        node.name = name ? name : "Generated static geometry";
        node.meshIndex = meshIndex;
        node.activeInScene = true;
        const std::uint32_t nodeIndex =
            static_cast<std::uint32_t>(scene.nodes.size());
        scene.nodes.push_back(std::move(node));
        scene.rootNodeIndices.push_back(nodeIndex);

        SceneMeshNodeInstance instance;
        instance.nodeIndex = nodeIndex;
        instance.meshIndex = meshIndex;
        ScenePrimitiveRange range;
        range.vertexOffset = vertexOffset;
        range.vertexCount = vertexCount;
        range.indexOffset = indexOffset;
        range.indexCount = indexCount;
        range.primitiveOffset = primitiveOffset;
        instance.primitives.push_back(range);
        scene.meshNodeInstances.push_back(std::move(instance));
        return true;
    }

    bool AppendSceneData(SceneData& destination, SceneData&& source)
    {
        if (!destination.IsValid() || !source.IsValid())
            return false;
        const auto fitsOffset = [](std::size_t destinationSize,
                                   std::size_t sourceSize)
        {
            return destinationSize <=
                    static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max()) &&
                sourceSize <=
                    static_cast<std::size_t>(
                        std::numeric_limits<std::uint32_t>::max()) -
                        destinationSize;
        };
        if (!fitsOffset(destination.vertices.size(), source.vertices.size()) ||
            !fitsOffset(destination.indices.size(), source.indices.size()) ||
            !fitsOffset(destination.materials.size(), source.materials.size()) ||
            !fitsOffset(destination.textures.size(), source.textures.size()) ||
            !fitsOffset(destination.nodes.size(), source.nodes.size()) ||
            !fitsOffset(destination.skins.size(), source.skins.size()) ||
            !fitsOffset(
                destination.primitiveMaterialIndices.size(),
                source.primitiveMaterialIndices.size()))
        {
            return false;
        }

        const std::uint32_t vertexOffset =
            static_cast<std::uint32_t>(destination.vertices.size());
        const std::uint32_t indexOffset =
            static_cast<std::uint32_t>(destination.indices.size());
        const std::uint32_t materialOffset =
            static_cast<std::uint32_t>(destination.materials.size());
        const std::uint32_t primitiveOffset = static_cast<std::uint32_t>(
            destination.primitiveMaterialIndices.size());
        const std::uint32_t textureOffset =
            static_cast<std::uint32_t>(destination.textures.size());
        const std::uint32_t nodeOffset =
            static_cast<std::uint32_t>(destination.nodes.size());
        const std::uint32_t skinOffset =
            static_cast<std::uint32_t>(destination.skins.size());
        std::uint32_t meshOffset = 0u;
        if (!NextMeshIndex(destination, meshOffset))
            return false;

        for (std::uint32_t& index : source.indices)
        {
            if (index > std::numeric_limits<std::uint32_t>::max() -
                    vertexOffset)
                return false;
            index += vertexOffset;
        }
        for (std::uint32_t& materialIndex :
             source.primitiveMaterialIndices)
        {
            if (materialIndex >
                std::numeric_limits<std::uint32_t>::max() -
                    materialOffset)
                return false;
            materialIndex += materialOffset;
        }
        for (SceneMaterial& material : source.materials)
        {
            std::uint32_t* textureIndices[] =
            {
                &material.baseColorTextureIndex,
                &material.metallicRoughnessTextureIndex,
                &material.normalTextureIndex
            };
            for (std::uint32_t* textureIndex : textureIndices)
            {
                if (*textureIndex == c_invalidSceneTextureIndex)
                    continue;
                const std::uint32_t localTextureIndex =
                    SceneTextureDescriptorIndex(*textureIndex);
                if (textureOffset >
                    c_sceneTextureIndexMask - localTextureIndex)
                    return false;
                *textureIndex =
                    (*textureIndex & ~c_sceneTextureIndexMask) |
                    (localTextureIndex + textureOffset);
            }
        }
        for (SceneNode& node : source.nodes)
        {
            if (node.parentIndex != c_invalidSceneNodeIndex)
                node.parentIndex += nodeOffset;
            for (std::uint32_t& childIndex : node.childIndices)
                childIndex += nodeOffset;
            if (node.meshIndex != c_invalidSceneMeshIndex)
                node.meshIndex += meshOffset;
            if (node.skinIndex != c_invalidSceneSkinIndex)
                node.skinIndex += skinOffset;
        }
        for (SceneSkin& skin : source.skins)
        {
            if (skin.skeletonRootNodeIndex != c_invalidSceneNodeIndex)
                skin.skeletonRootNodeIndex += nodeOffset;
            for (std::uint32_t& jointNodeIndex : skin.jointNodeIndices)
                jointNodeIndex += nodeOffset;
        }
        for (SceneMeshNodeInstance& instance : source.meshNodeInstances)
        {
            instance.nodeIndex += nodeOffset;
            instance.meshIndex += meshOffset;
            for (ScenePrimitiveRange& range : instance.primitives)
            {
                range.vertexOffset += vertexOffset;
                range.indexOffset += indexOffset;
                range.primitiveOffset += primitiveOffset;
            }
        }
        for (std::uint32_t& rootNodeIndex : source.rootNodeIndices)
            rootNodeIndex += nodeOffset;
        for (SceneAnimation& animation : source.animations)
        {
            for (SceneAnimationChannel& channel : animation.channels)
                channel.targetNodeIndex += nodeOffset;
        }

        if (!source.vertexSkinInfluences.empty() &&
            destination.vertexSkinInfluences.empty())
        {
            destination.vertexSkinInfluences.resize(
                destination.vertices.size());
        }
        if (!destination.vertexSkinInfluences.empty() &&
            source.vertexSkinInfluences.empty())
        {
            source.vertexSkinInfluences.resize(source.vertices.size());
        }

        destination.vertices.insert(
            destination.vertices.end(),
            std::make_move_iterator(source.vertices.begin()),
            std::make_move_iterator(source.vertices.end()));
        destination.indices.insert(
            destination.indices.end(),
            source.indices.begin(),
            source.indices.end());
        destination.materials.insert(
            destination.materials.end(),
            source.materials.begin(),
            source.materials.end());
        destination.primitiveMaterialIndices.insert(
            destination.primitiveMaterialIndices.end(),
            source.primitiveMaterialIndices.begin(),
            source.primitiveMaterialIndices.end());
        destination.textures.insert(
            destination.textures.end(),
            std::make_move_iterator(source.textures.begin()),
            std::make_move_iterator(source.textures.end()));
        destination.nodes.insert(
            destination.nodes.end(),
            std::make_move_iterator(source.nodes.begin()),
            std::make_move_iterator(source.nodes.end()));
        destination.skins.insert(
            destination.skins.end(),
            std::make_move_iterator(source.skins.begin()),
            std::make_move_iterator(source.skins.end()));
        destination.vertexSkinInfluences.insert(
            destination.vertexSkinInfluences.end(),
            source.vertexSkinInfluences.begin(),
            source.vertexSkinInfluences.end());
        destination.meshNodeInstances.insert(
            destination.meshNodeInstances.end(),
            std::make_move_iterator(source.meshNodeInstances.begin()),
            std::make_move_iterator(source.meshNodeInstances.end()));
        destination.rootNodeIndices.insert(
            destination.rootNodeIndices.end(),
            source.rootNodeIndices.begin(),
            source.rootNodeIndices.end());
        destination.animations.insert(
            destination.animations.end(),
            std::make_move_iterator(source.animations.begin()),
            std::make_move_iterator(source.animations.end()));
        return destination.IsValid();
    }
}

bool RayTracingManager::ConfigureSceneAnimation(const SceneData& scene)
{
    m_hasSceneAnimation = false;
    m_sceneAnimationTimeSeconds = 0.0;
    m_sceneAnimationDuration = 0.0f;
    m_sceneAnimationCurrentTime = 0.0f;
    m_sceneAnimationMeshNodeIndex = c_invalidSceneNodeIndex;
    m_sceneAnimationName.clear();
    m_sceneAnimationNodes.clear();
    m_sceneAnimationClips.clear();
    m_sceneAnimationClipIndex = 0u;
    m_sceneAnimationClip = {};
    m_sceneAnimationDefaultWorldInverse = IdentityMatrix();
    m_sceneAnimationInstanceTransform =
        { 1.0f, 0.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f };
    m_useImportedMeshInstances = false;
    m_importedMeshBlases.clear();
    m_importedMeshInstances.clear();
    m_importedSkinCount = 0u;
    m_importedSkinJointCount = 0u;
    m_importedSkinnedVertexCount = 0u;
    m_animatedSkinJointCount = 0u;
    m_skinJointTransformDelta = 0.0f;
    m_sceneSkinJointNodeIndices.clear();
    m_sceneSkins.clear();
    m_skinJointMatrices.clear();
    m_previousSkinJointMatrices.clear();
    m_skinJointMatrixCount = 0u;
    m_gpuSkinningActive = false;
    m_skinningUpdatePending = false;

    if (!scene.meshNodeInstances.empty())
        return ConfigureImportedMeshInstances(scene);

    if (scene.animations.empty() || scene.nodes.empty())
        return true;

    std::uint32_t meshNodeIndex = c_invalidSceneNodeIndex;
    for (std::size_t nodeIndex = 0;
         nodeIndex < scene.nodes.size();
         ++nodeIndex)
    {
        const SceneNode& node = scene.nodes[nodeIndex];
        if (!node.activeInScene ||
            node.meshIndex == c_invalidSceneMeshIndex)
        {
            continue;
        }
        if (meshNodeIndex != c_invalidSceneNodeIndex)
        {
            // The current renderer owns one BLAS instance for the complete
            // imported model. Independently animated mesh nodes require the
            // upcoming per-node BLAS/instance stage.
            return true;
        }
        meshNodeIndex = static_cast<std::uint32_t>(nodeIndex);
    }
    if (meshNodeIndex == c_invalidSceneNodeIndex)
        return true;

    const SceneAnimation& clip = scene.animations.front();
    bool affectsMeshTransform = false;
    for (const SceneAnimationChannel& channel : clip.channels)
    {
        if (channel.targetPath == SceneAnimationPath::Weights)
            continue;
        if (scene.nodes[channel.targetNodeIndex].hasMatrix)
        {
            // glTF animation targets are TRS properties. Matrix nodes need
            // decomposition before they can be animated safely.
            return true;
        }

        std::uint32_t currentNode = meshNodeIndex;
        while (currentNode != c_invalidSceneNodeIndex)
        {
            if (currentNode == channel.targetNodeIndex)
            {
                affectsMeshTransform = true;
                break;
            }
            currentNode = scene.nodes[currentNode].parentIndex;
        }
    }
    if (!affectsMeshTransform)
        return true;

    Matrix4 defaultWorldInverse;
    if (!InvertAffineMatrix(
        scene.nodes[meshNodeIndex].worldTransform,
        defaultWorldInverse))
    {
        return false;
    }

    m_sceneAnimationNodes = scene.nodes;
    m_sceneAnimationClips = scene.animations;
    m_sceneAnimationClipIndex = 0u;
    m_sceneAnimationClip = clip;
    m_sceneAnimationMeshNodeIndex = meshNodeIndex;
    m_sceneAnimationDefaultWorldInverse = defaultWorldInverse;
    m_sceneAnimationDuration =
        (std::max)(clip.endTime - clip.startTime, 0.0f);
    m_sceneAnimationName = clip.name.empty()
        ? "Animation 0"
        : clip.name;
    m_hasSceneAnimation = true;
    return EvaluateSceneAnimation(0.0);
}

bool RayTracingManager::EvaluateImportedSceneAnimation(double elapsedSeconds)
{
    if (!m_useImportedMeshInstances || m_sceneAnimationNodes.empty())
        return true;

    float sampleTime = 0.0f;
    if (m_hasSceneAnimation)
    {
        sampleTime = m_sceneAnimationClip.startTime;
        if (m_sceneAnimationDuration > 1.0e-6f)
        {
            double loopTime = std::fmod(
                elapsedSeconds,
                static_cast<double>(m_sceneAnimationDuration));
            if (loopTime < 0.0)
                loopTime += m_sceneAnimationDuration;
            sampleTime += static_cast<float>(loopTime);
        }
        m_sceneAnimationCurrentTime =
            sampleTime - m_sceneAnimationClip.startTime;
    }

    std::vector<NodeAnimationPose> poses(m_sceneAnimationNodes.size());
    std::vector<Matrix4> localTransforms(
        m_sceneAnimationNodes.size(), IdentityMatrix());
    for (std::size_t nodeIndex = 0;
         nodeIndex < m_sceneAnimationNodes.size(); ++nodeIndex)
    {
        const SceneNode& node = m_sceneAnimationNodes[nodeIndex];
        std::copy_n(node.translation, 3, poses[nodeIndex].translation);
        std::copy_n(node.rotation, 4, poses[nodeIndex].rotation);
        std::copy_n(node.scale, 3, poses[nodeIndex].scale);
    }

    if (m_hasSceneAnimation)
    {
        for (const SceneAnimationChannel& channel :
             m_sceneAnimationClip.channels)
        {
            if (channel.targetPath == SceneAnimationPath::Weights)
                continue;
            if (channel.targetNodeIndex >= poses.size() ||
                channel.samplerIndex >= m_sceneAnimationClip.samplers.size())
                return false;
            float value[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            if (!SampleAnimationSampler(
                    m_sceneAnimationClip.samplers[channel.samplerIndex],
                    channel.targetPath, sampleTime, value))
                return false;
            NodeAnimationPose& pose = poses[channel.targetNodeIndex];
            pose.animatedTrs = true;
            if (channel.targetPath == SceneAnimationPath::Translation)
                std::copy_n(value, 3, pose.translation);
            else if (channel.targetPath == SceneAnimationPath::Rotation)
                std::copy_n(value, 4, pose.rotation);
            else if (channel.targetPath == SceneAnimationPath::Scale)
                std::copy_n(value, 3, pose.scale);
        }
    }

    for (std::size_t nodeIndex = 0;
         nodeIndex < m_sceneAnimationNodes.size(); ++nodeIndex)
    {
        const SceneNode& node = m_sceneAnimationNodes[nodeIndex];
        if (node.hasMatrix && !poses[nodeIndex].animatedTrs)
            std::copy_n(node.localTransform, 16,
                localTransforms[nodeIndex].begin());
        else
            localTransforms[nodeIndex] = ComposeTrsMatrix(poses[nodeIndex]);
    }

    std::vector<Matrix4> worldTransforms(
        m_sceneAnimationNodes.size(), IdentityMatrix());
    std::vector<std::uint8_t> visitState(
        m_sceneAnimationNodes.size(), 0u);
    std::function<bool(std::uint32_t)> evaluateWorld =
        [&](std::uint32_t nodeIndex) -> bool
    {
        if (nodeIndex >= m_sceneAnimationNodes.size())
            return false;
        if (visitState[nodeIndex] == 2u)
            return true;
        if (visitState[nodeIndex] == 1u)
            return false;
        visitState[nodeIndex] = 1u;
        const std::uint32_t parent =
            m_sceneAnimationNodes[nodeIndex].parentIndex;
        if (parent == c_invalidSceneNodeIndex)
            worldTransforms[nodeIndex] = localTransforms[nodeIndex];
        else
        {
            if (!evaluateWorld(parent))
                return false;
            worldTransforms[nodeIndex] = MultiplyMatrix(
                worldTransforms[parent], localTransforms[nodeIndex]);
        }
        visitState[nodeIndex] = 2u;
        return true;
    };

    for (ImportedMeshInstance& instance : m_importedMeshInstances)
    {
        if (instance.meshBlasIndex >= m_importedMeshBlases.size() ||
            !evaluateWorld(instance.nodeIndex))
            return false;

        const ImportedMeshBlas& blas =
            m_importedMeshBlases[instance.meshBlasIndex];
        if (instance.skinIndex != c_invalidSceneSkinIndex)
        {
            if (!blas.skinned ||
                instance.skinIndex >= m_sceneSkins.size())
                return false;
            const SceneSkin& skin = m_sceneSkins[instance.skinIndex];
            if (skin.jointNodeIndices.size() !=
                    skin.inverseBindMatrices.size() ||
                instance.skinJointMatrixOffset +
                    skin.jointNodeIndices.size() >
                    m_skinJointMatrices.size())
            {
                return false;
            }

            for (std::size_t jointIndex = 0;
                 jointIndex < skin.jointNodeIndices.size();
                 ++jointIndex)
            {
                const std::uint32_t jointNodeIndex =
                    skin.jointNodeIndices[jointIndex];
                if (!evaluateWorld(jointNodeIndex))
                    return false;
                const Matrix4 worldFromBindMesh =
                    MultiplyMatrix(
                        MultiplyMatrix(
                            worldTransforms[jointNodeIndex],
                            skin.inverseBindMatrices[jointIndex]),
                        blas.referenceWorldInverse);
                const Matrix4 leftHanded =
                    ConvertRightHandedDeltaToLeftHanded(
                        worldFromBindMesh);
                for (const float value : leftHanded)
                {
                    if (!std::isfinite(value))
                        return false;
                }
                m_skinJointMatrices[
                    instance.skinJointMatrixOffset + jointIndex] =
                    leftHanded;
            }
            WriteInstanceTransform(
                IdentityMatrix(),
                instance.transform);
        }
        else
        {
            const Matrix4 delta = MultiplyMatrix(
                worldTransforms[instance.nodeIndex],
                blas.referenceWorldInverse);
            const Matrix4 leftHanded =
                ConvertRightHandedDeltaToLeftHanded(delta);
            for (const float value : leftHanded)
            {
                if (!std::isfinite(value))
                    return false;
            }
            WriteInstanceTransform(leftHanded, instance.transform);
        }
    }

    m_skinJointTransformDelta = 0.0f;
    for (const std::uint32_t jointNodeIndex :
         m_sceneSkinJointNodeIndices)
    {
        if (!evaluateWorld(jointNodeIndex))
            return false;
        for (std::size_t component = 0; component < 16u; ++component)
        {
            m_skinJointTransformDelta = (std::max)(
                m_skinJointTransformDelta,
                std::fabs(
                    worldTransforms[jointNodeIndex][component] -
                    m_sceneAnimationNodes[jointNodeIndex].
                        worldTransform[component]));
        }
    }
    m_skinningUpdatePending = m_skinJointMatrixCount > 0u;
    return true;
}

bool RayTracingManager::EvaluateSceneAnimation(double elapsedSeconds)
{
    if (m_useImportedMeshInstances)
        return EvaluateImportedSceneAnimation(elapsedSeconds);
    if (!m_hasSceneAnimation ||
        m_sceneAnimationMeshNodeIndex >=
            m_sceneAnimationNodes.size())
    {
        return true;
    }

    float sampleTime = m_sceneAnimationClip.startTime;
    if (m_sceneAnimationDuration > 1.0e-6f)
    {
        double loopTime = std::fmod(
            elapsedSeconds,
            static_cast<double>(m_sceneAnimationDuration));
        if (loopTime < 0.0)
            loopTime += m_sceneAnimationDuration;
        sampleTime += static_cast<float>(loopTime);
    }
    m_sceneAnimationCurrentTime =
        sampleTime - m_sceneAnimationClip.startTime;

    std::vector<NodeAnimationPose> poses(
        m_sceneAnimationNodes.size());
    std::vector<Matrix4> localTransforms(
        m_sceneAnimationNodes.size(),
        IdentityMatrix());
    for (std::size_t nodeIndex = 0;
         nodeIndex < m_sceneAnimationNodes.size();
         ++nodeIndex)
    {
        const SceneNode& node = m_sceneAnimationNodes[nodeIndex];
        NodeAnimationPose& pose = poses[nodeIndex];
        std::copy_n(node.translation, 3, pose.translation);
        std::copy_n(node.rotation, 4, pose.rotation);
        std::copy_n(node.scale, 3, pose.scale);
    }

    for (const SceneAnimationChannel& channel :
         m_sceneAnimationClip.channels)
    {
        if (channel.targetPath == SceneAnimationPath::Weights)
            continue;
        if (channel.targetNodeIndex >= poses.size() ||
            channel.samplerIndex >=
                m_sceneAnimationClip.samplers.size())
        {
            return false;
        }

        float sampledValue[4] =
            { 0.0f, 0.0f, 0.0f, 1.0f };
        if (!SampleAnimationSampler(
            m_sceneAnimationClip.samplers[channel.samplerIndex],
            channel.targetPath,
            sampleTime,
            sampledValue))
        {
            return false;
        }
        NodeAnimationPose& pose = poses[channel.targetNodeIndex];
        pose.animatedTrs = true;
        switch (channel.targetPath)
        {
        case SceneAnimationPath::Translation:
            std::copy_n(sampledValue, 3, pose.translation);
            break;
        case SceneAnimationPath::Rotation:
            std::copy_n(sampledValue, 4, pose.rotation);
            break;
        case SceneAnimationPath::Scale:
            std::copy_n(sampledValue, 3, pose.scale);
            break;
        case SceneAnimationPath::Weights:
            break;
        }
    }

    for (std::size_t nodeIndex = 0;
         nodeIndex < m_sceneAnimationNodes.size();
         ++nodeIndex)
    {
        const SceneNode& node = m_sceneAnimationNodes[nodeIndex];
        if (node.hasMatrix && !poses[nodeIndex].animatedTrs)
        {
            std::copy_n(
                node.localTransform,
                16,
                localTransforms[nodeIndex].begin());
        }
        else
        {
            localTransforms[nodeIndex] =
                ComposeTrsMatrix(poses[nodeIndex]);
        }
    }

    std::vector<Matrix4> worldTransforms(
        m_sceneAnimationNodes.size(),
        IdentityMatrix());
    std::vector<std::uint8_t> visitState(
        m_sceneAnimationNodes.size(),
        0u);
    std::function<bool(std::uint32_t)> evaluateWorld =
        [&](std::uint32_t nodeIndex) -> bool
    {
        if (nodeIndex >= m_sceneAnimationNodes.size())
            return false;
        if (visitState[nodeIndex] == 2u)
            return true;
        if (visitState[nodeIndex] == 1u)
            return false;
        visitState[nodeIndex] = 1u;
        const std::uint32_t parentIndex =
            m_sceneAnimationNodes[nodeIndex].parentIndex;
        if (parentIndex == c_invalidSceneNodeIndex)
        {
            worldTransforms[nodeIndex] =
                localTransforms[nodeIndex];
        }
        else
        {
            if (!evaluateWorld(parentIndex))
                return false;
            worldTransforms[nodeIndex] = MultiplyMatrix(
                worldTransforms[parentIndex],
                localTransforms[nodeIndex]);
        }
        visitState[nodeIndex] = 2u;
        return true;
    };

    if (!evaluateWorld(m_sceneAnimationMeshNodeIndex))
        return false;
    const Matrix4 rightHandedDelta = MultiplyMatrix(
        worldTransforms[m_sceneAnimationMeshNodeIndex],
        m_sceneAnimationDefaultWorldInverse);
    const Matrix4 leftHandedDelta =
        ConvertRightHandedDeltaToLeftHanded(rightHandedDelta);
    for (const float value : leftHandedDelta)
    {
        if (!std::isfinite(value))
            return false;
    }
    WriteInstanceTransform(
        leftHandedDelta,
        m_sceneAnimationInstanceTransform);
    return true;
}

void RayTracingManager::ResetSceneAnimation()
{
    if (!m_hasSceneAnimation)
        return;
    m_sceneAnimationTimeSeconds = 0.0;
    EvaluateSceneAnimation(0.0);
    m_previousSkinJointMatrices = m_skinJointMatrices;
    ResetAccumulation();
}

std::string RayTracingManager::GetSceneAnimationClipName(
    UINT clipIndex) const
{
    if (clipIndex >= m_sceneAnimationClips.size())
        return {};
    const SceneAnimation& clip = m_sceneAnimationClips[clipIndex];
    return clip.name.empty()
        ? "Clip " + std::to_string(clipIndex)
        : clip.name;
}

bool RayTracingManager::SetSceneAnimationClip(UINT clipIndex)
{
    if (clipIndex >= m_sceneAnimationClips.size())
        return false;
    if (clipIndex == m_sceneAnimationClipIndex)
        return true;

    m_sceneAnimationClipIndex = clipIndex;
    m_sceneAnimationClip = m_sceneAnimationClips[clipIndex];
    m_sceneAnimationName = GetSceneAnimationClipName(clipIndex);
    m_sceneAnimationDuration = (std::max)(
        m_sceneAnimationClip.endTime - m_sceneAnimationClip.startTime,
        0.0f);
    m_sceneAnimationCurrentTime = 0.0f;
    m_sceneAnimationTimeSeconds = 0.0;
    if (!EvaluateSceneAnimation(0.0))
        return false;
    m_previousSkinJointMatrices = m_skinJointMatrices;
    ResetAccumulation();
    return true;
}

void RayTracingManager::SetSceneAnimationEnabled(bool enabled)
{
    if (m_sceneAnimationEnabled == enabled)
        return;
    m_sceneAnimationEnabled = enabled;
    if (!enabled && !m_skinJointMatrices.empty())
    {
        m_previousSkinJointMatrices = m_skinJointMatrices;
        m_skinningUpdatePending = true;
    }
}

float RayTracingManager::GetSceneDiagonal() const
{
    float diagonalSquared = 0.0f;
    for (std::size_t component = 0; component < 3; ++component)
    {
        const float extent =
            m_sceneBoundsMax[component] - m_sceneBoundsMin[component];
        diagonalSquared += extent * extent;
    }
    return diagonalSquared > 0.00000001f
        ? std::sqrt(diagonalSquared)
        : 1.0f;
}

void RayTracingManager::SetExposure(float exposure)
{
    const float clampedExposure = exposure < -10.0f ? -10.0f : (exposure > 10.0f ? 10.0f : exposure);
    if (m_exposure == clampedExposure)
        return;

    m_exposure = clampedExposure;
}
void RayTracingManager::SetSceneType(UINT sceneType)
{
    const UINT clampedSceneType = sceneType <= c_sceneDynamicTransformTest
        ? sceneType
        : c_sceneCornellBox;
    if (m_sceneType == clampedSceneType)
        return;

    m_sceneType = clampedSceneType;
    ResetAccumulation();
    if (m_device)
        CreateAccelerationStructures();
}

bool RayTracingManager::ReloadPbrScene(
    const std::wstring& sceneFilePath,
    bool composeModelRoom,
    bool sponzaLite,
    const std::wstring& overlaySceneFilePath)
{
    m_sceneType = c_scenePbrGgx;
    m_sceneFilePath = sceneFilePath;
    m_overlaySceneFilePath = overlaySceneFilePath;
    m_composeModelRoom = composeModelRoom;
    m_sponzaLite = sponzaLite;
    ResetAccumulation();
    return !m_device || CreateAccelerationStructures();
}

bool RayTracingManager::CreateOutputTexture()
{
    bool createdDescriptorHeap = false;
    if (!m_descriptorHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = c_descriptorCount;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        HRESULT hr = m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descriptorHeap));
        if (ReportFailure(hr, L"Raytracing descriptor heap creation failed."))
            return false;

        m_descriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        createdDescriptorHeap = true;
    }

    m_outputTexture.Reset();
    m_accumulationTexture.Reset();
    m_previousAccumulationTexture.Reset();
    m_normalDepthTexture.Reset();
    m_previousNormalDepthTexture.Reset();
    m_materialGuideTexture.Reset();
    m_previousMaterialGuideTexture.Reset();
    m_metallicGuideTexture.Reset();
    m_directionalShadowGuideTexture.Reset();
    m_previousDirectionalShadowGuideTexture.Reset();
    m_diffuseIndirectAccumulationTexture.Reset();
    m_previousDiffuseIndirectAccumulationTexture.Reset();
    m_specularIndirectAccumulationTexture.Reset();
    m_previousSpecularIndirectAccumulationTexture.Reset();
    m_diffuseLuminanceMomentsTexture.Reset();
    m_previousDiffuseLuminanceMomentsTexture.Reset();
    m_specularLuminanceMomentsTexture.Reset();
    m_previousSpecularLuminanceMomentsTexture.Reset();
    m_atrousFilterTextureA.Reset();
    m_atrousFilterTextureB.Reset();
    m_atrousFilteredDiffuseTexture.Reset();

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Alignment = 0;
    textureDesc.Width = m_width;
    textureDesc.Height = m_height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = c_outputFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_outputTexture));
    if (ReportFailure(hr, L"Raytracing output texture creation failed."))
        return false;

    m_outputTexture->SetName(L"Raytracing output texture");

    D3D12_RESOURCE_DESC accumulationDesc = textureDesc;
    accumulationDesc.Format = c_accumulationFormat;

    hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &accumulationDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_accumulationTexture));
    if (ReportFailure(hr, L"Raytracing accumulation texture creation failed."))
        return false;

    m_accumulationTexture->SetName(L"Raytracing accumulation texture");

    const auto createFloatTexture =
        [&](const D3D12_RESOURCE_DESC& resourceDesc,
            Microsoft::WRL::ComPtr<ID3D12Resource>& resource,
            const wchar_t* debugName,
            const wchar_t* failureMessage) -> bool
    {
        HRESULT createResult = m_device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(&resource));
        if (ReportFailure(createResult, failureMessage))
            return false;
        resource->SetName(debugName);
        return true;
    };

    if (!createFloatTexture(
        accumulationDesc,
        m_previousAccumulationTexture,
        L"Raytracing previous accumulation texture",
        L"Raytracing previous accumulation texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_normalDepthTexture,
        L"Raytracing primary normal and depth",
        L"Raytracing normal/depth texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_previousNormalDepthTexture,
        L"Raytracing previous primary normal and depth",
        L"Raytracing previous normal/depth texture creation failed."))
    {
        return false;
    }
    D3D12_RESOURCE_DESC materialGuideDesc = accumulationDesc;
    materialGuideDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!createFloatTexture(
        materialGuideDesc,
        m_materialGuideTexture,
        L"Raytracing primary material guide",
        L"Raytracing material guide texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        materialGuideDesc,
        m_previousMaterialGuideTexture,
        L"Raytracing previous primary material guide",
        L"Raytracing previous material guide texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_diffuseIndirectAccumulationTexture,
        L"Raytracing diffuse indirect accumulation texture",
        L"Raytracing diffuse indirect accumulation texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_previousDiffuseIndirectAccumulationTexture,
        L"Raytracing previous diffuse indirect accumulation texture",
        L"Raytracing previous diffuse accumulation creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_specularIndirectAccumulationTexture,
        L"Raytracing specular indirect accumulation texture",
        L"Raytracing specular indirect accumulation texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_previousSpecularIndirectAccumulationTexture,
        L"Raytracing previous specular indirect accumulation texture",
        L"Raytracing previous specular accumulation creation failed."))
    {
        return false;
    }
    D3D12_RESOURCE_DESC momentsDesc = accumulationDesc;
    momentsDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    if (!createFloatTexture(
        momentsDesc,
        m_diffuseLuminanceMomentsTexture,
        L"Raytracing diffuse luminance moments",
        L"Raytracing diffuse moments texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        momentsDesc,
        m_previousDiffuseLuminanceMomentsTexture,
        L"Raytracing previous diffuse luminance moments",
        L"Raytracing previous diffuse moments creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        momentsDesc,
        m_specularLuminanceMomentsTexture,
        L"Raytracing specular luminance moments",
        L"Raytracing specular moments texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        momentsDesc,
        m_previousSpecularLuminanceMomentsTexture,
        L"Raytracing previous specular luminance moments",
        L"Raytracing previous specular moments creation failed."))
    {
        return false;
    }
    D3D12_RESOURCE_DESC metallicGuideDesc = accumulationDesc;
    metallicGuideDesc.Format = DXGI_FORMAT_R16_FLOAT;
    if (!createFloatTexture(
        metallicGuideDesc,
        m_metallicGuideTexture,
        L"Raytracing primary metallic guide",
        L"Raytracing metallic guide texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        metallicGuideDesc,
        m_directionalShadowGuideTexture,
        L"Raytracing directional shadow guide",
        L"Raytracing directional shadow guide texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        metallicGuideDesc,
        m_previousDirectionalShadowGuideTexture,
        L"Raytracing previous directional shadow guide",
        L"Raytracing previous directional shadow guide texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_atrousFilterTextureA,
        L"A-Trous filter ping texture",
        L"A-Trous filter ping texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_atrousFilterTextureB,
        L"A-Trous filter pong texture",
        L"A-Trous filter pong texture creation failed."))
    {
        return false;
    }
    if (!createFloatTexture(
        accumulationDesc,
        m_atrousFilteredDiffuseTexture,
        L"A-Trous filtered diffuse texture",
        L"A-Trous filtered diffuse texture creation failed."))
    {
        return false;
    }

    const auto descriptorHandle = [&](UINT descriptorIndex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr +=
            static_cast<SIZE_T>(descriptorIndex) * m_descriptorSize;
        return handle;
    };

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc = {};
    outputUavDesc.Format = c_outputFormat;
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = descriptorHandle(0);
    m_device->CreateUnorderedAccessView(
        m_outputTexture.Get(),
        nullptr,
        &outputUavDesc,
        uavHandle);

    D3D12_UNORDERED_ACCESS_VIEW_DESC accumulationUavDesc = {};
    accumulationUavDesc.Format = c_accumulationFormat;
    accumulationUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

    uavHandle.ptr += m_descriptorSize;
    m_device->CreateUnorderedAccessView(
        m_accumulationTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        uavHandle);

    m_device->CreateUnorderedAccessView(
        m_normalDepthTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(2));

    D3D12_UNORDERED_ACCESS_VIEW_DESC materialGuideUavDesc = {};
    materialGuideUavDesc.Format = materialGuideDesc.Format;
    materialGuideUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(
        m_materialGuideTexture.Get(),
        nullptr,
        &materialGuideUavDesc,
        descriptorHandle(3));
    D3D12_UNORDERED_ACCESS_VIEW_DESC metallicGuideUavDesc = {};
    metallicGuideUavDesc.Format = metallicGuideDesc.Format;
    metallicGuideUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(
        m_metallicGuideTexture.Get(),
        nullptr,
        &metallicGuideUavDesc,
        descriptorHandle(c_metallicGuideUavIndex));
    m_device->CreateUnorderedAccessView(
        m_directionalShadowGuideTexture.Get(),
        nullptr,
        &metallicGuideUavDesc,
        descriptorHandle(c_directionalShadowGuideUavIndex));
    m_device->CreateUnorderedAccessView(
        m_diffuseIndirectAccumulationTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(4));
    m_device->CreateUnorderedAccessView(
        m_specularIndirectAccumulationTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(5));
    D3D12_UNORDERED_ACCESS_VIEW_DESC momentsUavDesc = {};
    momentsUavDesc.Format = momentsDesc.Format;
    momentsUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_device->CreateUnorderedAccessView(
        m_diffuseLuminanceMomentsTexture.Get(),
        nullptr,
        &momentsUavDesc,
        descriptorHandle(6));
    m_device->CreateUnorderedAccessView(
        m_specularLuminanceMomentsTexture.Get(),
        nullptr,
        &momentsUavDesc,
        descriptorHandle(7));

    D3D12_SHADER_RESOURCE_VIEW_DESC floatTextureSrvDesc = {};
    floatTextureSrvDesc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    floatTextureSrvDesc.Format = c_accumulationFormat;
    floatTextureSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    floatTextureSrvDesc.Texture2D.MostDetailedMip = 0;
    floatTextureSrvDesc.Texture2D.MipLevels = 1;
    floatTextureSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    m_device->CreateShaderResourceView(
        m_diffuseIndirectAccumulationTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousDiffuseIndirectSrvIndex));
    m_device->CreateShaderResourceView(
        m_specularIndirectAccumulationTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousSpecularIndirectSrvIndex));
    m_device->CreateShaderResourceView(
        m_normalDepthTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousNormalDepthSrvIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC materialGuideSrvDesc =
        floatTextureSrvDesc;
    materialGuideSrvDesc.Format = materialGuideDesc.Format;
    m_device->CreateShaderResourceView(
        m_materialGuideTexture.Get(),
        &materialGuideSrvDesc,
        descriptorHandle(c_atrousMaterialGuideSrvIndex));
    D3D12_SHADER_RESOURCE_VIEW_DESC metallicGuideSrvDesc =
        floatTextureSrvDesc;
    metallicGuideSrvDesc.Format = metallicGuideDesc.Format;
    m_device->CreateShaderResourceView(
        m_metallicGuideTexture.Get(),
        &metallicGuideSrvDesc,
        descriptorHandle(c_atrousMetallicGuideSrvIndex));
    D3D12_SHADER_RESOURCE_VIEW_DESC momentsSrvDesc =
        floatTextureSrvDesc;
    momentsSrvDesc.Format = momentsDesc.Format;
    m_device->CreateShaderResourceView(
        m_diffuseLuminanceMomentsTexture.Get(),
        &momentsSrvDesc,
        descriptorHandle(c_atrousDiffuseMomentsSrvIndex));
    m_device->CreateShaderResourceView(
        m_specularLuminanceMomentsTexture.Get(),
        &momentsSrvDesc,
        descriptorHandle(c_atrousSpecularMomentsSrvIndex));
    m_device->CreateShaderResourceView(
        m_accumulationTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousTotalSrvIndex));
    m_device->CreateShaderResourceView(
        m_atrousFilterTextureA.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousFilterASrvIndex));
    m_device->CreateShaderResourceView(
        m_atrousFilterTextureB.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousFilterBSrvIndex));
    m_device->CreateShaderResourceView(
        m_atrousFilteredDiffuseTexture.Get(),
        &floatTextureSrvDesc,
        descriptorHandle(c_atrousFilteredDiffuseSrvIndex));

    m_device->CreateUnorderedAccessView(
        m_atrousFilterTextureA.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(c_atrousFilterAUavIndex));
    m_device->CreateUnorderedAccessView(
        m_atrousFilterTextureB.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(c_atrousFilterBUavIndex));
    m_device->CreateUnorderedAccessView(
        m_atrousFilteredDiffuseTexture.Get(),
        nullptr,
        &accumulationUavDesc,
        descriptorHandle(c_atrousFilteredDiffuseUavIndex));
    m_device->CreateUnorderedAccessView(
        m_outputTexture.Get(),
        nullptr,
        &outputUavDesc,
        descriptorHandle(c_atrousOutputUavIndex));

    if (m_environmentMap)
    {
        const D3D12_RESOURCE_DESC environmentDesc = m_environmentMap->GetDesc();

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = environmentDesc.Format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.TextureCube.MostDetailedMip = 0;
        srvDesc.TextureCube.MipLevels = environmentDesc.MipLevels;
        srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE environmentSrvHandle = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        environmentSrvHandle.ptr += static_cast<SIZE_T>(c_environmentDescriptorIndex) * m_descriptorSize;
        m_device->CreateShaderResourceView(m_environmentMap.Get(), &srvDesc, environmentSrvHandle);
    }

    if (createdDescriptorHeap)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
        nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrvDesc.Texture2D.MostDetailedMip = 0;
        nullSrvDesc.Texture2D.MipLevels = 1;
        nullSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr +=
            static_cast<SIZE_T>(c_materialTextureDescriptorIndex) * m_descriptorSize;
        for (UINT descriptorIndex = 0;
             descriptorIndex < c_materialTextureDescriptorCount;
             ++descriptorIndex)
        {
            m_device->CreateShaderResourceView(nullptr, &nullSrvDesc, handle);
            handle.ptr += m_descriptorSize;
        }
    }

    WriteTemporalHistoryDescriptors();
    return true;
}

bool RayTracingManager::UpdateDirectionalLightBuffer(UINT frameIndex)
{
    if (frameIndex >= c_tlasFrameCount ||
        !m_directionalLightBuffers[frameIndex])
    {
        return false;
    }

    GpuDirectionalLight light = {};
    std::copy(
        m_directionalLightDirection.begin(),
        m_directionalLightDirection.end(),
        light.direction);
    light.enabled =
        m_directionalLightAvailable &&
        m_directionalLightEnabled &&
        m_directionalLightIntensityScale > 0.0f
        ? 1u
        : 0u;
    for (UINT component = 0; component < 3u; ++component)
    {
        light.radiance[component] =
            m_directionalLightRadiance[component] *
            m_directionalLightIntensityScale;
    }
    light.samplingProbability =
        m_directionalLightSamplingProbability;

    void* destination = nullptr;
    const D3D12_RANGE noReadRange = { 0, 0 };
    const HRESULT hr = m_directionalLightBuffers[frameIndex]->Map(
        0,
        &noReadRange,
        &destination);
    if (ReportFailure(hr, L"Directional light buffer mapping failed."))
        return false;
    std::memcpy(destination, &light, sizeof(light));
    const D3D12_RANGE writtenRange = { 0, sizeof(light) };
    m_directionalLightBuffers[frameIndex]->Unmap(0, &writtenRange);
    return true;
}

void RayTracingManager::WriteTemporalHistoryDescriptors()
{
    if (!m_device || !m_descriptorHeap)
        return;

    const auto descriptorHandle = [&](UINT descriptorIndex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr +=
            static_cast<SIZE_T>(descriptorIndex) * m_descriptorSize;
        return handle;
    };

    D3D12_UNORDERED_ACCESS_VIEW_DESC float4UavDesc = {};
    float4UavDesc.Format = c_accumulationFormat;
    float4UavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_UNORDERED_ACCESS_VIEW_DESC materialUavDesc = float4UavDesc;
    materialUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_UNORDERED_ACCESS_VIEW_DESC momentsUavDesc = float4UavDesc;
    momentsUavDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    D3D12_UNORDERED_ACCESS_VIEW_DESC scalarUavDesc = float4UavDesc;
    scalarUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
    m_device->CreateUnorderedAccessView(
        m_accumulationTexture.Get(), nullptr, &float4UavDesc,
        descriptorHandle(1));
    m_device->CreateUnorderedAccessView(
        m_normalDepthTexture.Get(), nullptr, &float4UavDesc,
        descriptorHandle(2));
    m_device->CreateUnorderedAccessView(
        m_materialGuideTexture.Get(), nullptr, &materialUavDesc,
        descriptorHandle(3));
    m_device->CreateUnorderedAccessView(
        m_diffuseIndirectAccumulationTexture.Get(), nullptr, &float4UavDesc,
        descriptorHandle(4));
    m_device->CreateUnorderedAccessView(
        m_specularIndirectAccumulationTexture.Get(), nullptr, &float4UavDesc,
        descriptorHandle(5));
    m_device->CreateUnorderedAccessView(
        m_diffuseLuminanceMomentsTexture.Get(), nullptr, &momentsUavDesc,
        descriptorHandle(6));
    m_device->CreateUnorderedAccessView(
        m_specularLuminanceMomentsTexture.Get(), nullptr, &momentsUavDesc,
        descriptorHandle(7));
    m_device->CreateUnorderedAccessView(
        m_directionalShadowGuideTexture.Get(), nullptr, &scalarUavDesc,
        descriptorHandle(c_directionalShadowGuideUavIndex));
    D3D12_SHADER_RESOURCE_VIEW_DESC float4SrvDesc = {};
    float4SrvDesc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    float4SrvDesc.Format = c_accumulationFormat;
    float4SrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    float4SrvDesc.Texture2D.MostDetailedMip = 0;
    float4SrvDesc.Texture2D.MipLevels = 1;
    float4SrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
    D3D12_SHADER_RESOURCE_VIEW_DESC materialSrvDesc = float4SrvDesc;
    materialSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_SHADER_RESOURCE_VIEW_DESC momentsSrvDesc = float4SrvDesc;
    momentsSrvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
    D3D12_SHADER_RESOURCE_VIEW_DESC scalarSrvDesc = float4SrvDesc;
    scalarSrvDesc.Format = DXGI_FORMAT_R16_FLOAT;
    m_device->CreateShaderResourceView(
        m_diffuseIndirectAccumulationTexture.Get(), &float4SrvDesc,
        descriptorHandle(c_atrousDiffuseIndirectSrvIndex));
    m_device->CreateShaderResourceView(
        m_specularIndirectAccumulationTexture.Get(), &float4SrvDesc,
        descriptorHandle(c_atrousSpecularIndirectSrvIndex));
    m_device->CreateShaderResourceView(
        m_normalDepthTexture.Get(), &float4SrvDesc,
        descriptorHandle(c_atrousNormalDepthSrvIndex));
    m_device->CreateShaderResourceView(
        m_materialGuideTexture.Get(), &materialSrvDesc,
        descriptorHandle(c_atrousMaterialGuideSrvIndex));
    m_device->CreateShaderResourceView(
        m_diffuseLuminanceMomentsTexture.Get(), &momentsSrvDesc,
        descriptorHandle(c_atrousDiffuseMomentsSrvIndex));
    m_device->CreateShaderResourceView(
        m_specularLuminanceMomentsTexture.Get(), &momentsSrvDesc,
        descriptorHandle(c_atrousSpecularMomentsSrvIndex));
    m_device->CreateShaderResourceView(
        m_accumulationTexture.Get(), &float4SrvDesc,
        descriptorHandle(c_atrousTotalSrvIndex));

    m_device->CreateShaderResourceView(
        m_previousAccumulationTexture.Get(), &float4SrvDesc,
        descriptorHandle(c_temporalPreviousAccumulationSrvIndex));
    m_device->CreateShaderResourceView(
        m_previousNormalDepthTexture.Get(), &float4SrvDesc,
        descriptorHandle(c_temporalPreviousNormalDepthSrvIndex));
    m_device->CreateShaderResourceView(
        m_previousMaterialGuideTexture.Get(), &materialSrvDesc,
        descriptorHandle(c_temporalPreviousMaterialGuideSrvIndex));
    m_device->CreateShaderResourceView(
        m_previousDiffuseIndirectAccumulationTexture.Get(), &float4SrvDesc,
        descriptorHandle(c_temporalPreviousDiffuseSrvIndex));
    m_device->CreateShaderResourceView(
        m_previousSpecularIndirectAccumulationTexture.Get(), &float4SrvDesc,
        descriptorHandle(c_temporalPreviousSpecularSrvIndex));
    m_device->CreateShaderResourceView(
        m_previousDiffuseLuminanceMomentsTexture.Get(), &momentsSrvDesc,
        descriptorHandle(c_temporalPreviousDiffuseMomentsSrvIndex));
    m_device->CreateShaderResourceView(
        m_previousSpecularLuminanceMomentsTexture.Get(), &momentsSrvDesc,
        descriptorHandle(c_temporalPreviousSpecularMomentsSrvIndex));
    m_device->CreateShaderResourceView(
        m_previousDirectionalShadowGuideTexture.Get(), &scalarSrvDesc,
        descriptorHandle(c_temporalPreviousDirectionalShadowSrvIndex));
    const UINT transformFrame =
        static_cast<UINT>(m_frameIndex % c_tlasFrameCount);
    D3D12_SHADER_RESOURCE_VIEW_DESC transformSrvDesc = {};
    transformSrvDesc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    transformSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    transformSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    transformSrvDesc.Buffer.FirstElement = 0;
    transformSrvDesc.Buffer.NumElements = (std::max)(
        1u,
        static_cast<UINT>(m_previousInstanceTransforms.size()));
    transformSrvDesc.Buffer.StructureByteStride =
        sizeof(std::array<float, 12>);
    transformSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    m_device->CreateShaderResourceView(
        m_previousInstanceTransformBuffers[transformFrame].Get(),
        &transformSrvDesc,
        descriptorHandle(c_previousInstanceTransformsSrvIndex));
    D3D12_SHADER_RESOURCE_VIEW_DESC previousPositionSrvDesc = {};
    previousPositionSrvDesc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    previousPositionSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    previousPositionSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    previousPositionSrvDesc.Buffer.FirstElement = 0;
    previousPositionSrvDesc.Buffer.NumElements = (std::max)(1u, m_vertexCount);
    previousPositionSrvDesc.Buffer.StructureByteStride =
        sizeof(std::array<float, 4>);
    previousPositionSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    m_device->CreateShaderResourceView(
        m_previousSkinnedPositionBuffer.Get(),
        &previousPositionSrvDesc,
        descriptorHandle(c_previousSkinnedPositionsSrvIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC directionalLightSrvDesc = {};
    directionalLightSrvDesc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    directionalLightSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
    directionalLightSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    directionalLightSrvDesc.Buffer.FirstElement = 0;
    directionalLightSrvDesc.Buffer.NumElements = 1;
    directionalLightSrvDesc.Buffer.StructureByteStride =
        sizeof(GpuDirectionalLight);
    directionalLightSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    m_device->CreateShaderResourceView(
        m_directionalLightBuffers[transformFrame].Get(),
        &directionalLightSrvDesc,
        descriptorHandle(c_directionalLightSrvIndex));
}

bool RayTracingManager::CreateStatisticsResources()
{
    const UINT64 statisticsSize =
        sizeof(UINT) * static_cast<UINT64>(c_statisticsCounterCount);

    const D3D12_HEAP_PROPERTIES defaultHeap =
        CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC statisticsDesc = CreateBufferDesc(
        statisticsSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT hr = m_device->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &statisticsDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&m_statisticsBuffer));
    if (ReportFailure(hr, L"Statistics UAV creation failed."))
        return false;
    m_statisticsBuffer->SetName(L"Path tracing frame statistics");

    const D3D12_RESOURCE_DESC stagingDesc = CreateBufferDesc(statisticsSize);
    const D3D12_HEAP_PROPERTIES uploadHeap =
        CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    hr = m_device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &stagingDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_statisticsResetBuffer));
    if (ReportFailure(hr, L"Statistics reset buffer creation failed."))
        return false;
    m_statisticsResetBuffer->SetName(L"Path tracing statistics zero buffer");

    void* resetData = nullptr;
    D3D12_RANGE noReadRange = { 0, 0 };
    hr = m_statisticsResetBuffer->Map(0, &noReadRange, &resetData);
    if (ReportFailure(hr, L"Statistics reset buffer mapping failed."))
        return false;
    std::memset(resetData, 0, static_cast<std::size_t>(statisticsSize));
    D3D12_RANGE resetWriteRange = { 0, static_cast<SIZE_T>(statisticsSize) };
    m_statisticsResetBuffer->Unmap(0, &resetWriteRange);

    const D3D12_HEAP_PROPERTIES readbackHeap =
        CreateHeapProperties(D3D12_HEAP_TYPE_READBACK);
    hr = m_device->CreateCommittedResource(
        &readbackHeap,
        D3D12_HEAP_FLAG_NONE,
        &stagingDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_statisticsReadbackBuffer));
    if (ReportFailure(hr, L"Statistics readback buffer creation failed."))
        return false;
    m_statisticsReadbackBuffer->SetName(
        L"Path tracing frame statistics readback");
    return true;
}

void RayTracingManager::ReadFrameStatistics()
{
    if (!m_enableStatistics || !m_statisticsReadbackBuffer)
    {
        m_frameStatistics = {};
        return;
    }

    const SIZE_T statisticsSize =
        sizeof(UINT) * static_cast<SIZE_T>(c_statisticsCounterCount);
    D3D12_RANGE readRange = { 0, statisticsSize };
    UINT* counters = nullptr;
    const HRESULT hr = m_statisticsReadbackBuffer->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&counters));
    if (ReportFailure(hr, L"Statistics readback mapping failed."))
        return;

    for (UINT depth = 0; depth < c_statisticsRayDepthCount; ++depth)
        m_frameStatistics.raysByDepth[depth] = counters[depth];
    m_frameStatistics.primaryGuideRays =
        counters[c_statisticsPrimaryGuideRayIndex];
    m_frameStatistics.neeShadowRays =
        counters[c_statisticsNeeShadowRayIndex];
    m_frameStatistics.historyValidationShadowRays =
        counters[c_statisticsHistoryValidationShadowRayIndex];
    m_frameStatistics.shadowRays =
        m_frameStatistics.neeShadowRays +
        m_frameStatistics.historyValidationShadowRays;
    m_frameStatistics.hitCount = counters[c_statisticsHitIndex];
    m_frameStatistics.missCount = counters[c_statisticsMissIndex];

    D3D12_RANGE noWriteRange = { 0, 0 };
    m_statisticsReadbackBuffer->Unmap(0, &noWriteRange);
}


bool RayTracingManager::CreateEnvironmentMap()
{
    std::vector<std::uint8_t> ddsBytes;
    if (!ReadBinaryFile(GetEnvironmentMapPath(), ddsBytes))
    {
        std::wstring message = L"Environment DDS was not found.\nExpected: ";
        message += GetEnvironmentMapPath();
        ReportMessage(message);
        return false;
    }

    DdsCubemapData cubemap;
    if (!ParseLegacyRgba16FloatCubemapDds(ddsBytes, cubemap))
    {
        ReportMessage(L"Environment DDS must be a legacy A16B16G16R16F cubemap generated by iblbaker.");
        return false;
    }

    std::vector<GpuEnvironmentAliasEntry> environmentDistribution;
    if (!BuildEnvironmentAliasTable(
        cubemap,
        environmentDistribution,
        m_environmentPower))
    {
        ReportMessage(
            L"Environment importance distribution creation failed.");
        return false;
    }
    m_environmentResolution = cubemap.width;
    m_environmentTexelCount =
        static_cast<UINT>(environmentDistribution.size());
    const UINT64 environmentDistributionSize =
        sizeof(GpuEnvironmentAliasEntry) *
        environmentDistribution.size();
    const D3D12_HEAP_PROPERTIES distributionDefaultHeap =
        CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC distributionBufferDesc =
        CreateBufferDesc(environmentDistributionSize);
    HRESULT hr = m_device->CreateCommittedResource(
        &distributionDefaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &distributionBufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_environmentDistributionBuffer));
    if (ReportFailure(
        hr,
        L"Environment importance DEFAULT buffer creation failed."))
    {
        return false;
    }
    m_environmentDistributionBuffer->SetName(
        L"Environment importance alias table");

    Microsoft::WRL::ComPtr<ID3D12Resource>
        environmentDistributionUploadBuffer;
    if (!CreateUploadBuffer(
        environmentDistribution.data(),
        environmentDistributionSize,
        L"Environment importance alias table staging buffer",
        environmentDistributionUploadBuffer))
    {
        return false;
    }

    const UINT subresourceCount = c_cubeFaceCount * cubemap.mipCount;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Alignment = 0;
    textureDesc.Width = cubemap.width;
    textureDesc.Height = cubemap.height;
    textureDesc.DepthOrArraySize = static_cast<UINT16>(c_cubeFaceCount);
    textureDesc.MipLevels = static_cast<UINT16>(cubemap.mipCount);
    textureDesc.Format = cubemap.format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_HEAP_PROPERTIES defaultHeapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    hr = m_device->CreateCommittedResource(
        &defaultHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_environmentMap));
    if (ReportFailure(hr, L"Environment cubemap creation failed."))
        return false;

    m_environmentMap->SetName(L"IBL environment cubemap");

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresourceCount);
    std::vector<UINT> rowCounts(subresourceCount);
    std::vector<UINT64> rowSizes(subresourceCount);
    UINT64 uploadBufferSize = 0;
    m_device->GetCopyableFootprints(
        &textureDesc,
        0,
        subresourceCount,
        0,
        footprints.data(),
        rowCounts.data(),
        rowSizes.data(),
        &uploadBufferSize);

    const D3D12_HEAP_PROPERTIES uploadHeapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC uploadBufferDesc = CreateBufferDesc(uploadBufferSize);
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
    hr = m_device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));
    if (ReportFailure(hr, L"Environment cubemap upload buffer creation failed."))
        return false;

    uploadBuffer->SetName(L"IBL environment cubemap upload buffer");

    std::uint8_t* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = uploadBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData));
    if (ReportFailure(hr, L"Environment cubemap upload buffer mapping failed."))
        return false;

    std::memset(mappedData, 0, static_cast<std::size_t>(uploadBufferSize));
    std::size_t sourceOffset = 0;
    for (UINT face = 0; face < c_cubeFaceCount; ++face)
    {
        for (UINT mip = 0; mip < cubemap.mipCount; ++mip)
        {
            const UINT subresourceIndex = face * cubemap.mipCount + mip;
            const UINT mipWidth = GetMipDimension(cubemap.width, mip);
            const UINT mipHeight = GetMipDimension(cubemap.height, mip);
            const std::size_t sourceRowPitch = static_cast<std::size_t>(mipWidth) * cubemap.bytesPerPixel;
            const std::uint8_t* source = cubemap.texels.data() + sourceOffset;
            std::uint8_t* destination = mappedData + footprints[subresourceIndex].Offset;

            for (UINT row = 0; row < mipHeight; ++row)
            {
                std::memcpy(
                    destination + static_cast<std::size_t>(row) * footprints[subresourceIndex].Footprint.RowPitch,
                    source + static_cast<std::size_t>(row) * sourceRowPitch,
                    sourceRowPitch);
            }

            sourceOffset += sourceRowPitch * mipHeight;
        }
    }

    D3D12_RANGE writeRange = { 0, static_cast<SIZE_T>(uploadBufferSize) };
    uploadBuffer->Unmap(0, &writeRange);

    hr = m_buildCommandAllocator->Reset();
    if (ReportFailure(hr, L"Environment upload command allocator reset failed."))
        return false;

    hr = m_buildCommandList->Reset(m_buildCommandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"Environment upload command list reset failed."))
        return false;

    m_buildCommandList->CopyBufferRegion(
        m_environmentDistributionBuffer.Get(),
        0,
        environmentDistributionUploadBuffer.Get(),
        0,
        environmentDistributionSize);

    for (UINT subresourceIndex = 0; subresourceIndex < subresourceCount; ++subresourceIndex)
    {
        D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
        sourceLocation.pResource = uploadBuffer.Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprints[subresourceIndex];

        D3D12_TEXTURE_COPY_LOCATION destinationLocation = {};
        destinationLocation.pResource = m_environmentMap.Get();
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = subresourceIndex;

        m_buildCommandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
    }

    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource =
        m_environmentDistributionBuffer.Get();
    barriers[0].Transition.StateBefore =
        D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = m_environmentMap.Get();
    barriers[1].Transition.StateBefore =
        D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_buildCommandList->ResourceBarrier(2, barriers);

    if (!ExecuteBuildCommandListAndWait())
        return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = cubemap.format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srvDesc.TextureCube.MostDetailedMip = 0;
    srvDesc.TextureCube.MipLevels = cubemap.mipCount;
    srvDesc.TextureCube.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandle.ptr += static_cast<SIZE_T>(c_environmentDescriptorIndex) * m_descriptorSize;
    m_device->CreateShaderResourceView(m_environmentMap.Get(), &srvDesc, srvHandle);
    return true;
}

bool RayTracingManager::CreateGlobalRootSignature()
{
    D3D12_DESCRIPTOR_RANGE outputRanges[5] = {};
    outputRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRanges[0].NumDescriptors = 2;
    outputRanges[0].BaseShaderRegister = 0;
    outputRanges[0].RegisterSpace = 0;
    outputRanges[0].OffsetInDescriptorsFromTableStart = 0;
    outputRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRanges[1].NumDescriptors = 6;
    outputRanges[1].BaseShaderRegister = 3;
    outputRanges[1].RegisterSpace = 0;
    outputRanges[1].OffsetInDescriptorsFromTableStart = 2;
    outputRanges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRanges[2].NumDescriptors = 3;
    outputRanges[2].BaseShaderRegister = 9;
    outputRanges[2].RegisterSpace = 0;
    outputRanges[2].OffsetInDescriptorsFromTableStart =
        c_atrousFilterAUavIndex;
    outputRanges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRanges[3].NumDescriptors = 1;
    outputRanges[3].BaseShaderRegister = 12;
    outputRanges[3].RegisterSpace = 0;
    outputRanges[3].OffsetInDescriptorsFromTableStart =
        c_metallicGuideUavIndex;
    outputRanges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRanges[4].NumDescriptors = 1;
    outputRanges[4].BaseShaderRegister = 13;
    outputRanges[4].RegisterSpace = 0;
    outputRanges[4].OffsetInDescriptorsFromTableStart =
        c_directionalShadowGuideUavIndex;
    D3D12_DESCRIPTOR_RANGE environmentRange = {};
    environmentRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    environmentRange.NumDescriptors = 1;
    environmentRange.BaseShaderRegister = 3;
    environmentRange.RegisterSpace = 0;
    environmentRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE materialTextureRange = {};
    materialTextureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    materialTextureRange.NumDescriptors = c_materialTextureDescriptorCount;
    materialTextureRange.BaseShaderRegister = 6;
    materialTextureRange.RegisterSpace = 0;
    materialTextureRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE temporalHistoryRange = {};
    temporalHistoryRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    temporalHistoryRange.NumDescriptors = c_temporalDescriptorSrvCount;
    temporalHistoryRange.BaseShaderRegister = 265;
    temporalHistoryRange.RegisterSpace = 0;
    temporalHistoryRange.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[14] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[0].DescriptorTable.NumDescriptorRanges =
        _countof(outputRanges);
    rootParameters[0].DescriptorTable.pDescriptorRanges = outputRanges;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[1].Descriptor.ShaderRegister = 0;
    rootParameters[1].Descriptor.RegisterSpace = 0;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[2].Descriptor.ShaderRegister = 1;
    rootParameters[2].Descriptor.RegisterSpace = 0;
    rootParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[3].Descriptor.ShaderRegister = 2;
    rootParameters[3].Descriptor.RegisterSpace = 0;
    rootParameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[4].Constants.ShaderRegister = 0;
    rootParameters[4].Constants.RegisterSpace = 0;
    rootParameters[4].Constants.Num32BitValues = 42;
    rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[5].DescriptorTable.pDescriptorRanges = &environmentRange;
    rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[6].Descriptor.ShaderRegister = 4;
    rootParameters[6].Descriptor.RegisterSpace = 0;
    rootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[7].Descriptor.ShaderRegister = 5;
    rootParameters[7].Descriptor.RegisterSpace = 0;
    rootParameters[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[8].DescriptorTable.pDescriptorRanges = &materialTextureRange;
    rootParameters[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParameters[9].Descriptor.ShaderRegister = 2;
    rootParameters[9].Descriptor.RegisterSpace = 0;
    rootParameters[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[10].Descriptor.ShaderRegister = 262;
    rootParameters[10].Descriptor.RegisterSpace = 0;
    rootParameters[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[11].Descriptor.ShaderRegister = 263;
    rootParameters[11].Descriptor.RegisterSpace = 0;
    rootParameters[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rootParameters[12].Descriptor.ShaderRegister = 264;
    rootParameters[12].Descriptor.RegisterSpace = 0;
    rootParameters[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParameters[13].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[13].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[13].DescriptorTable.pDescriptorRanges =
        &temporalHistoryRange;
    rootParameters[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
    staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    staticSamplers[0].MipLODBias = 0.0f;
    staticSamplers[0].MaxAnisotropy = 1;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    staticSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    staticSamplers[0].MinLOD = 0.0f;
    staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].RegisterSpace = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    staticSamplers[1] = staticSamplers[0];
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[1].ShaderRegister = 1;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = _countof(staticSamplers);
    rootSignatureDesc.pStaticSamplers = staticSamplers;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &error);
    if (ReportFailure(hr, L"Raytracing root signature serialization failed."))
        return false;

    hr = m_device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_globalRootSignature));
    if (ReportFailure(hr, L"Raytracing global root signature creation failed."))
        return false;

    m_globalRootSignature->SetName(L"Raytracing global root signature");
    return true;
}

bool RayTracingManager::CreateAtrousPipeline()
{
    D3D12_DESCRIPTOR_RANGE descriptorRanges[10] = {};
    for (UINT rangeIndex = 0; rangeIndex < 9; ++rangeIndex)
    {
        descriptorRanges[rangeIndex].RangeType =
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        descriptorRanges[rangeIndex].NumDescriptors = 1;
        descriptorRanges[rangeIndex].BaseShaderRegister = rangeIndex;
        descriptorRanges[rangeIndex].OffsetInDescriptorsFromTableStart = 0;
    }
    descriptorRanges[9].RangeType =
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptorRanges[9].NumDescriptors = 1;
    descriptorRanges[9].BaseShaderRegister = 0;
    descriptorRanges[9].OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER rootParameters[11] = {};
    for (UINT parameterIndex = 0; parameterIndex < 10; ++parameterIndex)
    {
        rootParameters[parameterIndex].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[parameterIndex].DescriptorTable.NumDescriptorRanges =
            1;
        rootParameters[parameterIndex].DescriptorTable.pDescriptorRanges =
            &descriptorRanges[parameterIndex];
        rootParameters[parameterIndex].ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;
    }
    rootParameters[10].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[10].Constants.ShaderRegister = 0;
    rootParameters[10].Constants.Num32BitValues = 26;
    rootParameters[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &error);
    if (ReportFailure(
        hr,
        L"A-Trous root signature serialization failed."))
    {
        return false;
    }

    hr = m_device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_atrousRootSignature));
    if (ReportFailure(hr, L"A-Trous root signature creation failed."))
        return false;
    m_atrousRootSignature->SetName(L"A-Trous root signature");

    std::vector<std::uint8_t> shaderBytes;
    if (!LoadCompiledAtrousShader(shaderBytes))
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
    pipelineDesc.pRootSignature = m_atrousRootSignature.Get();
    pipelineDesc.CS.pShaderBytecode = shaderBytes.data();
    pipelineDesc.CS.BytecodeLength = shaderBytes.size();
    hr = m_device->CreateComputePipelineState(
        &pipelineDesc,
        IID_PPV_ARGS(&m_atrousPipelineState));
    if (ReportFailure(hr, L"A-Trous pipeline state creation failed."))
        return false;
    m_atrousPipelineState->SetName(L"A-Trous pipeline state");
    return true;
}

bool RayTracingManager::CreateTemporalColorClipPipeline()
{
    D3D12_DESCRIPTOR_RANGE descriptorRanges[12] = {};
    for (UINT rangeIndex = 0; rangeIndex < 6; ++rangeIndex)
    {
        descriptorRanges[rangeIndex].RangeType =
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        descriptorRanges[rangeIndex].NumDescriptors = 1;
        descriptorRanges[rangeIndex].BaseShaderRegister = rangeIndex;
        descriptorRanges[rangeIndex].OffsetInDescriptorsFromTableStart = 0;
    }
    for (UINT rangeIndex = 6; rangeIndex < 12; ++rangeIndex)
    {
        descriptorRanges[rangeIndex].RangeType =
            D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        descriptorRanges[rangeIndex].NumDescriptors = 1;
        descriptorRanges[rangeIndex].BaseShaderRegister = rangeIndex - 6;
        descriptorRanges[rangeIndex].OffsetInDescriptorsFromTableStart = 0;
    }

    D3D12_ROOT_PARAMETER rootParameters[13] = {};
    for (UINT parameterIndex = 0; parameterIndex < 12; ++parameterIndex)
    {
        rootParameters[parameterIndex].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[parameterIndex].DescriptorTable.NumDescriptorRanges =
            1;
        rootParameters[parameterIndex].DescriptorTable.pDescriptorRanges =
            &descriptorRanges[parameterIndex];
        rootParameters[parameterIndex].ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;
    }
    rootParameters[12].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[12].Constants.ShaderRegister = 0;
    rootParameters[12].Constants.Num32BitValues = 6;
    rootParameters[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &error);
    if (ReportFailure(
        hr,
        L"Temporal color clip root signature serialization failed."))
    {
        return false;
    }

    hr = m_device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_temporalColorClipRootSignature));
    if (ReportFailure(
        hr,
        L"Temporal color clip root signature creation failed."))
    {
        return false;
    }
    m_temporalColorClipRootSignature->SetName(
        L"Temporal color clip root signature");

    std::vector<std::uint8_t> shaderBytes;
    if (!LoadCompiledTemporalColorClipShader(shaderBytes))
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
    pipelineDesc.pRootSignature =
        m_temporalColorClipRootSignature.Get();
    pipelineDesc.CS.pShaderBytecode = shaderBytes.data();
    pipelineDesc.CS.BytecodeLength = shaderBytes.size();
    hr = m_device->CreateComputePipelineState(
        &pipelineDesc,
        IID_PPV_ARGS(&m_temporalColorClipPipelineState));
    if (ReportFailure(
        hr,
        L"Temporal color clip pipeline state creation failed."))
    {
        return false;
    }
    m_temporalColorClipPipelineState->SetName(
        L"Temporal color clip pipeline state");
    return true;
}

bool RayTracingManager::CreateSkinningPipeline()
{
    D3D12_ROOT_PARAMETER rootParameters[7] = {};
    for (UINT parameterIndex = 0; parameterIndex < 4u; ++parameterIndex)
    {
        rootParameters[parameterIndex].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParameters[parameterIndex].Descriptor.ShaderRegister =
            parameterIndex;
        rootParameters[parameterIndex].ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;
    }
    rootParameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParameters[4].Descriptor.ShaderRegister = 0u;
    rootParameters[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rootParameters[5].Descriptor.ShaderRegister = 1u;
    rootParameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParameters[6].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParameters[6].Constants.ShaderRegister = 0u;
    rootParameters[6].Constants.Num32BitValues = 4u;
    rootParameters[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &signature,
        &error);
    if (ReportFailure(hr, L"Skinning root signature serialization failed."))
        return false;

    hr = m_device->CreateRootSignature(
        0,
        signature->GetBufferPointer(),
        signature->GetBufferSize(),
        IID_PPV_ARGS(&m_skinningRootSignature));
    if (ReportFailure(hr, L"Skinning root signature creation failed."))
        return false;
    m_skinningRootSignature->SetName(L"Skinning root signature");

    std::vector<std::uint8_t> shaderBytes;
    if (!LoadCompiledSkinningShader(shaderBytes))
        return false;

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDesc = {};
    pipelineDesc.pRootSignature = m_skinningRootSignature.Get();
    pipelineDesc.CS.pShaderBytecode = shaderBytes.data();
    pipelineDesc.CS.BytecodeLength = shaderBytes.size();
    hr = m_device->CreateComputePipelineState(
        &pipelineDesc,
        IID_PPV_ARGS(&m_skinningPipelineState));
    if (ReportFailure(hr, L"Skinning pipeline state creation failed."))
        return false;
    m_skinningPipelineState->SetName(L"Skinning pipeline state");
    return true;
}

bool RayTracingManager::CreateRaytracingPipelineState()
{
    std::vector<std::uint8_t> shaderBytes;
    if (!LoadCompiledShader(shaderBytes))
        return false;

    D3D12_EXPORT_DESC shaderExports[5] = {};
    shaderExports[0].Name = c_rayGenShaderName;
    shaderExports[0].ExportToRename = nullptr;
    shaderExports[0].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[1].Name = c_surfaceQueryClosestHitShaderName;
    shaderExports[1].ExportToRename = nullptr;
    shaderExports[1].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[2].Name = c_surfaceQueryMissShaderName;
    shaderExports[2].ExportToRename = nullptr;
    shaderExports[2].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[3].Name = c_shadowMissShaderName;
    shaderExports[3].ExportToRename = nullptr;
    shaderExports[3].Flags = D3D12_EXPORT_FLAG_NONE;
    shaderExports[4].Name = c_alphaMaskAnyHitShaderName;
    shaderExports[4].ExportToRename = nullptr;
    shaderExports[4].Flags = D3D12_EXPORT_FLAG_NONE;
    D3D12_DXIL_LIBRARY_DESC dxilLibraryDesc = {};
    dxilLibraryDesc.DXILLibrary.pShaderBytecode = shaderBytes.data();
    dxilLibraryDesc.DXILLibrary.BytecodeLength = shaderBytes.size();
    dxilLibraryDesc.NumExports = _countof(shaderExports);
    dxilLibraryDesc.pExports = shaderExports;

    D3D12_HIT_GROUP_DESC hitGroupDesc = {};
    hitGroupDesc.HitGroupExport = c_surfaceQueryHitGroupName;
    hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroupDesc.ClosestHitShaderImport =
        c_surfaceQueryClosestHitShaderName;
    hitGroupDesc.AnyHitShaderImport = c_alphaMaskAnyHitShaderName;

    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
    shaderConfig.MaxPayloadSizeInBytes = c_shaderPayloadSize;
    shaderConfig.MaxAttributeSizeInBytes = c_shaderAttributeSize;

    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSignature = {};
    globalRootSignature.pGlobalRootSignature = m_globalRootSignature.Get();

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
    pipelineConfig.MaxTraceRecursionDepth = 1u;

    D3D12_STATE_SUBOBJECT subobjects[5] = {};
    subobjects[0].Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    subobjects[0].pDesc = &dxilLibraryDesc;
    subobjects[1].Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subobjects[1].pDesc = &hitGroupDesc;
    subobjects[2].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobjects[2].pDesc = &shaderConfig;
    subobjects[3].Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
    subobjects[3].pDesc = &globalRootSignature;
    subobjects[4].Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobjects[4].pDesc = &pipelineConfig;

    D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = _countof(subobjects);
    stateObjectDesc.pSubobjects = subobjects;

    HRESULT hr = m_device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&m_stateObject));
    if (ReportFailure(hr, L"Raytracing pipeline state object creation failed."))
        return false;

    m_stateObject->SetName(L"Raytracing pipeline state object");
    return true;
}

bool RayTracingManager::CreateShaderTables()
{
    return CreateShaderTable(
        c_rayGenShaderName,
        m_rayGenShaderTable.ReleaseAndGetAddressOf(),
        &m_rayGenShaderRecordSize,
        L"RayGen shader table") &&
        CreateMissShaderTable() &&
        CreateShaderTable(
            c_surfaceQueryHitGroupName,
            m_hitGroupShaderTable.ReleaseAndGetAddressOf(),
            &m_hitGroupShaderRecordSize,
            L"HitGroup shader table");
}

bool RayTracingManager::CreateShaderTable(
    const wchar_t* shaderExportName,
    ID3D12Resource** shaderTable,
    UINT* shaderRecordSize,
    const wchar_t* debugName)
{
    Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> stateObjectProperties;
    HRESULT hr = m_stateObject.As(&stateObjectProperties);
    if (ReportFailure(hr, L"Raytracing state object properties query failed."))
        return false;

    void* shaderIdentifier = stateObjectProperties->GetShaderIdentifier(shaderExportName);
    if (!shaderIdentifier)
    {
        std::wstring message = L"Shader identifier was not found: ";
        message += shaderExportName;
        ReportMessage(message);
        return false;
    }

    *shaderRecordSize = AlignUp(
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    const UINT shaderTableSize = AlignUp(
        *shaderRecordSize,
        D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC bufferDesc = CreateBufferDesc(shaderTableSize);

    hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(shaderTable));
    if (ReportFailure(hr, L"Shader table creation failed."))
        return false;

    (*shaderTable)->SetName(debugName);

    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = (*shaderTable)->Map(0, &readRange, &mappedData);
    if (ReportFailure(hr, L"Shader table mapping failed."))
        return false;

    std::memset(mappedData, 0, shaderTableSize);
    std::memcpy(mappedData, shaderIdentifier, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    (*shaderTable)->Unmap(0, nullptr);

    return true;
}

bool RayTracingManager::CreateAccelerationStructures()
{
    if (!m_buildCommandQueue || !m_buildCommandAllocator || !m_buildCommandList || !m_buildFence)
    {
        if (!CreateBuildCommandObjects())
            return false;
    }

    if (!CreateStaticGeometryBuffers())
        return false;

    HRESULT hr = m_buildCommandAllocator->Reset();
    if (ReportFailure(hr, L"AS build command allocator reset failed."))
        return false;

    hr = m_buildCommandList->Reset(m_buildCommandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"AS build command list reset failed."))
        return false;

    if (!BuildBottomLevelAccelerationStructure())
        return false;

    std::vector<D3D12_RESOURCE_BARRIER> blasBarriers;
    auto appendBlasBarrier = [&blasBarriers](ID3D12Resource* resource)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
        blasBarriers.push_back(barrier);
    };
    if (m_useImportedMeshInstances)
    {
        for (const ImportedMeshBlas& blas : m_importedMeshBlases)
            appendBlasBarrier(blas.accelerationStructure.Get());
    }
    else
    {
        appendBlasBarrier(m_bottomLevelAS.Get());
    }

    if (m_hasDynamicSphere)
        appendBlasBarrier(m_dynamicSphereBottomLevelAS.Get());
    if (m_hasDynamicCube)
        appendBlasBarrier(m_dynamicCubeBottomLevelAS.Get());
    m_buildCommandList->ResourceBarrier(
        static_cast<UINT>(blasBarriers.size()),
        blasBarriers.data());

    if (!BuildTopLevelAccelerationStructure())
        return false;

    D3D12_RESOURCE_BARRIER tlasBarrier = {};
    tlasBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    tlasBarrier.UAV.pResource = m_topLevelAS.Get();
    m_buildCommandList->ResourceBarrier(1, &tlasBarrier);

    const bool buildSucceeded = ExecuteBuildCommandListAndWait();
    m_blasScratchBuffer.Reset();
    for (ImportedMeshBlas& blas : m_importedMeshBlases)
    {
        if (!blas.skinned)
            blas.scratchBuffer.Reset();
    }
    m_dynamicSphereBlasScratchBuffer.Reset();
    m_dynamicCubeBlasScratchBuffer.Reset();
    if (!m_hasSceneAnimation &&
        !m_hasDynamicSphere &&
        !m_hasDynamicCube)
        m_tlasScratchBuffer.Reset();
    return buildSucceeded;
}

bool RayTracingManager::CreateMissShaderTable()
{
    Microsoft::WRL::ComPtr<ID3D12StateObjectProperties>
        stateObjectProperties;
    HRESULT hr = m_stateObject.As(&stateObjectProperties);
    if (ReportFailure(
        hr,
        L"Raytracing state object properties query failed."))
    {
        return false;
    }

    const wchar_t* shaderNames[] =
    {
        c_surfaceQueryMissShaderName,
        c_shadowMissShaderName
    };
    m_missShaderRecordSize = AlignUp(
        D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
        D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    const UINT tableSize = AlignUp(
        m_missShaderRecordSize * _countof(shaderNames),
        D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

    const D3D12_HEAP_PROPERTIES heapProperties =
        CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC bufferDesc =
        CreateBufferDesc(tableSize);
    hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_missShaderTable));
    if (ReportFailure(hr, L"Miss shader table creation failed."))
        return false;

    m_missShaderTable->SetName(L"Miss shader table");
    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = m_missShaderTable->Map(0, &readRange, &mappedData);
    if (ReportFailure(hr, L"Miss shader table mapping failed."))
        return false;

    std::memset(mappedData, 0, tableSize);
    for (UINT shaderIndex = 0;
         shaderIndex < _countof(shaderNames);
         ++shaderIndex)
    {
        void* shaderIdentifier =
            stateObjectProperties->GetShaderIdentifier(
                shaderNames[shaderIndex]);
        if (!shaderIdentifier)
        {
            m_missShaderTable->Unmap(0, nullptr);
            ReportMessage(L"Miss shader identifier was not found.");
            return false;
        }
        std::memcpy(
            static_cast<std::uint8_t*>(mappedData) +
                shaderIndex * m_missShaderRecordSize,
            shaderIdentifier,
            D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    m_missShaderTable->Unmap(0, nullptr);
    return true;
}

bool RayTracingManager::CreateBuildCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_buildCommandQueue));
    if (ReportFailure(hr, L"AS build command queue creation failed."))
        return false;

    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_buildCommandAllocator));
    if (ReportFailure(hr, L"AS build command allocator creation failed."))
        return false;

    hr = m_device->CreateCommandList(
        0,
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_buildCommandAllocator.Get(),
        nullptr,
        IID_PPV_ARGS(&m_buildCommandList));
    if (ReportFailure(hr, L"AS build command list creation failed."))
        return false;

    hr = m_buildCommandList->Close();
    if (ReportFailure(hr, L"Initial AS build command list close failed."))
        return false;

    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_buildFence));
    if (ReportFailure(hr, L"AS build fence creation failed."))
        return false;

    m_buildFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_buildFenceEvent)
    {
        ReportMessage(L"AS build fence event creation failed.");
        return false;
    }

    return true;
}

bool RayTracingManager::CreateStaticGeometryBuffers()
{
    SceneData scene;
    SceneBounds modelBounds = {};
    GltfLoadReport loadReport;
    bool hasModelBounds = false;
    bool hasLoadReport = false;
    std::size_t areaLightCount = 0;
    m_directionalLightAvailable = false;
    m_directionalLightSamplingProbability = 0.5f;
    m_directionalLightDirection = { 0.0f, -1.0f, 0.0f };
    m_directionalLightRadiance = { 0.0f, 0.0f, 0.0f };
    m_hasDynamicSphere = false;
    m_hasDynamicCube = false;
    m_dynamicSceneFrameIndex = 0;
    m_staticGeometry = {};
    m_staticAlphaGeometry = {};
    m_dynamicSphereGeometry = {};
    m_dynamicCubeGeometry = {};
    m_hasStaticAlphaGeometry = false;
    m_skinBindPoseVertexBuffer.Reset();
    m_skinInfluenceBuffer.Reset();
    m_previousSkinnedPositionBuffer.Reset();
    for (auto& jointMatrixBuffer : m_skinJointMatrixBuffers)
        jointMatrixBuffer.Reset();
    for (auto& jointMatrixBuffer : m_previousSkinJointMatrixBuffers)
        jointMatrixBuffer.Reset();
    for (auto& directionalLightBuffer : m_directionalLightBuffers)
        directionalLightBuffer.Reset();
    m_gpuSkinningActive = false;
    if (!ConfigureSceneAnimation(SceneData{}))
        return false;
    const bool isPbrScene = m_sceneType == c_scenePbrGgx ||
        m_sceneType == c_scenePbrGpuValidation;
    if (isPbrScene && !m_sceneFilePath.empty())
    {
        std::wstring errorMessage;
        GltfLoadOptions loadOptions;
        loadOptions.skipNonOpaquePrimitives = m_sponzaLite;
        if (!LoadGltfSceneData(
            m_sceneFilePath,
            scene,
            errorMessage,
            loadOptions,
            &loadReport))
        {
            ReportMessage(
                L"glTF scene load failed.\nPath: " + m_sceneFilePath +
                L"\nReason: " + errorMessage);
            return false;
        }
        hasLoadReport = true;
        if (!scene.IsValid() || !ComputeSceneBounds(scene, modelBounds))
        {
            ReportMessage(L"Loaded glTF scene data or bounds are invalid.");
            return false;
        }
        hasModelBounds = true;
        if (m_sponzaLite)
        {
            std::vector<SceneAreaLight> lights;
            SponzaDirectionalLight directionalLight;
            if (!LoadSponzaLightConfig(
                m_sponzaLightConfigPath,
                lights,
                directionalLight,
                errorMessage))
            {
                ReportMessage(
                    L"Sponza light config load failed.\nPath: " +
                    m_sponzaLightConfigPath +
                    L"\nReason: " + errorMessage);
                return false;
            }
            const std::size_t lightVertexOffset = scene.vertices.size();
            const std::size_t lightIndexOffset = scene.indices.size();
            const std::size_t lightPrimitiveOffset =
                scene.primitiveMaterialIndices.size();
            if (!lights.empty() && !AppendAreaLights(scene, lights))
            {
                ReportMessage(
                    L"Failed to append the Sponza area-light geometry.");
                return false;
            }
            if (!lights.empty() && !m_overlaySceneFilePath.empty())
            {
                const auto validUintRange = [](
                    std::size_t offset,
                    std::size_t count)
                {
                    return offset <=
                            std::numeric_limits<std::uint32_t>::max() &&
                        count <=
                            std::numeric_limits<std::uint32_t>::max() -
                                offset;
                };
                const std::size_t lightVertexCount =
                    scene.vertices.size() - lightVertexOffset;
                const std::size_t lightIndexCount =
                    scene.indices.size() - lightIndexOffset;
                if (!validUintRange(lightVertexOffset, lightVertexCount) ||
                    !validUintRange(lightIndexOffset, lightIndexCount) ||
                    lightPrimitiveOffset >
                        std::numeric_limits<std::uint32_t>::max() ||
                    !AppendStaticGeometryInstance(
                        scene,
                        "Sponza area lights",
                        static_cast<std::uint32_t>(lightVertexOffset),
                        static_cast<std::uint32_t>(lightVertexCount),
                        static_cast<std::uint32_t>(lightIndexOffset),
                        static_cast<std::uint32_t>(lightIndexCount),
                        static_cast<std::uint32_t>(
                            lightPrimitiveOffset)))
                {
                    ReportMessage(
                        L"Failed to register Sponza area-light geometry "
                        L"as a static mesh instance.");
                    return false;
                }
            }
            areaLightCount = lights.size();
            m_directionalLightAvailable = directionalLight.enabled;
            m_directionalLightSamplingProbability =
                directionalLight.samplingProbability;
            std::copy(
                std::begin(directionalLight.direction),
                std::end(directionalLight.direction),
                m_directionalLightDirection.begin());
            std::copy(
                std::begin(directionalLight.radiance),
                std::end(directionalLight.radiance),
                m_directionalLightRadiance.begin());
        }
        if (!m_overlaySceneFilePath.empty())
        {
            SceneData overlayScene;
            GltfLoadOptions overlayLoadOptions;
            GltfLoadReport overlayLoadReport;
            if (!LoadGltfSceneData(
                    m_overlaySceneFilePath,
                    overlayScene,
                    errorMessage,
                    overlayLoadOptions,
                    &overlayLoadReport))
            {
                ReportMessage(
                    L"Overlay glTF scene load failed.\nPath: " +
                    m_overlaySceneFilePath +
                    L"\nReason: " + errorMessage);
                return false;
            }
            constexpr std::array<float, 3> overlayPosition =
                { 0.0f, 1.0f, 0.0f };
            constexpr float overlayScale = 1.0f;
            if (!ApplyUniformScenePlacement(
                    overlayScene,
                    overlayPosition,
                    overlayScale) ||
                !AppendSceneData(scene, std::move(overlayScene)))
            {
                ReportMessage(
                    L"Failed to place and merge the animated overlay "
                    L"scene into Sponza.");
                return false;
            }
        }
        if ((!m_composeModelRoom && !m_sponzaLite) ||
            !m_overlaySceneFilePath.empty())
        {
            if (!ConfigureSceneAnimation(scene))
            {
                ReportMessage(
                    L"Failed to configure glTF node animation playback.");
                return false;
            }
        }
        if (m_composeModelRoom && !AppendPbrModelRoom(scene, modelBounds))
        {
            ReportMessage(L"Failed to compose the PBR model room.");
            return false;
        }
    }
    else
    {
        if (isPbrScene)
            scene = CreatePbrGgxSceneData();
        else if (m_sceneType == c_sceneIndirectBounceStress)
            scene = CreateIndirectBounceStressSceneData();
        else if (m_sceneType == c_sceneDynamicTransformTest)
            scene = CreateDynamicTransformTestRoomSceneData();
        else
            scene = CreateCornellBoxSceneData();
    }

    if (!m_useImportedMeshInstances)
    {
    // Keep opaque and alpha-tested triangles in separate geometry descriptors
    // inside one static BLAS. Opaque triangles then bypass Any-Hit while
    // alpha-mask triangles retain their required Any-Hit test.
    std::vector<std::uint32_t> opaqueIndices;
    std::vector<std::uint32_t> alphaIndices;
    std::vector<std::uint32_t> opaqueMaterials;
    std::vector<std::uint32_t> alphaMaterials;
    opaqueIndices.reserve(scene.indices.size());
    alphaIndices.reserve(scene.indices.size());
    opaqueMaterials.reserve(scene.primitiveMaterialIndices.size());
    alphaMaterials.reserve(scene.primitiveMaterialIndices.size());
    for (std::size_t primitiveIndex = 0;
         primitiveIndex < scene.primitiveMaterialIndices.size();
         ++primitiveIndex)
    {
        const std::uint32_t materialIndex =
            scene.primitiveMaterialIndices[primitiveIndex];
        const bool alphaMasked =
            scene.materials[materialIndex].alphaCutoff >= 0.0f;
        std::vector<std::uint32_t>& destinationIndices =
            alphaMasked ? alphaIndices : opaqueIndices;
        std::vector<std::uint32_t>& destinationMaterials =
            alphaMasked ? alphaMaterials : opaqueMaterials;
        const std::size_t sourceIndex = primitiveIndex * 3u;
        destinationIndices.push_back(scene.indices[sourceIndex + 0u]);
        destinationIndices.push_back(scene.indices[sourceIndex + 1u]);
        destinationIndices.push_back(scene.indices[sourceIndex + 2u]);
        destinationMaterials.push_back(materialIndex);
    }

    const UINT staticVertexCount =
        static_cast<UINT>(scene.vertices.size());
    if (!opaqueIndices.empty() &&
        !alphaIndices.empty())
    {
        scene.indices.clear();
        scene.indices.reserve(opaqueIndices.size() + alphaIndices.size());
        scene.indices.insert(
            scene.indices.end(),
            opaqueIndices.begin(),
            opaqueIndices.end());
        scene.indices.insert(
            scene.indices.end(),
            alphaIndices.begin(),
            alphaIndices.end());

        scene.primitiveMaterialIndices.clear();
        scene.primitiveMaterialIndices.reserve(
            opaqueMaterials.size() + alphaMaterials.size());
        scene.primitiveMaterialIndices.insert(
            scene.primitiveMaterialIndices.end(),
            opaqueMaterials.begin(),
            opaqueMaterials.end());
        scene.primitiveMaterialIndices.insert(
            scene.primitiveMaterialIndices.end(),
            alphaMaterials.begin(),
            alphaMaterials.end());

        m_staticGeometry.vertexCount = staticVertexCount;
        m_staticGeometry.indexCount =
            static_cast<UINT>(opaqueIndices.size());
        m_staticAlphaGeometry.vertexCount = staticVertexCount;
        m_staticAlphaGeometry.indexOffset =
            m_staticGeometry.indexCount;
        m_staticAlphaGeometry.indexCount =
            static_cast<UINT>(alphaIndices.size());
        m_staticAlphaGeometry.primitiveOffset =
            static_cast<UINT>(opaqueMaterials.size());
        m_staticAlphaGeometry.containsAlphaMask = true;
        m_hasStaticAlphaGeometry = true;
    }
    else
    {
        m_staticGeometry.vertexCount = staticVertexCount;
        m_staticGeometry.indexCount =
            static_cast<UINT>(scene.indices.size());
        m_staticGeometry.containsAlphaMask = !alphaIndices.empty();
    }
    }

    if (m_sponzaLite && hasModelBounds)
    {
        const float extentX =
            modelBounds.maximum[0] - modelBounds.minimum[0];
        const float extentY =
            modelBounds.maximum[1] - modelBounds.minimum[1];
        const float extentZ =
            modelBounds.maximum[2] - modelBounds.minimum[2];
        const float sceneDiagonal = std::sqrt(
            extentX * extentX +
            extentY * extentY +
            extentZ * extentZ);
        m_dynamicSphereRadius =
            (std::max)(sceneDiagonal * 0.015f, 0.20f);
        // The complete left-to-right travel range is 3% of the scene
        // diagonal, so this value is the half-range around the center.
        m_dynamicSphereMotionAmplitude = sceneDiagonal * 0.015f;
        m_dynamicSphereTrackCenterX =
            (modelBounds.minimum[0] + modelBounds.maximum[0]) * 0.5f;
        m_dynamicSphereCenterZ =
            (modelBounds.minimum[2] + modelBounds.maximum[2]) * 0.5f;
        const float maximumGroundHeight =
            modelBounds.minimum[1] + extentY * 0.20f;
        float groundHeight = modelBounds.minimum[1];
        bool foundGround = false;
        for (int sampleIndex = -2; sampleIndex <= 2; ++sampleIndex)
        {
            const float sampleX =
                m_dynamicSphereTrackCenterX +
                m_dynamicSphereMotionAmplitude *
                static_cast<float>(sampleIndex) * 0.5f;
            float sampleHeight = 0.0f;
            if (FindWalkableSurfaceHeight(
                scene,
                sampleX,
                m_dynamicSphereCenterZ,
                maximumGroundHeight,
                sampleHeight))
            {
                groundHeight = foundGround
                    ? (std::max)(groundHeight, sampleHeight)
                    : sampleHeight;
                foundGround = true;
            }
        }
        m_dynamicSphereCenterY =
            groundHeight + m_dynamicSphereRadius;
        m_dynamicSpherePositionX =
            m_dynamicSphereTrackCenterX -
            m_dynamicSphereMotionAmplitude;
        m_dynamicSphereRollRadians = 0.0f;
        SceneData sphere =
            CreateRollingMetalSphereSceneData(m_dynamicSphereRadius);
        if (!sphere.IsValid())
        {
            ReportMessage(L"Generated rolling metal sphere data is invalid.");
            return false;
        }

        m_dynamicSphereGeometry.vertexOffset =
            static_cast<UINT>(scene.vertices.size());
        m_dynamicSphereGeometry.vertexCount =
            static_cast<UINT>(sphere.vertices.size());
        m_dynamicSphereGeometry.indexOffset =
            static_cast<UINT>(scene.indices.size());
        m_dynamicSphereGeometry.indexCount =
            static_cast<UINT>(sphere.indices.size());
        m_dynamicSphereGeometry.primitiveOffset =
            static_cast<UINT>(scene.primitiveMaterialIndices.size());
        const std::uint32_t materialOffset =
            static_cast<std::uint32_t>(scene.materials.size());

        scene.vertices.insert(
            scene.vertices.end(),
            sphere.vertices.begin(),
            sphere.vertices.end());
        if (!scene.vertexSkinInfluences.empty())
        {
            scene.vertexSkinInfluences.resize(
                scene.vertices.size());
        }
        scene.indices.insert(
            scene.indices.end(),
            sphere.indices.begin(),
            sphere.indices.end());
        scene.materials.insert(
            scene.materials.end(),
            sphere.materials.begin(),
            sphere.materials.end());
        for (const std::uint32_t materialIndex :
             sphere.primitiveMaterialIndices)
        {
            scene.primitiveMaterialIndices.push_back(
                materialOffset + materialIndex);
        }
        m_hasDynamicSphere = true;
    }
    else if (m_sceneType == c_sceneDynamicTransformTest)
    {
        m_dynamicSphereRadius = 0.68f;
        m_dynamicSphereMotionAmplitude = 2.20f;
        m_dynamicSphereTrackCenterX = 0.0f;
        m_dynamicSphereCenterY = -1.0f + m_dynamicSphereRadius;
        m_dynamicSphereCenterZ = 1.55f;
        m_dynamicSpherePositionX =
            m_dynamicSphereTrackCenterX - m_dynamicSphereMotionAmplitude;
        m_dynamicSphereRollRadians = 0.0f;

        SceneData sphere = CreateDynamicTransformTestSphereSceneData(
            m_dynamicSphereRadius,
            m_dynamicTestSphereMaterialPreset);
        if (!sphere.IsValid())
        {
            ReportMessage(L"Dynamic test sphere data is invalid.");
            return false;
        }
        m_dynamicSphereGeometry.vertexOffset =
            static_cast<UINT>(scene.vertices.size());
        m_dynamicSphereGeometry.vertexCount =
            static_cast<UINT>(sphere.vertices.size());
        m_dynamicSphereGeometry.indexOffset =
            static_cast<UINT>(scene.indices.size());
        m_dynamicSphereGeometry.indexCount =
            static_cast<UINT>(sphere.indices.size());
        m_dynamicSphereGeometry.primitiveOffset =
            static_cast<UINT>(scene.primitiveMaterialIndices.size());
        std::uint32_t materialOffset =
            static_cast<std::uint32_t>(scene.materials.size());
        scene.vertices.insert(
            scene.vertices.end(),
            sphere.vertices.begin(),
            sphere.vertices.end());
        scene.indices.insert(
            scene.indices.end(),
            sphere.indices.begin(),
            sphere.indices.end());
        scene.materials.insert(
            scene.materials.end(),
            sphere.materials.begin(),
            sphere.materials.end());
        for (std::uint32_t materialIndex :
             sphere.primitiveMaterialIndices)
        {
            scene.primitiveMaterialIndices.push_back(
                materialOffset + materialIndex);
        }
        m_hasDynamicSphere = true;

        m_dynamicCubeHalfExtent = 0.68f;
        m_dynamicCubeCenterX = 1.35f;
        m_dynamicCubeCenterY = -1.0f + m_dynamicCubeHalfExtent;
        m_dynamicCubeCenterZ = 1.90f;
        m_dynamicCubePositionX = m_dynamicCubeCenterX;
        m_dynamicCubePositionZ = m_dynamicCubeCenterZ;
        m_dynamicCubeRotationY = 0.0f;
        SceneData cube = CreateDynamicTransformTestCubeSceneData(
            m_dynamicCubeHalfExtent,
            m_dynamicTestCubeMaterialPreset);
        if (!cube.IsValid())
        {
            ReportMessage(L"Dynamic test cube data is invalid.");
            return false;
        }
        m_dynamicCubeGeometry.vertexOffset =
            static_cast<UINT>(scene.vertices.size());
        m_dynamicCubeGeometry.vertexCount =
            static_cast<UINT>(cube.vertices.size());
        m_dynamicCubeGeometry.indexOffset =
            static_cast<UINT>(scene.indices.size());
        m_dynamicCubeGeometry.indexCount =
            static_cast<UINT>(cube.indices.size());
        m_dynamicCubeGeometry.primitiveOffset =
            static_cast<UINT>(scene.primitiveMaterialIndices.size());
        materialOffset = static_cast<std::uint32_t>(scene.materials.size());
        scene.vertices.insert(
            scene.vertices.end(),
            cube.vertices.begin(),
            cube.vertices.end());
        scene.indices.insert(
            scene.indices.end(),
            cube.indices.begin(),
            cube.indices.end());
        scene.materials.insert(
            scene.materials.end(),
            cube.materials.begin(),
            cube.materials.end());
        for (std::uint32_t materialIndex :
             cube.primitiveMaterialIndices)
        {
            scene.primitiveMaterialIndices.push_back(
                materialOffset + materialIndex);
        }
        m_hasDynamicCube = true;
    }

    GpuDirectionalLight gpuDirectionalLight = {};
    std::copy(
        m_directionalLightDirection.begin(),
        m_directionalLightDirection.end(),
        gpuDirectionalLight.direction);
    gpuDirectionalLight.enabled =
        m_directionalLightAvailable &&
        m_directionalLightEnabled &&
        m_directionalLightIntensityScale > 0.0f
        ? 1u
        : 0u;
    for (UINT component = 0; component < 3u; ++component)
    {
        gpuDirectionalLight.radiance[component] =
            m_directionalLightRadiance[component] *
            m_directionalLightIntensityScale;
    }
    gpuDirectionalLight.samplingProbability =
        m_directionalLightSamplingProbability;
    for (UINT frameIndex = 0; frameIndex < c_tlasFrameCount; ++frameIndex)
    {
        if (!CreateUploadBuffer(
                &gpuDirectionalLight,
                sizeof(gpuDirectionalLight),
                L"Directional light frame buffer",
                m_directionalLightBuffers[frameIndex]))
        {
            return false;
        }
    }

    if (!scene.IsValid())
    {
        ReportMessage(L"Generated scene data is invalid.");
        return false;
    }

    m_autoFrameCamera = hasModelBounds;
    if (m_autoFrameCamera)
    {
        for (std::size_t component = 0; component < 3; ++component)
        {
            m_sceneBoundsMin[component] = modelBounds.minimum[component];
            m_sceneBoundsMax[component] = modelBounds.maximum[component];
        }
        UpdateCameraFromSceneBounds();
    }
    else if (m_sceneType == c_sceneIndirectBounceStress)
    {
        m_cameraPosition = { 0.0f, 0.10f, -4.25f };
        m_cameraTarget = { 1.45f, 0.05f, -0.70f };
    }
    else if (m_sceneType == c_sceneDynamicTransformTest)
    {
        m_sceneBoundsMin = { -4.0f, -1.0f, -1.0f };
        m_sceneBoundsMax = { 4.0f, 2.8f, 5.0f };
        m_cameraPosition = { 0.0f, 0.25f, -5.80f };
        m_cameraTarget = { 0.0f, 0.15f, 1.65f };
    }
    else
    {
        m_cameraPosition = { 0.0f, 0.15f, -1.2f };
        m_cameraTarget = { 0.0f, 0.0f, 0.0f };
    }

    m_vertexCount = static_cast<UINT>(scene.vertices.size());
    m_indexCount = static_cast<UINT>(scene.indices.size());

    const UINT staticGeometryCount =
        m_hasStaticAlphaGeometry ? 2u : 1u;
    const UINT dynamicInstanceCount =
        (m_hasDynamicSphere ? 1u : 0u) +
        (m_hasDynamicCube ? 1u : 0u);
    const UINT staticInstanceCount = m_useImportedMeshInstances
        ? static_cast<UINT>(m_importedMeshInstances.size())
        : 1u;
    const UINT instanceCount = staticInstanceCount + dynamicInstanceCount;
    const UINT staticGeometryMetadataOffset = instanceCount;
    UINT nextDynamicGeometryMetadataOffset =
        staticGeometryMetadataOffset + staticGeometryCount;
    std::vector<SceneMetadataEntry> sceneMetadata;
    sceneMetadata.reserve(
        instanceCount + staticGeometryCount + dynamicInstanceCount);
    sceneMetadata.push_back(
        {
            staticGeometryMetadataOffset,
            m_hasSceneAnimation
                ? c_sceneMetadataFlagDynamic
                : 0u,
            0u,
            0u
        });
    if (m_hasDynamicSphere)
    {
        sceneMetadata.push_back(
            {
                nextDynamicGeometryMetadataOffset++,
                c_sceneMetadataFlagDynamic,
                0u,
                0u
            });
    }
    if (m_hasDynamicCube)
    {
        sceneMetadata.push_back(
            {
                nextDynamicGeometryMetadataOffset++,
                c_sceneMetadataFlagDynamic,
                0u,
                0u
            });
    }

    auto appendGeometryMetadata = [&sceneMetadata](
        const GeometryRange& geometry)
    {
        sceneMetadata.push_back(
            {
                geometry.vertexOffset,
                geometry.indexOffset,
                geometry.primitiveOffset,
                0u
            });
    };
    appendGeometryMetadata(m_staticGeometry);
    if (m_hasStaticAlphaGeometry)
        appendGeometryMetadata(m_staticAlphaGeometry);
    if (m_hasDynamicSphere)
        appendGeometryMetadata(m_dynamicSphereGeometry);
    if (m_hasDynamicCube)
        appendGeometryMetadata(m_dynamicCubeGeometry);

    if (m_useImportedMeshInstances)
    {
        sceneMetadata.assign(instanceCount, SceneMetadataEntry{});
        for (UINT instanceIndex = 0;
             instanceIndex < staticInstanceCount; ++instanceIndex)
        {
            const ImportedMeshInstance& instance =
                m_importedMeshInstances[instanceIndex];
            if (instance.meshBlasIndex >= m_importedMeshBlases.size())
                return false;
            const ImportedMeshBlas& blas =
                m_importedMeshBlases[instance.meshBlasIndex];
            if (instance.primitiveOffsets.size() != blas.geometries.size())
                return false;

            sceneMetadata[instanceIndex] =
            {
                static_cast<UINT>(sceneMetadata.size()),
                (instance.animated
                    ? c_sceneMetadataFlagDynamic
                    : 0u) |
                (instance.skinIndex != c_invalidSceneSkinIndex
                    ? c_sceneMetadataFlagSkinned
                    : 0u),
                0u,
                0u
            };
            for (std::size_t geometryIndex = 0;
                 geometryIndex < blas.geometries.size(); ++geometryIndex)
            {
                const GeometryRange& geometry = blas.geometries[geometryIndex];
                sceneMetadata.push_back(
                {
                    geometry.vertexOffset,
                    geometry.indexOffset,
                    instance.primitiveOffsets[geometryIndex],
                    0u
                });
            }
        }
        UINT dynamicInstanceIndex = staticInstanceCount;
        if (m_hasDynamicSphere)
        {
            sceneMetadata[dynamicInstanceIndex++] =
            {
                static_cast<UINT>(sceneMetadata.size()),
                c_sceneMetadataFlagDynamic,
                0u,
                0u
            };
            appendGeometryMetadata(m_dynamicSphereGeometry);
        }
        if (m_hasDynamicCube)
        {
            sceneMetadata[dynamicInstanceIndex] =
            {
                static_cast<UINT>(sceneMetadata.size()),
                c_sceneMetadataFlagDynamic,
                0u,
                0u
            };
            appendGeometryMetadata(m_dynamicCubeGeometry);
        }
    }

    const UINT staticIndexCount =
        m_useImportedMeshInstances
            ? static_cast<UINT>(scene.indices.size())
            : m_staticGeometry.indexCount +
        (m_hasStaticAlphaGeometry
            ? m_staticAlphaGeometry.indexCount
            : 0u);
    std::vector<GpuEmissiveTriangle> emissiveTriangles =
        BuildEmissiveTriangles(
            scene,
            staticIndexCount,
            m_areaLightPower);
    m_emissiveTriangleCount =
        static_cast<UINT>(emissiveTriangles.size());
    if (emissiveTriangles.empty())
        emissiveTriangles.push_back({});

    HRESULT hr = m_buildCommandAllocator->Reset();
    if (ReportFailure(
        hr,
        L"Static scene upload command allocator reset failed."))
    {
        return false;
    }
    hr = m_buildCommandList->Reset(m_buildCommandAllocator.Get(), nullptr);
    if (ReportFailure(
        hr,
        L"Static scene upload command list reset failed."))
    {
        return false;
    }

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadBuffers;
    uploadBuffers.reserve(6u);
    auto createStaticGpuBuffer = [this, &uploadBuffers](
        const void* data,
        UINT64 sizeInBytes,
        const wchar_t* debugName,
        Microsoft::WRL::ComPtr<ID3D12Resource>& destination,
        D3D12_RESOURCE_FLAGS resourceFlags =
            D3D12_RESOURCE_FLAG_NONE) -> bool
    {
        const D3D12_HEAP_PROPERTIES defaultHeap =
            CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        const D3D12_RESOURCE_DESC bufferDesc =
            CreateBufferDesc(sizeInBytes, resourceFlags);
        HRESULT createResult = m_device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&destination));
        if (ReportFailure(
            createResult,
            L"Static scene DEFAULT buffer creation failed."))
        {
            return false;
        }
        destination->SetName(debugName);

        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
        if (!CreateUploadBuffer(
            data,
            sizeInBytes,
            L"Static scene staging buffer",
            uploadBuffer))
        {
            return false;
        }

        m_buildCommandList->CopyBufferRegion(
            destination.Get(),
            0,
            uploadBuffer.Get(),
            0,
            sizeInBytes);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_buildCommandList->ResourceBarrier(1, &barrier);
        uploadBuffers.push_back(uploadBuffer);
        return true;
    };

    if (!createStaticGpuBuffer(
            scene.vertices.data(),
            sizeof(SceneVertex) * scene.vertices.size(),
            L"Raytracing scene vertex buffer",
            m_vertexBuffer,
            m_skinJointMatrixCount > 0u
                ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                : D3D12_RESOURCE_FLAG_NONE) ||
        !createStaticGpuBuffer(
            scene.indices.data(),
            sizeof(std::uint32_t) * scene.indices.size(),
            L"Raytracing scene index buffer",
            m_indexBuffer) ||
        !createStaticGpuBuffer(
            scene.materials.data(),
            sizeof(SceneMaterial) * scene.materials.size(),
            L"Raytracing scene material buffer",
            m_sceneMaterialBuffer) ||
        !createStaticGpuBuffer(
            scene.primitiveMaterialIndices.data(),
            sizeof(std::uint32_t) * scene.primitiveMaterialIndices.size(),
            L"Raytracing primitive material index buffer",
            m_primitiveMaterialIndexBuffer) ||
        !createStaticGpuBuffer(
            sceneMetadata.data(),
            sizeof(SceneMetadataEntry) * sceneMetadata.size(),
            L"Raytracing scene metadata buffer",
            m_sceneMetadataBuffer) ||
        !createStaticGpuBuffer(
            emissiveTriangles.data(),
            sizeof(GpuEmissiveTriangle) * emissiveTriangles.size(),
            L"Raytracing emissive triangle buffer",
            m_emissiveTriangleBuffer))
    {
        return false;
    }
    if (m_skinJointMatrixCount > 0u)
    {
        std::vector<std::array<float, 4>> previousPositions(
            scene.vertices.size());
        for (std::size_t vertexIndex = 0;
             vertexIndex < scene.vertices.size();
             ++vertexIndex)
        {
            previousPositions[vertexIndex] =
            {
                scene.vertices[vertexIndex].position[0],
                scene.vertices[vertexIndex].position[1],
                scene.vertices[vertexIndex].position[2],
                1.0f
            };
        }
        if (scene.vertexSkinInfluences.size() != scene.vertices.size() ||
            m_skinJointMatrices.size() != m_skinJointMatrixCount ||
            m_previousSkinJointMatrices.size() != m_skinJointMatrixCount ||
            !createStaticGpuBuffer(
                scene.vertices.data(),
                sizeof(SceneVertex) * scene.vertices.size(),
                L"Skin bind-pose vertex buffer",
                m_skinBindPoseVertexBuffer) ||
            !createStaticGpuBuffer(
                scene.vertexSkinInfluences.data(),
                sizeof(SceneVertexSkinInfluence) *
                    scene.vertexSkinInfluences.size(),
                L"Skin influence buffer",
                m_skinInfluenceBuffer) ||
            !createStaticGpuBuffer(
                previousPositions.data(),
                sizeof(std::array<float, 4>) * previousPositions.size(),
                L"Previous skinned vertex position buffer",
                m_previousSkinnedPositionBuffer,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        {
            return false;
        }
        for (UINT frameIndex = 0;
             frameIndex < c_tlasFrameCount;
             ++frameIndex)
        {
            if (!CreateUploadBuffer(
                m_skinJointMatrices.data(),
                sizeof(Matrix4) * m_skinJointMatrices.size(),
                L"Skin joint matrix palette",
                m_skinJointMatrixBuffers[frameIndex]))
            {
                return false;
            }
            if (!CreateUploadBuffer(
                m_previousSkinJointMatrices.data(),
                sizeof(Matrix4) * m_previousSkinJointMatrices.size(),
                L"Previous skin joint matrix palette",
                m_previousSkinJointMatrixBuffers[frameIndex]))
            {
                return false;
            }
        }
        m_gpuSkinningActive = true;
    }
    if (!ExecuteBuildCommandListAndWait())
        return false;

    if (m_sponzaLite && hasLoadReport)
    {
        const std::wstring manifestPath = m_sceneManifestPath.empty()
            ? L"BenchmarkOutput\\SponzaLite\\scene_manifest.json"
            : m_sceneManifestPath;
        SponzaSceneManifestSettings manifestSettings;
        manifestSettings.areaLightCount =
            static_cast<std::uint32_t>(areaLightCount);
        manifestSettings.dynamicMetalSphere = m_hasDynamicSphere;
        std::wstring errorMessage;
        if (!WriteSponzaSceneManifest(
            manifestPath,
            m_sceneFilePath,
            loadReport,
            manifestSettings,
            errorMessage))
        {
            ReportMessage(
                L"Sponza-lite manifest creation failed.\nPath: " +
                manifestPath + L"\nReason: " + errorMessage);
            return false;
        }
    }

    return CreateMaterialTextures(scene);
}

void RayTracingManager::UpdateCameraFromSceneBounds()
{
    const float aspectRatio = static_cast<float>(std::max(m_width, 1u)) /
        static_cast<float>(std::max(m_height, 1u));
    const float tanHalfVerticalFov = std::tan(c_verticalFovRadians * 0.5f);
    const float tanHalfHorizontalFov = tanHalfVerticalFov * aspectRatio;

    std::array<float, 3> halfExtent = {};
    for (std::size_t component = 0; component < 3; ++component)
    {
        m_cameraTarget[component] =
            (m_sceneBoundsMin[component] + m_sceneBoundsMax[component]) * 0.5f;
        halfExtent[component] =
            (m_sceneBoundsMax[component] - m_sceneBoundsMin[component]) * 0.5f;
    }

    const float verticalFitDistance =
        halfExtent[1] / std::max(tanHalfVerticalFov, 1.0e-4f);
    const float horizontalFitDistance =
        halfExtent[0] / std::max(tanHalfHorizontalFov, 1.0e-4f);
    const float maximumExtent = std::max(
        halfExtent[0],
        std::max(halfExtent[1], halfExtent[2]));
    const float fitDistance = std::max(
        std::max(verticalFitDistance, horizontalFitDistance),
        std::max(maximumExtent * 0.05f, 0.01f));
    const float cameraDistance =
        halfExtent[2] + fitDistance * c_cameraFrameMargin;

    m_cameraPosition =
    {
        m_cameraTarget[0],
        m_cameraTarget[1],
        m_cameraTarget[2] - cameraDistance
    };
}

bool RayTracingManager::CreateMaterialTextures(const SceneData& scene)
{
    if (scene.textures.size() > c_materialTextureDescriptorCount)
    {
        ReportMessage(L"The scene exceeds the 256 material texture limit.");
        return false;
    }

    m_materialTextures.clear();

    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvDesc = {};
    nullSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrvDesc.Texture2D.MostDetailedMip = 0;
    nullSrvDesc.Texture2D.MipLevels = 1;
    nullSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    D3D12_CPU_DESCRIPTOR_HANDLE descriptorHandle =
        m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    descriptorHandle.ptr +=
        static_cast<SIZE_T>(c_materialTextureDescriptorIndex) * m_descriptorSize;
    for (UINT descriptorIndex = 0;
         descriptorIndex < c_materialTextureDescriptorCount;
         ++descriptorIndex)
    {
        m_device->CreateShaderResourceView(nullptr, &nullSrvDesc, descriptorHandle);
        descriptorHandle.ptr += m_descriptorSize;
    }

    if (scene.textures.empty())
        return true;

    HRESULT hr = m_buildCommandAllocator->Reset();
    if (ReportFailure(hr, L"Material texture command allocator reset failed."))
        return false;
    hr = m_buildCommandList->Reset(m_buildCommandAllocator.Get(), nullptr);
    if (ReportFailure(hr, L"Material texture command list reset failed."))
        return false;

    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploadBuffers;
    uploadBuffers.reserve(scene.textures.size());
    m_materialTextures.reserve(scene.textures.size());

    for (std::size_t textureIndex = 0;
         textureIndex < scene.textures.size();
         ++textureIndex)
    {
        const SceneTexture& source = scene.textures[textureIndex];
        if (source.mips.empty() ||
            source.mips.size() > std::numeric_limits<UINT16>::max())
        {
            ReportMessage(L"A material texture has an invalid mip chain.");
            return false;
        }
        const UINT mipCount = static_cast<UINT>(source.mips.size());
        const SceneTextureMip& baseMip = source.mips.front();

        D3D12_RESOURCE_DESC textureDesc = {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Alignment = 0;
        textureDesc.Width = baseMip.width;
        textureDesc.Height = baseMip.height;
        textureDesc.DepthOrArraySize = 1;
        textureDesc.MipLevels = static_cast<UINT16>(mipCount);
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        textureDesc.SampleDesc.Count = 1;
        textureDesc.SampleDesc.Quality = 0;
        textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3D12Resource> gpuTexture;
        const D3D12_HEAP_PROPERTIES defaultHeap =
            CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        hr = m_device->CreateCommittedResource(
            &defaultHeap,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&gpuTexture));
        if (ReportFailure(hr, L"Material texture creation failed."))
            return false;

        std::wstring textureName =
            L"Material texture " + std::to_wstring(textureIndex);
        gpuTexture->SetName(textureName.c_str());

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mipCount);
        std::vector<UINT> rowCounts(mipCount);
        std::vector<UINT64> rowSizesInBytes(mipCount);
        UINT64 uploadSize = 0;
        m_device->GetCopyableFootprints(
            &textureDesc,
            0,
            mipCount,
            0,
            footprints.data(),
            rowCounts.data(),
            rowSizesInBytes.data(),
            &uploadSize);

        Microsoft::WRL::ComPtr<ID3D12Resource> uploadBuffer;
        const D3D12_HEAP_PROPERTIES uploadHeap =
            CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC uploadDesc = CreateBufferDesc(uploadSize);
        hr = m_device->CreateCommittedResource(
            &uploadHeap,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&uploadBuffer));
        if (ReportFailure(hr, L"Material texture upload buffer creation failed."))
            return false;

        void* mappedData = nullptr;
        const D3D12_RANGE readRange = { 0, 0 };
        hr = uploadBuffer->Map(0, &readRange, &mappedData);
        if (ReportFailure(hr, L"Material texture upload buffer mapping failed."))
            return false;

        for (UINT mipIndex = 0; mipIndex < mipCount; ++mipIndex)
        {
            const SceneTextureMip& sourceMip = source.mips[mipIndex];
            const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint =
                footprints[mipIndex];
            const std::size_t sourceRowPitch =
                static_cast<std::size_t>(sourceMip.width) * 4u;
            std::uint8_t* destination =
                static_cast<std::uint8_t*>(mappedData) + footprint.Offset;
            for (UINT row = 0; row < sourceMip.height; ++row)
            {
                std::memcpy(
                    destination +
                        static_cast<std::size_t>(row) *
                        footprint.Footprint.RowPitch,
                    sourceMip.rgba8.data() +
                        static_cast<std::size_t>(row) * sourceRowPitch,
                    sourceRowPitch);
            }
        }
        uploadBuffer->Unmap(0, nullptr);

        for (UINT mipIndex = 0; mipIndex < mipCount; ++mipIndex)
        {
            D3D12_TEXTURE_COPY_LOCATION destinationLocation = {};
            destinationLocation.pResource = gpuTexture.Get();
            destinationLocation.Type =
                D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destinationLocation.SubresourceIndex = mipIndex;

            D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
            sourceLocation.pResource = uploadBuffer.Get();
            sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sourceLocation.PlacedFootprint = footprints[mipIndex];
            m_buildCommandList->CopyTextureRegion(
                &destinationLocation,
                0,
                0,
                0,
                &sourceLocation,
                nullptr);
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = gpuTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_buildCommandList->ResourceBarrier(1, &barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = source.isSrgb != 0
            ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            : DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = mipCount;
        srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

        D3D12_CPU_DESCRIPTOR_HANDLE srvHandle =
            m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandle.ptr +=
            static_cast<SIZE_T>(c_materialTextureDescriptorIndex + textureIndex) *
            m_descriptorSize;
        m_device->CreateShaderResourceView(gpuTexture.Get(), &srvDesc, srvHandle);

        m_materialTextures.push_back(gpuTexture);
        uploadBuffers.push_back(uploadBuffer);
    }

    return ExecuteBuildCommandListAndWait();
}

bool RayTracingManager::BuildBottomLevelAccelerationStructure()
{
    if (m_useImportedMeshInstances)
    {
        for (ImportedMeshBlas& blas : m_importedMeshBlases)
        {
            if (!BuildBottomLevelAccelerationStructure(
                    blas.geometries.data(),
                    static_cast<UINT>(blas.geometries.size()),
                    L"Imported mesh bottom level acceleration structure",
                    blas.accelerationStructure,
                    blas.scratchBuffer,
                    blas.skinned))
                return false;
        }
    }
    else
    {
    const std::array<GeometryRange, 2> staticGeometries =
        { m_staticGeometry, m_staticAlphaGeometry };
    const UINT staticGeometryCount =
        m_hasStaticAlphaGeometry ? 2u : 1u;
    if (!BuildBottomLevelAccelerationStructure(
        staticGeometries.data(),
        staticGeometryCount,
        L"Static scene bottom level acceleration structure",
        m_bottomLevelAS,
        m_blasScratchBuffer))
    {
        return false;
    }
    }

    if (m_hasDynamicSphere &&
        !BuildBottomLevelAccelerationStructure(
            &m_dynamicSphereGeometry,
            1u,
            L"Rolling sphere bottom level acceleration structure",
            m_dynamicSphereBottomLevelAS,
            m_dynamicSphereBlasScratchBuffer))
    {
        return false;
    }
    if (m_hasDynamicCube &&
        !BuildBottomLevelAccelerationStructure(
            &m_dynamicCubeGeometry,
            1u,
            L"Dynamic test cube bottom level acceleration structure",
            m_dynamicCubeBottomLevelAS,
            m_dynamicCubeBlasScratchBuffer))
    {
        return false;
    }
    return true;
}

bool RayTracingManager::BuildBottomLevelAccelerationStructure(
    const GeometryRange* geometries,
    UINT geometryCount,
    const wchar_t* debugName,
    Microsoft::WRL::ComPtr<ID3D12Resource>& accelerationStructure,
    Microsoft::WRL::ComPtr<ID3D12Resource>& scratchBuffer,
    bool allowUpdate)
{
    if (!geometries || geometryCount == 0)
    {
        ReportMessage(L"BLAS geometry range is empty.");
        return false;
    }

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs(geometryCount);
    for (UINT geometryIndex = 0; geometryIndex < geometryCount; ++geometryIndex)
    {
        const GeometryRange& geometry = geometries[geometryIndex];
        if (geometry.vertexCount == 0 || geometry.indexCount == 0)
        {
            ReportMessage(L"BLAS geometry range is empty.");
            return false;
        }

        D3D12_RAYTRACING_GEOMETRY_DESC& geometryDesc =
            geometryDescs[geometryIndex];
        geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometryDesc.Flags = geometry.containsAlphaMask
            ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
            : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geometryDesc.Triangles.VertexBuffer.StartAddress =
            m_vertexBuffer->GetGPUVirtualAddress() +
            static_cast<UINT64>(geometry.vertexOffset) *
            sizeof(SceneVertex);
        geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(SceneVertex);
        geometryDesc.Triangles.VertexCount = geometry.vertexCount;
        geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geometryDesc.Triangles.IndexBuffer =
            m_indexBuffer->GetGPUVirtualAddress() +
            static_cast<UINT64>(geometry.indexOffset) *
            sizeof(std::uint32_t);
        geometryDesc.Triangles.IndexCount = geometry.indexCount;
        geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        geometryDesc.Triangles.Transform3x4 = 0;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (allowUpdate)
    {
        inputs.Flags |=
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = geometryCount;
    inputs.pGeometryDescs = geometryDescs.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
    if (prebuildInfo.ResultDataMaxSizeInBytes == 0)
    {
        ReportMessage(L"BLAS prebuild info returned an empty result size.");
        return false;
    }

    scratchBuffer.Reset();
    const UINT64 scratchSize = allowUpdate
        ? (std::max)(
            prebuildInfo.ScratchDataSizeInBytes,
            prebuildInfo.UpdateScratchDataSizeInBytes)
        : prebuildInfo.ScratchDataSizeInBytes;
    if (!CreateScratchBuffer(
        scratchSize,
        L"BLAS scratch buffer",
        scratchBuffer))
        return false;

    accelerationStructure.Reset();
    if (!CreateAccelerationStructureBuffer(
        prebuildInfo.ResultDataMaxSizeInBytes,
        debugName,
        accelerationStructure))
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData =
        scratchBuffer->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData =
        accelerationStructure->GetGPUVirtualAddress();

    m_buildCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    return true;
}

UINT RayTracingManager::GetStaticInstanceCount() const
{
    return m_useImportedMeshInstances
        ? static_cast<UINT>(m_importedMeshInstances.size())
        : 1u;
}

UINT RayTracingManager::PopulateStaticInstanceDescriptors(
    D3D12_RAYTRACING_INSTANCE_DESC* instanceDescs) const
{
    if (!instanceDescs)
        return 0u;
    if (!m_useImportedMeshInstances)
    {
        std::memcpy(
            instanceDescs[0].Transform,
            m_sceneAnimationInstanceTransform.data(),
            sizeof(instanceDescs[0].Transform));
        instanceDescs[0].InstanceID = 0u;
        instanceDescs[0].InstanceMask = 0xFF;
        instanceDescs[0].AccelerationStructure =
            m_bottomLevelAS->GetGPUVirtualAddress();
        return 1u;
    }

    for (UINT instanceIndex = 0;
         instanceIndex < m_importedMeshInstances.size(); ++instanceIndex)
    {
        const ImportedMeshInstance& instance =
            m_importedMeshInstances[instanceIndex];
        if (instance.meshBlasIndex >= m_importedMeshBlases.size())
            return 0u;
        const ImportedMeshBlas& blas =
            m_importedMeshBlases[instance.meshBlasIndex];
        if (!blas.accelerationStructure)
            return 0u;
        std::memcpy(
            instanceDescs[instanceIndex].Transform,
            instance.transform.data(),
            sizeof(instanceDescs[instanceIndex].Transform));
        instanceDescs[instanceIndex].InstanceID = instanceIndex;
        instanceDescs[instanceIndex].InstanceMask = 0xFF;
        instanceDescs[instanceIndex].AccelerationStructure =
            blas.accelerationStructure->GetGPUVirtualAddress();
    }
    return static_cast<UINT>(m_importedMeshInstances.size());
}

bool RayTracingManager::BuildTopLevelAccelerationStructure()
{
    const UINT staticInstanceCount = GetStaticInstanceCount();
    const UINT instanceCount =
        staticInstanceCount +
        (m_hasDynamicSphere ? 1u : 0u) +
        (m_hasDynamicCube ? 1u : 0u);
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(instanceCount);
    if (PopulateStaticInstanceDescriptors(instanceDescs.data()) !=
        staticInstanceCount)
        return false;

    UINT nextInstanceIndex = staticInstanceCount;
    if (m_hasDynamicSphere)
    {
        const float cosine = std::cos(m_dynamicSphereRollRadians);
        const float sine = std::sin(m_dynamicSphereRollRadians);
        D3D12_RAYTRACING_INSTANCE_DESC& sphereDesc =
            instanceDescs[nextInstanceIndex];
        sphereDesc.Transform[0][0] = cosine;
        sphereDesc.Transform[0][1] = -sine;
        sphereDesc.Transform[0][3] = m_dynamicSpherePositionX;
        sphereDesc.Transform[1][0] = sine;
        sphereDesc.Transform[1][1] = cosine;
        sphereDesc.Transform[1][3] = m_dynamicSphereCenterY;
        sphereDesc.Transform[2][2] = 1.0f;
        sphereDesc.Transform[2][3] = m_dynamicSphereCenterZ;
        sphereDesc.InstanceID = nextInstanceIndex;
        sphereDesc.InstanceMask =
            m_dynamicSphereVisible ? 0xFF : 0x00;
        sphereDesc.AccelerationStructure =
            m_dynamicSphereBottomLevelAS->GetGPUVirtualAddress();
        ++nextInstanceIndex;
    }

    if (m_hasDynamicCube)
    {
        const float cosine = std::cos(m_dynamicCubeRotationY);
        const float sine = std::sin(m_dynamicCubeRotationY);
        D3D12_RAYTRACING_INSTANCE_DESC& cubeDesc =
            instanceDescs[nextInstanceIndex];
        cubeDesc.Transform[0][0] = cosine;
        cubeDesc.Transform[0][2] = sine;
        cubeDesc.Transform[0][3] = m_dynamicCubePositionX;
        cubeDesc.Transform[1][1] = 1.0f;
        cubeDesc.Transform[1][3] = m_dynamicCubeCenterY;
        cubeDesc.Transform[2][0] = -sine;
        cubeDesc.Transform[2][2] = cosine;
        cubeDesc.Transform[2][3] = m_dynamicCubePositionZ;
        cubeDesc.InstanceID = nextInstanceIndex;
        cubeDesc.InstanceMask =
            m_dynamicCubeVisible ? 0xFF : 0x00;
        cubeDesc.AccelerationStructure =
            m_dynamicCubeBottomLevelAS->GetGPUVirtualAddress();
        ++nextInstanceIndex;
    }

    m_currentInstanceTransforms.resize(instanceCount);
    for (UINT instanceIndex = 0;
         instanceIndex < instanceCount;
         ++instanceIndex)
    {
        std::memcpy(
            m_currentInstanceTransforms[instanceIndex].data(),
            instanceDescs[instanceIndex].Transform,
            sizeof(instanceDescs[instanceIndex].Transform));
    }
    m_previousInstanceTransforms = m_currentInstanceTransforms;

    for (UINT frameIndex = 0;
         frameIndex < c_tlasFrameCount;
         ++frameIndex)
    {
        if (!CreateUploadBuffer(
            instanceDescs.data(),
            sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instanceCount,
            L"TLAS instance descriptor",
            m_instanceDescBuffers[frameIndex]))
        {
            return false;
        }
        if (!CreateUploadBuffer(
            m_previousInstanceTransforms.data(),
            sizeof(std::array<float, 12>) * instanceCount,
            L"Previous instance transforms",
            m_previousInstanceTransformBuffers[frameIndex]))
        {
            return false;
        }
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (m_hasSceneAnimation ||
        m_hasDynamicSphere ||
        m_hasDynamicCube)
    {
        inputs.Flags |=
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    }
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = instanceCount;
    inputs.InstanceDescs =
        m_instanceDescBuffers[0]->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuildInfo);
    if (prebuildInfo.ResultDataMaxSizeInBytes == 0)
    {
        ReportMessage(L"TLAS prebuild info returned an empty result size.");
        return false;
    }

    m_tlasScratchBuffer.Reset();
    const UINT64 scratchSize = (std::max)(
        prebuildInfo.ScratchDataSizeInBytes,
        prebuildInfo.UpdateScratchDataSizeInBytes);
    if (!CreateScratchBuffer(
        scratchSize,
        L"TLAS scratch buffer",
        m_tlasScratchBuffer))
        return false;

    if (!CreateAccelerationStructureBuffer(prebuildInfo.ResultDataMaxSizeInBytes, L"Top level acceleration structure", m_topLevelAS))
        return false;

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = m_tlasScratchBuffer->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData = m_topLevelAS->GetGPUVirtualAddress();

    m_buildCommandList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
    m_dynamicSphereVisibilityDirty = false;
    m_dynamicCubeVisibilityDirty = false;
    return true;
}

bool RayTracingManager::WriteInstanceDescriptors(UINT frameIndex)
{
    if (frameIndex >= c_tlasFrameCount ||
        !m_instanceDescBuffers[frameIndex])
    {
        return false;
    }

    D3D12_RAYTRACING_INSTANCE_DESC* instanceDescs = nullptr;
    const D3D12_RANGE readRange = { 0, 0 };
    HRESULT hr = m_instanceDescBuffers[frameIndex]->Map(
        0,
        &readRange,
        reinterpret_cast<void**>(&instanceDescs));
    if (ReportFailure(hr, L"TLAS instance descriptor mapping failed."))
        return false;

    const UINT staticInstanceCount = GetStaticInstanceCount();
    const UINT instanceCount =
        staticInstanceCount +
        (m_hasDynamicSphere ? 1u : 0u) +
        (m_hasDynamicCube ? 1u : 0u);
    std::memset(
        instanceDescs,
        0,
        sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instanceCount);
    if (PopulateStaticInstanceDescriptors(instanceDescs) !=
        staticInstanceCount)
    {
        m_instanceDescBuffers[frameIndex]->Unmap(0, nullptr);
        return false;
    }
    UINT nextInstanceIndex = staticInstanceCount;
    if (m_hasDynamicSphere)
    {
        const float cosine = std::cos(m_dynamicSphereRollRadians);
        const float sine = std::sin(m_dynamicSphereRollRadians);
        D3D12_RAYTRACING_INSTANCE_DESC& sphereDesc =
            instanceDescs[nextInstanceIndex];
        sphereDesc.Transform[0][0] = cosine;
        sphereDesc.Transform[0][1] = -sine;
        sphereDesc.Transform[0][3] = m_dynamicSpherePositionX;
        sphereDesc.Transform[1][0] = sine;
        sphereDesc.Transform[1][1] = cosine;
        sphereDesc.Transform[1][3] = m_dynamicSphereCenterY;
        sphereDesc.Transform[2][2] = 1.0f;
        sphereDesc.Transform[2][3] = m_dynamicSphereCenterZ;
        sphereDesc.InstanceID = nextInstanceIndex;
        sphereDesc.InstanceMask =
            m_dynamicSphereVisible ? 0xFF : 0x00;
        sphereDesc.AccelerationStructure =
            m_dynamicSphereBottomLevelAS->GetGPUVirtualAddress();
        ++nextInstanceIndex;
    }
    if (m_hasDynamicCube)
    {
        const float cosine = std::cos(m_dynamicCubeRotationY);
        const float sine = std::sin(m_dynamicCubeRotationY);
        D3D12_RAYTRACING_INSTANCE_DESC& cubeDesc =
            instanceDescs[nextInstanceIndex];
        cubeDesc.Transform[0][0] = cosine;
        cubeDesc.Transform[0][2] = sine;
        cubeDesc.Transform[0][3] = m_dynamicCubePositionX;
        cubeDesc.Transform[1][1] = 1.0f;
        cubeDesc.Transform[1][3] = m_dynamicCubeCenterY;
        cubeDesc.Transform[2][0] = -sine;
        cubeDesc.Transform[2][2] = cosine;
        cubeDesc.Transform[2][3] = m_dynamicCubePositionZ;
        cubeDesc.InstanceID = nextInstanceIndex;
        cubeDesc.InstanceMask =
            m_dynamicCubeVisible ? 0xFF : 0x00;
        cubeDesc.AccelerationStructure =
            m_dynamicCubeBottomLevelAS->GetGPUVirtualAddress();
    }

    m_currentInstanceTransforms.resize(instanceCount);
    for (UINT instanceIndex = 0;
         instanceIndex < instanceCount;
         ++instanceIndex)
    {
        std::memcpy(
            m_currentInstanceTransforms[instanceIndex].data(),
            instanceDescs[instanceIndex].Transform,
            sizeof(instanceDescs[instanceIndex].Transform));
    }

    m_instanceDescBuffers[frameIndex]->Unmap(0, nullptr);
    return true;
}

bool RayTracingManager::WritePreviousInstanceTransforms(UINT frameIndex)
{
    if (frameIndex >= c_tlasFrameCount ||
        !m_previousInstanceTransformBuffers[frameIndex] ||
        m_previousInstanceTransforms.empty())
    {
        return false;
    }

    void* mappedData = nullptr;
    const D3D12_RANGE readRange = { 0, 0 };
    HRESULT hr = m_previousInstanceTransformBuffers[frameIndex]->Map(
        0,
        &readRange,
        &mappedData);
    if (ReportFailure(hr, L"Previous instance transform mapping failed."))
        return false;

    const SIZE_T transformBytes =
        sizeof(std::array<float, 12>) *
        m_previousInstanceTransforms.size();
    std::memcpy(
        mappedData,
        m_previousInstanceTransforms.data(),
        transformBytes);
    const D3D12_RANGE writtenRange = { 0, transformBytes };
    m_previousInstanceTransformBuffers[frameIndex]->Unmap(
        0,
        &writtenRange);
    return true;
}

void RayTracingManager::UpdateDynamicObjectMotion()
{
    constexpr double framesPerSecond = 60.0;
    constexpr double motionStartSeconds = 20.0;
    constexpr double motionDurationSeconds = 5.0;
    constexpr double twoPi = 6.28318530717958647692;
    constexpr double radiansToDegrees = 57.2957795130823208768;

    const double timeSeconds =
        static_cast<double>(m_dynamicSceneFrameIndex) / framesPerSecond;
    double phase = 0.0;
    double phaseVelocity = 0.0;
    if (m_dynamicSphereDeterministicTimeline &&
        timeSeconds >= motionStartSeconds &&
        timeSeconds <= motionStartSeconds + motionDurationSeconds)
    {
        const double normalizedTime =
            (timeSeconds - motionStartSeconds) /
            motionDurationSeconds;
        phase = twoPi * normalizedTime;
        phaseVelocity = twoPi / motionDurationSeconds;
    }
    else if (!m_dynamicSphereDeterministicTimeline)
    {
        const double loopTime = std::fmod(timeSeconds, motionDurationSeconds);
        phase = twoPi * loopTime / motionDurationSeconds;
        phaseVelocity = twoPi / motionDurationSeconds;
    }

    double maximumLinearSpeed = 0.0;
    double maximumAngularSpeed = 0.0;
    if (m_hasDynamicSphere &&
        m_dynamicSphereVisible &&
        m_dynamicSphereAnimationEnabled)
    {
        const double position =
            static_cast<double>(m_dynamicSphereTrackCenterX) -
            static_cast<double>(m_dynamicSphereMotionAmplitude) *
            std::cos(phase);
        const double linearVelocity =
            static_cast<double>(m_dynamicSphereMotionAmplitude) *
            phaseVelocity *
            std::sin(phase);
        m_dynamicSpherePositionX = static_cast<float>(position);
        const double traveledDistance =
            position -
            (static_cast<double>(m_dynamicSphereTrackCenterX) -
             static_cast<double>(m_dynamicSphereMotionAmplitude));
        m_dynamicSphereRollRadians = static_cast<float>(
            -traveledDistance /
            (std::max)(
                static_cast<double>(m_dynamicSphereRadius),
                0.000001));
        maximumLinearSpeed = std::abs(linearVelocity);
        maximumAngularSpeed =
            maximumLinearSpeed /
            (std::max)(
                static_cast<double>(m_dynamicSphereRadius),
                0.000001) *
            radiansToDegrees;
    }

    if (m_hasDynamicCube &&
        m_dynamicCubeVisible &&
        m_dynamicCubeAnimationEnabled)
    {
        constexpr double cubeTravelX = 0.42;
        constexpr double cubeTravelZ = 0.24;
        m_dynamicCubePositionX = static_cast<float>(
            static_cast<double>(m_dynamicCubeCenterX) +
            cubeTravelX * std::sin(phase));
        m_dynamicCubePositionZ = static_cast<float>(
            static_cast<double>(m_dynamicCubeCenterZ) +
            cubeTravelZ * (std::cos(phase) - 1.0));
        m_dynamicCubeRotationY = static_cast<float>(phase);
        const double cubeLinearSpeed = phaseVelocity * std::sqrt(
            cubeTravelX * cubeTravelX *
                std::cos(phase) * std::cos(phase) +
            cubeTravelZ * cubeTravelZ *
                std::sin(phase) * std::sin(phase));
        maximumLinearSpeed =
            (std::max)(maximumLinearSpeed, cubeLinearSpeed);
        maximumAngularSpeed = (std::max)(
            maximumAngularSpeed,
            phaseVelocity * radiansToDegrees);
    }
    m_dynamicObjectLinearSpeed = maximumLinearSpeed;
    m_dynamicObjectAngularSpeed = maximumAngularSpeed;
    ++m_dynamicSceneFrameIndex;
}

bool RayTracingManager::DispatchSkinningAndUpdateBlases(
    ID3D12GraphicsCommandList4* commandList,
    UINT frameIndex)
{
    if (!m_gpuSkinningActive)
        return true;
    if (!commandList ||
        frameIndex >= c_tlasFrameCount ||
        !m_skinningRootSignature ||
        !m_skinningPipelineState ||
        !m_skinBindPoseVertexBuffer ||
        !m_skinInfluenceBuffer ||
        !m_skinJointMatrixBuffers[frameIndex] ||
        !m_previousSkinJointMatrixBuffers[frameIndex] ||
        !m_previousSkinnedPositionBuffer ||
        !m_vertexBuffer ||
        m_skinJointMatrices.empty() ||
        m_previousSkinJointMatrices.size() != m_skinJointMatrices.size())
    {
        return false;
    }

    void* mappedMatrices = nullptr;
    const D3D12_RANGE readRange = { 0, 0 };
    HRESULT hr = m_skinJointMatrixBuffers[frameIndex]->Map(
        0,
        &readRange,
        &mappedMatrices);
    if (ReportFailure(hr, L"Skin joint matrix palette mapping failed."))
        return false;
    const SIZE_T matrixBytes =
        sizeof(Matrix4) * m_skinJointMatrices.size();
    std::memcpy(
        mappedMatrices,
        m_skinJointMatrices.data(),
        matrixBytes);
    const D3D12_RANGE writtenRange = { 0, matrixBytes };
    m_skinJointMatrixBuffers[frameIndex]->Unmap(
        0,
        &writtenRange);

    void* mappedPreviousMatrices = nullptr;
    hr = m_previousSkinJointMatrixBuffers[frameIndex]->Map(
        0,
        &readRange,
        &mappedPreviousMatrices);
    if (ReportFailure(
        hr,
        L"Previous skin joint matrix palette mapping failed."))
    {
        return false;
    }
    std::memcpy(
        mappedPreviousMatrices,
        m_previousSkinJointMatrices.data(),
        matrixBytes);
    m_previousSkinJointMatrixBuffers[frameIndex]->Unmap(
        0,
        &writtenRange);

    ID3D12Resource* skinningOutputs[] =
    {
        m_vertexBuffer.Get(),
        m_previousSkinnedPositionBuffer.Get()
    };
    D3D12_RESOURCE_BARRIER toUnorderedAccess[_countof(skinningOutputs)] = {};
    for (UINT resourceIndex = 0;
         resourceIndex < _countof(skinningOutputs);
         ++resourceIndex)
    {
        toUnorderedAccess[resourceIndex].Type =
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUnorderedAccess[resourceIndex].Transition.pResource =
            skinningOutputs[resourceIndex];
        toUnorderedAccess[resourceIndex].Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        toUnorderedAccess[resourceIndex].Transition.StateAfter =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUnorderedAccess[resourceIndex].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(
        _countof(toUnorderedAccess),
        toUnorderedAccess);

    commandList->SetPipelineState(m_skinningPipelineState.Get());
    commandList->SetComputeRootSignature(m_skinningRootSignature.Get());
    commandList->SetComputeRootShaderResourceView(
        0,
        m_skinBindPoseVertexBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        1,
        m_skinInfluenceBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        2,
        m_skinJointMatrixBuffers[frameIndex]->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(
        3,
        m_previousSkinJointMatrixBuffers[frameIndex]->
            GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(
        4,
        m_vertexBuffer->GetGPUVirtualAddress());
    commandList->SetComputeRootUnorderedAccessView(
        5,
        m_previousSkinnedPositionBuffer->GetGPUVirtualAddress());

    for (const ImportedMeshInstance& instance :
         m_importedMeshInstances)
    {
        if (instance.skinIndex == c_invalidSceneSkinIndex)
            continue;
        for (const ScenePrimitiveRange& range :
             instance.skinVertexRanges)
        {
            if (range.vertexCount == 0u)
                continue;
            const UINT constants[4] =
            {
                range.vertexOffset,
                range.vertexCount,
                instance.skinJointMatrixOffset,
                0u
            };
            commandList->SetComputeRoot32BitConstants(
                6,
                _countof(constants),
                constants,
                0);
            commandList->Dispatch(
                (range.vertexCount + 63u) / 64u,
                1u,
                1u);
        }
    }

    D3D12_RESOURCE_BARRIER outputUavBarriers[_countof(skinningOutputs)] = {};
    D3D12_RESOURCE_BARRIER toShaderResource[_countof(skinningOutputs)] = {};
    for (UINT resourceIndex = 0;
         resourceIndex < _countof(skinningOutputs);
         ++resourceIndex)
    {
        outputUavBarriers[resourceIndex].Type =
            D3D12_RESOURCE_BARRIER_TYPE_UAV;
        outputUavBarriers[resourceIndex].UAV.pResource =
            skinningOutputs[resourceIndex];
        toShaderResource[resourceIndex].Type =
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toShaderResource[resourceIndex].Transition.pResource =
            skinningOutputs[resourceIndex];
        toShaderResource[resourceIndex].Transition.StateBefore =
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toShaderResource[resourceIndex].Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        toShaderResource[resourceIndex].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(
        _countof(outputUavBarriers),
        outputUavBarriers);
    commandList->ResourceBarrier(
        _countof(toShaderResource),
        toShaderResource);

    std::vector<D3D12_RESOURCE_BARRIER> blasBarriers;
    for (ImportedMeshBlas& blas : m_importedMeshBlases)
    {
        if (!blas.skinned)
            continue;
        if (!blas.accelerationStructure ||
            !blas.scratchBuffer ||
            blas.geometries.empty())
        {
            return false;
        }

        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometryDescs(
            blas.geometries.size());
        for (std::size_t geometryIndex = 0;
             geometryIndex < blas.geometries.size();
             ++geometryIndex)
        {
            const GeometryRange& geometry =
                blas.geometries[geometryIndex];
            D3D12_RAYTRACING_GEOMETRY_DESC& geometryDesc =
                geometryDescs[geometryIndex];
            geometryDesc.Type =
                D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometryDesc.Flags = geometry.containsAlphaMask
                ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE
                : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geometryDesc.Triangles.VertexBuffer.StartAddress =
                m_vertexBuffer->GetGPUVirtualAddress() +
                static_cast<UINT64>(geometry.vertexOffset) *
                    sizeof(SceneVertex);
            geometryDesc.Triangles.VertexBuffer.StrideInBytes =
                sizeof(SceneVertex);
            geometryDesc.Triangles.VertexCount = geometry.vertexCount;
            geometryDesc.Triangles.VertexFormat =
                DXGI_FORMAT_R32G32B32_FLOAT;
            geometryDesc.Triangles.IndexBuffer =
                m_indexBuffer->GetGPUVirtualAddress() +
                static_cast<UINT64>(geometry.indexOffset) *
                    sizeof(std::uint32_t);
            geometryDesc.Triangles.IndexCount = geometry.indexCount;
            geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
        buildDesc.Inputs.Type =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        buildDesc.Inputs.Flags =
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        buildDesc.Inputs.DescsLayout =
            D3D12_ELEMENTS_LAYOUT_ARRAY;
        buildDesc.Inputs.NumDescs =
            static_cast<UINT>(geometryDescs.size());
        buildDesc.Inputs.pGeometryDescs = geometryDescs.data();
        buildDesc.SourceAccelerationStructureData =
            blas.accelerationStructure->GetGPUVirtualAddress();
        buildDesc.DestAccelerationStructureData =
            blas.accelerationStructure->GetGPUVirtualAddress();
        buildDesc.ScratchAccelerationStructureData =
            blas.scratchBuffer->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(
            &buildDesc,
            0,
            nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = blas.accelerationStructure.Get();
        blasBarriers.push_back(barrier);
    }
    if (!blasBarriers.empty())
    {
        commandList->ResourceBarrier(
            static_cast<UINT>(blasBarriers.size()),
            blasBarriers.data());
    }
    m_dynamicObjectMovedThisFrame = true;
    m_skinningUpdatePending = false;
    return true;
}

bool RayTracingManager::UpdateTopLevelAccelerationStructure(
    ID3D12GraphicsCommandList4* commandList)
{
    m_dynamicObjectMovedThisFrame = false;
    const bool updateSceneAnimation =
        m_hasSceneAnimation && m_sceneAnimationEnabled;
    const bool updateSkinning =
        m_gpuSkinningActive &&
        (updateSceneAnimation || m_skinningUpdatePending);
    if (!updateSceneAnimation &&
        !updateSkinning &&
        !m_hasDynamicSphere &&
        !m_hasDynamicCube)
        return true;
    if (!commandList || !m_topLevelAS || !m_tlasScratchBuffer)
        return false;

    const bool visibilityChanged =
        m_dynamicSphereVisibilityDirty ||
        m_dynamicCubeVisibilityDirty;
    const float previousSpherePosition = m_dynamicSpherePositionX;
    const float previousSphereRoll = m_dynamicSphereRollRadians;
    const float previousCubePositionX = m_dynamicCubePositionX;
    const float previousCubePositionZ = m_dynamicCubePositionZ;
    const float previousCubeRotation = m_dynamicCubeRotationY;
    if (updateSceneAnimation)
    {
        if (m_gpuSkinningActive)
            m_previousSkinJointMatrices = m_skinJointMatrices;
        m_sceneAnimationTimeSeconds += m_frameDeltaSeconds;
        if (!EvaluateSceneAnimation(m_sceneAnimationTimeSeconds))
            return false;
    }
    if (m_hasDynamicSphere || m_hasDynamicCube)
        UpdateDynamicObjectMotion();
    m_previousInstanceTransforms = m_currentInstanceTransforms;

    const UINT descriptorFrame =
        static_cast<UINT>(m_frameIndex % c_tlasFrameCount);
    if (updateSkinning &&
        !DispatchSkinningAndUpdateBlases(
            commandList,
            descriptorFrame))
    {
        return false;
    }
    if (!WritePreviousInstanceTransforms(descriptorFrame))
        return false;

    bool sceneAnimationTransformChanged = false;
    if (m_hasSceneAnimation && m_useImportedMeshInstances)
    {
        if (m_currentInstanceTransforms.size() <
            m_importedMeshInstances.size())
            sceneAnimationTransformChanged = true;
        for (std::size_t instanceIndex = 0;
             !sceneAnimationTransformChanged &&
             instanceIndex < m_importedMeshInstances.size(); ++instanceIndex)
        {
            for (std::size_t component = 0; component < 12u; ++component)
            {
                if (std::abs(
                    m_currentInstanceTransforms[instanceIndex][component] -
                    m_importedMeshInstances[instanceIndex].
                        transform[component]) > 1.0e-7f)
                {
                    sceneAnimationTransformChanged = true;
                    break;
                }
            }
        }
    }
    else if (m_hasSceneAnimation)
    {
        sceneAnimationTransformChanged =
            m_currentInstanceTransforms.empty();
        for (std::size_t component = 0;
             !sceneAnimationTransformChanged && component < 12u;
             ++component)
        {
            sceneAnimationTransformChanged = std::abs(
                m_currentInstanceTransforms[0][component] -
                m_sceneAnimationInstanceTransform[component]) > 1.0e-7f;
        }
    }
    // A skinned BLAS can change its bounds even though its TLAS instance
    // transform remains the identity. Refit the TLAS after skinning so it
    // does not keep culling rays against the bind-pose instance bounds.
    const bool transformChanged =
        updateSkinning ||
        sceneAnimationTransformChanged ||
        std::abs(
            previousSpherePosition - m_dynamicSpherePositionX) > 1.0e-7f ||
        std::abs(
            previousSphereRoll - m_dynamicSphereRollRadians) > 1.0e-7f ||
        std::abs(
            previousCubePositionX - m_dynamicCubePositionX) > 1.0e-7f ||
        std::abs(
            previousCubePositionZ - m_dynamicCubePositionZ) > 1.0e-7f ||
        std::abs(
            previousCubeRotation - m_dynamicCubeRotationY) > 1.0e-7f;
    if (!visibilityChanged && !transformChanged)
    {
        return true;
    }

    if (!WriteInstanceDescriptors(descriptorFrame))
    {
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc = {};
    buildDesc.Inputs.Type =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    buildDesc.Inputs.Flags =
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    buildDesc.Inputs.NumDescs =
        GetStaticInstanceCount() +
        (m_hasDynamicSphere ? 1u : 0u) +
        (m_hasDynamicCube ? 1u : 0u);
    buildDesc.Inputs.InstanceDescs =
        m_instanceDescBuffers[descriptorFrame]->GetGPUVirtualAddress();
    buildDesc.SourceAccelerationStructureData =
        m_topLevelAS->GetGPUVirtualAddress();
    buildDesc.DestAccelerationStructureData =
        m_topLevelAS->GetGPUVirtualAddress();
    buildDesc.ScratchAccelerationStructureData =
        m_tlasScratchBuffer->GetGPUVirtualAddress();

    commandList->BuildRaytracingAccelerationStructure(
        &buildDesc,
        0,
        nullptr);
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = m_topLevelAS.Get();
    commandList->ResourceBarrier(1, &barrier);
    m_dynamicSphereVisibilityDirty = false;
    m_dynamicCubeVisibilityDirty = false;
    m_dynamicObjectMovedThisFrame = true;
    return true;
}

bool RayTracingManager::ExecuteBuildCommandListAndWait()
{
    HRESULT hr = m_buildCommandList->Close();
    if (ReportFailure(hr, L"AS build command list close failed."))
        return false;

    ID3D12CommandList* commandLists[] = { m_buildCommandList.Get() };
    m_buildCommandQueue->ExecuteCommandLists(1, commandLists);

    const UINT64 fenceToWaitFor = ++m_buildFenceValue;
    hr = m_buildCommandQueue->Signal(m_buildFence.Get(), fenceToWaitFor);
    if (ReportFailure(hr, L"AS build fence signal failed."))
        return false;

    if (m_buildFence->GetCompletedValue() < fenceToWaitFor)
    {
        hr = m_buildFence->SetEventOnCompletion(fenceToWaitFor, m_buildFenceEvent);
        if (ReportFailure(hr, L"AS build fence event setup failed."))
            return false;

        WaitForSingleObject(m_buildFenceEvent, INFINITE);
    }

    return true;
}

bool RayTracingManager::CreateUploadBuffer(
    const void* data,
    UINT64 sizeInBytes,
    const wchar_t* debugName,
    Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC bufferDesc = CreateBufferDesc(sizeInBytes);

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (ReportFailure(hr, L"Upload buffer creation failed."))
        return false;

    resource->SetName(debugName);

    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    hr = resource->Map(0, &readRange, &mappedData);
    if (ReportFailure(hr, L"Upload buffer mapping failed."))
        return false;

    std::memcpy(mappedData, data, static_cast<std::size_t>(sizeInBytes));
    resource->Unmap(0, nullptr);

    return true;
}

bool RayTracingManager::CreateAccelerationStructureBuffer(
    UINT64 sizeInBytes,
    const wchar_t* debugName,
    Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
    const UINT64 alignedSize = AlignUp64(
        sizeInBytes,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC bufferDesc = CreateBufferDesc(
        alignedSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (ReportFailure(hr, L"Acceleration structure buffer creation failed."))
        return false;

    resource->SetName(debugName);
    return true;
}

bool RayTracingManager::CreateScratchBuffer(
    UINT64 sizeInBytes,
    const wchar_t* debugName,
    Microsoft::WRL::ComPtr<ID3D12Resource>& resource)
{
    const UINT64 alignedSize = AlignUp64(
        sizeInBytes,
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    const D3D12_HEAP_PROPERTIES heapProperties = CreateHeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC bufferDesc = CreateBufferDesc(
        alignedSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        nullptr,
        IID_PPV_ARGS(&resource));
    if (ReportFailure(hr, L"Scratch buffer creation failed."))
        return false;

    resource->SetName(debugName);
    return true;
}

bool RayTracingManager::LoadCompiledShader(std::vector<std::uint8_t>& shaderBytes) const
{
    const std::wstring shaderPath = GetCompiledShaderPath();
    if (ReadBinaryFile(shaderPath, shaderBytes))
        return true;

    std::wstring message = L"Compiled raytracing shader was not found.\n";
    message += L"Expected: ";
    message += shaderPath;
    message += L"\nBuild the project once so the HLSL custom build step can create it.";
    ReportMessage(message);
    return false;
}

bool RayTracingManager::LoadCompiledAtrousShader(
    std::vector<std::uint8_t>& shaderBytes) const
{
    const std::wstring shaderPath = GetCompiledAtrousShaderPath();
    if (ReadBinaryFile(shaderPath, shaderBytes))
        return true;

    std::wstring message = L"Compiled A-Trous shader was not found.\n";
    message += L"Expected: ";
    message += shaderPath;
    message +=
        L"\nBuild the project once so the HLSL custom build step can create it.";
    ReportMessage(message);
    return false;
}

bool RayTracingManager::LoadCompiledTemporalColorClipShader(
    std::vector<std::uint8_t>& shaderBytes) const
{
    const std::wstring shaderPath =
        GetCompiledTemporalColorClipShaderPath();
    if (ReadBinaryFile(shaderPath, shaderBytes))
        return true;

    std::wstring message =
        L"Compiled temporal color clip shader was not found.\n";
    message += L"Expected: ";
    message += shaderPath;
    message +=
        L"\nBuild the project once so the HLSL custom build step can create it.";
    ReportMessage(message);
    return false;
}

bool RayTracingManager::LoadCompiledSkinningShader(
    std::vector<std::uint8_t>& shaderBytes) const
{
    const std::wstring shaderPath = GetCompiledSkinningShaderPath();
    if (ReadBinaryFile(shaderPath, shaderBytes))
        return true;

    std::wstring message = L"Compiled skinning shader was not found. Expected: ";
    message += shaderPath;
    ReportMessage(message);
    return false;
}

bool RayTracingManager::ReadBinaryFile(const std::wstring& path, std::vector<std::uint8_t>& bytes) const
{
    ScopedFileHandle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    if (!file.IsValid())
        return false;

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(file.value, &fileSize) || fileSize.QuadPart <= 0)
        return false;

    bytes.resize(static_cast<std::size_t>(fileSize.QuadPart));

    DWORD bytesRead = 0;
    const BOOL readSucceeded = ReadFile(
        file.value,
        bytes.data(),
        static_cast<DWORD>(bytes.size()),
        &bytesRead,
        nullptr);

    return readSucceeded && bytesRead == bytes.size();
}

std::wstring RayTracingManager::GetEnvironmentMapPath() const
{
    std::wstring modulePath(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, &modulePath[0], static_cast<DWORD>(modulePath.size()));
    while (length == modulePath.size())
    {
        modulePath.resize(modulePath.size() * 2);
        length = GetModuleFileNameW(nullptr, &modulePath[0], static_cast<DWORD>(modulePath.size()));
    }

    modulePath.resize(length);
    const std::wstring::size_type slash = modulePath.find_last_of(L"\\/");
    const std::wstring executableDir = slash == std::wstring::npos
        ? std::wstring()
        : modulePath.substr(0, slash + 1);

    return executableDir + c_environmentMapRelativePath;
}
std::wstring RayTracingManager::GetCompiledShaderPath() const
{
    std::wstring modulePath(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, &modulePath[0], static_cast<DWORD>(modulePath.size()));
    while (length == modulePath.size())
    {
        modulePath.resize(modulePath.size() * 2);
        length = GetModuleFileNameW(nullptr, &modulePath[0], static_cast<DWORD>(modulePath.size()));
    }

    modulePath.resize(length);
    const std::wstring::size_type slash = modulePath.find_last_of(L"\\/");
    const std::wstring executableDir = slash == std::wstring::npos
        ? std::wstring()
        : modulePath.substr(0, slash + 1);

    return executableDir + c_compiledShaderRelativePath;
}

std::wstring RayTracingManager::GetCompiledAtrousShaderPath() const
{
    std::wstring modulePath(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(
        nullptr,
        &modulePath[0],
        static_cast<DWORD>(modulePath.size()));
    while (length == modulePath.size())
    {
        modulePath.resize(modulePath.size() * 2);
        length = GetModuleFileNameW(
            nullptr,
            &modulePath[0],
            static_cast<DWORD>(modulePath.size()));
    }

    modulePath.resize(length);
    const std::wstring::size_type slash =
        modulePath.find_last_of(L"\\/");
    const std::wstring executableDir = slash == std::wstring::npos
        ? std::wstring()
        : modulePath.substr(0, slash + 1);
    return executableDir + c_compiledAtrousShaderRelativePath;
}

std::wstring RayTracingManager::GetCompiledTemporalColorClipShaderPath() const
{
    std::wstring modulePath(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(
        nullptr,
        &modulePath[0],
        static_cast<DWORD>(modulePath.size()));
    while (length == modulePath.size())
    {
        modulePath.resize(modulePath.size() * 2);
        length = GetModuleFileNameW(
            nullptr,
            &modulePath[0],
            static_cast<DWORD>(modulePath.size()));
    }

    modulePath.resize(length);
    const std::wstring::size_type slash =
        modulePath.find_last_of(L"\\/");
    const std::wstring executableDir = slash == std::wstring::npos
        ? std::wstring()
        : modulePath.substr(0, slash + 1);
    return executableDir + c_compiledTemporalColorClipShaderRelativePath;
}

std::wstring RayTracingManager::GetCompiledSkinningShaderPath() const
{
    std::wstring modulePath(MAX_PATH, static_cast<wchar_t>(0));
    DWORD length = GetModuleFileNameW(
        nullptr,
        &modulePath[0],
        static_cast<DWORD>(modulePath.size()));
    while (length == modulePath.size())
    {
        modulePath.resize(modulePath.size() * 2);
        length = GetModuleFileNameW(
            nullptr,
            &modulePath[0],
            static_cast<DWORD>(modulePath.size()));
    }

    modulePath.resize(length);
    const std::wstring::size_type slash =
        modulePath.find_last_of(L"/");
    const std::wstring::size_type backslash =
        modulePath.find_last_of(static_cast<wchar_t>(92));
    const std::wstring::size_type separator =
        slash == std::wstring::npos
            ? backslash
            : (backslash == std::wstring::npos
                ? slash
                : (std::max)(slash, backslash));
    const std::wstring executableDir =
        separator == std::wstring::npos
            ? std::wstring()
            : modulePath.substr(0, separator + 1);
    return executableDir + c_compiledSkinningShaderRelativePath;
}

bool RayTracingManager::ReportFailure(HRESULT hr, const wchar_t* message) const
{
    if (SUCCEEDED(hr))
        return false;

    std::wostringstream text;
    text << message
         << L"\nHRESULT: 0x"
         << std::uppercase
         << std::hex
         << std::setw(8)
         << std::setfill(L'0')
         << static_cast<unsigned int>(hr);

    MessageBoxW(m_hWnd, text.str().c_str(), L"DXR Error", MB_OK | MB_ICONERROR);
    return true;
}

void RayTracingManager::ReportMessage(const std::wstring& message) const
{
    MessageBoxW(m_hWnd, message.c_str(), L"DXR Error", MB_OK | MB_ICONERROR);
}




