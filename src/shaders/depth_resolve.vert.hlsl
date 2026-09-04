// Vertex shader for the depth resolve.
//
// The depth target carries a stencil plane, so a resolve of it into the guest's
// R32_FLOAT destination cannot be a copy: the two formats are in different copy
// groups on both APIs. It is this draw instead, and the source rectangle it has
// to honour arrives as push constants because the covering triangle is
// generated from SV_VertexID and has no vertex buffer to carry it.
//
// The compiler is invoked with -fvk-invert-y for vertex shaders, so this is
// written for the D3D clip space convention.

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

[[vk::push_constant]]
struct Constants {
    float2 uvScale;
    float2 uvOffset;
} constants;

VSOutput VSMain(uint id : SV_VertexID) {
    const float2 corner = float2(float((id << 1) & 2), float(id & 2));
    VSOutput output;
    output.uv = corner * constants.uvScale + constants.uvOffset;
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}
