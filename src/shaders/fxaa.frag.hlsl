// FXAA over the finished world image, after the game's own outline pass.
//
// The outlines are drawn per pixel from resolved depth, so multisampling cannot
// smooth them; only a pass over the final colour can. Alpha is carried through
// because the guest composites this image with blending.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 0)]] Texture2D<float4> sourceTexture : register(t0);
[[vk::binding(1, 0)]] SamplerState sourceSampler : register(s1);

[[vk::push_constant]]
struct Constants {
    float2 texel;
    float2 pad;
} constants;

static const float kEdgeThreshold = 0.125;
static const float kEdgeThresholdMin = 0.0312;
static const float kSubpixel = 0.75;
static const int kSearchSteps = 10;
static const float kSearchStride[kSearchSteps] = {1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0};

float Luma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

float LumaAt(float2 uv) {
    return Luma(sourceTexture.SampleLevel(sourceSampler, uv, 0).rgb);
}

float4 PSMain(VSOutput input) : SV_TARGET {
    const float2 uv = input.uv;
    const float2 t = constants.texel;
    const float4 centre = sourceTexture.SampleLevel(sourceSampler, uv, 0);

    const float lumaM = Luma(centre.rgb);
    const float lumaN = LumaAt(uv + float2(0.0, -t.y));
    const float lumaS = LumaAt(uv + float2(0.0, t.y));
    const float lumaW = LumaAt(uv + float2(-t.x, 0.0));
    const float lumaE = LumaAt(uv + float2(t.x, 0.0));

    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float range = lumaMax - lumaMin;
    if (range < max(kEdgeThresholdMin, lumaMax * kEdgeThreshold))
        return centre;

    const float lumaNW = LumaAt(uv + float2(-t.x, -t.y));
    const float lumaNE = LumaAt(uv + float2(t.x, -t.y));
    const float lumaSW = LumaAt(uv + float2(-t.x, t.y));
    const float lumaSE = LumaAt(uv + float2(t.x, t.y));

    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;
    const float lumaWCorners = lumaNW + lumaSW;
    const float lumaECorners = lumaNE + lumaSE;
    const float lumaNCorners = lumaNW + lumaNE;
    const float lumaSCorners = lumaSW + lumaSE;

    const float edgeH = abs(-2.0 * lumaW + lumaWCorners) + abs(-2.0 * lumaM + lumaNS) * 2.0 +
                        abs(-2.0 * lumaE + lumaECorners);
    const float edgeV = abs(-2.0 * lumaN + lumaNCorners) + abs(-2.0 * lumaM + lumaWE) * 2.0 +
                        abs(-2.0 * lumaS + lumaSCorners);
    const bool horizontal = edgeH >= edgeV;

    // Which side of the pixel the edge lies on.
    const float luma1 = horizontal ? lumaN : lumaW;
    const float luma2 = horizontal ? lumaS : lumaE;
    const float gradient1 = luma1 - lumaM;
    const float gradient2 = luma2 - lumaM;
    const bool steepest1 = abs(gradient1) >= abs(gradient2);
    const float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float stepLength = horizontal ? t.y : t.x;
    float lumaLocalAverage;
    if (steepest1) {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaM);
    } else {
        lumaLocalAverage = 0.5 * (luma2 + lumaM);
    }

    float2 edgeUv = uv;
    if (horizontal)
        edgeUv.y += stepLength * 0.5;
    else
        edgeUv.x += stepLength * 0.5;

    // Walk along the edge both ways until its contrast ends.
    const float2 offset = horizontal ? float2(t.x, 0.0) : float2(0.0, t.y);
    float2 uv1 = edgeUv - offset;
    float2 uv2 = edgeUv + offset;
    float lumaEnd1 = LumaAt(uv1) - lumaLocalAverage;
    float lumaEnd2 = LumaAt(uv2) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;

    [loop]
    for (int i = 0; i < kSearchSteps && !(reached1 && reached2); ++i) {
        if (!reached1) {
            uv1 -= offset * kSearchStride[i];
            lumaEnd1 = LumaAt(uv1) - lumaLocalAverage;
            reached1 = abs(lumaEnd1) >= gradientScaled;
        }
        if (!reached2) {
            uv2 += offset * kSearchStride[i];
            lumaEnd2 = LumaAt(uv2) - lumaLocalAverage;
            reached2 = abs(lumaEnd2) >= gradientScaled;
        }
    }

    const float distance1 = horizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    const float distance2 = horizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    const bool nearer1 = distance1 < distance2;
    const float distanceFinal = min(distance1, distance2);
    const float edgeLength = distance1 + distance2;
    const bool centreSmaller = lumaM < lumaLocalAverage;
    const bool correctVariation = ((nearer1 ? lumaEnd1 : lumaEnd2) < 0.0) != centreSmaller;
    const float edgeOffset = correctVariation ? -distanceFinal / edgeLength + 0.5 : 0.0;

    // Subpixel aliasing, for features thinner than the edge search sees.
    const float lumaAverage =
        (1.0 / 12.0) * (2.0 * (lumaNS + lumaWE) + lumaWCorners + lumaECorners);
    const float subpixel1 = saturate(abs(lumaAverage - lumaM) / range);
    const float subpixel2 = (-2.0 * subpixel1 + 3.0) * subpixel1 * subpixel1;
    const float subpixelOffset = subpixel2 * subpixel2 * kSubpixel;

    const float finalOffset = max(edgeOffset, subpixelOffset);
    float2 finalUv = uv;
    if (horizontal)
        finalUv.y += finalOffset * stepLength;
    else
        finalUv.x += finalOffset * stepLength;
    return float4(sourceTexture.SampleLevel(sourceSampler, finalUv, 0).rgb, centre.a);
}
