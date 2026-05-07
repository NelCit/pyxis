// Pyxis unit test — SceneWorldInit.
//
// Plan §41 M0 exit criterion:
//   "SceneWorld::Init constructs a flecs::world, registers every component,
//    registers the PYXIS_PHASE_* custom pipeline with no-op systems, and
//    tears down cleanly. Verified by a unit test (`SceneWorldInit`)."
//
// The test exercises the Public/ surface (SceneWorldFacade) — Flecs is
// never visible from a unit test, by §30.11 design.

#include <Pyxis/Renderer/SceneWorldFacade.h>

#include <gtest/gtest.h>

namespace {

TEST(SceneWorldInit, ConstructAndDestructIsClean) {
    pyxis::SceneWorldFacade scene;
    EXPECT_FALSE(scene.IsAlive());
    EXPECT_EQ(scene.TickCount(), 0u);

    const auto status = scene.Init();
    EXPECT_EQ(status, pyxis::SceneWorldStatus::Ok);
    EXPECT_TRUE(scene.IsAlive());

    scene.Shutdown();
    EXPECT_FALSE(scene.IsAlive());
}

TEST(SceneWorldInit, TickAdvancesTickCount) {
    pyxis::SceneWorldFacade scene;
    ASSERT_EQ(scene.Init(), pyxis::SceneWorldStatus::Ok);

    EXPECT_EQ(scene.TickCount(), 0u);
    scene.Tick();
    EXPECT_EQ(scene.TickCount(), 1u);
    scene.Tick();
    scene.Tick();
    EXPECT_EQ(scene.TickCount(), 3u);
}

TEST(SceneWorldInit, RepeatedInitIsIdempotent) {
    pyxis::SceneWorldFacade scene;
    ASSERT_EQ(scene.Init(), pyxis::SceneWorldStatus::Ok);
    ASSERT_EQ(scene.Init(), pyxis::SceneWorldStatus::Ok);
    EXPECT_TRUE(scene.IsAlive());
}

TEST(SceneWorldInit, ShutdownThenReinit) {
    pyxis::SceneWorldFacade scene;
    ASSERT_EQ(scene.Init(), pyxis::SceneWorldStatus::Ok);
    scene.Tick();
    scene.Shutdown();
    EXPECT_FALSE(scene.IsAlive());

    ASSERT_EQ(scene.Init(), pyxis::SceneWorldStatus::Ok);
    EXPECT_TRUE(scene.IsAlive());
    EXPECT_EQ(scene.TickCount(), 0u);  // Shutdown resets the counter.
    scene.Tick();
    EXPECT_EQ(scene.TickCount(), 1u);
}

// Plan §19.7 — slot + generation handle bits are encoded in
// kHandleSlotMask / kHandleGenerationMask.  Asserting them at compile
// time keeps the layout pinned (a non-additive change becomes an
// instantly-failing build, not a silent regression).
static_assert(pyxis::kHandleSlotBits        == 24, "slot bits must be 24 (§19.7)");
static_assert(pyxis::kHandleGenerationBits  ==  8, "generation bits must be 8 (§19.7)");
static_assert(pyxis::kHandleSlotMask        == 0x00FFFFFFu);
static_assert(pyxis::kHandleGenerationMask  == 0xFF000000u);
static_assert(pyxis::kMaxFramesInFlight     ==  3, "kMaxFramesInFlight must be 3 (§33.1)");

}  // namespace
