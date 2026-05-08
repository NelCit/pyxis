// Pyxis renderer — miss shader.
//
// Plan §41 M3. Slang [shader("miss")] + Slang.cmake `-stage miss`.
//
// Sets `payload.color` to a flat sky colour and `payload.hitT = -1.0`
// (the miss sentinel) so the raygen shader can branch on miss vs
// closest-hit if it wants to. M3 ships a constant gradient-free
// sky; the dome-light path that samples a lat-long EXR arrives at
// M7 (§7), driven by GpuScene::AddLight with `LightDesc::Kind::Dome`.

#include "ShaderInterop.slang"

[shader("miss")]
void MissMain(inout HitInfo payload) {
    payload.color = float3(0.55f, 0.65f, 0.85f);   // overcast-sky blue.
    payload.hitT  = -1.0f;
}
