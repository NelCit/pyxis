// Pyxis renderer — NrdProvider implementation. See NrdProvider.h's file
// comment for the licensing posture, staging plan, and the SPIR-V
// binding-offset flattening design this file implements.

#include "Nrd/NrdProvider.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

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

  if (!CreatePipelines(instanceDesc))
  {
    nrd::DestroyInstance(*_instance);
    _instance = nullptr;
    return;
  }

  _permanentPool = MakePoolDescs(instanceDesc.permanentPool, instanceDesc.permanentPoolSize);
  _transientPool = MakePoolDescs(instanceDesc.transientPool, instanceDesc.transientPoolSize);

  _usable = true;
  log.Info(log::RENDER,
           "NrdProvider: instance created (RELAX_DIFFUSE_SPECULAR + SIGMA_SHADOW, "
               + std::to_string(instanceDesc.pipelinesNum) + " pipelines, "
               + std::to_string(instanceDesc.samplersNum) + " samplers, "
               + std::to_string(instanceDesc.permanentPoolSize) + " permanent + "
               + std::to_string(instanceDesc.transientPoolSize)
               + " transient pool textures) -- runtime evaluation staged (see file comment)");
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

    // ---- Binding layout: one flattened Vulkan descriptor set --------
    // See NrdProvider.h's "SPIR-V BINDING-OFFSET FLATTENING" section for
    // the full derivation. `slot` below is always the RAW NRD HLSL
    // register index; `bindingOffsets` (set once per layout) is what
    // shifts each item into the final Vulkan binding number, matching
    // NVRHI's own `bindingLocation = registerOffset + item.slot` formula
    // (src/vulkan/vulkan-resource-bindings.cpp).
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

    // Shared constant buffer -- only pipelines that actually reference it
    // declare the binding (PipelineDesc::hasConstantData).
    if (pipelineDesc.hasConstantData)
      layoutDesc.bindings.push_back(
          nvrhi::BindingLayoutItem::VolatileConstantBuffer(instanceDesc.constantBufferRegisterIndex));

    // Samplers -- every pipeline gets the full, shared sampler set
    // (instanceDesc.samplers[], "s" registers starting at
    // samplersBaseRegisterIndex), matching NRDIntegration.hpp's own
    // per-pipeline-layout-shared root-sampler list.
    for (uint32_t i = 0; i < instanceDesc.samplersNum; ++i)
      layoutDesc.bindings.push_back(
          nvrhi::BindingLayoutItem::Sampler(instanceDesc.samplersBaseRegisterIndex + i));

    nvrhi::BindingLayoutHandle bindingLayout = _device->createBindingLayout(layoutDesc);
    if (!bindingLayout)
    {
      log.Error(log::RENDER,
               "NrdProvider: createBindingLayout failed for pipeline '" + debugName + "'");
      return false;
    }

    nvrhi::ComputePipelineDesc computeDesc;
    computeDesc.CS = shader;
    computeDesc.bindingLayouts = {bindingLayout};
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
