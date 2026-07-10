// Pyxis renderer — DlssProvider (DLSS Stage 2a: real Streamline hookup).
//
// rtx-realtime-alignment-design.md, "DLSS — corrected stance" + "DLSS
// scope includes upscaling" (owner, 2026-07-05). Stage 1 shipped DLL
// discovery + symbol resolution with NO slInit call (device interop
// wasn't available yet). Stage 2a adds:
//   - Initialize(): called once by PyxisRenderer's ctor with the
//     VulkanContext the device manager already created the VkInstance/
//     VkDevice with (pyxis_platform's VkDeviceManager(Headless) already
//     ran the PRE-device half of the Streamline bootstrap -- slInit +
//     slGetFeatureRequirements, see Device/DlssDeviceExtensions.h in
//     pyxis_platform -- because ProgrammingGuideManualHooking.md 3.0 is
//     explicit that for Vulkan "device still must be created AFTER the
//     slInit call". DlssProvider therefore does NOT call slInit itself;
//     it only calls slSetVulkanInfo (needs the now-existing device/queue)
//     and slIsFeatureSupported, then resolves the per-frame function
//     pointers via slGetFeatureFunction.
//   - GetOptimalRenderResolution(): slDLSSGetOptimalSettings wrapper.
//   - Evaluate(): the per-frame slGetNewFrameToken -> slSetConstants ->
//     slDLSSSetOptions -> slSetTagForFrame -> slEvaluateFeature sequence
//     (ProgrammingGuideDLSS.md sections 3-7), run from DlssPass::Execute.
//
// Deliberately Vulkan/NVRHI-agnostic in ITS OWN header (raw void*/uint32_t
// fields on FrameInputs/TaggedImage, mirroring VulkanContext's own
// convention) so this file doesn't need <vulkan/vulkan.h> or nvrhi.h --
// DlssPass (Private/Passes/DlssPass.cpp) owns every NVRHI resource-state
// transition and hands this class raw native handles + dimensions only.
// DlssProvider.cpp DOES include <vulkan/vulkan.h> (transitively available
// via Pyxis::Platform -> Pyxis::Vulkan) to build the sl::Resource /
// sl::VulkanInfo structs the vendored Streamline headers require.
//
// Never vendors Streamline SDK BINARIES (NVIDIA RTX SDKs License §4(e) --
// see this header's original file comment); the interposer + NGX plugin
// DLLs are always runtime-loaded from OUTSIDE the repo. The vendored
// HEADERS (thirdparty/streamline/include/, MIT-licensed, LICENSE.txt
// alongside) are the only Streamline material that lives in the repo.
//
// Private-only — this type never crosses the pyxis_renderer.dll boundary.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pyxis {

struct VulkanContext;  // Pyxis::Platform, Public/Pyxis/Platform/Device/VulkanContext.h

class DlssProvider final {
 public:
  // Which probe step the result reflects. Ordered so a later enumerator
  // always means "got further than every earlier one" — useful for at-a-
  // glance log reading, not compared numerically anywhere in code.
  enum class ProbeStage : uint32_t {
    NotProbed = 0,
    DllDiscovery = 1,          // sl.interposer.dll search / LoadLibraryW
    SymbolResolution = 2,      // GetProcAddress for the Stage 2 entry points
    DeviceInteropPending = 3,  // discovery + symbols both OK; slInit needs Stage 2.
    // Stage 2a additions — ordered strictly after DeviceInteropPending so
    // Probe()'s pre-Initialize() callers (still reachable if a caller
    // never wires a VulkanContext through) see the exact same reason
    // string Stage 1 always produced.
    DeviceInteropFailed = 4,   // Initialize() ran but slSetVulkanInfo/slIsFeatureSupported failed.
    Usable = 5,                // Initialize() succeeded; DLSS-SR is available on this adapter.
  };

  struct Availability {
    // True once Initialize() has run slSetVulkanInfo + slIsFeatureSupported
    // successfully. Stage 1 could never reach true; Stage 2a can.
    bool usable = false;
    ProbeStage failedAt = ProbeStage::NotProbed;
    // Human-readable, logged verbatim by PyxisRenderer's per-frame
    // "denoiser: requested=... effective=..." resolution line. Private-only
        // struct, so a plain std::string is fine (no DLL-boundary crossing —
    // contrast with the public Error.h ErrorMessage fixed-buffer POD).
    std::string reason;
  };

  DlssProvider();
  ~DlssProvider();

  DlssProvider(const DlssProvider&) = delete;
  DlssProvider& operator=(const DlssProvider&) = delete;

