// Pyxis M1 triangle — fragment shader.

struct PsIn
{
    float4 position : SV_POSITION;
    float3 color    : COLOR0;
};

[shader("fragment")]
float4 main(PsIn input) : SV_TARGET
{
    return float4(input.color, 1.0);
}
