// Pyxis renderer — NrdProvider (RTX-alignment 2026-07-10: NRD backend —
// creation + resource translation (Stage 1) AND per-frame evaluation
// (Stage 2, this revision); render-graph wiring is still a follow-up).
//
// LICENSING: NVIDIA NRD (Real-time Denoisers) is under the proprietary
// "NVIDIA RTX SDKs LICENSE" — it is fetched into the BUILD TREE ONLY by
// CMake's FetchContent (see _cmake/Thirdparty.cmake's PYXIS_WITH_NRD /
// pyxis_thirdparty_require_nrd()), never vendored into this Apache-2.0
// repo, exactly like the DLSS/Streamline posture (Private/Dlss/
// DlssProvider.h's own file comment). This entire file is compiled ONLY
// when PYXIS_WITH_NRD=ON — CMake-gated at the source-list level
// (sources/pyxis_renderer/CMakeLists.txt's `if(PYXIS_WITH_NRD)` block),
// not #ifdef-guarded internally — so there is no #ifdef soup here: when
// the option is OFF, this .h/.cpp pair is simply never added to the
// target and never compiled.
//
// NRD API version pin: v4.17.3 (PYXIS_NRD_GIT_TAG, _cmake/Thirdparty.cmake).
// Bump that tag and this comment together; NRD's own NRDDescs.h /
// NRDSettings.h carry a `static_assert` that trips if the three headers
// (NRD.h/NRDDescs.h/NRDSettings.h) ever drift out of version lockstep.
//
// STAGING PLAN (this PR ships the first TWO bullets):
//   1. DONE (prior PR) — construct the nrd::Instance for two denoisers
//      (RELAX_DIFFUSE_SPECULAR + SIGMA_SHADOW) and translate its
//      pipeline/resource contract onto NVRHI: one nvrhi::ShaderHandle +
//      nvrhi::BindingLayoutHandle + nvrhi::ComputePipelineHandle per
//      nrd::PipelineDesc, one nvrhi::SamplerHandle per nrd::Sampler, and
//      the permanent/transient pool textures (sized via Resize()).
//   2. THIS FILE — per-frame translation (Evaluate()): a pack pre-pass
//      (this provider's own nrd_pack.slang, dispatched by DispatchPack())
//      converts Pyxis's unpacked G-buffer guides into NRD's expected
//      encodings, then SetCommonSettings (camera/frame-index plumbing) +
//      SetDenoiserSettings (nrd::RelaxSettings — NOT
//      "RelaxDiffuseSpecularSettings", see below) + GetComputeDispatches
//      (walks the returned nrd::DispatchDesc array, builds one
//      nvrhi::BindingSetDesc per dispatch from its ResourceDesc list —
//      permanent/transient pool indices resolve through
//      PermanentPoolTexture()/TransientPoolTexture(), "real" resource
//      types resolve through a per-call resource snapshot built from
//      FrameInputs — and records commandList->setComputeState + dispatch
//      calls). RELAX_DIFFUSE_SPECULAR only this stage; SIGMA_SHADOW stays
//      created-but-undispatched (its pipelines/pools exist, nothing calls
//      GetComputeDispatches for its identifier yet).
//
//      TWO corrections versus this stage's own design brief, found by
//      reading the actually-fetched v4.17.3 headers (not assumed from the
//      brief) — see BuildCommonSettings()'s doc comment in the .cpp for
//      the full derivation of both:
//        - nrd::RelaxSettings is the ONE struct RELAX_DIFFUSE_SPECULAR (and
//          every other RELAX_* denoiser) takes — there is no separate
//          "RelaxDiffuseSpecularSettings" type in NRDSettings.h.
//        - CommonSettings' float[16] matrices want COLUMN-MAJOR storage
//          (NRDSettings.h: "layout - column-major"), the OPPOSITE of this
//          codebase's own row-major storage (CLAUDE.md §10) that
//          DlssProviderFrame.cpp's ToSlMatrix passes to Streamline
//          UNTRANSPOSED — NRD needs an explicit transpose on the way in.
//   3. THEN — render-graph wiring: a new NrdPass (Private/Passes/) that
//      replaces the builtin DenoiseTemporalPass/DenoiseHistoryFixPass/
//      DenoiseAtrousPass chain when a future RenderSettings::denoiser ==
//      DENOISER_NRD selects it (mirrors DlssPass's "no-op unless the
//      resolved effective setting picks this provider" gating). The
//      builtin chain remains the default and the fallback when NRD isn't
//      staged/usable — same ladder shape DlssProvider uses for DLSS.
//      Evaluate() below is DORMANT until this lands — nothing in
//      PyxisRenderer/RenderGraph calls it yet.
//
// SPIR-V BINDING-OFFSET FLATTENING (the trickiest part of step 1):
// NRD's HLSL shaders declare SRVs/UAVs in one register space
// (`resourcesSpaceIndex`, = NRD_RESOURCES_SPACE_INDEX = 0) and the
// constant buffer + samplers in ANOTHER (`constantBufferAndSamplersSpaceIndex`,
// = NRD_CONSTANT_BUFFER_AND_SAMPLERS_SPACE_INDEX = 1) — see
// Shaders/InstanceImpl.cpp's #defines in the fetched NRD source. NVIDIA's
// own reference integration (Integration/NRDIntegration.hpp's
// `_CreateResources()`, present in the fetched tree alongside NRD.h)
// resolves this for Vulkan by flattening EVERYTHING into ONE descriptor
// set: it shifts each resource's raw HLSL register index by a per-KIND
// offset from `nrd::GetLibraryDesc()->spirvBindingOffsets`
// (textureOffset for SRVs, storageTextureAndBufferOffset for UAVs,
// constantBufferOffset for the CB, samplerOffset for samplers) rather
// than mapping the two HLSL "space" numbers to two different Vulkan
// descriptor sets. That is exactly the shape nvrhi::BindingLayoutDesc's
// own `bindingOffsets` (VulkanBindingOffsets) field was built for —
// nvrhi's Vulkan backend computes
// `finalVulkanBinding = bindingOffsets.<kindOffset> + item.slot`
// (src/vulkan/vulkan-resource-bindings.cpp, getRegisterOffsetForResourceType
// + BindingLayout::BindingLayout), the SAME formula NRDIntegration.hpp
// applies by hand. So: every BindingLayoutItem below is built with
// `slot` = the RAW NRD register index (resourcesBaseRegisterIndex +
// local range index for SRV/UAV, constantBufferRegisterIndex for the CB,
// samplersBaseRegisterIndex + i for samplers), and the ONE
// nvrhi::BindingLayoutDesc's `bindingOffsets` is set from
// `spirvBindingOffsets` wholesale — see NrdProvider.cpp's CreatePipelines().
//
// CONFIRMED via the actual fetched build (build/dev/_deps/nrd-src/
// CMakeLists.txt + this repo's own build log): NRD's SPIRV permutations
// are compiled with ShaderMake's `--flatten` flag plus explicit per-kind
// shifts -- `--sRegShift 0 --bRegShift 2 --uRegShift 3 --tRegShift 20`
// (CMakeLists.txt's SPIRV_SREG_OFFSET/SPIRV_BREG_OFFSET/SPIRV_UREG_OFFSET/
// SPIRV_TREG_OFFSET, fed into the SAME g_NrdLibraryDesc.spirvBindingOffsets
// this class reads at runtime via nrd::GetLibraryDesc() -- Source/
// Wrapper.cpp). `--flatten` is exactly ShaderMake's "merge every HLSL
// register space into ONE Vulkan descriptor set" mode, so the two HLSL
// "space" numbers (resourcesSpaceIndex=0, constantBufferAndSamplersSpaceIndex=1)
// are a DXC-level register-shift convenience only -- NOT two different
// Vulkan sets. This is exactly the single-nvrhi::BindingLayoutDesc-per-
// pipeline shape implemented below: every item's `slot` is the raw HLSL
// register index, and `bindingOffsets` (read fresh from
// nrd::GetLibraryDesc() every time, never hardcoded) supplies the same
// per-kind shift ShaderMake baked into the SPIR-V, so nvrhi's Vulkan
// backend reconstructs the identical final binding number
// (`bindingOffsets.<kind> + item.slot`, src/vulkan/vulkan-resource-
// bindings.cpp) that the shader itself expects. No known discrepancy
// remains for pipeline/layout creation; the per-frame translation stage
// (step 2 above) is still the first point an actual nvrhi::BindingSet
// gets bound and dispatched, so that is where the Vulkan validation
// layer (NVRHI_WITH_VALIDATION=ON, already on for this project) gets a
// chance to catch anything this reasoning missed.
//
// Private-only — never crosses the pyxis_renderer.dll boundary, and (per
// the CMake source-list gating above) only exists in PYXIS_WITH_NRD=ON
// builds at all.

