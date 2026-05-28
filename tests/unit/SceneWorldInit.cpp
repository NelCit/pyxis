// Pyxis unit test — SceneWorldInit.
//
// Plan §41 M0 exit criterion (restated for RFC 0009): the §8 / §30.11 custom
// Flecs phase pipeline registers cleanly on a fresh world and ticks in the
// declared order. The former app-side `SceneWorldFacade` (a parallel, no-op
// world the app constructed + Tick()'d) was retired in RFC 0009 once GpuScene
// became the real SceneWorld owner; this test now pins the surviving primitive
// the GpuScene commit path is built on — `scene::RegisterPhasePipeline`.
//
// §35 white-box exception: the test compiles Private/Scene/Phases.cpp directly
// and links Flecs (the renderer DLL does not export this Private symbol).

#include "Scene/Phases.h"

#include <Pyxis/Renderer/Forward.h>

#include <gtest/gtest.h>

#include <flecs.h>
#include <vector>

namespace {

using namespace pyxis::scene;

TEST(SceneWorldInit, RegisterPhasePipelineTicksCleanly) {
  flecs::world world;
  const flecs::entity pipeline = RegisterPhasePipeline(world);
  EXPECT_NE(pipeline.id(), 0u);  // a real pipeline entity was built + set.

  // A no-op system on every §30.11 phase; world.progress() must run them all
  // without error (the M0 "registers the PYXIS_PHASE_* pipeline + tears down
  // cleanly" criterion, now against the real primitive).
  int ran = 0;
  world.system("T_UploadTextures").kind<PhaseUploadTextures>().run(
      [&ran](flecs::iter&) { ++ran; });
  world.system("T_UploadMaterials").kind<PhaseUploadMaterials>().run(
      [&ran](flecs::iter&) { ++ran; });
  world.system("T_ExtractMeshes").kind<PhaseExtractMeshes>().run(
      [&ran](flecs::iter&) { ++ran; });
  world.system("T_BuildBlas").kind<PhaseBuildBlas>().run(
      [&ran](flecs::iter&) { ++ran; });
  world.system("T_RebuildTlas").kind<PhaseRebuildTlas>().run(
      [&ran](flecs::iter&) { ++ran; });
  world.system("T_UpdateBindless").kind<PhaseUpdateBindless>().run(
      [&ran](flecs::iter&) { ++ran; });
  world.system("T_ClearDirty").kind<PhaseClearDirty>().run(
      [&ran](flecs::iter&) { ++ran; });

  EXPECT_TRUE(world.progress());
  EXPECT_EQ(ran, 7);  // every phase's system fired exactly once.
}

TEST(SceneWorldInit, PhasesRunInDeclaredOrder) {
  flecs::world world;
  RegisterPhasePipeline(world);

  // Systems are registered out of phase order on purpose; the pipeline must
  // still run them in the §30.11 phase order (textures → … → clear-dirty).
  // Reordering phases is an RFC-gated change (§44), so this pins it.
  std::vector<int> order;
  world.system("T_ClearDirty").kind<PhaseClearDirty>().run(
      [&order](flecs::iter&) { order.push_back(6); });
  world.system("T_UploadTextures").kind<PhaseUploadTextures>().run(
      [&order](flecs::iter&) { order.push_back(0); });
  world.system("T_BuildBlas").kind<PhaseBuildBlas>().run(
      [&order](flecs::iter&) { order.push_back(3); });
  world.system("T_UploadMaterials").kind<PhaseUploadMaterials>().run(
      [&order](flecs::iter&) { order.push_back(1); });
  world.system("T_RebuildTlas").kind<PhaseRebuildTlas>().run(
      [&order](flecs::iter&) { order.push_back(4); });
  world.system("T_ExtractMeshes").kind<PhaseExtractMeshes>().run(
      [&order](flecs::iter&) { order.push_back(2); });
  world.system("T_UpdateBindless").kind<PhaseUpdateBindless>().run(
      [&order](flecs::iter&) { order.push_back(5); });

  world.progress();
  const std::vector<int> expected{0, 1, 2, 3, 4, 5, 6};
  EXPECT_EQ(order, expected);
}

// Plan §19.7 — slot + generation handle bits are encoded in
// HANDLE_SLOT_MASK / HANDLE_GENERATION_MASK.  Asserting them at compile
// time keeps the layout pinned (a non-additive change becomes an
// instantly-failing build, not a silent regression).
static_assert(pyxis::HANDLE_SLOT_BITS == 24, "slot bits must be 24 (§19.7)");
static_assert(pyxis::HANDLE_GENERATION_BITS == 8, "generation bits must be 8 (§19.7)");
static_assert(pyxis::HANDLE_SLOT_MASK == 0x00FFFFFFu);
static_assert(pyxis::HANDLE_GENERATION_MASK == 0xFF000000u);
static_assert(pyxis::MAX_FRAMES_IN_FLIGHT == 3, "MAX_FRAMES_IN_FLIGHT must be 3 (§33.1)");

}  // namespace
