// Pyxis renderer — NrdProvider implementation. See NrdProvider.h's file
// comment for the licensing posture, staging plan, and the SPIR-V
// binding-offset flattening design this file implements.

#include "Nrd/NrdProvider.h"

#include "Passes/CameraJitter.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <array>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>

namespace pyxis {

namespace {

// Ceiling division — matches NRD's own reference integration
// (Integration/NRDIntegration.hpp's DivideUp) for deriving a pool
// texture's actual pixel size from InstanceDesc's downsampleFactor.
uint32_t DivideUp(uint32_t x, uint16_t y) noexcept {
  return (x + static_cast<uint32_t>(y) - 1u) / static_cast<uint32_t>(y);
}

// FNV-1a-style pointer hash — same file-local convention every other
// pass's binding-set cache in this codebase uses (DenoiseTemporalPass.cpp/
// AccumulationPass.cpp's own copies); duplicated here rather than shared
// so this file stays self-contained under its PYXIS_WITH_NRD gating. Takes
// a raw pointer+count (not std::initializer_list) because Stage 2's
// per-dispatch cache key has a RUNTIME-sized resource list.
std::uint64_t HashPointers(const void* const* pointers, std::size_t count) noexcept {
  std::uint64_t key = 1469598103934665603ull;
  for (std::size_t ptrIndex = 0; ptrIndex < count; ++ptrIndex) {
    auto bits = reinterpret_cast<std::uintptr_t>(pointers[ptrIndex]);
    for (int byte = 0; byte < 8; ++byte) {
      key ^= static_cast<std::uint64_t>(bits & 0xFFu);
      key *= 1099511628211ull;
      bits >>= 8;
    }
  }
  return key;
}

// NRD's CommonSettings float[16] arrays want COLUMN-MAJOR storage of a
// column-vector-convention matrix (NRDSettings.h: "usage - vector is a
// column" / "layout - column-major"). Pyxis stores the SAME column-
// vector-convention matrix ROW-MAJOR (CLAUDE.md §10; DlssProviderFrame.cpp's
// ToSlMatrix confirms Streamline wants that identical row-major layout
// verbatim, no transpose). Row-major storage of a matrix M and column-major
// storage of the SAME M are transposes of each other as raw float arrays
// (colMajor[4c+r] = M[r][c] = rowMajor[4r+c] of M), so this function
// transposes on the way in. `dst` is zeroed (not left as NRD's own
// identity-ish default) when `src` is null so a caller that legitimately
// has no "previous" matrix yet (frame 0) gets an honest all-zero matrix
// rather than a silently-wrong one.
void TransposeMatrix16(const float* src, float dst[16]) noexcept {
  if (src == nullptr) {
    for (int i = 0; i < 16; ++i) dst[i] = 0.0f;
    return;
  }
  for (int row = 0; row < 4; ++row)
    for (int col = 0; col < 4; ++col)
      dst[col * 4 + row] = src[row * 4 + col];
}

// nrd_pack.slang's gParams — hand-rolled (NOT a ShaderInterop.slang
// PYXIS_INTEROP_STRUCT; this stage's scope keeps that shared file
// untouched) but kept in exact byte-for-byte lockstep with that shader's
// own local `struct NrdPackParams` cbuffer declaration. 16 bytes satisfies
// the project's default cbuffer alignment rule (§10/§23) by hand, same as
// the shared macro would enforce for us.
struct NrdPackParams {
  uint32_t width = 0;
  uint32_t height = 0;
  float _reserved0 = 0.0f;
  float _reserved1 = 0.0f;
};
static_assert(sizeof(NrdPackParams) == 16,
             "NrdPackParams must match nrd_pack.slang's gParams exactly");

// Upper bound on nrd::DispatchDesc::resourcesNum for any single dispatch —
// RELAX_DIFFUSE_SPECULAR's real per-pipeline maximum (G-buffer guides +
// diffuse/specular noisy in/out + a handful of permanent/transient history
// buffers) is well under this; generous headroom lets a bad wiring log-
// and-skip instead of overflowing a fixed-size local array (no heap
// allocation in this per-frame loop — §30.10's "no allocations in a pass's
// per-frame body" spirit, even though Evaluate() isn't an IRenderPass
// itself).
constexpr uint32_t MAX_DISPATCH_RESOURCES = 32;

}  // namespace

NrdProvider::NrdProvider(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();

  if (_device == nullptr)
  {
    log.Error(log::RENDER, "NrdProvider: constructed with a null nvrhi::IDevice");
    return;
  }

  const nrd::DenoiserDesc denoisers[] = {
      {RELAX_DIFFUSE_SPECULAR_ID, nrd::Denoiser::RELAX_DIFFUSE_SPECULAR},
      {SIGMA_SHADOW_ID, nrd::Denoiser::SIGMA_SHADOW},
  };

  nrd::InstanceCreationDesc creationDesc{};
  // allocationCallbacks left zero-initialized -- nrd::CreateInstance
  // (Source/Wrapper.cpp) substitutes its own aligned malloc/realloc/free
  // whenever any of the three callback pointers is null, so there is
  // nothing for us to wire up here.
  creationDesc.denoisers = denoisers;
  creationDesc.denoisersNum = static_cast<uint32_t>(std::size(denoisers));

  const nrd::Result createResult = nrd::CreateInstance(creationDesc, _instance);
  if (createResult != nrd::Result::SUCCESS || _instance == nullptr)
  {
    log.Error(log::RENDER, "NrdProvider: nrd::CreateInstance failed (nrd::Result="
                               + std::to_string(static_cast<uint32_t>(createResult)) + ")");
    _instance = nullptr;
    return;
  }

  const nrd::InstanceDesc& instanceDesc = *nrd::GetInstanceDesc(*_instance);

  if (!CreateSamplers(instanceDesc))
  {
    nrd::DestroyInstance(*_instance);
    _instance = nullptr;
    return;
  }

  // SET-1 LAYOUT — shared samplers + constant buffer (first-light fix,
  // 2026-07-10; see NrdProvider.h's _set1Layout comment for the two-space
  // derivation). The LAYOUT must exist BEFORE CreatePipelines (every NRD
  // compute pipeline's bindingLayouts = {set0, set1}); the binding SET is
  // built later, after CreateConstantBuffer.
  {
    const nrd::LibraryDesc& libraryDesc = *nrd::GetLibraryDesc();
    nvrhi::BindingLayoutDesc set1Desc;
    set1Desc.visibility = nvrhi::ShaderType::Compute;
    set1Desc.bindingOffsets.shaderResource = 0;
    set1Desc.bindingOffsets.unorderedAccess = 0;
    set1Desc.bindingOffsets.sampler = libraryDesc.spirvBindingOffsets.samplerOffset;
    set1Desc.bindingOffsets.constantBuffer = libraryDesc.spirvBindingOffsets.constantBufferOffset;
    for (uint32_t i = 0; i < instanceDesc.samplersNum; ++i)
      set1Desc.bindings.push_back(
          nvrhi::BindingLayoutItem::Sampler(instanceDesc.samplersBaseRegisterIndex + i));
    set1Desc.bindings.push_back(
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(instanceDesc.constantBufferRegisterIndex));
    _set1Layout = _device->createBindingLayout(set1Desc);
    if (!_set1Layout)
    {
      log.Error(log::RENDER, "NrdProvider: SET-1 layout creation failed");
      nrd::DestroyInstance(*_instance);
      _instance = nullptr;
      return;
    }
  }

  if (!CreatePipelines(instanceDesc))
  {
    nrd::DestroyInstance(*_instance);
    _instance = nullptr;
    return;
  }

  // Stage 2 — the shared per-dispatch constant buffer and this provider's
  // own G-buffer -> NRD-input-encoding pack pipeline. Both gate IsUsable()
  // exactly like every other ctor-time step above: Evaluate() is dormant
  // until a future PR wires it into the render graph, but it must be
  // FULLY ready (not merely "usable enough for the Stage 1 accessors")
  // the moment IsUsable() reports true, per this class's own contract.
  if (!CreateConstantBuffer(instanceDesc))
  {
    nrd::DestroyInstance(*_instance);
    _instance = nullptr;
    return;
  }

  // SET-1 BINDING SET — the samplers + CB instance data for the shared
  // layout created above (needs _nrdConstantBuffer, hence after
  // CreateConstantBuffer).
  {
    nvrhi::BindingSetDesc set1SetDesc;
    for (uint32_t i = 0; i < instanceDesc.samplersNum; ++i)
      set1SetDesc.bindings.push_back(nvrhi::BindingSetItem::Sampler(
          instanceDesc.samplersBaseRegisterIndex + i, _samplers[i]));
    set1SetDesc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(
        instanceDesc.constantBufferRegisterIndex, _nrdConstantBuffer));
    _set1BindingSet = _device->createBindingSet(set1SetDesc, _set1Layout);
    if (!_set1BindingSet)
    {
      log.Error(log::RENDER, "NrdProvider: SET-1 binding-set creation failed");
      nrd::DestroyInstance(*_instance);
      _instance = nullptr;
      return;
    }
  }