#pragma once

#include <NRD.h>
#include <NRDDescs.h>
#include <NRDSettings.h>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pyxis {

class NrdProvider final {
 public:
  // Fixed nrd::Identifier assignment for the two denoisers this provider
  // instantiates (Constructor's InstanceCreationDesc). Exposed so
  // Evaluate()'s own SetDenoiserSettings / GetComputeDispatches calls
  // don't have to duplicate these literals.
  static constexpr nrd::Identifier RELAX_DIFFUSE_SPECULAR_ID = 0;
  static constexpr nrd::Identifier SIGMA_SHADOW_ID = 1;

  // `device` is borrowed — owned by PyxisRenderer, outlives this provider
  // (same lifetime contract every other ctor-injected provider/pass uses
  // in this codebase, e.g. DlssProvider's future Initialize(VulkanContext&)
  // caller). Runs nrd::CreateInstance for {RELAX_DIFFUSE_SPECULAR,
  // SIGMA_SHADOW}, then translates the resulting nrd::InstanceDesc's
  // samplers + pipelines onto NVRHI objects. Never throws (renderer is
  // /EHs-c-); on any failure this leaves IsUsable() == false and logs via
  // Logging::Get() — same graceful-degradation contract DlssProvider uses,
  // never PYXIS_FATAL (NRD is optional, day-one-absent on most machines).
  explicit NrdProvider(nvrhi::IDevice* device);
  ~NrdProvider();

