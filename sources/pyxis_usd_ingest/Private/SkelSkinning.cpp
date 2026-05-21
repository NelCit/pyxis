// Pyxis USD ingest — UsdSkel CPU skinning implementation.

#include "SkelSkinning.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdSkel/animMapper.h>
#include <pxr/usd/usdSkel/animQuery.h>
#include <pxr/usd/usdSkel/binding.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/blendShapeQuery.h>
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
  // V2.A.4 / PR5 — blend-shape coverage. Track how many skinning
  // targets carried `skel:blendShapeTargets` rels and how many of
  // those we actually deformed (skipped if the BlendShapeQuery
  // couldn't be constructed or sub-shape weight remapping failed).
  std::size_t blendShapeTargets = 0;
  std::size_t blendShapesApplied = 0;

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

      // V2.A.4 / PR5 — anim-level blend-shape weights at the requested
      // time. May be empty (no blend-shape track authored on the
      // animation, or the skeleton has no SkelAnimation source). The
      // per-skinning-target step below remaps these into the target's
      // own blend-shape order before applying.
      const pxr::UsdSkelAnimQuery& animQuery = skelQuery.GetAnimQuery();
      pxr::VtFloatArray animBlendShapeWeights;
      if (animQuery)
        animQuery.ComputeBlendShapeWeights(&animBlendShapeWeights, timeCode);

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

        // V2.A.4 / PR5 — apply blend-shape deformation BEFORE skinning.
        // The proper UsdSkel pipeline is "rest points → blend-shape
        // deltas → joint skinning"; running skinning first would
        // displace blend-shape offsets along the rest pose's joint
        // bind transforms instead of the deformed-mesh joint
        // transforms (subtle but visibly wrong for facial
        // animation rigs which animate eyelid / mouth pose via
        // blend shapes only). See UsdSkelBakeSkinning for the
        // reference pipeline this mirrors.
        if (skinQuery.HasBlendShapes() && !animBlendShapeWeights.empty())
        {
          ++blendShapeTargets;
          // Remap anim-order weights to this skinning target's
          // blend-shape order via the AnimMapper the SkinningQuery
          // pre-built at cache-population time. When no mapper is
          // bound, the anim weights already match the target order.
          pxr::VtFloatArray remappedWeights;
          if (const pxr::UsdSkelAnimMapperRefPtr& mapper =
                  skinQuery.GetBlendShapeMapper())
          {
            if (!mapper->Remap(animBlendShapeWeights, &remappedWeights))
              remappedWeights = animBlendShapeWeights;
          }
          else
          {
            remappedWeights = animBlendShapeWeights;
          }

          // Construct the BlendShapeQuery from the prim's BindingAPI.
          // Provides ComputeSubShapeWeights (handles inbetween-shape
          // resolution) + ComputeDeformedPoints (per-point delta
          // application). Failures fall through to non-blend-shape
          // skinning — better to skin the rest pose than to no-op
          // the whole skinning step on a soft USD error.
          const pxr::UsdSkelBindingAPI bindingApi(skinnedPrim);
          const pxr::UsdSkelBlendShapeQuery bsQuery(bindingApi);
          if (bsQuery.IsValid())
          {
            pxr::VtFloatArray subShapeWeights;
            pxr::VtUIntArray  blendShapeIndices;
            pxr::VtUIntArray  subShapeIndices;
            if (bsQuery.ComputeSubShapeWeights(remappedWeights,
                                                &subShapeWeights,
                                                &blendShapeIndices,
                                                &subShapeIndices))
            {
              const std::vector<pxr::VtIntArray> blendShapePointIndices =
                  bsQuery.ComputeBlendShapePointIndices();
              const std::vector<pxr::VtVec3fArray> subShapePointOffsets =
                  bsQuery.ComputeSubShapePointOffsets();
              if (bsQuery.ComputeDeformedPoints(subShapeWeights,
                                                 blendShapeIndices,
                                                 subShapeIndices,
                                                 blendShapePointIndices,
                                                 subShapePointOffsets,
                                                 points))
              {
                ++blendShapesApplied;
              }
            }
          }
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
    if (blendShapeTargets > 0)
    {
      log.Info(log::APP,
          "SkelSkinning: " + std::to_string(blendShapesApplied)
              + " of " + std::to_string(blendShapeTargets)
              + " blend-shape-bearing skinning target(s) deformed (PR5).");
    }
  }
  return out;
}

}  // namespace pyxis::usd_ingest
