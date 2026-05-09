// Pyxis app — SceneResolver unit tests.
//
// Plan §29.4.a chain semantics. The CLI / config / bundled fall-
// through is also smoke-tested by hand against the built `pyxis.exe`;
// these tests pin the deterministic (no-Vulkan, no-USD) parts of
// the resolver: enum-label round-trip, bundled-default lookup,
// missing-input fall-through.
//
// SceneResolver.cpp is compiled directly into the test exe (same
// `peek into Private/` exception §35 grants the unit-test harness).
// CliArgs / Configuration headers are pure POD declarations so we
// don't need to drag the matching .cpp files in.

#include "Scene/SceneResolver.h"

#include "CliArgs.h"
#include "Config/Configuration.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

using pyxis::AssetLocator;
using pyxis::app::CliArgs;
using pyxis::app::Configuration;
using pyxis::app::BundledDefaultScenePath;
using pyxis::app::ResolveScene;
using pyxis::app::ResolvedScene;
using pyxis::app::SceneSource;
using pyxis::app::SceneSourceLabel;

namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// SceneSourceLabel — exact strings the §29.4.a spec promises in the
// resolved-source spdlog line. Pinned so log-grep tooling /
// docs / support tickets stay in sync.
// -----------------------------------------------------------------------------
TEST(SceneResolver, SourceLabelsMatchSpec) {
  EXPECT_EQ(SceneSourceLabel(SceneSource::Cli), "cli");
  EXPECT_EQ(SceneSourceLabel(SceneSource::Config), "config");
  EXPECT_EQ(SceneSourceLabel(SceneSource::Recent), "recent");
  EXPECT_EQ(SceneSourceLabel(SceneSource::Bundled), "bundled");
  EXPECT_EQ(SceneSourceLabel(SceneSource::None), "none");
}

// -----------------------------------------------------------------------------
// BundledDefaultScenePath — the test exe lives next to pyxis.exe in
// <build>/bin/<config>/, so the POST_BUILD copy in pyxis_app/CMakeLists.txt
// also makes the bundled file reachable from here.
// -----------------------------------------------------------------------------
TEST(SceneResolver, BundledDefaultExistsAfterBuild) {
  const std::string path = BundledDefaultScenePath();
  ASSERT_FALSE(path.empty()) << "Resources/scenes/default.usd missing — "
                             << "POST_BUILD copy step in pyxis_app/CMakeLists.txt didn't fire.";
  EXPECT_TRUE(fs::exists(fs::path(path)));
}

// -----------------------------------------------------------------------------
// ResolveScene — chain step 4 (bundled default). Empty CLI + empty
// config → SceneSource::Bundled. The §29.4.a "must produce a
// renderable image" contract: every Pyxis launch reaches at least
// step 4 unless the install is broken.
// -----------------------------------------------------------------------------
TEST(SceneResolver, EmptyChainResolvesToBundled) {
  const CliArgs cli;
  const Configuration config;
  const ResolvedScene scene = ResolveScene(cli, config);
  EXPECT_EQ(scene.source, SceneSource::Bundled);
  EXPECT_FALSE(scene.path.empty());
}

// -----------------------------------------------------------------------------
// ResolveScene — chain step 1 wins when --scene points at an
// existing file. Use the bundled default itself as a known-good path
// so the test doesn't need a separate fixture file.
// -----------------------------------------------------------------------------
TEST(SceneResolver, CliPathTakesPrecedence) {
  const std::string bundled = BundledDefaultScenePath();
  ASSERT_FALSE(bundled.empty());

  CliArgs cli;
  cli.scenePath = bundled;
  const Configuration config;
  const ResolvedScene scene = ResolveScene(cli, config);
  EXPECT_EQ(scene.source, SceneSource::Cli);
  // Compare against an absolute-form normalised path so platform
  // separator differences ('/' vs '\\') don't trip the test.
  EXPECT_EQ(fs::path(scene.path), fs::path(bundled));
}

// -----------------------------------------------------------------------------
// ResolveScene — chain step 2 wins when CLI is empty but
// `paths.scene` points at an existing file.
// -----------------------------------------------------------------------------
TEST(SceneResolver, ConfigPathFallsThroughCli) {
  const std::string bundled = BundledDefaultScenePath();
  ASSERT_FALSE(bundled.empty());

  const CliArgs cli;
  Configuration config;
  config.paths.scene = bundled;
  const ResolvedScene scene = ResolveScene(cli, config);
  EXPECT_EQ(scene.source, SceneSource::Config);
}

// -----------------------------------------------------------------------------
// ResolveScene — missing CLI input falls through, missing config
// input falls through, then bundled wins. Verifies the fall-through
// behaviour the §29.4.a "must produce a renderable image" contract
// depends on.
// -----------------------------------------------------------------------------
TEST(SceneResolver, MissingCliAndConfigFallThroughToBundled) {
  CliArgs cli;
  cli.scenePath = "C:/pyxis-tests/does-not-exist-cli.usd";
  Configuration config;
  config.paths.scene = "C:/pyxis-tests/does-not-exist-config.usd";

  const ResolvedScene scene = ResolveScene(cli, config);
  EXPECT_EQ(scene.source, SceneSource::Bundled);
}
