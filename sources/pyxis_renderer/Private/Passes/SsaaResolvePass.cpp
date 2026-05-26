// Pyxis renderer — SSAA box-downsample resolve pass implementation.

#include "Passes/SsaaResolvePass.h"

#include "RenderGraph/PassContext.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Profiler.h>

// ShaderInterop.slang is on the renderer's private include path and
// defines SsaaDownsampleUniforms under #ifdef __cplusplus.
#include "ShaderInterop.slang"

#include <algorithm>
#include <fstream>
#include <ios>
#include <vector>

namespace pyxis {

namespace {

[[nodiscard]] std::vector<char> ReadBinaryFile(std::string_view path) noexcept {
  std::ifstream stream(std::string{path}, std::ios::binary | std::ios::ate);
  if (!stream)
    return {};
  const std::streamsize fileSize = stream.tellg();
  if (fileSize <= 0)
    return {};
  std::vector<char> bytes(static_cast<std::size_t>(fileSize));
  stream.seekg(0, std::ios::beg);
  stream.read(bytes.data(), fileSize);
  return bytes;
}

}  // namespace

SsaaResolvePass::SsaaResolvePass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/ssaa_downsample.spv");

  const auto bytes = ReadBinaryFile(spvPath.View());
  if (bytes.empty()) {
    log.Error(log::RENDER, "SsaaResolvePass: failed to load ssaa_downsample.spv");
    return;
  }
  nvrhi::ShaderDesc shaderDesc{};
  shaderDesc.shaderType = nvrhi::ShaderType::Compute;
  shaderDesc.entryName = "main";
  shaderDesc.debugName = "ssaa_downsample";
  _shader = _device->createShader(shaderDesc, bytes.data(), bytes.size());
  if (!_shader) {
    log.Error(log::RENDER, "SsaaResolvePass: createShader failed");
    return;
  }

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  // Zero NVRHI's default per-type Vulkan binding offsets (SRV+0,
  // sampler+128, CB+256, UAV+384) so the layout's binding numbers map
  // 1:1 to the shader's explicit `[[vk::binding(N, 0)]]` slots — same
  // convention PathTracePass uses. Without this, Texture_UAV(1) lands
  // at Vulkan binding 385 while the shader declares binding 1 →
  // "gDest not declared in pipeline layout" → the dispatch no-ops.
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),               // source super-res color
      nvrhi::BindingLayoutItem::Texture_UAV(1),               // destination output-res
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),    // SsaaDownsampleUniforms (volatile)
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "SsaaResolvePass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "SsaaResolvePass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::SsaaDownsampleUniforms);
  cbDesc.isConstantBuffer = true;
  // Volatile: writeBuffer'd once per Execute on the open command list.
  // NVRHI auto-versions volatile cbuffers per write so the dispatch
  // sees the just-written params (a non-volatile cbuffer's mid-list
  // writeBuffer wasn't landing → destWidth/destHeight read 0 → every
  // thread early-returned → black resolve).
  cbDesc.isVolatile = true;
  cbDesc.maxVersions = 16;
  cbDesc.debugName = "SsaaResolve.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "SsaaResolvePass: createBuffer(params) failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "SsaaResolvePass: initialised (box-downsample compute)");
}

nvrhi::BindingSetHandle SsaaResolvePass::GetOrCreateBindingSet(
    nvrhi::ITexture* source, nvrhi::ITexture* dest) {
  // FNV1a-64 over the two pointers — stable identity for the cache.
  uint64_t key = 1469598103934665603ull;
  auto mix = [&key](const void* ptr) {
    auto bits = reinterpret_cast<std::uintptr_t>(ptr);
    for (int byte = 0; byte < 8; ++byte) {
      key ^= static_cast<uint64_t>(bits & 0xFFu);
      key *= 1099511628211ull;
      bits >>= 8;
    }
  };
  mix(source);
  mix(dest);

  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, source),
      nvrhi::BindingSetItem::Texture_UAV(1, dest),
      nvrhi::BindingSetItem::ConstantBuffer(2, _paramsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  // Bounded cache — swapchain rebuilds / SSAA-factor changes churn a
  // handful of (source, dest) pairs; cap so stale dangling-pointer
  // entries don't accumulate across re-inits.
  if (_bindingSetCache.size() > 8)
    _bindingSetCache.clear();
  _bindingSetCache.emplace(key, set);
  return set;
}

void SsaaResolvePass::Execute(nvrhi::ICommandList* commandList,
                              const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr)
    return;

  const uint32_t factor = std::max(1u, context.settings->ssaaFactor);
  nvrhi::ITexture* const source = context.targets->color;
  nvrhi::ITexture* const dest = context.targets->colorResolved;
  // Runs only when a resolve target is bound. The standalone viewer binds it for
  // BOTH the supersampled (factor > 1, box-downsample) AND the non-supersampled
  // (factor 1) present — at factor 1 the pass is a pure sRGB present-encode
  // (LinearToSrgb passthrough). Kit's PyxisEngine never binds colorResolved, so
  // this stays a no-op for Omniverse (its output is unaffected).
  if (source == nullptr || dest == nullptr)
    return;

  // settings.{width,height} are the super-res render dims; the resolve
  // output is that divided by the factor.
  const uint32_t destWidth = context.settings->width / factor;
  const uint32_t destHeight = context.settings->height / factor;
  if (destWidth == 0u || destHeight == 0u)
    return;

  const Profiler::GpuScope gpuScope(*context.profiler, commandList, "pass.SsaaResolve");

  shaderinterop::SsaaDownsampleUniforms params{};
  params.destWidth = destWidth;
  params.destHeight = destHeight;
  params.factor = factor;
  params._ssaaPad0 = 0u;
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(source, dest);
  if (!bindingSet)
    return;

  // Explicit barriers: the source was just written by PathTracePass
  // (UAV / render-target) and must be readable as a shader resource;
  // the dest must be in UnorderedAccess for the compute write. NVRHI's
  // auto-tracking usually handles this, but the cross-pass UAV→SRV
  // hazard on the shared color AOV needs the explicit transition to be
  // reliable (it was reading stale/zero otherwise → black resolve).
  commandList->setTextureState(source, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(dest, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::ComputeState state;
  state.pipeline = _pipeline;
  state.bindings = {bindingSet};
  commandList->setComputeState(state);

  // 8×8 thread group (matches [numthreads(8,8,1)] in the shader);
  // ceil-divide the destination dims into groups.
  const uint32_t groupsX = (destWidth + 7u) / 8u;
  const uint32_t groupsY = (destHeight + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);
}

}  // namespace pyxis
