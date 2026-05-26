// RFC 0004 Option A — see PyxisHydraHost.h.

#include "PyxisHydraHost.h"

// RFC 0006: the single Pyxis Hydra delegate + engine now live in pyxis_hydra
// (the fork collapsed onto them once both shared Kit's nv-usd). The Kit CMake
// adds sources/pyxis_hydra/Private to the include path + compiles those .cpp.
#include "HdPyxisRenderDelegate.h"
#include "PyxisEngine.h"

#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/driver.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <memory>
#include <utility>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Minimal task: Sync registers the render pass's collection (so SyncAll syncs
// the rprims), Execute drives the pass (-> PyxisEngine::RenderFrame). Mirrors
// the task HdEngineSmoke used; lives here now that the host owns the engine loop.
class PyxisRenderTask final : public HdTask
{
public:
    PyxisRenderTask(HdRenderPassSharedPtr pass,
                    HdRenderPassAovBindingVector bindings,
                    HdCamera* camera,
                    uint32_t width,
                    uint32_t height)
        : HdTask(SdfPath::EmptyPath()),
          _pass(std::move(pass)),
          _bindings(std::move(bindings)),
          _camera(camera),
          _width(width),
          _height(height)
    {
    }

    void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* bits) override
    {
        _pass->Sync();
        *bits = HdChangeTracker::Clean;
    }

    void Prepare(HdTaskContext*, HdRenderIndex*) override {}

    void Execute(HdTaskContext*) override
    {
        auto state = std::make_shared<HdRenderPassState>();
        state->SetAovBindings(_bindings);  // host binds the color render buffer.
        if (_camera)
        {
            state->SetCamera(_camera);  // -> GpuScene::SetCamera on sync.
            state->SetViewport(GfVec4d(0, 0, _width, _height));
        }
        _pass->Execute(state, GetRenderTags());
    }

    const TfTokenVector& GetRenderTags() const override
    {
        static const TfTokenVector TAGS = { HdRenderTagTokens->geometry };
        return TAGS;
    }

private:
    HdRenderPassSharedPtr _pass;
    HdRenderPassAovBindingVector _bindings;
    HdCamera* _camera;
    uint32_t _width;
    uint32_t _height;
};

} // namespace

PyxisHydraHost::PyxisHydraHost(uint32_t width, uint32_t height)
    : _width(width), _height(height), _colorBufferPath("/pyxisHostColor")
{
    _delegate = new HdPyxisRenderDelegate();
    if (_delegate->Engine() == nullptr)
    {
        return;  // GPU / interop unavailable; IsValid() stays false.
    }

    _index = HdRenderIndex::New(_delegate, HdDriverVector{});
    if (_index == nullptr)
    {
        return;
    }

    const HdRprimCollection collection(HdTokens->geometry, HdReprSelector(HdReprTokens->hull));
    _pass = _delegate->CreateRenderPass(_index, collection);

    // Host-owned color AOV (RGBA16F, matches PyxisEngine's internal target) the
    // delegate composites into — the surface a viewport / usdview binds.
    _colorBuffer = static_cast<HdRenderBuffer*>(
        _delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, _colorBufferPath));
    if (_colorBuffer)
    {
        _colorBuffer->Allocate(GfVec3i(int(_width), int(_height), 1), HdFormatFloat16Vec4,
                               /*multiSampled*/ false);
    }
}

PyxisHydraHost::~PyxisHydraHost()
{
    // The render index owns inserted scene indices; the delegate owns the Bprim.
    delete _index;     // also releases the render pass + the color Bprim.
    delete _delegate;  // tears down PyxisEngine.
}

bool PyxisHydraHost::IsValid() const
{
    return _delegate != nullptr && _delegate->Engine() != nullptr && _index != nullptr &&
           _pass && _colorBuffer != nullptr;
}

