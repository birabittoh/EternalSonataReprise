// Pixel shader for the final present blit.
//
// Applies the display ramp the guest's D3D library uploads on every swap when
// the title does not set one: sRGB decode followed by the BT.709 encode that
// VdGetCurrentDisplayGamma (type 2) asks for. See sub_8226A008 and sub_82269F58.
// The other blits copy guest images and must stay linear.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState sourceSampler : register(s1);

float3 DisplayRamp(float3 c) {
    c = saturate(c);
    float3 lin = select(c > 0.04045, pow((c + 0.055) / 1.055, 2.4), c / 12.92);
    return select(lin < 0.018, lin * 4.5, 1.099 * pow(lin, 0.45) - 0.099);
}

float4 PSMain(VSOutput input) : SV_TARGET {
    return float4(DisplayRamp(sourceTexture.Sample(sourceSampler, input.uv).rgb), 1.0);
}
