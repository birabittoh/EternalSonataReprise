// Pixel shader for the depth resolve.
//
// Point sampled, and the only channel that matters: the destination is
// R32_FLOAT and what the guest reads back out of it is the depth value itself,
// which the outline pass then takes a second difference of. Filtering it would
// invent depths that no fragment ever wrote.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState sourceSampler : register(s1);

float PSMain(VSOutput input) : SV_TARGET {
    return sourceTexture.Sample(sourceSampler, input.uv).r;
}