void PyxisHydraHost::SetStage(UsdStageRefPtr stage)
{
    if (!IsValid())
    {
        return;
    }
    if (_stageInserted && _insertedSceneIndex)
    {
        _index->RemoveSceneIndex(_insertedSceneIndex);
        _insertedSceneIndex = nullptr;
        _sceneIndices = UsdImagingSceneIndices{};
        _stageInserted = false;
    }
    _stage = std::move(stage);
    if (!_stage)
    {
        return;
    }

    // Bake the stage's authored units (metresPerUnit) + up-axis into the
    // delegate so prim/light/camera transforms land in the renderer's metres +
    // Y-up frame — Hydra's GetTransform returns raw stage units (the World
    // Lobby is centimetres, Z-up), and the closesthit's metre-calibrated
    // dome-AO / inverse-square constants only behave correctly in metres.
    // Matches pyxis_usd_ingest's StageWalker (§25.O.3 brightness parity).
    if (_delegate != nullptr)
    {
        _delegate->SetStageToWorld(UsdGeomGetStageMetersPerUnit(_stage),
                                   UsdGeomGetStageUpAxis(_stage) == UsdGeomTokens->z);
        // §25.O.3 — hand the delegate the stage so material Sync resolves bound
        // material prims through the SHARED FromUsdShade (byte-identical MDL /
        // MaterialX / UsdPreviewSurface translation with pyxis_usd_ingest).
        _delegate->SetStage(_stage);
    }

    // Full Hydra-2 scene-index chain (binding resolution + flatten). The bare
    // UsdImagingStageSceneIndex does NOT resolve material bindings.
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = _stage;
    _sceneIndices = UsdImagingCreateSceneIndices(info);

    // Insert the final scene index directly. The implicit-surface tessellation
    // wrapper (HdsiImplicitSurfaceSceneIndex) is gone: it only fed the per-prim
    // Hydra Sync path, which has been removed. Ingest is now StageWalker-only,
    // and StageWalker tessellates analytic prims (sphere/cube/cone/cylinder/
    // capsule/plane) itself, so the wrapper would be redundant work. Tracking the
    // inserted index keeps the RemoveSceneIndex teardown above unchanged.
    _insertedSceneIndex = _sceneIndices.finalSceneIndex;
    _index->InsertSceneIndex(_insertedSceneIndex, SdfPath::AbsoluteRootPath());
    _stageInserted = true;
}

bool PyxisHydraHost::Render(UsdTimeCode time, const SdfPath& cameraPath, uint32_t frames)
{
    if (!IsValid() || !_stageInserted)
    {
        return false;
    }

    // Refresh the stage scene index at the requested time (picks up live edits).
    _sceneIndices.stageSceneIndex->SetTime(time);
    _sceneIndices.stageSceneIndex->ApplyPendingUpdates();

    HdCamera* camera = nullptr;
    if (!cameraPath.IsEmpty())
    {
        camera = static_cast<HdCamera*>(_index->GetSprim(HdPrimTypeTokens->camera, cameraPath));
    }

    HdRenderPassAovBinding colorBinding;
    colorBinding.aovName = HdAovTokens->color;
    colorBinding.renderBuffer = _colorBuffer;

    HdEngine engine;
    HdTaskSharedPtrVector tasks = { std::make_shared<PyxisRenderTask>(
        _pass, HdRenderPassAovBindingVector{ colorBinding }, camera, _width, _height) };
    // Progressive accumulation: the first Execute syncs the prims, every Execute
    // drives PyxisEngine::RenderFrame which accumulates into the same target (the
    // camera is static), so `frames` calls converge the path-traced image exactly
    // as the live viewport does over `frames` displayed frames.
    const uint32_t passes = frames == 0 ? 1 : frames;
    for (uint32_t pass = 0; pass < passes; ++pass)
        engine.Execute(_index, &tasks);
    return true;
}

PXR_NS::HdRenderBuffer* PyxisHydraHost::ColorBuffer() const
{
    return _colorBuffer;
}

pyxis::hydra::PyxisEngine* PyxisHydraHost::Engine() const
{
    return _delegate ? _delegate->Engine() : nullptr;
}
