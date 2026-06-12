// Pyxis renderer — shared SPIR-V loading helper (renderer-private).
//
// The §9.1 ToneMapPass worked example anticipated a shared "ShaderLibrary" that
// passes take in their ctor; this is the minimal form of it. RaytracedLightingPass and
// SsaaResolvePass both loaded a .spv from disk into an nvrhi::ShaderHandle with
// byte-for-byte the same `ifstream(binary|ate) -> read -> createShader` body — the
// two copies had already drifted (`!stream` vs `!is_open()`). One definition here.

#pragma once

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace pyxis {

// Read a binary file fully into a byte vector. Empty vector on any failure.
[[nodiscard]] inline std::vector<char> ReadBinaryFile(std::string_view path) noexcept {
  std::ifstream stream(std::string{path}, std::ios::binary | std::ios::ate);
  if (!stream.is_open())
    return {};
  const std::streamsize fileSize = stream.tellg();
  if (fileSize <= 0)
    return {};
  std::vector<char> bytes(static_cast<std::size_t>(fileSize));
  stream.seekg(0, std::ios::beg);
  stream.read(bytes.data(), fileSize);
  return bytes;
}

// Load a SPIR-V shader from `path` into an nvrhi::ShaderHandle. Returns null and
// logs (log::RENDER, prefixed with `logPrefix`) on a missing/empty .spv or a
// createShader failure. `entry` is the SPIR-V entry-point name (Slang emits
// "main" for every [shader(...)] function — see RaytracedLightingPass note).
[[nodiscard]] inline nvrhi::ShaderHandle LoadSpirvShader(nvrhi::IDevice* device,
                                                         std::string_view path,
                                                         nvrhi::ShaderType stage,
                                                         const char* entry,
                                                         std::string_view logPrefix) noexcept {
  const std::vector<char> bytes = ReadBinaryFile(path);
  if (bytes.empty()) {
    std::string msg{logPrefix};
    msg.append(": failed to load shader ");
    msg.append(path);
    Logging::Get().Error(log::RENDER, msg);
    return nullptr;
  }
  nvrhi::ShaderDesc shaderDesc{};
  shaderDesc.shaderType = stage;
  shaderDesc.entryName = entry;
  shaderDesc.debugName = std::string{path};
  return device->createShader(shaderDesc, bytes.data(), bytes.size());
}

}  // namespace pyxis
