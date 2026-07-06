// Pyxis renderer — DLSS Stage 2a depth conversion (one pass = one shader).
//
// Streamline's DLSS-SR plugin reports kBufferTypeDepth (standard,
// projective, normalized-device depth) as a REQUIRED buffer tag —
// verified empirically via slGetFeatureRequirements(kFeatureDLSS) on this
// machine (Private/Dlss/DlssProvider.cpp's Initialize() log line: "4
// required buffer tag(s): 0 1 3 4", where 0 == sl::kBufferTypeDepth).
// kBufferTypeLinearDepth (49), the "either this or kBufferTypeDepth"
// alternative sl_core_types.h documents, is NOT in that required list —
// tagging with it instead produced sl::Result::eErrorMissingInputParameter
// from slEvaluateFeature (the actually-required tag was simply absent).
//
// Pyxis has no rasterizer depth buffer to reuse (it is a pure ray tracer —
// RaytracedGBufferPass's gViewZ is already-linear view-space distance,
// "0 on miss"). This tiny compute pass converts it to the standard
// Vulkan-style normalized device depth (near -> 0, far -> 1, matching
// `depthInverted = false` in DlssProvider's sl::Constants):
//   depth = far * (viewZ - near) / (viewZ * (far - near))
// Miss pixels (viewZ <= 0) map to the far plane (depth = 1) rather than
// evaluating the formula at viewZ = 0 (which would read as "at the
// camera", i.e. nearest — exactly backwards for a background pixel).
//
// Owned by DlssPass (Private/Passes/DlssPass.cpp) — dispatched once per
// frame, at render resolution, immediately before DlssProvider::Evaluate.
// Bindings (single Set 0, matched to DlssPass's own binding layout):
//   0 : Texture2D<float>     gViewZ    (SRV, RaytracedGBufferPass's output)
//   1 : RWTexture2D<float>   gDlssDepth (UAV, this pass's own output)
//   2 : ConstantBuffer<DlssDepthUniforms>

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float> gViewZ;
[[vk::binding(1, 0)]] RWTexture2D<float> gDlssDepth;
[[vk::binding(2, 0)]] ConstantBuffer<DlssDepthUniforms> gParams;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gParams.width || dispatchThreadId.y >= gParams.height)
        return;

    const uint2 pixel = dispatchThreadId.xy;
    const float viewZ = gViewZ[pixel];

    float depth;
    if (viewZ <= 0.0f)
    {
        depth = 1.0f;  // miss -> far plane, not "at the camera".
    }
    else
    {
        const float nearClip = gParams.nearClip;
        const float farClip = gParams.farClip;
        depth = (farClip * (viewZ - nearClip)) / (viewZ * (farClip - nearClip));
        depth = saturate(depth);
    }

    gDlssDepth[pixel] = depth;
}