  // Runs the probe chain exactly once; every subsequent call returns the
  // cached result (idempotent, cheap to call every frame from
  // PyxisRenderer::RenderFrame's effective-denoiser resolution). Logs the
  // full result via Logging::Get() the first time it runs, regardless of
  // outcome, so a machine WITH the SDK staged shows exactly which step it
  // reached.
  const Availability& Probe() noexcept;

  // DLSS Stage 2a — completes device interop. Call ONCE, from
  // PyxisRenderer's ctor, AFTER Probe() and only when Probe() reached
  // ProbeStage::DeviceInteropPending (i.e. Stage 1's discovery + symbol
  // resolution both succeeded). `vk` is the SAME VulkanContext the device
  // manager used to create the VkInstance/VkDevice
  // (IDeviceManager::GetVulkanContext()) -- pyxis_platform's
  // VkDeviceManager(Headless) already ran slInit + slGetFeatureRequirements
  // BEFORE that device existed (Device/DlssDeviceExtensions.h), so this
  // call does NOT call slInit again; it calls slSetVulkanInfo (device/
  // instance/queue info) then slIsFeatureSupported(kFeatureDLSS). On
  // success, resolves slDLSSGetOptimalSettings / slDLSSSetOptions /
  // slSetConstants / slSetTagForFrame / slEvaluateFeature /
  // slGetNewFrameToken / slAllocateResources / slFreeResources and flips
  // `usable` to true. Idempotent no-op if already initialised or if
  // Probe() never reached DeviceInteropPending. Safe to call even when the
  // SDK isn't staged (Probe() already failed earlier -> this is a no-op).
  void Initialize(const VulkanContext& vk) noexcept;

  [[nodiscard]] bool IsUsable() const noexcept { return _availability.usable; }
  [[nodiscard]] const Availability& LastResult() const noexcept { return _availability; }

  // DLSS Stage 2b — Ray Reconstruction (DLSS-D). "An extension to DLSS"
  // (ProgrammingGuideDLSS_RR.md 3.0): only ever probed from INSIDE
  // Initialize() above, and only once SR itself (`_availability.usable`)
  // is confirmed true — a machine with SR but no RR (older/missing
  // nvngx_dlssd.dll, non-RTX-40-class adapter, etc.) keeps full SR, this
  // just stays false. DlssPass checks this to pick the RR-vs-SR evaluate
  // path; PyxisRenderer checks it to decide the passMask forcing (RR
  // replaces the builtin denoiser chain entirely, so PASS_MASK_DENOISE is
  // forced OFF instead of ON) and to log the active ladder rung.
  [[nodiscard]] bool IsRRUsable() const noexcept { return _rrAvailability.usable; }
  [[nodiscard]] const Availability& LastRRResult() const noexcept { return _rrAvailability; }

  struct RenderResolution {
    uint32_t width = 0;
    uint32_t height = 0;
  };
  // slDLSSGetOptimalSettings wrapper (ProgrammingGuideDLSS.md section 3.0).
  // `dlssExecMode` is RenderSettings::RealTimeQuality::dlssExecMode's
  // DLSS_EXEC_MODE_* encoding (RenderSettings.h) -- translated to
  // sl::DLSSMode internally. Returns {0,0} when not usable or the call
  // fails; DlssPass falls back to native (renderRes == displayRes) on that.
  [[nodiscard]] RenderResolution GetOptimalRenderResolution(uint32_t displayWidth,
                                                             uint32_t displayHeight,
                                                             uint32_t dlssExecMode) noexcept;

  // DLSS Stage 2b — slDLSSDGetOptimalSettings wrapper. ProgrammingGuideDLSS_RR.md
  // 3.0: "DLSS-RR runs in the same Performance Quality Mode set for DLSS",
  // but the RR plugin exposes its own optimal-settings entry point (over
  // sl::DLSSDOptions, not plain sl::DLSSOptions — the guide's own code
  // sample names the wrong struct; sl_dlss_d.h's PFun_slDLSSDGetOptimalSettings
  // signature is authoritative), so this calls that directly rather than
  // assuming byte-identical results from GetOptimalRenderResolution above.
  // Returns {0,0} when RR isn't usable or the call fails; the caller falls
  // back to GetOptimalRenderResolution (SR) on that.
  [[nodiscard]] RenderResolution GetOptimalRenderResolutionRR(uint32_t displayWidth,
                                                               uint32_t displayHeight,
                                                               uint32_t dlssExecMode) noexcept;

