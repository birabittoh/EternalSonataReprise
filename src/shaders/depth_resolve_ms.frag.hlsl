// Pixel shader for the depth resolve out of a multisampled depth image.
//
// Sample 0 rather than an average: depth averaged across an edge is a depth no
// surface has, and the outline pass would draw a line along every edge of it.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2DMS<float4> sourceTexture : register(t0);

float PSMain(VSOutput input) : SV_TARGET {
    uint width, height, samples;
    sourceTexture.GetDimensions(width, height, samples);
    const int2 texel = int2(input.uv * float2(width, height));
    return sourceTexture.Load(texel, 0).r;
}
