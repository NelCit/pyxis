// Pyxis USD ingest — UsdSkel CPU skinning implementation.

#include "SkelSkinning.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdSkel/binding.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/skinningQuery.h>

namespace pyxis::usd_ingest {

SkelSkinnedPointsByPath ComputeSkelSkinnedPoints(
    const pxr::UsdStageRefPtr& stage,
    pxr::UsdTimeCode timeCode) noexcept
{
  SkelSkinnedPointsByPath out;
  if (!stage)
    return out;

  auto& log = Logging::Get();
  std::size_t skinningTargets = 0;

  for (const pxr::UsdPrim& prim : stage->Traverse())
  {
    if (!prim.IsA<pxr::UsdSkelRoot>())
      continue;
    const pxr::UsdSkelRoot root(prim);
    if (!root)
      continue;

    const pxr::UsdSkelCache cache;
    if (!cache.Populate(root, pxr::UsdTraverseInstanceProxies()))
    {
      log.Warn(log::APP,
          "SkelSkinning: UsdSkelCache::Populate failed for "
              + prim.GetPath().GetString() + " — skipping skeleton.");
      continue;
    }

    std::vector<pxr::UsdSkelBinding> bindings;
    if (!cache.ComputeSkelBindings(root, &bindings,
                                   pxr::UsdTraverseInstanceProxies()))
    {
      log.Warn(log::APP,
          "SkelSkinning: ComputeSkelBindings failed for "
              + prim.GetPath().GetString() + " — skipping.");
      continue;
    }

    for (const pxr::UsdSkelBinding& binding : bindings)
    {
      const pxr::UsdSkelSkeletonQuery skelQuery =
          cache.GetSkelQuery(binding.GetSkeleton());
      if (!skelQuery)
        continue;

      // Skinning joint transforms in skeleton space at the requested
      // time-code. These are the per-joint matrices to apply to the
      // skeleton-space mesh points.
      pxr::VtArray<pxr::GfMatrix4d> skinningXforms;
      if (!skelQuery.ComputeSkinningTransforms(&skinningXforms, timeCode))
      {
        log.Warn(log::APP,
            "SkelSkinning: ComputeSkinningTransforms failed for "
                + binding.GetSkeleton().GetPath().GetString());
        continue;
      }

      for (const pxr::UsdSkelSkinningQuery& skinQuery
           : binding.GetSkinningTargets())
      {
        if (!skinQuery)
          continue;
        const pxr::UsdPrim skinnedPrim = skinQuery.GetPrim();
        if (!skinnedPrim)
          continue;

        // Read the rest-pose points off the boundable prim. Skinning
        // happens in skeleton space; the binding transform on the
        // SkinningQuery handles mesh-local → skeleton-space.
        pxr::VtArray<pxr::GfVec3f> points;
        if (const pxr::UsdAttribute pointsAttr =
                skinnedPrim.GetAttribute(pxr::TfToken("points"));
            !pointsAttr || !pointsAttr.Get(&points, timeCode) || points.empty())
        {
          continue;
        }

        if (!skinQuery.ComputeSkinnedPoints(skinningXforms, &points, timeCode))
        {
          log.Warn(log::APP,
              "SkelSkinning: ComputeSkinnedPoints failed for "
                  + skinnedPrim.GetPath().GetString());
          continue;
        }
        out.emplace(skinnedPrim.GetPath().GetString(), std::move(points));
        ++skinningTargets;
      }
    }
  }

  if (skinningTargets > 0)
  {
    log.Info(log::APP,
        "SkelSkinning: " + std::to_string(skinningTargets)
            + " mesh(es) skinned at time-code "
            + std::to_string(timeCode.GetValue()) + " (V2.A.4).");
  }
  return out;
}

}  // namespace pyxis::usd_ingest
