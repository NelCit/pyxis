// Pyxis renderer -- linear -> sRGB present blit (compute).
//
// The viewer's display present-encode, split out of SsaaResolvePass so each pass
// has one job. Reads the base-res LINEAR color (either the path-tracer color AOV
// directly at SSAA factor 1, or the SsaaResolvePass downsampled LINEAR output at
// factor > 1) and writes the sRGB-OETF-encoded value into the present target,
// which is then raw-copied into the sRGB swapchain backbuffer and displays
// correctly. This is the SAME single sRGB encode that pyxis.exe's PNG writer and
// the Kit pxr viewport (HdxColorCorrectionTask) apply, so the standalone viewer
// matches headless / Omniverse byte-for-byte.
//
// Only the standalone viewer binds the present target; Kit's PyxisEngine never
// does, so Omniverse output is unaffected. Thread per output texel.

[[vk::binding(0, 0)]] Texture2D<float4>   gSource;   // base-res LINEAR color
[[vk::binding(1, 0)]] RWTexture2D<float4> gDest;     // base-res sRGB-encoded output

struct BlitToSrgbUniforms {
    uint destWidth;
    uint destHeight;
    uint _pad0;
    uint _pad1;
};
[[vk::binding(2, 0)]] ConstantBuffer<BlitToSrgbUniforms> gParams;

float LinearToSrgb(float val) {
    return (val <= 0.0031308f) ? (val * 12.92f)
                               : (1.055f * pow(val, 1.0f / 2.4f) - 0.055f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gParams.destWidth || tid.y >= gParams.destHeight)
        return;
    const float4 lin = gSource.Load(int3(int2(tid.xy), 0));
    // sRGB-encode RGB; pass alpha through linearly.
    gDest[tid.xy] = float4(LinearToSrgb(lin.r), LinearToSrgb(lin.g), LinearToSrgb(lin.b), lin.a);
}
