// Pyxis V2.A.18 — `material:binding:full` purpose + collection-based
// bindings via `material:binding:collection:<name>`. Mirrors the
// `ResolveBoundMaterial` logic in StageWalker.cpp (which calls
// `UsdShadeMaterialBindingAPI::ComputeBoundMaterial(full)`) so a
// future refactor that changes the call site has to break this test
// before merging.
//
// The unit test reimplements the resolver call directly (USD's API is
// the source of truth) — pinning the BEHAVIOUR the StageWalker relies
// on, not the StageWalker code path itself. The integration test
// `Golden.material_binding_collection` covers the end-to-end render
// path.

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/scope.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/tokens.h>

#include <gtest/gtest.h>

namespace {

// Resolve a prim's bound material the same way StageWalker.cpp does:
// try `full` first (final-quality preference); fall back to all-
// purpose. Returns the bound material's SdfPath string (empty if no
// binding resolves).
std::string ResolveBoundMaterialPath(const pxr::UsdPrim& prim)
{
  const pxr::UsdShadeMaterialBindingAPI bindingApi(prim);
  pxr::UsdShadeMaterial bound =
      bindingApi.ComputeBoundMaterial(pxr::UsdShadeTokens->full);
  if (!bound.GetPrim().IsValid())
    bound = bindingApi.ComputeBoundMaterial();
  if (!bound.GetPrim().IsValid())
    return {};
  return bound.GetPrim().GetPath().GetString();
}

}  // namespace

// V2.A.18 — `material:binding:full` overrides `material:binding`.
// Production pipelines author the universal binding as the
// `preview`-quality fallback and the `:full` purpose as the
// final-quality target — Pyxis is a final-quality renderer, so the
// `:full` purpose must win.
TEST(MaterialBindingResolutionV2A18, FullPurposeWinsOverAllPurpose)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("full_purpose.usda");

  // Two materials: AllPurpose (binding default) + Full (binding:full).
  const pxr::UsdShadeMaterial allPurpose =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Looks/AllPurpose"));
  const pxr::UsdShadeMaterial fullPurpose =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Looks/Full"));

  // Subject prim with both bindings authored.
  pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/World"));
  pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/World/Cube"));
  const pxr::UsdPrim cube = stage->GetPrimAtPath(pxr::SdfPath("/World/Cube"));
  const pxr::UsdShadeMaterialBindingAPI bindingApi =
      pxr::UsdShadeMaterialBindingAPI::Apply(cube);
  // All-purpose binding (would be the preview fallback in production).
  ASSERT_TRUE(bindingApi.Bind(allPurpose));
  // Full-purpose binding (the final-quality target).
  ASSERT_TRUE(bindingApi.Bind(fullPurpose, pxr::UsdShadeTokens->fallbackStrength,
                              pxr::UsdShadeTokens->full));

  EXPECT_EQ(ResolveBoundMaterialPath(cube), "/Looks/Full");
}

// V2.A.18 — when only the all-purpose binding is authored, the
// `:full` query falls back to all-purpose (verified inside USD's
// resolver). Pyxis's StageWalker chains a second `ComputeBoundMaterial()`
// call as a belt-and-braces fallback; this test pins that the
// combined logic resolves correctly on common-case scenes.
TEST(MaterialBindingResolutionV2A18, AllPurposeOnlyStillResolves)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("all_only.usda");

  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Looks/Solo"));
  pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/World"));
  pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/World/Cube"));
  const pxr::UsdPrim cube = stage->GetPrimAtPath(pxr::SdfPath("/World/Cube"));
  const pxr::UsdShadeMaterialBindingAPI bindingApi =
      pxr::UsdShadeMaterialBindingAPI::Apply(cube);
  ASSERT_TRUE(bindingApi.Bind(material));

  EXPECT_EQ(ResolveBoundMaterialPath(cube), "/Looks/Solo");
}

