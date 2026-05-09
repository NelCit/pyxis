// Pyxis renderer — public Error / Expected types.
//
// Plan §18.3 + §20. Every public API method that can fail returns
// `pyxis::Expected<T>` (a `std::expected<T, pyxis::Error>` alias). The
// `Error` POD is byte-stable and crosses DLL boundaries safely:
//
//   - `ErrorMessage` is an inline-owning fixed-capacity string (§18.9
//     ABI rule: no STL containers in public PODs); the renderer
//     truncates with a trailing "..." past CAPACITY rather than
//     allocating.
//   - `ErrorKind` is the §20 catalogue, encoded in a `uint16_t` so
//     callers can pattern-match without string parsing.
//   - `Error::source` is "file:line" filled in by the PYXIS_ERROR
//     macro at the failure site (§30 / §20). __FILE__ is mapped to a
//     repo-relative path by Compiler.cmake's
//     `-fmacro-prefix-map=<repo>=.` so absolute paths don't bleed
//     into the binary.
//
// Threading: `Error` is trivially copyable. `MakeError` uses vsnprintf
// only — no allocation, no exceptions — so it is safe to call from the
// /EHs-c- exception-free perimeter and from any thread.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string_view>

namespace pyxis {

// §20 catalogue. Reviewers append values; never renumber, never
// reorder existing values — `ErrorKind` is part of the byte-stable
// public surface.
enum class ErrorKind : uint16_t {
  Ok = 0,

  // Configuration (§26 / §27).
  ConfigMissingField,
  ConfigBadType,
  ConfigBadEnum,
  ConfigOutOfRange,

  // I/O.
  FileNotFound,
  FilePermissionDenied,
  FileCorrupt,

  // USD.
  UsdStageOpenFailed,
  UsdPrimNotFound,
  UsdMaterialUnsupported,

  // Texture.
  TextureDecodeFailed,
  TextureFormatUnsupported,
  TextureBudgetExceeded,

  // Shader.
  ShaderCompileFailed,
  ShaderLinkFailed,
  ShaderEntryPointMissing,

  // Vulkan / NVRHI.
  DeviceLost,
  FeatureMissing,
  OutOfMemoryGpu,
  ValidationError,

  // Acceleration structures (§16).
  BlasBudgetExceeded,
  AccelStructBuildFailed,
  TlasInstanceLimitExceeded,

  // AOV (§18.4 / §19.8).
  AovFormatUnsupported,
  AovUnknownToken,
  AovColorRequired,
  AovTargetMissing,

  // Handles (§18.5 / §19.7).
  InvalidHandle,
  HandleStaleGeneration,

  // Lifecycle / cancellation (§19.2).
  CommitCancelled,

  // Capability gates (§19 / §43).
  PickRequiresAovs,
  GeometryKindUnsupported,

  // I/O quota (§47).
  FileQuotaExceeded,

  // Render graph (§9.2).
  RenderGraphMissingProducer,
  RenderGraphDuplicateImport,
  RenderGraphUnknownRef,

  // Generic.
  InvalidArgument,
  InvalidState,
  NotImplemented,
};

// ABI-safe inline-owning string. CAPACITY is sized to fit the longest
// realistic diagnostic with a SdfPath embedded (Moana SdfPaths can
// reach ~150 chars); 240 leaves room for a verb + path + parameters
// without truncation in the common case.
struct ErrorMessage {
  static constexpr std::size_t CAPACITY = 240;

  std::array<char, CAPACITY> data{};
  uint16_t size = 0;  // not including null terminator

  [[nodiscard]] std::string_view View() const noexcept { return {data.data(), size}; }
};

struct Error {
  ErrorKind kind = ErrorKind::Ok;
  ErrorMessage message;  // human-readable, sdfPath included where relevant
  ErrorMessage source;   // "file:line" via PYXIS_ERROR(...)
};

// Aliased on `std::expected` so callers can swap their own
// implementation later if needed; matches §18.3 spec.
template <class T>
using Expected = std::expected<T, Error>;

namespace detail {

// MakeError is the implementation behind PYXIS_ERROR. printf-style
// formatting via vsnprintf — std::format would pull `<format>` which
// can throw on bad runtime values, breaking the /EHs-c- perimeter
// (§30.1). Truncates with a trailing "..." if the formatted message
// exceeds ErrorMessage::CAPACITY.
[[nodiscard]] PYXIS_RENDERER_API Error MakeError(ErrorKind kind, const char* file, int line,
                                                 const char* fmt, ...) noexcept;

}  // namespace detail

}  // namespace pyxis

// Canonical Error constructor. Fills `kind`, `message` (printf-style),
// and `source` ("file:line") with one call.
//
//   return PYXIS_ERROR(pyxis::ErrorKind::FileNotFound,
//                      "no such file: %.*s",
//                      static_cast<int>(path.size()), path.data());
//
// Returns `pyxis::Error` by value; wrap in `std::unexpected{...}` at
// the call site if returning into an `Expected<T>`.
#define PYXIS_ERROR(kind, ...) ::pyxis::detail::MakeError((kind), __FILE__, __LINE__, __VA_ARGS__)

// Propagate an `Expected<T>` failure upward unchanged.
//
//   PYXIS_TRY(scene.CommitResources(commandList));   // Expected<void>
//
// Discards the success value — useful when you don't need it. To keep
// the value, write the explicit pattern instead, since C++23 has no
// portable way to inline-yield-or-return from a macro (statement-
// expressions are a GCC extension that /permissive- rejects):
//
//   auto result = scene.CreateMesh(desc);
//   if (!result) return std::unexpected{ std::move(result).error() };
//   MeshHandle handle = *result;
#define PYXIS_TRY(expr)                                       \
  do                                                          \
  {                                                           \
    auto _pyxisTry = (expr);                                  \
    if (!_pyxisTry)                                           \
    {                                                         \
      return ::std::unexpected{std::move(_pyxisTry).error()}; \
    }                                                         \
  } while (false)
