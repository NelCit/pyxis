// Pyxis renderer — DenoiserResources (RTX-alignment design, Phase B).

#include "Passes/DenoiserResources.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <string>

namespace pyxis {

namespace {

nvrhi::TextureHandle MakeHistoryTexture(nvrhi::IDevice* device, uint32_t width, uint32_t height,
                                        const char* debugName) {
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = nvrhi::Format::RGBA16_FLOAT;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;
  desc.isShaderResource = true;
  desc.debugName = debugName;
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  return device->createTexture(desc);
}

}  // namespace

bool DenoiserResources::Ensure(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return false;
  if (_width == width && _height == height && _diffuse[0] && _diffuse[1] && _specular[0]
      && _specular[1] && _normalViewZ[0] && _normalViewZ[1] && _diffuseFast[0] && _diffuseFast[1]
      && _specularFast[0] && _specularFast[1])
    return true;

  std::array<nvrhi::TextureHandle, 2> diffuse;
  std::array<nvrhi::TextureHandle, 2> specular;
  std::array<nvrhi::TextureHandle, 2> normalViewZ;
  std::array<nvrhi::TextureHandle, 2> diffuseFast;
  std::array<nvrhi::TextureHandle, 2> specularFast;
  diffuse[0] = MakeHistoryTexture(_device, width, height, "Denoiser.Diffuse0");
  diffuse[1] = MakeHistoryTexture(_device, width, height, "Denoiser.Diffuse1");
  specular[0] = MakeHistoryTexture(_device, width, height, "Denoiser.Specular0");
  specular[1] = MakeHistoryTexture(_device, width, height, "Denoiser.Specular1");
  normalViewZ[0] = MakeHistoryTexture(_device, width, height, "Denoiser.NormalViewZ0");
  normalViewZ[1] = MakeHistoryTexture(_device, width, height, "Denoiser.NormalViewZ1");
  // Work item 2 — FAST accumulator pair (see class doc comment header).
  diffuseFast[0] = MakeHistoryTexture(_device, width, height, "Denoiser.DiffuseFast0");
  diffuseFast[1] = MakeHistoryTexture(_device, width, height, "Denoiser.DiffuseFast1");
  specularFast[0] = MakeHistoryTexture(_device, width, height, "Denoiser.SpecularFast0");
  specularFast[1] = MakeHistoryTexture(_device, width, height, "Denoiser.SpecularFast1");

  if (!diffuse[0] || !diffuse[1] || !specular[0] || !specular[1] || !normalViewZ[0]
      || !normalViewZ[1] || !diffuseFast[0] || !diffuseFast[1] || !specularFast[0]
      || !specularFast[1]) {
    Logging::Get().Error(log::RENDER, "DenoiserResources: createTexture(history, "
                                          + std::to_string(width) + "x" + std::to_string(height)
                                          + ") failed; keeping previous allocation");
    return false;
  }

  _diffuse = diffuse;
  _specular = specular;
  _normalViewZ = normalViewZ;
  _diffuseFast = diffuseFast;
  _specularFast = specularFast;
  _width = width;
  _height = height;
  _prevIndex = 0u;
  _currIndex = 1u;
  _hasHistory = false;  // fresh textures — no previous-frame data yet.
  return true;
}

}  // namespace pyxis