// V2.A.18 — collection-based bindings. A material can bind to a
// COLLECTION of prims (`material:binding:collection:<name>` on an
// ancestor) instead of authoring per-prim direct relationships.
// Production pipelines use this to bind a hero asset's "metal" or
// "glass" subset in one place.
//
// USD's `ComputeBoundMaterial` walks ancestors looking for collection
// bindings whose `includes/excludes` resolve to `prim`. The resolver
// then picks the strongest binding (collection vs direct).
TEST(MaterialBindingResolutionV2A18, CollectionBindingAppliesToIncludedPrims)
{
  const pxr::UsdStageRefPtr stage =
      pxr::UsdStage::CreateInMemory("collection_binding.usda");
  pxr::UsdGeomScope::Define(stage, pxr::SdfPath("/World"));
  pxr::UsdGeomScope::Define(stage, pxr::SdfPath("/World/Looks"));

  // Material that the collection will bind to.
  const pxr::UsdShadeMaterial metalMat =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/World/Looks/Metal"));

  // Three subject cubes; only A and C are in the "metal" collection.
  pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/World/CubeA"));
  pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/World/CubeB"));
  pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/World/CubeC"));
  const pxr::UsdPrim cubeA = stage->GetPrimAtPath(pxr::SdfPath("/World/CubeA"));
  const pxr::UsdPrim cubeB = stage->GetPrimAtPath(pxr::SdfPath("/World/CubeB"));
  const pxr::UsdPrim cubeC = stage->GetPrimAtPath(pxr::SdfPath("/World/CubeC"));

  // Author the collection on /World with explicit includes.
  const pxr::UsdPrim world = stage->GetPrimAtPath(pxr::SdfPath("/World"));
  const pxr::UsdCollectionAPI metalCollection =
      pxr::UsdCollectionAPI::Apply(world, pxr::TfToken("metal"));
  metalCollection.CreateIncludesRel().AddTarget(cubeA.GetPath());
  metalCollection.CreateIncludesRel().AddTarget(cubeC.GetPath());

  // Bind the collection to the material.
  const pxr::UsdShadeMaterialBindingAPI bindingApi =
      pxr::UsdShadeMaterialBindingAPI::Apply(world);
  ASSERT_TRUE(bindingApi.Bind(metalCollection, metalMat,
                              pxr::TfToken("metal"),
                              pxr::UsdShadeTokens->fallbackStrength));

  EXPECT_EQ(ResolveBoundMaterialPath(cubeA), "/World/Looks/Metal");
  EXPECT_EQ(ResolveBoundMaterialPath(cubeB), "");  // not in collection
  EXPECT_EQ(ResolveBoundMaterialPath(cubeC), "/World/Looks/Metal");
}

// V2.A.18 — `bindingStrength = strongerThanDescendants` makes the
// ancestor's binding win against a descendant's direct binding. The
// opposite (`weakerThanDescendants`) lets the descendant override.
// Pyxis honours USD's strength rules transitively via the
// resolver — this test pins that we don't accidentally fall through
// to a weaker binding.
TEST(MaterialBindingResolutionV2A18, StrongerThanDescendantsOverridesChild)
{
  const pxr::UsdStageRefPtr stage =
      pxr::UsdStage::CreateInMemory("strength.usda");
  pxr::UsdGeomScope::Define(stage, pxr::SdfPath("/World"));
  pxr::UsdGeomScope::Define(stage, pxr::SdfPath("/World/Looks"));
  const pxr::UsdShadeMaterial ancestorMat =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/World/Looks/Ancestor"));
  const pxr::UsdShadeMaterial childMat =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/World/Looks/Child"));

  pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/World/Group/Cube"));
  const pxr::UsdPrim cube = stage->GetPrimAtPath(pxr::SdfPath("/World/Group/Cube"));
  const pxr::UsdPrim group = stage->GetPrimAtPath(pxr::SdfPath("/World/Group"));

  // Group binds the ancestor material with strongerThanDescendants.
  const pxr::UsdShadeMaterialBindingAPI groupBinding =
      pxr::UsdShadeMaterialBindingAPI::Apply(group);
  ASSERT_TRUE(groupBinding.Bind(ancestorMat,
                                pxr::UsdShadeTokens->strongerThanDescendants));

  // Cube tries to bind the child material directly.
  const pxr::UsdShadeMaterialBindingAPI cubeBinding =
      pxr::UsdShadeMaterialBindingAPI::Apply(cube);
  ASSERT_TRUE(cubeBinding.Bind(childMat));

  // The ancestor wins because of strongerThanDescendants.
  EXPECT_EQ(ResolveBoundMaterialPath(cube), "/World/Looks/Ancestor");
}
