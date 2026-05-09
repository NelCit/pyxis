// Pyxis USD-direct ingest — StageWalker implementation.
//
// M4 stub level: opens the stage, counts the prims it would normally
// emit, and returns. Real GpuScene mutation calls (CreateMesh,
// AppendInstance, AddLight, SetCamera) layer in alongside the M4
// engine integration on this milestone branch. The §25.O.3 byte-
// equal regression depends on SdfPath-sorted traversal order; the
// `std::stable_sort` of stage->Traverse() is established here so
// future fills don't have to revisit traversal semantics.

#include "Pyxis/UsdIngest/StageWalker.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <string>
#include <vector>

namespace pyxis::usd_ingest {

IngestStats StageWalker::WalkFile(std::string_view usdPath, GpuScene& scene) {
  auto& log = Logging::Get();
  const std::string pathString{usdPath};
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(pathString);
  if (!stage)
  {
    log.Error(log::APP, std::string{"StageWalker: failed to open "} + pathString);
    return {};
  }
  const IngestStats stats = WalkStage(stage, scene);
  log.Info(log::APP,
           "StageWalker: " + pathString + " walked — "
               + std::to_string(stats.meshesEmitted) + " meshes, "
               + std::to_string(stats.instancesEmitted) + " instances, "
               + std::to_string(stats.lightsEmitted) + " lights, "
               + std::to_string(stats.camerasEmitted) + " cameras, "
               + std::to_string(stats.skipped) + " skipped.");
  return stats;
}

IngestStats StageWalker::WalkStage(const pxr::UsdStageRefPtr& stage,
                                   GpuScene& /*scene*/) {
  IngestStats stats{};
  if (!stage)
    return stats;

  // Collect every prim, sort by SdfPath. §25.O.3: SdfPath-sorted is
  // the P0 byte-equal invariant — both adapters must emit instances
  // in this exact order.
  std::vector<pxr::UsdPrim> prims;
  for (const pxr::UsdPrim& prim : stage->Traverse())
    prims.push_back(prim);
  std::stable_sort(prims.begin(), prims.end(),
                   [](const pxr::UsdPrim& lhs, const pxr::UsdPrim& rhs) {
                     return lhs.GetPath() < rhs.GetPath();
                   });

  for (const pxr::UsdPrim& prim : prims)
  {
    // M4 stub: count the prim. Real impl extracts points / topology
    // / primvars via UsdGeomMesh + UsdGeomPrimvarsAPI, fans ngons to
    // triangles, calls GpuScene::CreateMesh + AppendInstance.
    // UsdGeomSphere is parametric — the renderer side eventually
    // tessellates (or treats as analytic); counted the same as a
    // mesh for stats purposes.
    if (prim.IsA<pxr::UsdGeomMesh>() || prim.IsA<pxr::UsdGeomSphere>())
    {
      ++stats.meshesEmitted;
      ++stats.instancesEmitted;
    }
    else if (prim.IsA<pxr::UsdGeomCamera>())
    {
      ++stats.camerasEmitted;
    }
    else if (prim.IsA<pxr::UsdLuxDistantLight>() || prim.IsA<pxr::UsdLuxDomeLight>()
             || prim.IsA<pxr::UsdLuxRectLight>())
    {
      ++stats.lightsEmitted;
    }
    else if (prim.IsA<pxr::UsdShadeMaterial>())
    {
      ++stats.materialsEmitted;
    }
    else
    {
      // Xform / Scope / unsupported types — counted as skipped only
      // if it's a non-grouping prim (Xforms are skipped silently
      // since they contribute transforms, not emitted prims).
      if (!prim.GetTypeName().IsEmpty() && prim.GetTypeName().GetString() != "Xform"
          && prim.GetTypeName().GetString() != "Scope")
      {
        ++stats.skipped;
      }
    }
  }

  return stats;
}

}  // namespace pyxis::usd_ingest
