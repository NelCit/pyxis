// Pyxis USD ingest — UsdRenderSettings / UsdRenderProduct / UsdRenderVar
// pre-scan. Loaded BEFORE device init so the app can apply width /
// height / output-path overlays before allocating backbuffer textures.

#include "Pyxis/UsdIngest/RenderSettingsAuthored.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <pxr/base/gf/vec2i.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/var.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace pyxis::usd_ingest {

namespace {

// Truncating copy: write at most `dstSize - 1` chars + null. Mirrors
// the NamedCameraView::name pattern; consumers see a clean
// null-terminated buffer regardless of source length.
void CopyTruncated(char* dst, std::size_t dstSize, const std::string& src) noexcept
{
  if (dstSize == 0)
    return;
  const std::size_t copyLen = std::min(src.size(), dstSize - 1);
  std::memcpy(dst, src.data(), copyLen);
  dst[copyLen] = '\0';
}

}  // namespace

RenderSettingsAuthored PrescanRenderSettings(std::string_view usdPath,
                                              std::string_view productNameOverride) noexcept
{
  RenderSettingsAuthored out;
  if (usdPath.empty())
    return out;

  auto& log = Logging::Get();
  const std::string pathString{usdPath};

  // Open the stage. USD's layer cache makes repeated opens of the
  // same path cheap (the layer stack is shared across UsdStage
  // instances) so the pre-scan + the full ingest don't pay for
  // disk I/O twice.
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(pathString);
  if (!stage)
    return out;

  // Walk every prim. UsdRenderSettings / Product / Var are typed
  // schemas (well-known prim types); scan all prims and bucket by
  // typed accessor. Sorted by SdfPath so multi-product fixtures
  // pick the same "first" deterministically across re-runs.
  std::vector<pxr::UsdPrim> renderSettingsPrims;
  std::vector<pxr::UsdPrim> renderProductPrims;
  for (const pxr::UsdPrim& prim : stage->Traverse())
  {
    if (prim.IsA<pxr::UsdRenderSettings>())
      renderSettingsPrims.push_back(prim);
    else if (prim.IsA<pxr::UsdRenderProduct>())
      renderProductPrims.push_back(prim);
    else if (prim.IsA<pxr::UsdRenderVar>())
      ++out.renderVarCount;
  }
  out.renderProductCount = static_cast<uint32_t>(renderProductPrims.size());
  out.hasRenderSettings = !renderSettingsPrims.empty();

  std::stable_sort(renderSettingsPrims.begin(), renderSettingsPrims.end(),
                   [](const pxr::UsdPrim& lhs, const pxr::UsdPrim& rhs) {
                     return lhs.GetPath() < rhs.GetPath();
                   });
  std::stable_sort(renderProductPrims.begin(), renderProductPrims.end(),
                   [](const pxr::UsdPrim& lhs, const pxr::UsdPrim& rhs) {
                     return lhs.GetPath() < rhs.GetPath();
                   });

  // Pull settings from the first authored UsdRenderSettings (multi-
  // RenderSettings scenes are spec-legal but unusual; matching Storm
  // / Karma's "first by SdfPath" rule keeps cross-renderer fixtures
  // stable).
  if (!renderSettingsPrims.empty())
  {
    const pxr::UsdRenderSettings settings(renderSettingsPrims.front());
    if (settings)
    {
      pxr::GfVec2i resolution{0, 0};
      if (settings.GetResolutionAttr()
          && settings.GetResolutionAttr().Get(&resolution)
          && resolution[0] > 0 && resolution[1] > 0)
      {
        out.resolutionWidth  = static_cast<uint32_t>(resolution[0]);
        out.resolutionHeight = static_cast<uint32_t>(resolution[1]);
      }
      // Camera rel — UsdRenderSettings doesn't always author this
      // (it's optional + can come from the RenderProduct instead);
      // capture if present.
      if (const pxr::UsdRelationship cameraRel = settings.GetCameraRel())
      {
        pxr::SdfPathVector targets;
        cameraRel.GetTargets(&targets);
        if (!targets.empty())
          CopyTruncated(out.cameraSdfPath, sizeof(out.cameraSdfPath),
                        targets[0].GetString());
      }
    }
  }

  // Pick the active RenderProduct. Default: first by SdfPath. With
  // `productNameOverride`, prefer the product whose path's leaf name
  // matches (case-sensitive) so an operator authoring
  // `/Render/Products/{Beauty,DenoiseInput}` can pick via CLI.
  pxr::UsdPrim activeProductPrim;
  if (!productNameOverride.empty())
  {
    const std::string overrideStr{productNameOverride};
    for (const pxr::UsdPrim& prim : renderProductPrims)
    {
      if (prim.GetName().GetString() == overrideStr)
      {
        activeProductPrim = prim;
        break;
      }
    }
    if (!activeProductPrim && !renderProductPrims.empty())
    {
      log.Warn(log::APP,
               "PrescanRenderSettings: --render-product '" + overrideStr
                   + "' did not match any authored UsdRenderProduct on the "
                     "stage; falling back to first-by-SdfPath.");
    }
  }
  if (!activeProductPrim && !renderProductPrims.empty())
    activeProductPrim = renderProductPrims.front();

  if (activeProductPrim)
  {
    CopyTruncated(out.activeProductPath, sizeof(out.activeProductPath),
                  activeProductPrim.GetPath().GetString());
    const pxr::UsdRenderProduct product(activeProductPrim);
    if (product)
    {
      // Product name = the output file path / token. Authored as a
      // TfToken in newer USD; convert to string.
      if (const pxr::UsdAttribute productNameAttr = product.GetProductNameAttr())
      {
        pxr::TfToken productName;
        if (productNameAttr.Get(&productName) && !productName.IsEmpty())
        {
          CopyTruncated(out.outputFile, sizeof(out.outputFile),
                        productName.GetString());
        }
      }
      // RenderProduct camera overrides Settings camera if authored.
      if (const pxr::UsdRelationship cameraRel = product.GetCameraRel())
      {
        pxr::SdfPathVector targets;
        cameraRel.GetTargets(&targets);
        if (!targets.empty())
          CopyTruncated(out.cameraSdfPath, sizeof(out.cameraSdfPath),
                        targets[0].GetString());
      }
    }
  }

  // Diagnostic log — single line per pre-scan run so the operator
  // sees authoring round-tripped without grepping for individual
  // attr reads.
  if (out.hasRenderSettings || out.renderProductCount > 0)
  {
    log.Info(log::APP,
             "PrescanRenderSettings: " + pathString + " — "
                 + std::to_string(renderSettingsPrims.size())
                 + " UsdRenderSettings, "
                 + std::to_string(out.renderProductCount) + " UsdRenderProduct, "
                 + std::to_string(out.renderVarCount) + " UsdRenderVar; "
                 + "resolution=("
                 + std::to_string(out.resolutionWidth) + "x"
                 + std::to_string(out.resolutionHeight) + ") "
                 + "camera='" + std::string{out.cameraSdfPath} + "' "
                 + "output='" + std::string{out.outputFile} + "' "
                 + "(V2.A.27).");
  }
  return out;
}

}  // namespace pyxis::usd_ingest
