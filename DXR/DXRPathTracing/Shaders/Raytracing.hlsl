RWTexture2D<float4> g_output : register(u0);

[shader("raygeneration")]
void MyRaygenShader_RadianceRay()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    float2 uv = (float2(launchIndex) + 0.5f) / float2(launchDim);

    g_output[launchIndex] = float4(uv, 0.0f, 1.0f);
}