  if (!CreatePackPipeline())
  {
    nrd::DestroyInstance(*_instance);
    _instance = nullptr;
    return;
  }
  _packReady = true;

  _permanentPool = MakePoolDescs(instanceDesc.permanentPool, instanceDesc.permanentPoolSize);
  _transientPool = MakePoolDescs(instanceDesc.transientPool, instanceDesc.transientPoolSize);

  _usable = true;
  log.Info(log::RENDER,
           "NrdProvider: instance created (RELAX_DIFFUSE_SPECULAR + SIGMA_SHADOW, "
               + std::to_string(instanceDesc.pipelinesNum) + " pipelines, "
               + std::to_string(instanceDesc.samplersNum) + " samplers, "
               + std::to_string(instanceDesc.permanentPoolSize) + " permanent + "
               + std::to_string(instanceDesc.transientPoolSize)
               + " transient pool textures) -- Evaluate() ready (dormant until render-graph "
               + "wiring lands, see file comment)");
}

NrdProvider::~NrdProvider() {
  // NVRHI handles (samplers/pipelines/pool textures) release themselves
  // via RefCountPtr as the member vectors are destroyed; only the raw NRD
  // instance needs an explicit teardown call.
  if (_instance != nullptr)
    nrd::DestroyInstance(*_instance);
}

bool NrdProvider::CreateSamplers(const nrd::InstanceDesc& instanceDesc) {
  auto& log = Logging::Get();

  _samplers.clear();
  _samplers.reserve(instanceDesc.samplersNum);
  for (uint32_t i = 0; i < instanceDesc.samplersNum; ++i)
  {
    const nrd::Sampler nrdSampler = instanceDesc.samplers[i];

    // nrd::Sampler is only ever NEAREST_CLAMP or LINEAR_CLAMP (NRDDescs.h)
    // -- both clamp-to-edge, differing only in filtering.
    nvrhi::SamplerDesc desc;
    desc.setAllFilters(nrdSampler == nrd::Sampler::LINEAR_CLAMP);
    desc.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);

    nvrhi::SamplerHandle sampler = _device->createSampler(desc);
    if (!sampler)
    {
      log.Error(log::RENDER,
               "NrdProvider: createSampler failed (index=" + std::to_string(i) + ")");
      return false;
    }
    _samplers.push_back(std::move(sampler));
  }
  return true;
}

bool NrdProvider::CreatePipelines(const nrd::InstanceDesc& instanceDesc) {
  auto& log = Logging::Get();
  const nrd::LibraryDesc& libraryDesc = *nrd::GetLibraryDesc();

  _pipelines.clear();
  _pipelines.reserve(instanceDesc.pipelinesNum);

  for (uint32_t p = 0; p < instanceDesc.pipelinesNum; ++p)
  {
    const nrd::PipelineDesc& pipelineDesc = instanceDesc.pipelines[p];
    const std::string debugName = std::string{"Nrd."} + pipelineDesc.shaderIdentifier;

    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    // instanceDesc.shaderEntryPoint is "NRD_CS_MAIN" for every NRD
    // pipeline (NRDDescs.h) -- NOT nvrhi::ShaderDesc's own "main" default.
    shaderDesc.entryName = instanceDesc.shaderEntryPoint;
    shaderDesc.debugName = debugName;

    nvrhi::ShaderHandle shader =
        _device->createShader(shaderDesc, pipelineDesc.computeShaderSPIRV.bytecode,
                              static_cast<size_t>(pipelineDesc.computeShaderSPIRV.size));
    if (!shader)
    {
      log.Error(log::RENDER, "NrdProvider: createShader failed for pipeline '" + debugName + "'");
      return false;
    }

    // ---- Binding layout SET 0: this pipeline's SRV/UAV resources ONLY
    // (first-light fix, 2026-07-10 — see NrdProvider.h's _set1Layout
    // comment: NRD's samplers + constant buffer live in register SPACE 1
    // = Vulkan SET 1, verified via VUID-...-07988; they are provided by
    // the shared _set1Layout/_set1BindingSet, NOT here). `slot` is the RAW
    // NRD HLSL register index; `bindingOffsets` shifts each item into the
    // final Vulkan binding number, matching NVRHI's own `bindingLocation =
    // registerOffset + item.slot` (src/vulkan/vulkan-resource-bindings.cpp).
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.bindingOffsets.shaderResource = libraryDesc.spirvBindingOffsets.textureOffset;
    layoutDesc.bindingOffsets.unorderedAccess =
        libraryDesc.spirvBindingOffsets.storageTextureAndBufferOffset;
    layoutDesc.bindingOffsets.constantBuffer = libraryDesc.spirvBindingOffsets.constantBufferOffset;
    layoutDesc.bindingOffsets.sampler = libraryDesc.spirvBindingOffsets.samplerOffset;

    // Up to two resourceRanges (NRDDescs.h's PipelineDesc comment): an
    // optional TEXTURE (SRV, "t" registers) range followed by an optional
    // STORAGE_TEXTURE (UAV, "u" registers) range. "t" and "u" are separate
    // HLSL register namespaces, so each range's local index restarts from
    // instanceDesc.resourcesBaseRegisterIndex independently of the other.
    uint32_t textureSlot = instanceDesc.resourcesBaseRegisterIndex;
    uint32_t storageSlot = instanceDesc.resourcesBaseRegisterIndex;
    for (uint32_t r = 0; r < pipelineDesc.resourceRangesNum; ++r)
    {
      const nrd::ResourceRangeDesc& range = pipelineDesc.resourceRanges[r];
      if (range.descriptorType == nrd::DescriptorType::TEXTURE)
      {
        for (uint32_t i = 0; i < range.descriptorsNum; ++i)
          layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(textureSlot++));
      }
      else  // STORAGE_TEXTURE
      {
        for (uint32_t i = 0; i < range.descriptorsNum; ++i)
          layoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(storageSlot++));
      }
    }

    // (Samplers + constant buffer intentionally NOT added here — they live
    // in the shared SET-1 layout; see NrdProvider.h's _set1Layout comment.)
    nvrhi::BindingLayoutHandle bindingLayout = _device->createBindingLayout(layoutDesc);
    if (!bindingLayout)
    {
      log.Error(log::RENDER,
               "NrdProvider: createBindingLayout failed for pipeline '" + debugName + "'");
      return false;
    }

    nvrhi::ComputePipelineDesc computeDesc;
    computeDesc.CS = shader;
    // SET 0 = this pipeline's resources; SET 1 = the shared samplers + CB
    // (see _set1Layout's doc comment — NRD's SPIRV keeps them in space 1).
    computeDesc.bindingLayouts = {bindingLayout, _set1Layout};
    nvrhi::ComputePipelineHandle pipeline = _device->createComputePipeline(computeDesc);
    if (!pipeline)
    {
      log.Error(log::RENDER,
               "NrdProvider: createComputePipeline failed for pipeline '" + debugName + "'");
      return false;
    }

    _pipelines.push_back(
        Pipeline{std::move(shader), std::move(bindingLayout), std::move(pipeline)});
  }
  return true;
}

