struct Vertex
{
    float3 position;
    float3 normal;
    float2 texCoord;
    float4 tangent;
};

struct SkinInfluence
{
    uint4 jointIndices;
    float4 jointWeights;
};

StructuredBuffer<Vertex> g_bindPoseVertices : register(t0);
StructuredBuffer<SkinInfluence> g_skinInfluences : register(t1);
StructuredBuffer<float4x4> g_jointMatrices : register(t2);
RWStructuredBuffer<Vertex> g_skinnedVertices : register(u0);

cbuffer SkinningConstants : register(b0)
{
    uint g_vertexOffset;
    uint g_vertexCount;
    uint g_jointMatrixOffset;
    uint g_padding;
};

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_vertexCount)
        return;

    const uint vertexIndex = g_vertexOffset + dispatchThreadId.x;
    const Vertex bindVertex = g_bindPoseVertices[vertexIndex];
    const SkinInfluence influence = g_skinInfluences[vertexIndex];
    const float weightSum =
        influence.jointWeights.x + influence.jointWeights.y +
        influence.jointWeights.z + influence.jointWeights.w;
    if (weightSum <= 1.0e-8f)
    {
        g_skinnedVertices[vertexIndex] = bindVertex;
        return;
    }

    float4 skinnedPosition = 0.0f;
    float3 skinnedNormal = 0.0f;
    float3 skinnedTangent = 0.0f;
    [unroll]
    for (uint component = 0u; component < 4u; ++component)
    {
        const float weight = influence.jointWeights[component] / weightSum;
        if (weight <= 0.0f)
            continue;

        const float4x4 jointMatrix = g_jointMatrices[
            g_jointMatrixOffset + influence.jointIndices[component]];
        skinnedPosition += weight * mul(
            jointMatrix,
            float4(bindVertex.position, 1.0f));
        skinnedNormal += weight * mul(
            (float3x3)jointMatrix,
            bindVertex.normal);
        skinnedTangent += weight * mul(
            (float3x3)jointMatrix,
            bindVertex.tangent.xyz);
    }

    Vertex result = bindVertex;
    result.position = skinnedPosition.xyz /
        max(abs(skinnedPosition.w), 1.0e-8f);
    result.normal = normalize(skinnedNormal);
    result.tangent.xyz = normalize(skinnedTangent);
    g_skinnedVertices[vertexIndex] = result;
}
