// RFC 0004 Stage 3 (C4-full) — headless HdEngine smoke for the Pyxis delegate.
//
// Drives a USD stage through HdPyxisOmniRenderDelegate exactly as a Kit viewport
// would (UsdImagingStageSceneIndex -> HdRenderIndex -> HdEngine), and verifies
// the FULL chain end-to-end on real hardware WITHOUT Kit:
//   HdEngine -> HdPyxisOmniMesh::Sync -> GpuScene::CreateMesh/AppendInstance
//            -> render pass -> PyxisEngine::RenderFrame -> CommitResources (TLAS)
// Success = the synced mesh shows up in the committed scene (instanceCount >= 1)
// and a frame reads back. Exit 0 = pass, non-zero = fail.

#include "HdPyxisOmniRenderDelegate.h"
#include "PyxisEngine.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Minimal task: Sync registers the render pass's collection (so SyncAll syncs
// the rprims), Execute drives the pass (-> PyxisEngine::RenderFrame).
class RenderTask final : public HdTask {
 public:
  explicit RenderTask(HdRenderPassSharedPtr pass)
      : HdTask(SdfPath::EmptyPath()), _pass(std::move(pass)) {}
  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* bits) override {
    _pass->Sync();
    *bits = HdChangeTracker::Clean;
  }
  void Prepare(HdTaskContext*, HdRenderIndex*) override {}
  void Execute(HdTaskContext*) override {
    _pass->Execute(std::make_shared<HdRenderPassState>(), GetRenderTags());
  }
  const TfTokenVector& GetRenderTags() const override {
    static const TfTokenVector tags = {HdRenderTagTokens->geometry};
    return tags;
  }

 private:
  HdRenderPassSharedPtr _pass;
};

}  // namespace

int main() {
  // 1. A USD stage with one triangle mesh.
  UsdStageRefPtr stage = UsdStage::CreateInMemory();
  UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/tri"));
  VtVec3fArray points = {GfVec3f(0, 0, 2), GfVec3f(1, 0, 2), GfVec3f(0, 1, 2)};
  mesh.CreatePointsAttr().Set(points);
  mesh.CreateFaceVertexCountsAttr().Set(VtIntArray{3});
  mesh.CreateFaceVertexIndicesAttr().Set(VtIntArray{0, 1, 2});

  // 2. Render index + the Pyxis delegate.
  auto* delegate = new HdPyxisOmniRenderDelegate();
  if (delegate->Engine() == nullptr) {
    std::fprintf(stderr, "FAIL: PyxisEngine did not initialise (no GPU / interop?).\n");
    return 2;
  }
  HdRenderIndex* index = HdRenderIndex::New(delegate, HdDriverVector{});
  if (index == nullptr) {
    std::fprintf(stderr, "FAIL: HdRenderIndex::New returned null.\n");
    return 3;
  }

  // 3. Feed the stage in via the Hydra-2 scene index.
  UsdImagingStageSceneIndexRefPtr sceneIndex = UsdImagingStageSceneIndex::New();
  sceneIndex->SetStage(stage);
  sceneIndex->SetTime(UsdTimeCode::Default());
  index->InsertSceneIndex(sceneIndex, SdfPath::AbsoluteRootPath());
  sceneIndex->ApplyPendingUpdates();

  // 4. Drive a frame.
  HdRprimCollection collection(HdTokens->geometry, HdReprSelector(HdReprTokens->hull));
  HdRenderPassSharedPtr pass = delegate->CreateRenderPass(index, collection);
  HdEngine engine;
  HdTaskSharedPtrVector tasks = {std::make_shared<RenderTask>(pass)};
  engine.Execute(index, &tasks);

  // 5. Verify the FSD mesh adapter fed geometry through to Pyxis + it rendered.
  const uint64_t instanceCount = delegate->Engine()->LastInstanceCount();
  const uint64_t meshCount = delegate->Engine()->LastMeshCount();
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  const bool readback = delegate->Engine()->ReadbackColorHdr(pixels, width, height);

  std::printf("HdEngineSmoke: meshCount=%llu instanceCount=%llu readback=%d (%ux%u)\n",
              static_cast<unsigned long long>(meshCount),
              static_cast<unsigned long long>(instanceCount), readback ? 1 : 0, width, height);

  delete index;
  delete delegate;

  if (meshCount < 1 || instanceCount < 1) {
    std::fprintf(stderr, "FAIL: stage geometry did not reach GpuScene via the delegate.\n");
    return 4;
  }
  if (!readback) {
    std::fprintf(stderr, "FAIL: no frame read back from the engine.\n");
    return 5;
  }
  std::printf("PASS: HdEngine drove the Pyxis delegate end-to-end.\n");
  return 0;
}