  // One tagged Vulkan image, raw-handle-only (§18.9-style convention —
  // same shape as VulkanContext's own fields) so this header never needs
  // <vulkan/vulkan.h>. `layout` is the VkImageLayout the image is in AT
  // THE MOMENT Evaluate() is called (DlssPass has already transitioned it
  // there via NVRHI's setTextureState/commitBarriers).
  struct TaggedImage {
    void* image = nullptr;      // VkImage
    void* imageView = nullptr;  // VkImageView
    uint32_t format = 0;        // VkFormat
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layout = 0;        // VkImageLayout
  };

  // Every per-frame input Evaluate() needs. Matrices are row-major,
  // 16-value flattened arrays (matches both sl::float4x4's row[4] layout
  // and hlslpp::store's own flattening -- see SceneBindings.cpp's
  // identical convention -- so no transpose is needed at the call site).
  struct FrameInputs {
    uint64_t frameIndex = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    uint32_t dlssExecMode = 0;
    // True on the first DLSS-active frame (or right after a render-
    // resolution change) -- no valid history yet.
    bool reset = false;
    bool orthographic = false;
    // Pixel-space sub-pixel offset, same convention as
    // RtxSamplingUniforms::jitterX/Y (ShaderInterop.slang) -- [-0.5, 0.5].
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    const float* cameraViewToClip = nullptr;  // 16 floats, row-major
    const float* clipToCameraView = nullptr;  // 16 floats, row-major
    const float* clipToPrevClip = nullptr;    // 16 floats, row-major
    const float* prevClipToClip = nullptr;    // 16 floats, row-major
    float cameraPos[3] = {0.0f, 0.0f, 0.0f};
    float cameraUp[3] = {0.0f, 1.0f, 0.0f};
    float cameraRight[3] = {1.0f, 0.0f, 0.0f};
    float cameraFwd[3] = {0.0f, 0.0f, -1.0f};
    float cameraNear = 0.01f;
    float cameraFar = 10000.0f;
    // renderRes, kBufferTypeScalingInputColor (composite's linear HDR output).
    TaggedImage colorIn;
    // displayRes, kBufferTypeScalingOutputColor (this pass's own UAV target).
    TaggedImage colorOut;
    // renderRes, kBufferTypeDepth — standard normalized-device depth,
    // produced by DlssPass's own dlss_depth_convert.slang dispatch from
    // gViewZ (already-linear view-space distance, NOT itself a valid
    // depth-buffer value). kBufferTypeDepth (not the seemingly-more-
    // convenient kBufferTypeLinearDepth alternative) because
    // slGetFeatureRequirements(kFeatureDLSS) lists tag id 0
    // (kBufferTypeDepth) as REQUIRED on this adapter/driver — see
    // dlss_depth_convert.slang's file comment for the full citation.
    TaggedImage depth;
    // renderRes, kBufferTypeMotionVectors (gMotionVector, pixel-space).
    TaggedImage mvec;
    // 1x1 kBufferTypeExposure — the display exposure gain
    // (ComputeEffectiveExposureScale, ExposureMath.h) so DLSS uses PYX's own
    // exposure instead of its internal auto-exposure estimate (which diverged
    // on the World Lobby's 12000-nit dome and over-brightened the frame ~1.5x).
    // Null image => Evaluate falls back to useAutoExposure (the pre-2026-07-08
    // behaviour) so the pass still runs if the exposure texture failed to
    // create.
    TaggedImage exposure;
    void* vkCommandBuffer = nullptr;  // VkCommandBuffer
  };

  // Runs slGetNewFrameToken -> slSetConstants -> slDLSSSetOptions ->
  // slSetTagForFrame -> slEvaluateFeature, in that order
  // (ProgrammingGuideDLSS.md sections 3.0/6.0/5.0/4.0/7.0 respectively —
  // options/constants/tags must all be set before slEvaluateFeature, but
  // their own relative order doesn't matter per the guide). Returns false
  // (and logs once) on any step's failure; DlssPass then leaves colorOut
  // untouched and the frame degrades to whatever it held last (same
  // shape as every other pass's shader-load-failure gate). Render thread
  // only, one call per frame, one viewport (v1 -- no multi-viewport).
  [[nodiscard]] bool Evaluate(const FrameInputs& inputs) noexcept;