  NrdProvider(const NrdProvider&) = delete;
  NrdProvider& operator=(const NrdProvider&) = delete;

  // True once construction fully succeeded (instance created, every
  // sampler/pipeline/binding-layout built). False leaves every accessor
  // below returning null / zero — callers (the future NrdPass) must gate
  // on this before touching anything else, mirroring
  // DlssProvider::IsUsable()'s contract.
  [[nodiscard]] bool IsUsable() const noexcept { return _usable; }

  // (Re)allocates the permanent + transient pool textures at
  // DivideUp(renderWidth, textureDesc.downsampleFactor) x
  // DivideUp(renderHeight, textureDesc.downsampleFactor) for every
  // nrd::TextureDesc in InstanceDesc::permanentPool/transientPool. No-op
  // if usable is false, if either dimension is 0, or if `renderWidth` /
  // `renderHeight` match the last call (matches every other pass's
  // EnsureXxx(width,height) resize-on-demand convention — §30.10, no
  // allocations inside a per-frame Execute()). Render thread only.
  void Resize(uint32_t renderWidth, uint32_t renderHeight);

  [[nodiscard]] uint32_t RenderWidth() const noexcept { return _renderWidth; }
  [[nodiscard]] uint32_t RenderHeight() const noexcept { return _renderHeight; }

