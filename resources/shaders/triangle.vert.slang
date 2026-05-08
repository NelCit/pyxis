// Pyxis M1 triangle — vertex shader.
//
// No vertex buffer; positions + colours are inlined and indexed by
// SV_VertexID. Output goes through a colour interpolant to the
// fragment shader. Plan §10 / §23 — row-major matrices, row-vector
// multiplication (no matrices used here, but the convention is set).

struct VsOut
{
    float4 position : SV_POSITION;
    float3 color    : COLOR0;
};

[shader("vertex")]
VsOut main(uint vid : SV_VertexID)
{
    static const float2 positions[3] = {
        float2( 0.0,  0.6),
        float2(-0.6, -0.6),
        float2( 0.6, -0.6),
    };
    static const float3 colors[3] = {
        float3(1.0, 0.0, 0.0),
        float3(0.0, 1.0, 0.0),
        float3(0.0, 0.0, 1.0),
    };

    VsOut o;
    o.position = float4(positions[vid], 0.0, 1.0);
    o.color    = colors[vid];
    return o;
}