  // DLSS Stage 2b — every per-frame input EvaluateRR() needs. Mirrors
  // FrameInputs' camera/jitter/color/depth/mvec fields exactly (kept as a
  // SEPARATE struct rather than growing FrameInputs with optional RR-only
  // fields — SR and RR are two different Streamline features with two
  // different tag sets; sharing one struct would make it unclear which
  // fields a given Evaluate*() call actually reads) plus the four
  // additional RR guide buffers (ProgrammingGuideDLSS_RR.md §4.1) and the
  // world<->view matrices DLSSDOptions needs for its specular-hit-distance
  // reprojection.
  struct FrameInputsRR {
    uint64_t frameIndex = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    uint32_t dlssExecMode = 0;
    bool orthographic = false;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    const float* cameraViewToClip = nullptr;  // 16 floats, row-major
    const float* clipToCameraView = nullptr;  // 16 floats, row-major
    const float* clipToPrevClip = nullptr;    // 16 floats, row-major
    const float* prevClipToClip = nullptr;    // 16 floats, row-major
    // DLSSDOptions::worldToCameraView / cameraViewToWorld — §4.1.9's
    // kBufferTypeSpecularHitDistance reprojection needs both, in addition
    // to the clip-space matrices above. cameraViewToWorld is the SAME
    // matrix FrameInputs' cameraPos/Up/Right/Fwd are already extracted
    // from (SceneBindings::LastWorldFromView()); worldToCameraView is its
    // inverse (DlssPass.cpp computes both from the one SceneBindings
    // accessor via hlslpp::inverse, same pattern as its existing
    // clipToPrevClip/prevClipToClip derivation).
    const float* worldToCameraView = nullptr;  // 16 floats, row-major
    const float* cameraViewToWorld = nullptr;  // 16 floats, row-major
    float cameraPos[3] = {0.0f, 0.0f, 0.0f};
    float cameraUp[3] = {0.0f, 1.0f, 0.0f};
    float cameraRight[3] = {1.0f, 0.0f, 0.0f};
    float cameraFwd[3] = {0.0f, 0.0f, -1.0f};
    float cameraNear = 0.01f;
    float cameraFar = 10000.0f;
    TaggedImage colorIn;               // renderRes, kBufferTypeScalingInputColor (RAW/noisy composite).
    TaggedImage colorOut;              // displayRes, kBufferTypeScalingOutputColor.
    TaggedImage depth;                 // renderRes, kBufferTypeDepth (dlss_depth_convert.slang output).
    TaggedImage mvec;                  // renderRes, kBufferTypeMotionVectors.
    TaggedImage albedo;                // renderRes, kBufferTypeAlbedo (gAlbedo — diffuse).
    TaggedImage specularAlbedo;        // renderRes, kBufferTypeSpecularAlbedo (gSpecularAlbedo, new Stage 2b guide).
    TaggedImage normalRoughness;       // renderRes, kBufferTypeNormalRoughness (gNormalRoughness, ePacked).
    TaggedImage specularHitDistance;   // renderRes, kBufferTypeSpecularHitDistance (gReflections, .a = hitT).
    void* vkCommandBuffer = nullptr;   // VkCommandBuffer
  };

  // DLSS Stage 2b — Ray Reconstruction's per-frame evaluate. Same
  // slGetNewFrameToken -> slSetConstants -> slDLSSDSetOptions ->
  // slSetTagForFrame -> slEvaluateFeature(kFeatureDLSS_RR) sequence
  // Evaluate() above runs for SR, against sl::kFeatureDLSS_RR instead of
  // sl::kFeatureDLSS and with the larger RR tag set (§4.1). Only called by
  // DlssPass when IsRRUsable() is true; returns false (and logs once) on
  // any step's failure, same graceful-degradation contract as Evaluate().
  [[nodiscard]] bool EvaluateRR(const FrameInputsRR& inputs) noexcept;

  // Releases the DLSS feature's Streamline-side resources
  // (slFreeResources) -- called from DlssPass's dtor before the device is
  // torn down. No-op if never usable.
  void ReleaseResources() noexcept;

 private:
  bool _probed = false;
  Availability _availability;
  // DLSS Stage 2b — Ray Reconstruction's own Availability, set inside
  // Initialize() (never independently probed — RR is only checked once SR
  // itself succeeds). See IsRRUsable()'s doc comment above.
  Availability _rrAvailability;
  // HMODULE, kept opaque here so this header doesn't need <windows.h>
  // (§30 — Private headers avoid it where easy, even though it's not the
  // hard Public/ ban). Non-null only after a successful LoadLibraryW;
  // released in the destructor.
  void* _interposerModule = nullptr;

  // DLSS Stage 2a — PIMPL for the resolved Streamline function pointers +
  // viewport/frame-token state. Kept out of this header so it doesn't need
  // <sl.h> (mirrors the rest of the class's "no Streamline types in the
  // public-to-this-DLL header" discipline); defined in DlssProvider.cpp.
  struct Impl;
  std::unique_ptr<Impl> _impl;
  bool _initialized = false;  // Initialize() ran (regardless of outcome) — idempotency guard.
};

}  // namespace pyxis
