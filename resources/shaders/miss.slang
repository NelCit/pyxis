// Pyxis renderer — miss shader.
//
// Plan §41 M7-simple. Reads the M7 lights buffer at binding 5 to
// find the FIRST authored Dome light and emits its `color × intensity`
// as the background — replacing the M3-era hardcoded sky blue.
// Falls back to a soft sky-blue when no dome is authored (preserves
// the M3/M5/M6 fixture renders that don't ship a Dome light).
//
// **No HDRI lat-long sampling here yet** — that's the M7-full /
// IBL importance sampler the user fills in alongside the closesthit
// per §7. Once it lands, this shader will sample the dome's
// `envMapTex` (already declared in `LightGpu`) at the ray direction's
// spherical coords. Until then the dome's authored color is the
// best signal we have, and at least the background follows what the
// `.usd` actually requests.

#include "ShaderInterop.slang"

[[vk::binding(5, 0)]] StructuredBuffer<LightGpu> gLights;

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in raygen.slang.
[shader("miss")]
void main(inout HitInfo payload) {
    // M3 fallback colour — soft sky-blue. Used when no authored
    // dome is found (M3 cube fixture, M5 three-material fixture,
    // M6 instanced grid all hit this path).
    float3 background = float3(0.55, 0.65, 0.85);

    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    for (uint i = 0; i < lightCount; ++i) {
        const LightGpu L = gLights[i];
        if (L.intensity <= 0.0) continue;        // sentinel / disabled
        if (L.kind != LIGHT_KIND_DOME) continue; // first dome wins
        background = float3(L.colorR, L.colorG, L.colorB) * L.intensity;
        break;
    }

    payload.color = background;
    payload.hitT  = -1.0f;  // miss sentinel — raygen branches on this.
}
