// Pyxis M17 / V2.A.12 — ArResolver visibility. Pyxis rides the default
// ArResolver in v2; we log its type at stage open so resolver-context
// misconfigurations are diagnosable. This test verifies that USD's
// resolver is available + reachable, which is the precondition the
// StageWalker log relies on.

#include <pxr/usd/ar/resolver.h>

#include <gtest/gtest.h>

#include <string>

TEST(ArResolverVisibility, GlobalResolverIsReachable)
{
  // Calling ArGetResolver() should never throw or return null in any
  // sane USD build — the default ArDefaultResolver is always present.
  const pxr::ArResolver& resolver = pxr::ArGetResolver();
  const std::string resolverType = typeid(resolver).name();
  EXPECT_FALSE(resolverType.empty());
}

TEST(ArResolverVisibility, ResolverHandlesFilesystemPath)
{
  // ArGetResolver().Resolve(...) on an absolute filesystem path
  // returns the resolved path (or empty if not found). We just exercise
  // the call to prove the resolver is healthy.
  const pxr::ArResolver& resolver = pxr::ArGetResolver();
  const std::string anchored = resolver.CreateIdentifier("nonexistent.usda");
  // CreateIdentifier never throws even for missing files; it returns
  // a normalised identifier string.
  EXPECT_FALSE(anchored.empty());
}