std::vector<NrdProvider::PoolTexture> NrdProvider::MakePoolDescs(const nrd::TextureDesc* pool,
                                                                  uint32_t poolSize) {
  std::vector<PoolTexture> result;
  result.reserve(poolSize);
  for (uint32_t i = 0; i < poolSize; ++i)
    result.push_back(PoolTexture{pool[i], nullptr});
  return result;
}

void NrdProvider::ResizePool(std::vector<PoolTexture>& pool, uint32_t renderWidth,
                             uint32_t renderHeight, const char* debugPrefix) {
  auto& log = Logging::Get();

  for (uint32_t i = 0; i < static_cast<uint32_t>(pool.size()); ++i)
  {
    PoolTexture& entry = pool[i];
    const uint32_t w = DivideUp(renderWidth, entry.desc.downsampleFactor);
    const uint32_t h = DivideUp(renderHeight, entry.desc.downsampleFactor);
    const std::string debugName = std::string{debugPrefix} + "[" + std::to_string(i) + "]";

    nvrhi::TextureDesc textureDesc;
    textureDesc.width = w;
    textureDesc.height = h;
    textureDesc.dimension = nvrhi::TextureDimension::Texture2D;
    textureDesc.format = MapFormat(entry.desc.format);
    // NRD reuses the same physical pool texture as an SRV in one dispatch
    // and a UAV in another (Integration/NRDIntegration.hpp's `_CreateResources`
    // creates every pool texture with SHADER_RESOURCE | SHADER_RESOURCE_STORAGE
    // usage) -- both flags on here, matching that.
    textureDesc.isShaderResource = true;
    textureDesc.isUAV = true;
    textureDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    textureDesc.keepInitialState = true;
    textureDesc.debugName = debugName;

    entry.texture = _device->createTexture(textureDesc);
    if (!entry.texture)
      log.Error(log::RENDER, "NrdProvider: createTexture failed for '" + debugName + "'");
  }
}

void NrdProvider::Resize(uint32_t renderWidth, uint32_t renderHeight) {
  if (!_usable)
    return;
  if (renderWidth == 0u || renderHeight == 0u)
    return;
  if (renderWidth == _renderWidth && renderHeight == _renderHeight)
    return;

  _renderWidth = renderWidth;
  _renderHeight = renderHeight;

  ResizePool(_permanentPool, renderWidth, renderHeight, "Nrd.Permanent");
  ResizePool(_transientPool, renderWidth, renderHeight, "Nrd.Transient");
  ResizePackedTextures(renderWidth, renderHeight);

  // A resolution change invalidates every pool/packed texture's contents
  // (freshly (re)allocated, undefined GPU contents until first written)
  // and any caller-side history — force exactly one
  // nrd::AccumulationMode::CLEAR_AND_RESTART frame the next time
  // Evaluate() runs, same "resolutionChanged -> reset" contract
  // DlssProviderFrame.cpp's Evaluate() applies for DLSS.
  _pendingReset = true;

  // Every previously-built binding set referenced the OLD pool/packed
  // texture objects, now destroyed by the ResizePool/ResizePackedTextures
  // calls above (createTexture on an already-populated nvrhi::TextureHandle
  // member releases the prior object) — stale cache entries would bind
  // freed resources. Both caches rebuild lazily, on demand, against the
  // new textures.
  _dispatchBindingSetCache.clear();
  _packBindingSetCache.clear();
}

nvrhi::IShader* NrdProvider::PipelineShader(uint32_t index) const noexcept {
  return index < _pipelines.size() ? _pipelines[index].shader.Get() : nullptr;
}

nvrhi::IBindingLayout* NrdProvider::PipelineBindingLayout(uint32_t index) const noexcept {
  return index < _pipelines.size() ? _pipelines[index].bindingLayout.Get() : nullptr;
}

nvrhi::IComputePipeline* NrdProvider::PipelineObject(uint32_t index) const noexcept {
  return index < _pipelines.size() ? _pipelines[index].pipeline.Get() : nullptr;
}

nvrhi::ISampler* NrdProvider::SamplerAt(uint32_t index) const noexcept {
  return index < _samplers.size() ? _samplers[index].Get() : nullptr;
}

nvrhi::ITexture* NrdProvider::PermanentPoolTexture(uint32_t index) const noexcept {
  return index < _permanentPool.size() ? _permanentPool[index].texture.Get() : nullptr;
}

nvrhi::ITexture* NrdProvider::TransientPoolTexture(uint32_t index) const noexcept {
  return index < _transientPool.size() ? _transientPool[index].texture.Get() : nullptr;
}

// =============================================================================
// Stage 2 — per-frame evaluation path.
// =============================================================================

