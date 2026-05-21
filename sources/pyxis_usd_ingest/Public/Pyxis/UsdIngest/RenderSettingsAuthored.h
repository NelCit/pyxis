// Pyxis USD ingest — values parsed from UsdRenderSettings +
// UsdRenderProduct + UsdRenderVar prims on the stage.
//
// V2.A.27 — production scenes (Omniverse, Maya-USD, Houdini-USD)
// increasingly author render configuration in USD itself rather than
// in an out-of-band JSON. UsdRenderSettings carries the global knobs
// (resolution, aspect ratio, active camera); UsdRenderProduct
// declares one output (file path + AOV list + camera override);
// UsdRenderVar declares one AOV by name + format. Pyxis v2 reads the
// settings + the first RenderProduct's name + counts the
// RenderVars; the AOV bindings + multi-product selection extend
// from this POD in future work.
//
// Overlay precedence at the app level: CLI explicit > USD authored >
// JSON > defaults. The pre-scan happens before device init so the
// resolution can shape backbuffer / texture allocations correctly.

#pragma once

#include <Pyxis/UsdIngest/UsdIngestApi.h>

#include <cstdint>
#include <string_view>

namespace pyxis::usd_ingest {

// Inline-string POD for the camera + output-file SdfPath / file path.
// Up to 255 chars each; longer values truncate. Mirrors the
// `NamedCameraView::name` pattern (§18.9 STL-container-free public
// surface). Empty / zero fields mean "not authored — caller keeps
// whatever it already had."
struct PYXIS_USD_INGEST_API RenderSettingsAuthored {
  uint32_t resolutionWidth   = 0;   // 0 = not authored
  uint32_t resolutionHeight  = 0;   // 0 = not authored
  uint32_t renderProductCount = 0;  // total UsdRenderProduct prims found
  uint32_t renderVarCount    = 0;   // total UsdRenderVar prims found
  // True iff the stage authored at least one UsdRenderSettings prim
  // (so the caller can distinguish "USD scene didn't author any
  // RenderSettings" from "authored but at zero resolution").
  bool     hasRenderSettings = false;
  // 3-byte align pad before the next uint32-aligned region. Same
  // `_pad*` underscore-prefix convention as the §22.3 `_reserved`
  // slots on LightDesc / OpenPBRMaterialDesc; clang-tidy's
  // identifier-naming style allows the leading underscore for
  // these slot-purpose fields specifically.
  // NOLINTNEXTLINE(readability-identifier-naming)
  uint8_t  _pad0[3] = {0, 0, 0};
  // Camera SdfPath — UsdRenderSettings.camera rel target, or the
  // first authored UsdRenderProduct.camera if RenderSettings didn't
  // author a camera rel. Null-terminated; empty = not authored.
  char     cameraSdfPath[256] = {0};
  // Output file from the active (first) UsdRenderProduct's
  // `productName` attr. Null-terminated; empty = not authored.
  char     outputFile[256]   = {0};
  // SdfPath of the active (first by SdfPath sort) RenderProduct.
  // Null-terminated; empty = no RenderProduct authored.
  char     activeProductPath[256] = {0};
};

// Open the stage at `usdPath` (lightweight — USD's layer cache makes
// the second open cheap after a normal full ingest hits the same
// path) + scan for UsdRenderSettings / UsdRenderProduct / UsdRenderVar
// prims. Returns the parsed POD. On any failure (file missing,
// invalid layer) returns a default-init struct.
//
// If `productNameOverride` is non-empty, picks the matching
// RenderProduct by SdfPath segment match (e.g. "Beauty" matches
// `/Render/Products/Beauty`); falls back to the first product by
// SdfPath sort if no match.
[[nodiscard]] PYXIS_USD_INGEST_API RenderSettingsAuthored PrescanRenderSettings(
    std::string_view usdPath,
    std::string_view productNameOverride = {}) noexcept;

}  // namespace pyxis::usd_ingest
