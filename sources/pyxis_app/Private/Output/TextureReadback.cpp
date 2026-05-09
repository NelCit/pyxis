// Pyxis app — synchronous texture readback helper.

#include "Output/TextureReadback.h"

#include <utility>

namespace pyxis::app {

std::expected<TextureReadback, std::string> TextureReadback::RecordCopy(
    nvrhi::IDevice* device, nvrhi::ICommandList* commandList, nvrhi::ITexture* source,
    const char* debugName) noexcept {
  if (device == nullptr)
  {
    return std::unexpected{std::string{"TextureReadback::RecordCopy: null device"}};
  }
  if (commandList == nullptr)
  {
    return std::unexpected{std::string{"TextureReadback::RecordCopy: null commandList"}};
  }
  if (source == nullptr)
  {
    return std::unexpected{std::string{"TextureReadback::RecordCopy: null source"}};
  }

  const auto& sourceDesc = source->getDesc();

  nvrhi::TextureDesc stagingDesc;
  stagingDesc.format = sourceDesc.format;
  stagingDesc.width = sourceDesc.width;
  stagingDesc.height = sourceDesc.height;
  stagingDesc.dimension = nvrhi::TextureDimension::Texture2D;
  stagingDesc.debugName = debugName != nullptr ? debugName : "readback-staging";

  nvrhi::StagingTextureHandle staging =
      device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
  if (!staging)
  {
    return std::unexpected{std::string{"TextureReadback::RecordCopy: createStagingTexture failed"}};
  }

  commandList->copyTexture(staging.Get(), nvrhi::TextureSlice{}, source, nvrhi::TextureSlice{});

  TextureReadback result;
  result._device = device;
  result._staging = std::move(staging);
  result._width = sourceDesc.width;
  result._height = sourceDesc.height;
  result._format = sourceDesc.format;
  return result;
}

std::expected<void, std::string> TextureReadback::Map() noexcept {
  if (_device == nullptr || !_staging)
  {
    return std::unexpected{
        std::string{"TextureReadback::Map: helper not initialised by RecordCopy"}};
  }
  if (_mapped != nullptr)
  {
    return std::unexpected{std::string{"TextureReadback::Map: already mapped"}};
  }
  _mapped = _device->mapStagingTexture(_staging.Get(), nvrhi::TextureSlice{},
                                       nvrhi::CpuAccessMode::Read, &_rowPitch);
  if (_mapped == nullptr)
  {
    return std::unexpected{std::string{"TextureReadback::Map: mapStagingTexture failed"}};
  }
  return {};
}

TextureReadback::TextureReadback(TextureReadback&& other) noexcept
    : _device(other._device),
      _staging(std::move(other._staging)),
      _mapped(other._mapped),
      _rowPitch(other._rowPitch),
      _width(other._width),
      _height(other._height),
      _format(other._format) {
  other._device = nullptr;
  other._mapped = nullptr;
  other._rowPitch = 0;
  other._width = 0;
  other._height = 0;
  other._format = nvrhi::Format::UNKNOWN;
}

TextureReadback& TextureReadback::operator=(TextureReadback&& other) noexcept {
  if (this != &other)
  {
    if (_mapped != nullptr && _device != nullptr && _staging)
    {
      _device->unmapStagingTexture(_staging.Get());
    }
    _device = other._device;
    _staging = std::move(other._staging);
    _mapped = other._mapped;
    _rowPitch = other._rowPitch;
    _width = other._width;
    _height = other._height;
    _format = other._format;
    other._device = nullptr;
    other._mapped = nullptr;
    other._rowPitch = 0;
    other._width = 0;
    other._height = 0;
    other._format = nvrhi::Format::UNKNOWN;
  }
  return *this;
}

TextureReadback::~TextureReadback() noexcept {
  if (_mapped != nullptr && _device != nullptr && _staging)
  {
    _device->unmapStagingTexture(_staging.Get());
  }
}

}  // namespace pyxis::app