bool NrdProvider::CreateConstantBuffer(const nrd::InstanceDesc& instanceDesc) {
  // No pipeline in this instance references the shared CB at all --
  // shouldn't happen for RELAX_DIFFUSE_SPECULAR, but stay defensive rather
  // than creating a zero-byte buffer.
  if (instanceDesc.constantBufferMaxDataSize == 0u)
    return true;

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = instanceDesc.constantBufferMaxDataSize;
  cbDesc.isConstantBuffer = true;
  cbDesc.isVolatile = true;
  // RELAX_DIFFUSE_SPECULAR issues roughly a dozen dispatches per frame
  // (optional hit-distance reconstruction (off by default) + diffuse/
  // specular prepass + temporal accumulation + history clamping/fix +
  // spatial variance estimation + RelaxSettings::atrousIterationNum (5 by
  // default) A-trous passes), most of which write a fresh constant-buffer
  // slice. 2048 versions comfortably covers that even across several
  // frames-in-flight before the GPU retires the oldest version
  // (MAX_FRAMES_IN_FLIGHT = 3) -- same "maxVersions starvation" hazard
  // DenoiseTemporalPass.cpp's own doc comment describes for headless mode
  // (no per-frame GPU wait), sized with generous headroom instead of the
  // bare minimum.
  cbDesc.maxVersions = 2048;
  cbDesc.debugName = "Nrd.ConstantBuffer";
  _nrdConstantBuffer = _device->createBuffer(cbDesc);
  if (!_nrdConstantBuffer)
  {
    Logging::Get().Error(log::RENDER, "NrdProvider: createBuffer(NRD constant buffer) failed");
    return false;
  }
  return true;
}