  // Stage 2 — every per-frame input Evaluate() needs. Every
  // nvrhi::ITexture* is BORROWED (render-graph/PassContext-owned) and must
  // stay valid for the duration of the Evaluate() call; NrdProvider never
  // extends their lifetime. Modelled on DlssProvider::FrameInputs' own
  // shape (Private/Dlss/DlssProvider.h) — ctor-injected borrowed pointers,
  // no ownership transfer.
  struct FrameInputs {
    uint64_t frameIndex = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;

    // Current/previous camera matrices, in PYXIS'S OWN row-major storage /
    // column-vector convention (CLAUDE.md §10: v' = M * v, translation in
    // the last column) — i.e. the EXACT SAME layout SceneBindings/
    // DlssProviderFrame.cpp already pass around (ToSlMatrix there is a
    // straight memcpy of this same convention into Streamline's own
    // row-major sl::float4x4 — no transpose). NRD wants the OPPOSITE array
    // layout for these SAME column-vector matrices (NRDSettings.h: "layout
    // - column-major") — BuildCommonSettings() in the .cpp transposes on
    // the way in; callers here must NOT pre-transpose.
    const float* worldToView = nullptr;      // 16 floats
    const float* viewToClip = nullptr;       // 16 floats
    const float* worldToViewPrev = nullptr;  // 16 floats
    const float* viewToClipPrev = nullptr;   // 16 floats

    // Sub-pixel camera jitter for THIS frame, in PIXELS, [-0.5, 0.5], SAME
    // sign convention as camera_ray.slang's `launchIndex + 0.5 + jitter`
    // (Passes/CameraJitter.h) — handed to nrd::CommonSettings::cameraJitter
    // unchanged (see BuildCommonSettings()'s doc comment for why no sign
    // flip is needed here, unlike motion vectors below). The PREVIOUS
    // frame's jitter is NOT a field here — Passes/CameraJitter.h's
    // ComputeHaltonJitter is a pure function of frameIndex, so
    // BuildCommonSettings() recomputes it from `frameIndex - 1` instead of
    // trusting a second caller-supplied value that could drift.
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    // ---- G-buffer guides (borrowed; render-resolution) -------------------
    // RG16F, pixel-space, PYXIS'S OWN sign convention (motionVector =
    // currentPixel - previousPixel — raytraced_gbuffer.slang,
    // denoise_temporal.slang). NRD's IN_MV wants the OPPOSITE convention
    // ("MV = previous - current", NRDDescs.h) at normalized-UV scale, not
    // pixel scale — BuildCommonSettings() supplies the negated,
    // size-normalized motionVectorScale that reconciles both differences;
    // do NOT pre-negate or pre-scale this texture's contents.
    nvrhi::ITexture* motionVector = nullptr;

    // World-space normal xyz in [-1,1] + linear roughness in .w, RGBA16F
    // (raytraced_gbuffer.slang's gNormalRoughness, UNPACKED form). Packed
    // into NRD's expected IN_NORMAL_ROUGHNESS encoding by this provider's
    // OWN nrd_pack.slang dispatch (DispatchPack()) — never bound directly.
    nvrhi::ITexture* normalRoughness = nullptr;

    // R32F linear view-space depth (raytraced_gbuffer.slang's gViewZ,
    // "0 on miss"). Bound to IN_VIEWZ UNCHANGED. KNOWN GAP (flagged, not
    // fixed, this stage — raytraced_gbuffer.slang is out of this stage's
    // scope): NRD wants miss/sky pixels at viewZ >= CommonSettings::
    // denoisingRange so they're excluded from denoising (NRDSettings.h);
    // ours read as viewZ == 0 ("at the camera") instead. A future PR
    // should either fix the sky sentinel at the source or add a small
    // remap into nrd_pack.slang alongside the normal/roughness pack.
    nvrhi::ITexture* viewZ = nullptr;

