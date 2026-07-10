// Pyxis renderer — NrdProvider (RTX-alignment 2026-07-10: NRD backend
// skeleton — creation + resource translation, NO per-frame evaluate yet).
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
// STAGING PLAN (this PR ships only the first bullet):
//   1. THIS FILE — construct the nrd::Instance for two denoisers
//      (RELAX_DIFFUSE_SPECULAR + SIGMA_SHADOW) and translate its
//      pipeline/resource contract onto NVRHI: one nvrhi::ShaderHandle +
//      nvrhi::BindingLayoutHandle + nvrhi::ComputePipelineHandle per
//      nrd::PipelineDesc, one nvrhi::SamplerHandle per nrd::Sampler, and
//      the permanent/transient pool textures (sized via Resize()).
//   2. NEXT — per-frame translation: SetCommonSettings (camera/frame-index
//      plumbing) + SetDenoiserSettings (RelaxDiffuseSpecularSettings /
//      SigmaSettings) + GetComputeDispatches (walks the returned
//      nrd::DispatchDesc array, builds one nvrhi::BindingSetDesc per
//      dispatch from its ResourceDesc list — permanent/transient pool
//      indices resolve through PermanentPoolTexture()/TransientPoolTexture()
//      below, "real" resource types resolve through a resource-snapshot
//      the pass supplies — and records commandList->setComputeState +
///     dispatch calls).
//   3. THEN — render-graph wiring: a new NrdPass (Private/Passes/) that
//      replaces the builtin DenoiseTemporalPass/DenoiseHistoryFixPass/
//      DenoiseAtrousPass chain when a future RenderSettings::denoiser ==
//      DENOISER_NRD selects it (mirrors DlssPass's "no-op unless the
//      resolved effective setting picks this provider" gating). The
//      builtin chain remains the default and the fallback when NRD isn't
//      staged/usable — same ladder shape DlssProvider uses for DLSS.
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
#include <vector>

namespace pyxis {

class NrdProvider final {
 public:
  // Fixed nrd::Identifier assignment for the two denoisers this provider
  // instantiates (Constructor's InstanceCreationDesc). Exposed so the
  // future per-frame stage (SetDenoiserSettings / GetComputeDispatches)
  // doesn't have to duplicate these literals.
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

  // Accessors the future per-frame translation (SetCommonSettings ->
  // SetDenoiserSettings -> GetComputeDispatches -> per-dispatch binding-set
  // build + commandList->dispatch) will need. Exposed now so that stage
  // doesn't require a second round of surface changes to this class. All
  // return null (or 0) on an out-of-range index rather than asserting —
  // no exceptions cross this class's boundary (/EHs-c-).
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

  nvrhi::IDevice* _device = nullptr;  // borrowed.
  nrd::Instance* _instance = nullptr;
  bool _usable = false;

  std::vector<nvrhi::SamplerHandle> _samplers;  // parallel to InstanceDesc::samplers[].
  std::vector<Pipeline> _pipelines;             // parallel to InstanceDesc::pipelines[].
  std::vector<PoolTexture> _permanentPool;      // parallel to InstanceDesc::permanentPool[].
  std::vector<PoolTexture> _transientPool;      // parallel to InstanceDesc::transientPool[].

  uint32_t _renderWidth = 0;
  uint32_t _renderHeight = 0;
};

}  // namespace pyxis
