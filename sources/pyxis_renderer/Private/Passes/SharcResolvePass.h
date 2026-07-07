// Pyxis renderer — SharcResolvePass (RTX-alignment design,
// rtx-realtime-alignment-design.md, 2026-07-07): the SHARC world-space
// radiance-cache Resolve compute pass + the OWNER of the three cache buffers.
//
// SHARC (Spatial Hash Radiance Cache) is an OPTIONAL infinite-bounce
// indirect-diffuse GI mode, gated on RealTimeQuality::passMask bit 9
// (PASS_MASK_SHARC_GI). This pass owns the hash / accumulation / resolved
// buffers (persistent across the accumulation frames, cleared once on the first
// enabled frame) and, each frame, runs sharc_resolve.slang to fold the
// per-frame radiance sums (written by IndirectDiffusePass's SHARC UPDATE path)
// into a cross-frame running mean. It runs BEFORE IndirectDiffusePass so that
// pass's QUERY reads the freshly-resolved data. IndirectDiffusePass is
// ctor-injected a reference to this pass to reach the three buffers (same
// structural-dependency convention DenoiseHistoryFixPass↔DenoiseTemporalPass
// uses). A COMPUTE pass, no SceneBindings. Clean-room; see radiance_cache.slang
// for the algorithm + NVIDIA-licensing provenance note.
//
// Bindings (single Set 0, matched to sharc_resolve.slang):
//   0 : RWByteAddressBuffer gAccum    (16 B/cell — this frame's sum + count)
//   1 : RWByteAddressBuffer gResolved (16 B/cell — cross-frame resolved radiance)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>

namespace pyxis {

class SharcResolvePass final : public IRenderPass {
 public:
  // MUST match SHARC_CAPACITY in resources/shaders/radiance_cache.slang.
  static constexpr uint32_t CAPACITY = 1u << 21;  // 2,097,152 voxel cells
  static constexpr uint32_t HASH_BYTES = CAPACITY * 4u;    // uint32 checksum / cell
  static constexpr uint32_t VOXEL_BYTES = CAPACITY * 16u;  // uint4 / float4 per cell

  explicit SharcResolvePass(nvrhi::IDevice* device);
  ~SharcResolvePass() override;
  SharcResolvePass(const SharcResolvePass&) = delete;
  SharcResolvePass& operator=(const SharcResolvePass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.SharcResolve"; }

  // The three cache buffers, for IndirectDiffusePass to bind (Set 1). Valid
  // once the ctor succeeded (IsReady()); null otherwise.
  [[nodiscard]] nvrhi::IBuffer* HashBuffer() const noexcept { return _hashBuffer.Get(); }
  [[nodiscard]] nvrhi::IBuffer* AccumBuffer() const noexcept { return _accumBuffer.Get(); }
  [[nodiscard]] nvrhi::IBuffer* ResolvedBuffer() const noexcept { return _resolvedBuffer.Get(); }
  [[nodiscard]] bool IsReady() const noexcept { return _ready; }

 private:
  nvrhi::IDevice* _device = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BindingSetHandle _bindingSet;

  nvrhi::BufferHandle _hashBuffer;
  nvrhi::BufferHandle _accumBuffer;
  nvrhi::BufferHandle _resolvedBuffer;

  bool _ready = false;
  // The cache persists + converges across the accumulation frames; it is
  // cleared to zero exactly once, on the first frame the pass is enabled.
  bool _cleared = false;

  void ClearCache(nvrhi::ICommandList* commandList);
};

}  // namespace pyxis