    // RGBA16F radiance (.rgb) + hit distance (.a); PRE-pack-shader, i.e.
    // exactly what the diffuse/specular signal passes wrote
    // (indirect_diffuse.slang's gIndirectDiffuse / reflections.slang's
    // gReflections). NOTE: gIndirectDiffuse's .a is currently always 1.0
    // (indirect_diffuse.slang has no real hit-distance signal yet) — the
    // packed IN_DIFF_RADIANCE_HITDIST will carry that placeholder through
    // until a future PR gives diffuse GI a genuine hit distance. Repacked
    // (sanitized) into IN_DIFF/IN_SPEC_RADIANCE_HITDIST by DispatchPack().
    nvrhi::ITexture* diffuseRadianceHitDist = nullptr;
    nvrhi::ITexture* specRadianceHitDist = nullptr;

    // Caller-owned RGBA16F outputs, render-resolution — this Evaluate()
    // call WRITES the final denoised radiance + diffuse history length
    // into these (OUT_DIFF/OUT_SPEC_RADIANCE_HITDIST); it does not
    // allocate or resize them (matches every other pass's "caller supplies
    // the destination" convention, e.g. DlssPass::Output()).
    nvrhi::ITexture* outDiffuseRadianceHitDist = nullptr;
    nvrhi::ITexture* outSpecRadianceHitDist = nullptr;
  };

  // Runs ONE frame of RELAX_DIFFUSE_SPECULAR: DispatchPack() (unpacked
  // G-buffer guides -> NRD's expected encodings) -> nrd::SetCommonSettings
  // -> nrd::SetDenoiserSettings(RelaxSettings{}) -> nrd::GetComputeDispatches
  // (RELAX_DIFFUSE_SPECULAR_ID only — SIGMA_SHADOW stays dormant) -> for
  // each nrd::DispatchDesc: resolve its ResourceDesc list (permanent/
  // transient pool indices, or the FrameInputs-built resource snapshot for
  // "real" types), build/reuse a cached nvrhi::BindingSet, upload the
  // dispatch's constant-buffer slice into the shared volatile CB (skipped
  // when NRD reports it's byte-identical to the previous dispatch's), and
  // record commandList->setComputeState + dispatch. This SEQUENCE mirrors
  // NRD's own reference integration (Integration/NRDIntegration.hpp's
  // Denoise()/_Dispatch()) exactly — that shape is dictated by NRD's
  // public API contract, not a stylistic choice — but every line of code
  // is this project's own (§30 conventions: HashPointers-keyed binding-set
  // cache per DenoiseTemporalPass.cpp's established pattern, log-and-
  // degrade error handling, no exceptions).
  //
  // Returns false (and logs once) on any step's failure — same graceful-
  // degradation contract DlssProvider::Evaluate uses; never PYXIS_FATAL.
  // No-op (returns false) if !IsUsable(), commandList is null, either
  // dimension is 0, or `inputs.renderWidth/renderHeight` doesn't match the
  // size Resize() last allocated pool/packed textures at (the caller must
  // Resize() to the frame's render resolution before calling this, same
  // "Resize then Execute" contract every other resizable pass/provider
  // here follows).
  //
  // DORMANT this stage — see the file comment's staging plan. Render
  // thread only, one call per frame.
  [[nodiscard]] bool Evaluate(nvrhi::ICommandList* commandList, const FrameInputs& inputs) noexcept;

  // Accessors the per-frame translation above (and this stage's own
  // constructor) use. Exposed publicly too so a future NrdPass /
  // unit test can inspect the translated pipeline/sampler/pool objects
  // directly. All return null (or 0) on an out-of-range index rather than
  // asserting — no exceptions cross this class's boundary (/EHs-c-).
  [[nodiscard]] nrd::Instance* Instance() const noexcept { return _instance; }

  [[nodiscard]] uint32_t PipelineCount() const noexcept {
    return static_cast<uint32_t>(_pipelines.size());
  }
  [[nodiscard]] nvrhi::IShader* PipelineShader(uint32_t index) const noexcept;
  [[nodiscard]] nvrhi::IBindingLayout* PipelineBindingLayout(uint32_t index) const noexcept;
  [[nodiscard]] nvrhi::IComputePipeline* PipelineObject(uint32_t index) const noexcept;

