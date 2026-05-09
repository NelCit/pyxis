// Pyxis renderer — SceneWorld lifecycle facade.
//
// The §8 SceneWorld is `Private/`; nothing about Flecs (`flecs::world`,
// components, queries, observers) is exposed through Public/. This facade
// is the single Public/ entry point: a Construct/Tick/Destroy lifecycle
// that the M0 SceneWorldInit unit test and the Application orchestration
// can drive without ever including a Flecs header (plan §30.11).
//
// Once §18's GpuScene + PyxisRenderer ship, the application talks to those
// instead. SceneWorldFacade stays around as the lower-level "I just want a
// world" affordance for tooling and tests.

#pragma once

#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>

namespace pyxis {

// One-shot lifecycle status. Same shape as the platform-side
// DeviceManagerCreateStatus — the application maps both onto its global
// exit-code table (plan §41 M0: exit codes 0=ok, 2=device init fail,
// 3=config fail).
enum class SceneWorldStatus : uint8_t {
  Ok = 0,
  FlecsInitFailed,
  PhasePipelineInvalid,
  Unknown,
};

class PYXIS_RENDERER_API SceneWorldFacade final {
 public:
  SceneWorldFacade();
  ~SceneWorldFacade();

  SceneWorldFacade(const SceneWorldFacade&) = delete;
  SceneWorldFacade& operator=(const SceneWorldFacade&) = delete;

  // -------------------------------------------------------------------
  // Lifecycle.
  //   Init  — constructs the flecs::world, registers every component,
  //           registers the PYXIS_PHASE_* custom pipeline, registers the
  //           no-op systems, registers observers, brings up Flecs
  //           Explorer when PYXIS_DEBUG_TOOLS is on.
  //   Tick  — runs world.progress() once.  In the M0 unit test this is
  //           how we assert the phase pipeline survives a roundtrip.
  //   Shutdown — drains the Flecs world deterministically.
  // -------------------------------------------------------------------
  [[nodiscard]] SceneWorldStatus Init() noexcept;
  void Tick() noexcept;
  void Shutdown() noexcept;

  // True iff Init() succeeded and Shutdown() has not been called.
  [[nodiscard]] bool IsAlive() const noexcept;

  // Number of times the per-phase no-op systems have been ticked. Used
  // by the SceneWorldInit unit test to assert that registration *plus*
  // pipeline traversal both work.
  [[nodiscard]] uint64_t TickCount() const noexcept;

 private:
  struct Impl;
  Impl* _impl = nullptr;
};

}  // namespace pyxis
