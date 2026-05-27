// Pyxis platform — synchronous GPU->CPU texture readback helper.

#include <Pyxis/Platform/Gpu/TextureReadback.h>

#include <cstring>
#include <utility>

namespace pyxis {

std::expected<void, std::string> TextureReadback::RecordCopy(nvrhi::IDevice* device,
                                                             nvrhi::ICommandList* commandList,
                                                             nvrhi::ITexture* source,
                                                             const char* debugName) noexcept {
  if (device == nullptr)
    return std::unexpected{std::string{"TextureReadback::RecordCopy: null device"}};
  if (commandList == nullptr)
    return std::unexpected{std::string{"TextureReadback::RecordCopy: null commandList"}};
  if (source == nullptr)
    return std::unexpected{std::string{"TextureReadback::RecordCopy: null source"}};

  const nvrhi::TextureDesc& sourceDesc = source->getDesc();

  // Drop any prior mapping before recording a new copy (reuse across frames).
  Unmap();

  // Reuse the staging texture when it already matches; otherwise (re)create it.
  if (!_staging || _width != sourceDesc.width || _height != sourceDesc.height
      || _format != sourceDesc.format) {
    nvrhi::TextureDesc stagingDesc;
    stagingDesc.format = sourceDesc.format;
    stagingDesc.width = sourceDesc.width;
    stagingDesc.height = sourceDesc.height;
    stagingDesc.dimension = nvrhi::TextureDimension::Texture2D;
    stagingDesc.debugName = debugName != nullptr ? debugName : "readback-staging";
    _staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
    if (!_staging)
      return std::unexpected{std::string{"TextureReadback::RecordCopy: createStagingTexture failed"}};
    _width = sourceDesc.width;
    _height = sourceDesc.height;
    _format = sourceDesc.format;
  }
  _device = device;

  commandList->copyTexture(_staging.Get(), nvrhi::TextureSlice{}, source, nvrhi::TextureSlice{});
  return {};
}

std::expected<TextureReadback, std::string> TextureReadback::RecordCopyOnce(
    nvrhi::IDevice* device, nvrhi::ICommandList* commandList, nvrhi::ITexture* source,
    const char* debugName) noexcept {
  TextureReadback readback;
  if (auto recorded = readback.RecordCopy(device, commandList, source, debugName); !recorded)
    return std::unexpected{recorded.error()};
  return readback;
}

std::expected<void, std::string> TextureReadback::Map() noexcept {
  if (_device == nullptr || !_staging)
    return std::unexpected{std::string{"TextureReadback::Map: helper not initialised by RecordCopy"}};
  if (_mapped != nullptr)
    return std::unexpected{std::string{"TextureReadback::Map: already mapped"}};
  _mapped = _device->mapStagingTexture(_staging.Get(), nvrhi::TextureSlice{},
                                       nvrhi::CpuAccessMode::Read, &_rowPitch);
  if (_mapped == nullptr)
    return std::unexpected{std::string{"TextureReadback::Map: mapStagingTexture failed"}};
  return {};
}

void TextureReadback::Unmap() noexcept {
  if (_mapped != nullptr && _device != nullptr && _staging) {
    _device->unmapStagingTexture(_staging.Get());
    _mapped = nullptr;
  }
}

void TextureReadback::CopyTightlyInto(std::vector<uint8_t>& out) const noexcept {
  if (_mapped == nullptr || _width == 0 || _height == 0)
    return;
  const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(_format);
  const size_t bytesPerPixel = info.bytesPerBlock;  // non-block formats: bytes/texel
  const size_t tightRow = static_cast<size_t>(_width) * bytesPerPixel;
  out.resize(tightRow * _height);  // reuses capacity across frames
  const auto* src = static_cast<const uint8_t*>(_mapped);
  for (uint32_t row = 0; row < _height; ++row)
    std::memcpy(out.data() + row * tightRow, src + row * _rowPitch, tightRow);
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
  if (this != &other) {
    Unmap();
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

TextureReadback::~TextureReadback() noexcept { Unmap(); }

}  // namespace pyxis