  [[nodiscard]] nvrhi::ISampler* SamplerAt(uint32_t index) const noexcept;

  [[nodiscard]] uint32_t PermanentPoolSize() const noexcept {
    return static_cast<uint32_t>(_permanentPool.size());
  }
  [[nodiscard]] nvrhi::ITexture* PermanentPoolTexture(uint32_t index) const noexcept;

  [[nodiscard]] uint32_t TransientPoolSize() const noexcept {
    return static_cast<uint32_t>(_transientPool.size());
  }
  [[nodiscard]] nvrhi::ITexture* TransientPoolTexture(uint32_t index) const noexcept;

 private:
  // One compiled compute pipeline per nrd::PipelineDesc (InstanceDesc::
  // pipelines[]), in the same order/index GetComputeDispatches' returned
  // nrd::DispatchDesc::pipelineIndex refers back into.
  struct Pipeline {
    nvrhi::ShaderHandle shader;
    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::ComputePipelineHandle pipeline;
  };

  // One entry per InstanceDesc::permanentPool[]/transientPool[] slot.
  // `desc` is kept (not just the derived nvrhi format/size) so Resize()
  // can be called again later without re-querying nrd::GetInstanceDesc.
  struct PoolTexture {
    nrd::TextureDesc desc{};
    nvrhi::TextureHandle texture;
  };

  [[nodiscard]] bool CreateSamplers(const nrd::InstanceDesc& instanceDesc);
  [[nodiscard]] bool CreatePipelines(const nrd::InstanceDesc& instanceDesc);
  static std::vector<PoolTexture> MakePoolDescs(const nrd::TextureDesc* pool, uint32_t poolSize);
  void ResizePool(std::vector<PoolTexture>& pool, uint32_t renderWidth, uint32_t renderHeight,
                  const char* debugPrefix);
  [[nodiscard]] static nvrhi::Format MapFormat(nrd::Format format) noexcept;

  // ---- Stage 2 additions (per-frame evaluation) ------------------------

  // Ctor-time: the ONE volatile constant buffer every NRD dispatch this
  // frame streams its constant data through (instanceDesc.
  // constantBufferRegisterIndex is shared across every RELAX pipeline —
  // NRDDescs.h's InstanceDesc comment). Sized instanceDesc.
  // constantBufferMaxDataSize, maxVersions large enough to cover every
  // dispatch this frame (RELAX_DIFFUSE_SPECULAR's own dispatch count,
  // roughly a dozen) across several frames-in-flight before the GPU
  // retires the oldest version — see the .cpp definition for the exact
  // count/citation. No-op (returns true) if the instance has no
  // constant-buffer-bearing pipeline at all.
  [[nodiscard]] bool CreateConstantBuffer(const nrd::InstanceDesc& instanceDesc);

  // Ctor-time: loads nrd_pack.slang's compiled PackMain, builds its
  // (this-provider-owned, NOT NRD's own SPIR-V-flattened) binding layout +
  // compute pipeline, and creates its own small params constant buffer.
  [[nodiscard]] bool CreatePackPipeline();

  // Resize()-time: (re)allocates the three textures nrd_pack.slang writes
  // into (IN_NORMAL_ROUGHNESS/IN_DIFF_RADIANCE_HITDIST/
  // IN_SPEC_RADIANCE_HITDIST in NRD's OWN expected encodings) at
  // `renderWidth` x `renderHeight`. Same resize-on-demand shape as
  // ResizePool.
  void ResizePackedTextures(uint32_t renderWidth, uint32_t renderHeight);

  // Builds the nrd::CommonSettings for `inputs` — matrix transpose,
  // motion-vector scale/sign reconciliation, jitter, frame index/reset.
  // See the .cpp definition for the full derivation of every field that
  // isn't a straight passthrough.
  [[nodiscard]] nrd::CommonSettings BuildCommonSettings(const FrameInputs& inputs) const noexcept;