bool NrdProvider::CreatePackPipeline() {
  auto& log = Logging::Get();

  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/nrd_pack.spv");
  _packShader =
      LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main", "Nrd.Pack");
  if (!_packShader)
    return false;

  // This provider's OWN shader/layout -- a single, ordinary Set 0, no NRD
  // SPIR-V binding-offset flattening involved (that only applies to NRD's
  // own compiled-in shaders; see this file's header comment).
  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),              // 0 gNormalRoughnessIn
      nvrhi::BindingLayoutItem::Texture_SRV(1),              // 1 gDiffuseRadianceHitDistIn
      nvrhi::BindingLayoutItem::Texture_SRV(2),              // 2 gSpecRadianceHitDistIn
      nvrhi::BindingLayoutItem::Texture_UAV(3),              // 3 gNormalRoughnessOut
      nvrhi::BindingLayoutItem::Texture_UAV(4),              // 4 gDiffRadianceHitDistOut
      nvrhi::BindingLayoutItem::Texture_UAV(5),              // 5 gSpecRadianceHitDistOut
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(6),   // 6 gParams
  };
  _packBindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_packBindingLayout)
  {
    log.Error(log::RENDER, "NrdProvider: createBindingLayout(pack) failed");
    return false;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _packShader;
  pipelineDesc.bindingLayouts = {_packBindingLayout};
  _packPipeline = _device->createComputePipeline(pipelineDesc);
  if (!_packPipeline)
  {
    log.Error(log::RENDER, "NrdProvider: createComputePipeline(pack) failed");
    return false;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(NrdPackParams);
  cbDesc.isConstantBuffer = true;
  cbDesc.isVolatile = true;
  // 512 -- codebase precedent for a written-once-per-frame volatile CB
  // (TonemapPass.Params / AutoExposurePass.Params / DlssPass's own params
  // buffer), not the ~2048 the SHARED NRD dispatch CB above needs (this
  // buffer is written exactly once per Evaluate() call, not once per
  // NRD dispatch).
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "Nrd.PackParams";
  _packParamsBuffer = _device->createBuffer(cbDesc);
  if (!_packParamsBuffer)
  {
    log.Error(log::RENDER, "NrdProvider: createBuffer(pack params) failed");
    return false;
  }

  return true;
}

void NrdProvider::ResizePackedTextures(uint32_t renderWidth, uint32_t renderHeight) {
  auto& log = Logging::Get();

  // NRD_NORMAL_ENCODING is compiled into the fetched NRD build as
  // RGBA16_UNORM for this project (_cmake/Thirdparty.cmake overrides NRD's
  // own CMakeLists.txt default of R10G10B10A2_UNORM before FetchContent
  // pulls it in -- see that file's PYXIS_WITH_NRD block). Read the choice
  // back from nrd::GetLibraryDesc() at runtime rather than trusting the
  // override silently took effect: a build directory configured BEFORE
  // this override existed left NRD_NORMAL_ENCODING cached at its old value
  // (CMake cache variables persist across reconfigures unless FORCE'd,
  // which this override does, but a stale already-fetched nrd-src tree
  // from an even older commit wouldn't have re-run CMake at all) -- this
  // check fails LOUDLY here instead of nrd_pack.slang's RGBA16_UNORM
  // output silently mismatching what NRD's compiled shaders actually
  // decode.
  const nrd::NormalEncoding normalEncoding = nrd::GetLibraryDesc()->normalEncoding;
  if (normalEncoding != nrd::NormalEncoding::RGBA16_UNORM)
  {
    log.Error(log::RENDER,
             "NrdProvider: expected NRD_NORMAL_ENCODING == RGBA16_UNORM (see "
             "_cmake/Thirdparty.cmake's PYXIS_WITH_NRD block) but the fetched NRD build "
             "reports nrd::NormalEncoding="
                 + std::to_string(static_cast<uint32_t>(normalEncoding))
                 + " -- nrd_pack.slang's packed IN_NORMAL_ROUGHNESS output will not match "
                   "what NRD's compiled-in shaders decode; re-run CMake configure to pick up "
                   "the cache-var override");
  }

  auto makeTexture = [this, &log, renderWidth, renderHeight](nvrhi::TextureHandle& handle,
                                                              nvrhi::Format format,
                                                              const char* debugName) {
    nvrhi::TextureDesc desc;
    desc.width = renderWidth;
    desc.height = renderHeight;
    desc.dimension = nvrhi::TextureDimension::Texture2D;
    desc.format = format;
    desc.isShaderResource = true;
    desc.isUAV = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    desc.debugName = debugName;
    handle = _device->createTexture(desc);
    if (!handle)
      log.Error(log::RENDER, std::string{"NrdProvider: createTexture failed for '"} + debugName
                                 + "'");
  };

  makeTexture(_packedNormalRoughness, nvrhi::Format::RGBA16_UNORM, "Nrd.Pack.NormalRoughness");
  makeTexture(_packedDiffuseRadianceHitDist, nvrhi::Format::RGBA16_FLOAT,
             "Nrd.Pack.DiffuseRadianceHitDist");
  makeTexture(_packedSpecRadianceHitDist, nvrhi::Format::RGBA16_FLOAT,
             "Nrd.Pack.SpecRadianceHitDist");
}

nrd::CommonSettings NrdProvider::BuildCommonSettings(const FrameInputs& inputs) const noexcept {
  // See NrdProvider.h's file comment ("STAGING PLAN", bullet 2) for the
  // short version of the two corrections below; this is the full
  // derivation for each of the THREE non-obvious fields.
  nrd::CommonSettings settings{};  // every untouched field keeps NRD's own default (NRDSettings.h).

  // ---- Matrices: TRANSPOSE, not a straight copy -------------------------
  // NRDSettings.h's CommonSettings doc comment: "usage - vector is a
  // column" / "layout - column-major". Pyxis's OWN convention (CLAUDE.md
  // §10) is ALSO column-vector math (v' = M * v) but ROW-MAJOR storage --
  // confirmed identical to what Streamline/DLSS wants verbatim
  // (DlssProviderFrame.cpp's ToSlMatrix is a straight memcpy of this same
  // layout, no transpose). Row-major and column-major storage of the SAME
  // column-vector matrix are transposes of each other as raw float arrays,
  // so -- unlike the DLSS path -- NRD's arrays need an explicit transpose
  // here. (This is a correction versus this stage's own design brief, which
  // stated NRD wants row-major arrays "like ours"; the actually-fetched
  // v4.17.3 NRDSettings.h says column-major, and this function follows the
  // header, not the brief.)
  TransposeMatrix16(inputs.viewToClip, settings.viewToClipMatrix);
  TransposeMatrix16(inputs.viewToClipPrev, settings.viewToClipMatrixPrev);
  TransposeMatrix16(inputs.worldToView, settings.worldToViewMatrix);
  TransposeMatrix16(inputs.worldToViewPrev, settings.worldToViewMatrixPrev);
  // worldPrevToWorldMatrix left at NRD's own identity default
  // (NRDSettings.h) -- Pyxis has no "moving coordinate system" concept yet
  // (no §43 reservation for it either); static-world identity is exactly
  // correct for every scene this stage targets.

  // ---- Motion-vector scale: negate AND normalize, not {1,1,0} -----------
  // Our gMotionVector is PIXEL-space with motionVector = currentPixel -
  // previousPixel (raytraced_gbuffer.slang's own derivation; denoise_
  // temporal.slang's reprojection -- `prevPixelF = pixel + 0.5 -
  // motionVector` -- reads it the same way). NRD's IN_MV wants the
  // OPPOSITE sign ("MV = previous - current", NRDDescs.h's ResourceType::
  // IN_MV comment) and NORMALIZED-UV-space usage ("pixelUvPrev = pixelUv +
  // mv.xy" with pixelUv in the (0;1) range, CommonSettings::
  // motionVectorScale's own doc comment -- confirmed against the compiled
  // shader itself, RELAX_TemporalAccumulation.cs.hlsl's literal
  // `pixelUv = (pixelPos+0.5)*gRectSizeInv; prevUVSMB = pixelUv + mv.xy`).
  // The negative sign fixes the direction flip; the 1/renderSize factor
  // fixes the pixel -> normalized-UV unit mismatch. Both corrections are
  // required together -- {1,1,0} (this stage's own design brief) would
  // only be correct if IN_MV already stored NRD-convention normalized
  // deltas, which our gMotionVector does not.
  settings.motionVectorScale[0] =
      inputs.renderWidth > 0u ? -1.0f / static_cast<float>(inputs.renderWidth) : 0.0f;
  settings.motionVectorScale[1] =
      inputs.renderHeight > 0u ? -1.0f / static_cast<float>(inputs.renderHeight) : 0.0f;
  settings.motionVectorScale[2] = 0.0f;  // 2D screen-space motion only -- no world-space MV signal.
  settings.isMotionVectorInWorldSpace = false;

  // ---- Jitter: direct passthrough (SAME sign, unlike motion vectors) ----
  // camera_ray.slang's BuildCameraRay samples at `launchIndex + 0.5 +
  // float2(gSampling.jitterX, gSampling.jitterY)` -- i.e. EXACTLY NRD's own
  // "sampleUv = pixelUv + cameraJitter" formula (CommonSettings::
  // cameraJitter's doc comment), pixelUv being the pixel-space center, same
  // units NRD wants. No sign flip, no scale -- unlike motion vectors above.
  settings.cameraJitter[0] = inputs.jitterX;
  settings.cameraJitter[1] = inputs.jitterY;
  // Previous frame's jitter -- Passes/CameraJitter.h's ComputeHaltonJitter
  // is a PURE function of frameIndex (Halton(2,3) + a fixed Cranley-
  // Patterson rotation), so frame (N-1)'s jitter is exactly
  // ComputeHaltonJitter(N-1). Recomputed here instead of threading a
  // second value through FrameInputs, so a caller can never accidentally
  // pass a stale/mismatched previous-jitter value -- the same "one
  // function, two consumers, cannot drift" reasoning CameraJitter.h's own
  // file comment documents for DlssPass.
  const std::uint64_t prevFrameIndex = inputs.frameIndex > 0u ? inputs.frameIndex - 1u : 0u;
  const auto jitterPrev = ComputeHaltonJitter(prevFrameIndex);
  settings.cameraJitterPrev[0] = jitterPrev.x;
  settings.cameraJitterPrev[1] = jitterPrev.y;

  // ---- Sizes -- no dynamic-resolution-scaling support this stage -------
  settings.resourceSize[0] = static_cast<uint16_t>(inputs.renderWidth);
  settings.resourceSize[1] = static_cast<uint16_t>(inputs.renderHeight);
  settings.resourceSizePrev[0] = static_cast<uint16_t>(inputs.renderWidth);
  settings.resourceSizePrev[1] = static_cast<uint16_t>(inputs.renderHeight);
  settings.rectSize[0] = static_cast<uint16_t>(inputs.renderWidth);
  settings.rectSize[1] = static_cast<uint16_t>(inputs.renderHeight);
  settings.rectSizePrev[0] = static_cast<uint16_t>(inputs.renderWidth);
  settings.rectSizePrev[1] = static_cast<uint16_t>(inputs.renderHeight);

  // denoisingRange explicitly set to NRD's own default (NRDSettings.h) --
  // "big enough" per this stage's brief; no scene-specific far-plane
  // wiring exists yet to do better. Written out explicitly (rather than
  // silently relying on the struct default) so the choice is visible here.
  settings.denoisingRange = 500000.0f;
  // viewZScale / disocclusionThreshold / ... all left at NRD's own
  // defaults -- gViewZ is already full-precision world-space linear depth
  // (R32F), no FP16 rescale needed.

  settings.frameIndex = static_cast<uint32_t>(inputs.frameIndex & 0xFFFFFFFFu);
  settings.accumulationMode =
      _pendingReset ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;

  return settings;
}

nvrhi::IBindingSet* NrdProvider::GetOrCreatePackBindingSet(const FrameInputs& inputs) {
  const void* keyParts[] = {
      inputs.normalRoughness,       inputs.diffuseRadianceHitDist,
      inputs.specRadianceHitDist,   _packedNormalRoughness.Get(),
      _packedDiffuseRadianceHitDist.Get(), _packedSpecRadianceHitDist.Get(),
  };
  const std::uint64_t key = HashPointers(keyParts, std::size(keyParts));
  if (auto cached = _packBindingSetCache.find(key); cached != _packBindingSetCache.end())
    return cached->second.Get();

  constexpr std::size_t MAX_CACHE_ENTRIES = 4;  // matches DenoiseTemporalPass.cpp's own cap.
  if (_packBindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _packBindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, inputs.normalRoughness),
      nvrhi::BindingSetItem::Texture_SRV(1, inputs.diffuseRadianceHitDist),
      nvrhi::BindingSetItem::Texture_SRV(2, inputs.specRadianceHitDist),
      nvrhi::BindingSetItem::Texture_UAV(3, _packedNormalRoughness),
      nvrhi::BindingSetItem::Texture_UAV(4, _packedDiffuseRadianceHitDist),
      nvrhi::BindingSetItem::Texture_UAV(5, _packedSpecRadianceHitDist),
      nvrhi::BindingSetItem::ConstantBuffer(6, _packParamsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _packBindingLayout);
  if (!set)
  {
    Logging::Get().Error(log::RENDER, "NrdProvider: createBindingSet(pack) failed");
    return nullptr;
  }
  nvrhi::IBindingSet* const result = set.Get();
  _packBindingSetCache.emplace(key, std::move(set));
  return result;
}

void NrdProvider::DispatchPack(nvrhi::ICommandList* commandList, const FrameInputs& inputs) {
  if (!_packReady)
    return;
  if (inputs.normalRoughness == nullptr || inputs.diffuseRadianceHitDist == nullptr
      || inputs.specRadianceHitDist == nullptr)
    return;

  NrdPackParams params{};
  params.width = _renderWidth;
  params.height = _renderHeight;
  commandList->writeBuffer(_packParamsBuffer, &params, sizeof(params));

  nvrhi::IBindingSet* const bindingSet = GetOrCreatePackBindingSet(inputs);
  if (bindingSet == nullptr)
    return;

  commandList->setTextureState(inputs.normalRoughness, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(inputs.diffuseRadianceHitDist, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(inputs.specRadianceHitDist, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_packedNormalRoughness, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_packedDiffuseRadianceHitDist, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_packedSpecRadianceHitDist, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::ComputeState computeState;
  computeState.pipeline = _packPipeline;
  computeState.bindings = {bindingSet};
  commandList->setComputeState(computeState);

  const uint32_t groupsX = (_renderWidth + 7u) / 8u;
  const uint32_t groupsY = (_renderHeight + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);
}

bool NrdProvider::Evaluate(nvrhi::ICommandList* commandList, const FrameInputs& inputs) noexcept {
  if (!_usable || commandList == nullptr)
    return false;
  if (inputs.renderWidth == 0u || inputs.renderHeight == 0u)
    return false;
  auto& log = Logging::Get();
  if (inputs.renderWidth != _renderWidth || inputs.renderHeight != _renderHeight)
  {
    // Caller must Resize() to THIS frame's render resolution before
    // calling Evaluate() -- same "Resize then Execute" contract every
    // other resizable pass/provider in this codebase follows. A mismatch
    // here means the caller forgot; pool/packed textures were sized for
    // _renderWidth/_renderHeight already and NrdProvider will not silently
    // adapt to a different size mid-call.
    log.Error(log::RENDER,
             "NrdProvider::Evaluate: inputs.renderWidth/Height ("
                 + std::to_string(inputs.renderWidth) + "x" + std::to_string(inputs.renderHeight)
                 + ") doesn't match the size Resize() last allocated ("
                 + std::to_string(_renderWidth) + "x" + std::to_string(_renderHeight) + ")");
    return false;
  }

  // ---- 1. Pack pre-pass: unpacked G-buffer guides -> NRD's expected encodings.
  DispatchPack(commandList, inputs);

  // ---- 2. SetCommonSettings.
  const nrd::CommonSettings commonSettings = BuildCommonSettings(inputs);
  const nrd::Result commonResult = nrd::SetCommonSettings(*_instance, commonSettings);
  if (commonResult != nrd::Result::SUCCESS)
  {
    log.Error(log::RENDER, "NrdProvider::Evaluate: nrd::SetCommonSettings failed (nrd::Result="
                               + std::to_string(static_cast<uint32_t>(commonResult)) + ")");
    return false;
  }

  // ---- 3. SetDenoiserSettings -- nrd::RelaxSettings is the ONE struct
  // every RELAX_* denoiser (including RELAX_DIFFUSE_SPECULAR) takes; there
  // is no separate "RelaxDiffuseSpecularSettings" type in the actually-
  // fetched v4.17.3 NRDSettings.h (a correction versus this stage's own
  // design brief). NRD's own defaults -- no RenderSettings-driven tuning
  // wired up yet, out of this stage's scope (RenderSettings.h is
  // untouched).
  const nrd::RelaxSettings relaxSettings{};
  const nrd::Result denoiserResult =
      nrd::SetDenoiserSettings(*_instance, RELAX_DIFFUSE_SPECULAR_ID, &relaxSettings);
  if (denoiserResult != nrd::Result::SUCCESS)
  {
    log.Error(log::RENDER, "NrdProvider::Evaluate: nrd::SetDenoiserSettings failed (nrd::Result="
                               + std::to_string(static_cast<uint32_t>(denoiserResult)) + ")");
    return false;
  }

  // ---- 4. GetComputeDispatches -- RELAX_DIFFUSE_SPECULAR_ID only; SIGMA_SHADOW
  // stays created-but-undispatched this stage (see file comment).
  const nrd::Identifier denoisers[] = {RELAX_DIFFUSE_SPECULAR_ID};
  const nrd::DispatchDesc* dispatchDescs = nullptr;
  uint32_t dispatchDescsNum = 0;
  const nrd::Result dispatchResult = nrd::GetComputeDispatches(
      *_instance, denoisers, static_cast<uint32_t>(std::size(denoisers)), dispatchDescs,
      dispatchDescsNum);
  if (dispatchResult != nrd::Result::SUCCESS)
  {
    log.Error(log::RENDER, "NrdProvider::Evaluate: nrd::GetComputeDispatches failed (nrd::Result="
                               + std::to_string(static_cast<uint32_t>(dispatchResult)) + ")");
    return false;
  }

  // ---- 5. Resource snapshot -- "real" (non-pool) resource types resolve
  // through this array; pool types resolve through Xxx PoolTexture() below.
  std::array<nvrhi::ITexture*, static_cast<std::size_t>(nrd::ResourceType::MAX_NUM)> snapshot{};
  snapshot[static_cast<std::size_t>(nrd::ResourceType::IN_MV)] = inputs.motionVector;
  snapshot[static_cast<std::size_t>(nrd::ResourceType::IN_NORMAL_ROUGHNESS)] =
      _packedNormalRoughness.Get();
  snapshot[static_cast<std::size_t>(nrd::ResourceType::IN_VIEWZ)] = inputs.viewZ;
  snapshot[static_cast<std::size_t>(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST)] =
      _packedDiffuseRadianceHitDist.Get();
  snapshot[static_cast<std::size_t>(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST)] =
      _packedSpecRadianceHitDist.Get();
  snapshot[static_cast<std::size_t>(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST)] =
      inputs.outDiffuseRadianceHitDist;
  snapshot[static_cast<std::size_t>(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST)] =
      inputs.outSpecRadianceHitDist;

  const nrd::InstanceDesc& instanceDesc = *nrd::GetInstanceDesc(*_instance);

  // ---- 6. Per-dispatch translation -- mirrors NRD's own reference
  // integration's call sequence (Integration/NRDIntegration.hpp's
  // Denoise()/_Dispatch()), which is dictated by NRD's public API contract
  // (resolve ResourceDesc list -> binding set -> constants -> barriers ->
  // dispatch), not copied from it.
  // See the constant-buffer upload comment inside the loop: the very first
  // CB-carrying dispatch of every Evaluate must write regardless of NRD's
  // cross-call matches-previous flag (volatile-CB versions are per command
  // list).
  bool cbWrittenThisCall = false;
  for (uint32_t dispatchIndex = 0; dispatchIndex < dispatchDescsNum; ++dispatchIndex)
  {
    const nrd::DispatchDesc& dispatch = dispatchDescs[dispatchIndex];
    if (dispatch.pipelineIndex >= _pipelines.size())
    {
      log.Error(log::RENDER, "NrdProvider::Evaluate: dispatch pipelineIndex out of range");
      continue;
    }
    if (dispatch.resourcesNum > MAX_DISPATCH_RESOURCES)
    {
      log.Error(log::RENDER, "NrdProvider::Evaluate: dispatch resourcesNum ("
                                 + std::to_string(dispatch.resourcesNum) + ") exceeds "
                                 + std::to_string(MAX_DISPATCH_RESOURCES));
      continue;
    }

    const Pipeline& pipeline = _pipelines[dispatch.pipelineIndex];

    nvrhi::ITexture* resolved[MAX_DISPATCH_RESOURCES] = {};
    bool resourcesOk = true;
    for (uint32_t resourceIndex = 0; resourceIndex < dispatch.resourcesNum; ++resourceIndex)
    {
      const nrd::ResourceDesc& resourceDesc = dispatch.resources[resourceIndex];
      nvrhi::ITexture* tex = nullptr;
      if (resourceDesc.type == nrd::ResourceType::TRANSIENT_POOL)
        tex = TransientPoolTexture(resourceDesc.indexInPool);
      else if (resourceDesc.type == nrd::ResourceType::PERMANENT_POOL)
        tex = PermanentPoolTexture(resourceDesc.indexInPool);
      else
        tex = snapshot[static_cast<std::size_t>(resourceDesc.type)];
      if (tex == nullptr)
      {
        resourcesOk = false;
        break;
      }
      resolved[resourceIndex] = tex;
    }
    if (!resourcesOk)
    {
      log.Error(log::RENDER, std::string{"NrdProvider::Evaluate: dispatch '"}
                                 + (dispatch.name != nullptr ? dispatch.name : "?")
                                 + "' references an unbound resource -- skipping");
      continue;
    }

    // Cache key: pipeline identity + every resolved resource pointer, in
    // order -- ping-ponged pool textures swap roles frame to frame, so the
    // key must be recomputed every frame even though the same dispatch
    // recurs every frame.
    const void* keyParts[MAX_DISPATCH_RESOURCES + 1] = {};
    keyParts[0] = pipeline.pipeline.Get();
    for (uint32_t resourceIndex = 0; resourceIndex < dispatch.resourcesNum; ++resourceIndex)
      keyParts[resourceIndex + 1] = resolved[resourceIndex];
    const std::uint64_t key = HashPointers(keyParts, dispatch.resourcesNum + 1u);

    nvrhi::IBindingSet* bindingSet = nullptr;
    if (auto cached = _dispatchBindingSetCache.find(key); cached != _dispatchBindingSetCache.end())
    {
      bindingSet = cached->second.Get();
    }
    else
    {
      // RELAX_DIFFUSE_SPECULAR's own dispatch/ping-pong-role permutation
      // count is small and stable frame-to-frame -- generous headroom,
      // same cap-then-clear precedent as DenoiseTemporalPass.cpp
      // (MAX_CACHE_ENTRIES=4), scaled up for NRD's larger dispatch count.
      constexpr std::size_t MAX_CACHE_ENTRIES = 256;
      if (_dispatchBindingSetCache.size() >= MAX_CACHE_ENTRIES)
        _dispatchBindingSetCache.clear();

      nvrhi::BindingSetDesc setDesc;
      uint32_t textureSlot = instanceDesc.resourcesBaseRegisterIndex;
      uint32_t storageSlot = instanceDesc.resourcesBaseRegisterIndex;
      for (uint32_t resourceIndex = 0; resourceIndex < dispatch.resourcesNum; ++resourceIndex)
      {
        const nrd::ResourceDesc& resourceDesc = dispatch.resources[resourceIndex];
        if (resourceDesc.descriptorType == nrd::DescriptorType::TEXTURE)
          setDesc.bindings.push_back(
              nvrhi::BindingSetItem::Texture_SRV(textureSlot++, resolved[resourceIndex]));
        else
          setDesc.bindings.push_back(
              nvrhi::BindingSetItem::Texture_UAV(storageSlot++, resolved[resourceIndex]));
      }
      // (Samplers + CB live in the shared SET-1 binding set — see
      // _set1Layout's doc comment; this per-dispatch set is SET 0 only.)
      nvrhi::BindingSetHandle newSet = _device->createBindingSet(setDesc, pipeline.bindingLayout);
      if (!newSet)
      {
        log.Error(log::RENDER, "NrdProvider::Evaluate: createBindingSet failed for dispatch '"
                                   + std::string{dispatch.name != nullptr ? dispatch.name : "?"}
                                   + "'");
        continue;
      }
      bindingSet = newSet.Get();
      _dispatchBindingSetCache.emplace(key, std::move(newSet));
    }

    // ---- Constant-buffer upload -- NRD's constantBufferDataMatchesPrevious-
    // Dispatch flag lets identical consecutive uploads be skipped, BUT its
    // "previous dispatch" spans Evaluate() calls (frames), while nvrhi's
    // volatile-CB versioning is PER COMMAND LIST: a frame whose FIRST
    // CB-carrying dispatch reported matches-previous would write nothing on
    // this frame's fresh command list and the binding would resolve to a
    // stale/undefined version — the first-light "black output" bug
    // (2026-07-10 debug: dispatch inventory healthy, packed inputs healthy
    // via bypass probe, output black). Force-write the first CB-carrying
    // dispatch of every Evaluate; honor the skip only within the same call.
    if (dispatch.constantBufferDataSize > 0u && _nrdConstantBuffer
        && (!dispatch.constantBufferDataMatchesPreviousDispatch || !cbWrittenThisCall))
    {
      commandList->writeBuffer(_nrdConstantBuffer, dispatch.constantBufferData,
                               dispatch.constantBufferDataSize);
      cbWrittenThisCall = true;
    }

    // ---- Explicit resource-state transitions, per dispatch -- pool
    // textures ping-pong between SRV and UAV roles dispatch to dispatch
    // within the SAME frame (NRD's own reference integration barriers
    // exactly this way in Integration/NRDIntegration.hpp's _Dispatch()).
    for (uint32_t resourceIndex = 0; resourceIndex < dispatch.resourcesNum; ++resourceIndex)
    {
      const nrd::ResourceDesc& resourceDesc = dispatch.resources[resourceIndex];
      const nvrhi::ResourceStates resourceState =
          (resourceDesc.descriptorType == nrd::DescriptorType::TEXTURE)
              ? nvrhi::ResourceStates::ShaderResource
              : nvrhi::ResourceStates::UnorderedAccess;
      commandList->setTextureState(resolved[resourceIndex], nvrhi::AllSubresources, resourceState);
    }
    commandList->commitBarriers();

    nvrhi::ComputeState computeState;
    computeState.pipeline = pipeline.pipeline;
    // SET 0 = per-dispatch resources; SET 1 = shared samplers + CB.
    computeState.bindings = {bindingSet, _set1BindingSet};
    commandList->setComputeState(computeState);
    commandList->dispatch(dispatch.gridWidth, dispatch.gridHeight, 1u);
  }

  _pendingReset = false;
  return true;
}

nvrhi::Format NrdProvider::MapFormat(nrd::Format format) noexcept {
  switch (format)
  {
    case nrd::Format::R8_UNORM: return nvrhi::Format::R8_UNORM;
    case nrd::Format::R8_SNORM: return nvrhi::Format::R8_SNORM;
    case nrd::Format::R8_UINT: return nvrhi::Format::R8_UINT;
    case nrd::Format::R8_SINT: return nvrhi::Format::R8_SINT;

    case nrd::Format::RG8_UNORM: return nvrhi::Format::RG8_UNORM;
    case nrd::Format::RG8_SNORM: return nvrhi::Format::RG8_SNORM;
    case nrd::Format::RG8_UINT: return nvrhi::Format::RG8_UINT;
    case nrd::Format::RG8_SINT: return nvrhi::Format::RG8_SINT;

    case nrd::Format::RGBA8_UNORM: return nvrhi::Format::RGBA8_UNORM;
    case nrd::Format::RGBA8_SNORM: return nvrhi::Format::RGBA8_SNORM;
    case nrd::Format::RGBA8_UINT: return nvrhi::Format::RGBA8_UINT;
    case nrd::Format::RGBA8_SINT: return nvrhi::Format::RGBA8_SINT;
    case nrd::Format::RGBA8_SRGB: return nvrhi::Format::SRGBA8_UNORM;

    case nrd::Format::R16_UNORM: return nvrhi::Format::R16_UNORM;
    case nrd::Format::R16_SNORM: return nvrhi::Format::R16_SNORM;
    case nrd::Format::R16_UINT: return nvrhi::Format::R16_UINT;
    case nrd::Format::R16_SINT: return nvrhi::Format::R16_SINT;
    case nrd::Format::R16_SFLOAT: return nvrhi::Format::R16_FLOAT;

    case nrd::Format::RG16_UNORM: return nvrhi::Format::RG16_UNORM;
    case nrd::Format::RG16_SNORM: return nvrhi::Format::RG16_SNORM;
    case nrd::Format::RG16_UINT: return nvrhi::Format::RG16_UINT;
    case nrd::Format::RG16_SINT: return nvrhi::Format::RG16_SINT;
    case nrd::Format::RG16_SFLOAT: return nvrhi::Format::RG16_FLOAT;

    case nrd::Format::RGBA16_UNORM: return nvrhi::Format::RGBA16_UNORM;
    case nrd::Format::RGBA16_SNORM: return nvrhi::Format::RGBA16_SNORM;
    case nrd::Format::RGBA16_UINT: return nvrhi::Format::RGBA16_UINT;
    case nrd::Format::RGBA16_SINT: return nvrhi::Format::RGBA16_SINT;
    case nrd::Format::RGBA16_SFLOAT: return nvrhi::Format::RGBA16_FLOAT;

    case nrd::Format::R32_UINT: return nvrhi::Format::R32_UINT;
    case nrd::Format::R32_SINT: return nvrhi::Format::R32_SINT;
    case nrd::Format::R32_SFLOAT: return nvrhi::Format::R32_FLOAT;

    case nrd::Format::RG32_UINT: return nvrhi::Format::RG32_UINT;
    case nrd::Format::RG32_SINT: return nvrhi::Format::RG32_SINT;
    case nrd::Format::RG32_SFLOAT: return nvrhi::Format::RG32_FLOAT;

    case nrd::Format::RGB32_UINT: return nvrhi::Format::RGB32_UINT;
    case nrd::Format::RGB32_SINT: return nvrhi::Format::RGB32_SINT;
    case nrd::Format::RGB32_SFLOAT: return nvrhi::Format::RGB32_FLOAT;

    case nrd::Format::RGBA32_UINT: return nvrhi::Format::RGBA32_UINT;
    case nrd::Format::RGBA32_SINT: return nvrhi::Format::RGBA32_SINT;
    case nrd::Format::RGBA32_SFLOAT: return nvrhi::Format::RGBA32_FLOAT;

    case nrd::Format::R10_G10_B10_A2_UNORM: return nvrhi::Format::R10G10B10A2_UNORM;
    case nrd::Format::R11_G11_B10_UFLOAT: return nvrhi::Format::R11G11B10_FLOAT;

    // nvrhi::Format (nvrhi/nvrhi.h) has NEITHER an unsigned-integer
    // R10G10B10A2 format NOR a R9G9B9E5 shared-exponent format -- these
    // two nrd::Format enumerators have no nvrhi equivalent. Empirically
    // unreachable for the two denoisers this provider instantiates
    // (RELAX_DIFFUSE_SPECULAR + SIGMA_SHADOW never allocate a pool texture
    // in either format -- they belong to other denoisers'/signals' packed-
    // normal or shared-exponent-HDR paths this provider doesn't
    // instantiate), but the switch stays exhaustive so a future denoiser
    // addition fails loudly here instead of silently mis-mapping a format.
    // No PYXIS_ASSERT: that macro (§30's three-tier error scheme) has no
    // concrete definition anywhere in this codebase yet (verified via
    // repo-wide search) -- this instead follows the log-and-degrade
    // convention every other provider here uses (e.g. DlssProvider.cpp)
    // rather than inventing a new assert convention in passing.
    case nrd::Format::R10_G10_B10_A2_UINT:
    case nrd::Format::R9_G9_B9_E5_UFLOAT:
    case nrd::Format::MAX_NUM:
    default:
      Logging::Get().Error(log::RENDER,
                           "NrdProvider: nrd::Format " + std::to_string(static_cast<uint32_t>(format))
                               + " has no nvrhi::Format equivalent");
      return nvrhi::Format::UNKNOWN;
  }
}

}  // namespace pyxis
