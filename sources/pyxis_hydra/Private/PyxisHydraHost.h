// RFC 0004 Option A — reusable host that drives the Pyxis Hydra delegate over a
// USD stage and renders one frame into a host-owned color AOV.
//
// This is the proven HdEngineSmoke driving logic (delegate + HdRenderIndex +
// UsdImagingCreateSceneIndices + render pass + AOV binding + camera +
// HdEngine::Execute) extracted into a class with two clients:
//   * tests/HdEngineSmoke.cpp  — headless verification (authored in-memory stage)
//   * the in-Kit C++ viewport panel — drives the live edited stage each frame
//
// USD-side only (nv-usd 25.11); the renderer stays USD-free behind PyxisEngine.

#pragma once

#include <pxr/pxr.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>

#include <cstdint>

namespace pyxis::hydra {
class PyxisEngine;
}

PXR_NAMESPACE_OPEN_SCOPE
class HdRenderIndex;
class HdPyxisRenderDelegate;
PXR_NAMESPACE_CLOSE_SCOPE

// Owns a Pyxis Hydra render stack (delegate + render index + render pass + a
// host color render buffer) sized once at construction. Feed it a stage with
// SetStage, then call Render per frame.
class PyxisHydraHost
{
public:
    // Allocates the delegate (which stands up PyxisEngine: headless device +
    // GpuScene + PyxisRenderer + exporter), the render index, the render pass,
    // and an RGBA16F color render buffer of the given dimensions. Check
    // IsValid() afterwards — construction does not throw.
    PyxisHydraHost(uint32_t width, uint32_t height);
    ~PyxisHydraHost();

    PyxisHydraHost(const PyxisHydraHost&) = delete;
    PyxisHydraHost& operator=(const PyxisHydraHost&) = delete;

    // True if PyxisEngine initialised (GPU + interop available) and the render
    // stack was created. When false, Render is a no-op returning false.
    bool IsValid() const;

    // Insert (or replace) the stage's full Hydra-2 scene-index chain
    // (UsdImagingCreateSceneIndices: binding resolution + flatten). Call when the
    // active stage changes; for live edits on the same stage, Render reapplies
    // pending updates each frame.
    void SetStage(PXR_NS::UsdStageRefPtr stage);

    // Render at `time`, framing through the camera prim at `cameraPath` (empty
    // path = no camera override -> PyxisEngine default). `frames` drives the path
    // tracer's progressive accumulation: the scene syncs once, then RenderFrame
    // runs `frames` times so the image converges exactly as the live viewport
    // does over time (1 = a single noisy sample). Returns false if !IsValid() or
    // no stage set.
    bool Render(PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default(),
                const PXR_NS::SdfPath& cameraPath = PXR_NS::SdfPath(),
                uint32_t frames = 1);

    // The composited color AOV (RGBA16F). Map()/Unmap() to read pixels. Valid
    // after a successful Render. Null if !IsValid().
    PXR_NS::HdRenderBuffer* ColorBuffer() const;

    // The underlying engine (counts, exported-texture handle for zero-copy
    // display). Null if !IsValid().
    pyxis::hydra::PyxisEngine* Engine() const;

    uint32_t Width() const { return _width; }
    uint32_t Height() const { return _height; }

private:
    uint32_t _width;
    uint32_t _height;

    PXR_NS::HdPyxisRenderDelegate* _delegate = nullptr;  // owned
    PXR_NS::HdRenderIndex* _index = nullptr;                 // owned
    PXR_NS::HdRenderPassSharedPtr _pass;
    PXR_NS::HdRenderBuffer* _colorBuffer = nullptr;   // owned by the delegate
    PXR_NS::SdfPath _colorBufferPath;

    PXR_NS::UsdStageRefPtr _stage;
    PXR_NS::UsdImagingSceneIndices _sceneIndices;
    // The scene index actually inserted into the render index (the implicit-
    // surface->mesh wrapper around _sceneIndices.finalSceneIndex). Tracked so
    // SetStage removes the SAME pointer it inserted.
    PXR_NS::HdSceneIndexBaseRefPtr _insertedSceneIndex;
    bool _stageInserted = false;
};