  // Dispatches nrd_pack.slang against `inputs`' raw G-buffer guides,
  // writing _packedNormalRoughness/_packedDiffuseRadianceHitDist/
  // _packedSpecRadianceHitDist. Called once at the top of Evaluate(),
  // before the NRD dispatch loop. No-op if the pack pipeline failed to
  // build (CreatePackPipeline() returned false at construction — IsUsable()
  // would already be false in that case, so this guard is defensive, not a
  // second gate callers need to check).
  void DispatchPack(nvrhi::ICommandList* commandList, const FrameInputs& inputs);

  // Binding-set cache for the pack dispatch (keyed on the caller's raw
  // G-buffer texture pointers + this provider's own packed-texture
  // pointers — both are stable frame-to-frame in practice, so this cache
  // is expected to warm to a single entry after frame 1, same as every
  // other pass's GetOrCreateXxxBindingSet in this codebase).
  [[nodiscard]] nvrhi::IBindingSet* GetOrCreatePackBindingSet(const FrameInputs& inputs);

  nvrhi::IDevice* _device = nullptr;  // borrowed.
  nrd::Instance* _instance = nullptr;
  bool _usable = false;

  std::vector<nvrhi::SamplerHandle> _samplers;  // parallel to InstanceDesc::samplers[].
  std::vector<Pipeline> _pipelines;             // parallel to InstanceDesc::pipelines[].
  std::vector<PoolTexture> _permanentPool;      // parallel to InstanceDesc::permanentPool[].
  std::vector<PoolTexture> _transientPool;      // parallel to InstanceDesc::transientPool[].

  uint32_t _renderWidth = 0;
  uint32_t _renderHeight = 0;

  // ---- Stage 2 members --------------------------------------------------

  // Shared volatile CB every NRD dispatch's constant data streams through
  // (see CreateConstantBuffer()'s doc comment above).
  nvrhi::BufferHandle _nrdConstantBuffer;

  // This provider's own G-buffer -> NRD-input-encoding pack pipeline
  // (nrd_pack.slang) + its owned output textures (Resize()-time
  // allocation, matching the permanent/transient pools' own sizing
  // convention) + its small params CB.
  nvrhi::ShaderHandle _packShader;
  nvrhi::BindingLayoutHandle _packBindingLayout;
  nvrhi::ComputePipelineHandle _packPipeline;
  nvrhi::BufferHandle _packParamsBuffer;
  bool _packReady = false;

  nvrhi::TextureHandle _packedNormalRoughness;          // RGBA16_UNORM.
  nvrhi::TextureHandle _packedDiffuseRadianceHitDist;   // RGBA16_FLOAT.
  nvrhi::TextureHandle _packedSpecRadianceHitDist;      // RGBA16_FLOAT.

  // True on construction and again after any Resize() that actually
  // reallocates (pool/packed textures hold fresh, undefined GPU contents,
  // and any caller-side history is invalid too) — consumed by Evaluate()
  // to force exactly one nrd::AccumulationMode::CLEAR_AND_RESTART frame,
  // then cleared. Same "first-frame/resolution-change forces a history
  // reset" contract DlssProviderFrame.cpp's own `resolutionChanged` check
  // implements for DLSS.
  bool _pendingReset = true;

  // Binding-set caches for the per-frame dispatch translation, keyed via
  // HashPointers (NrdProvider.cpp's file-local helper, same convention as
  // DenoiseTemporalPass.cpp/AccumulationPass.cpp's own per-file copies) —
  // resolved resource pointers change identity as NRD's pool textures
  // ping-pong roles frame to frame, so a plain "cache by dispatch index"
  // scheme would go stale silently.
  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _dispatchBindingSetCache;
  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _packBindingSetCache;
};

}  // namespace pyxis
