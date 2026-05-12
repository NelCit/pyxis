// Pyxis USD ingest — UsdSkel CPU skinning. V2.A.4.
//
// At ingest time we walk every UsdSkelRoot, populate a UsdSkelCache,
// and pre-compute a skinned-point map keyed by mesh SdfPath. The
// per-mesh prep pass then consults the map and, when the mesh is a
// skinning target, swaps the cage `points` for the skinned ones
// before continuing through the existing PreparedMesh path. The
// closesthit shader is unchanged — skinned meshes look like regular
// rigid meshes from the GPU's perspective.
//
// Time-code: the skinning is computed at the resolved `frameNumber`
// the operator passed to StageWalker (V2.A.13). Per-frame motion is
// driven by re-running ingest at a different time-code; the M30
// auto-pilot push has no per-frame mutation queue yet.

#pragma once

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

#include <string>
#include <unordered_map>

namespace pyxis::usd_ingest {

// Map of skinned-mesh SdfPath → deformed point set at the requested
// time-code. Empty when the stage authors no UsdSkelRoot.
using SkelSkinnedPointsByPath =
    std::unordered_map<std::string, pxr::VtArray<pxr::GfVec3f>>;

// Walk every UsdSkelRoot under `stage`, populate a UsdSkelCache, and
// emit deformed points for each discovered skinning target at the
// given time-code. Failures (no skeleton, query invalid, etc.) are
// logged + skipped — the entry simply doesn't appear in the map and
// the caller renders the mesh at its rest pose.
[[nodiscard]] SkelSkinnedPointsByPath ComputeSkelSkinnedPoints(
    const pxr::UsdStageRefPtr& stage,
    pxr::UsdTimeCode timeCode) noexcept;

}  // namespace pyxis::usd_ingest
