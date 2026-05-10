// Pyxis app — caller-side AOV texture ownership.

#include "Render/AovTextures.h"

namespace pyxis::app {

std::expected<AovTextures, std::string> AovTextures::Create(nvrhi::IDevice* device, uint32_t width,
                                                            uint32_t height) noexcept {
  if (device == nullptr)
  {
    return std::unexpected{std::string{"AovTextures::Create: null device"}};
  }
  if (width == 0 || height == 0)
  {
    return std::unexpected{std::string{"AovTextures::Create: zero dim"}};
  }

  // initialState=RenderTarget + keepInitialState=true matches what the
  // viewer swapchain wrap and the previous headless offscreen RT used,
  // so NVRHI's tracker observes identical first-use transitions across
  // modes. Without keepInitialState NVRHI would assume the texture
  // starts in Common state and inject an extra barrier on first use,
  // which would invalidate the §33.7 byte-equal EXR contract.
  nvrhi::TextureDesc desc;
  // BGRA8_UNORM (linear) — NVRHI's `SBGRA8_UNORM` maps to
  // `VK_FORMAT_B8G8R8A8_SRGB` which does NOT support
  // `VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT` (sRGB encoding can't be
  // written by an unordered RW image write). M3's PathTracePass
  // binds this AOV as `RWTexture2D<float4>`, so storage is
  // mandatory; swap to the linear variant. M5+ promotes color to
  // RGBA16_FLOAT per plan §18.4 once HDR fidelity is needed and
  // the tonemap pass takes over the LDR conversion responsibility.
  desc.format = nvrhi::Format::BGRA8_UNORM;
  desc.width = width;
  desc.height = height;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isRenderTarget = true;
  desc.isUAV = true;  // M3 PathTracePass writes via RWTexture2D<float4>.
  desc.debugName = "aov.color";
  desc.initialState = nvrhi::ResourceStates::RenderTarget;
  desc.keepInitialState = true;

  AovTextures result;
  result.color = device->createTexture(desc);
  if (!result.color)
  {
    return std::unexpected{std::string{"AovTextures::Create: createTexture(aov.color) failed"}};
  }

  // Helper for the M7 raw-AOV textures: same dimension / UAV /
  // initialState as `color`, just a different format + debug name.
  auto makeAov = [&](nvrhi::Format fmt, const char* dbgName) -> nvrhi::TextureHandle {
    nvrhi::TextureDesc aovDesc = desc;
    aovDesc.format    = fmt;
    aovDesc.debugName = dbgName;
    return device->createTexture(aovDesc);
  };
  result.colorHdr    = makeAov(nvrhi::Format::RGBA16_FLOAT, "aov.colorHdr");
  result.normal      = makeAov(nvrhi::Format::RGBA16_FLOAT, "aov.normal");
  result.depth       = makeAov(nvrhi::Format::R32_FLOAT,    "aov.depth");
  result.primId      = makeAov(nvrhi::Format::R32_UINT,     "aov.primId");
  result.materialId  = makeAov(nvrhi::Format::R32_UINT,     "aov.materialId");
  result.baseColor   = makeAov(nvrhi::Format::RGBA16_FLOAT, "aov.baseColor");
  result.worldPos    = makeAov(nvrhi::Format::RGBA32_FLOAT, "aov.worldPos");
  // Tier 1 Hydra-canonical AOVs.
  result.alpha       = makeAov(nvrhi::Format::R8_UNORM,     "aov.alpha");
  result.elementId   = makeAov(nvrhi::Format::R32_UINT,     "aov.elementId");
  result.normalEye   = makeAov(nvrhi::Format::RGBA16_FLOAT, "aov.normalEye");
  result.worldPosEye = makeAov(nvrhi::Format::RGBA32_FLOAT, "aov.worldPosEye");
  if (!result.colorHdr || !result.normal || !result.depth || !result.primId
      || !result.materialId || !result.baseColor || !result.worldPos
      || !result.alpha || !result.elementId || !result.normalEye || !result.worldPosEye)
  {
    return std::unexpected{std::string{"AovTextures::Create: createTexture(raw AOV) failed"}};
  }

  // Pick-result buffer pair. The device-side buffer is the
  // RWStructuredBuffer the raygen writes; the staging buffer is
  // host-mapped after a per-frame copy for one-frame-stale CPU
  // readback. 80 bytes per element matches shaderinterop::PickResult
  // (5 rows of 16: color/depth, normal/primId, baseColor/materialId,
  // worldHit/pad, pixelXY/pad).
  constexpr uint32_t PICK_RESULT_BYTES = 80;
  nvrhi::BufferDesc pickDesc;
  pickDesc.byteSize = PICK_RESULT_BYTES;
  pickDesc.structStride = PICK_RESULT_BYTES;
  pickDesc.canHaveUAVs = true;
  pickDesc.debugName = "aov.pickResult";
  pickDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  pickDesc.keepInitialState = true;
  result.pickResult = device->createBuffer(pickDesc);
  if (!result.pickResult)
  {
    return std::unexpected{std::string{"AovTextures::Create: createBuffer(pickResult) failed"}};
  }

  nvrhi::BufferDesc stagingDesc;
  stagingDesc.byteSize = PICK_RESULT_BYTES;
  stagingDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
  stagingDesc.debugName = "aov.pickResult.staging";
  result.pickResultStaging = device->createBuffer(stagingDesc);
  if (!result.pickResultStaging)
  {
    return std::unexpected{
        std::string{"AovTextures::Create: createBuffer(pickResult.staging) failed"}};
  }

  result.width = width;
  result.height = height;
  return result;
}

}  // namespace pyxis::app
