// Pyxis app — `--variant` CLI parser.
//
// Plan §V2.A.2 (M12). The renderer's day-to-day default is "stage's
// authored variantSelection wins" — UsdStage::Open honours that
// transitively. `--variant` is the operator-side override that lets a
// CI run or a Pillar A regression fixture pick a *different* variant
// without re-authoring the .usd. The parser is pure-stdlib so the unit
// test can exercise it without dragging USD's headers into the test TU.
//
// Syntax (CLI grammar):
//   --variant <primPath>:<setName>=<value>[,<primPath>:<setName>=<value>...]
//
// Example:
//   --variant /World/Hero:shadingComplexity=high,/World/Cup:season=winter
//
// Whitespace around each comma-separated triple is tolerated. Entries
// that don't match `^/...:set=value$` are dropped (a log line surfaces
// at apply time inside IngestUsd, so a typo doesn't silently take the
// renderer to a different variant than the operator intended).

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pyxis::app {

// One parsed entry. Strings own their data so callers can hand the
// vector around without lifetime-pinning the original argv buffer.
struct VariantSelection {
  std::string primPath;  // Absolute SdfPath, e.g. "/World/Hero"
  std::string setName;   // VariantSet name, e.g. "shadingComplexity"
  std::string value;     // Selected variant within the set, e.g. "high"
};

// Tokenise the comma-separated --variant spec into structured records.
// Malformed entries (missing ':' or '=', empty path/set/value, non-
// absolute path) are dropped silently here; IngestUsd re-validates
// before authoring + logs per dropped entry.
[[nodiscard]] std::vector<VariantSelection> ParseVariantSelections(
    std::string_view spec) noexcept;

}  // namespace pyxis::app
