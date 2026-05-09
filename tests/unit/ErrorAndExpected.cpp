// Pyxis renderer — Error / Expected unit tests.
//
// Plan §18.3 + §20. These exercise the byte-stable public Error
// surface that every Expected<T>-returning method depends on. Locked
// down here so a regression in `MakeError`, the truncation policy, or
// the catalogue numbering breaks the test rather than silently
// flipping the diagnostic output of every renderer call.

#include <Pyxis/Renderer/Error.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

using pyxis::Error;
using pyxis::ErrorKind;
using pyxis::ErrorMessage;
using pyxis::Expected;

// -----------------------------------------------------------------------------
// ABI / layout — these are the byte-stable contracts (§18.9). Reviewers
// reject any layout change here without a major-version bump.
// -----------------------------------------------------------------------------
TEST(Error, ErrorMessageIsTriviallyCopyable) {
  static_assert(std::is_trivially_copyable_v<ErrorMessage>,
                "ErrorMessage must cross DLL boundaries safely.");
  static_assert(std::is_standard_layout_v<ErrorMessage>,
                "ErrorMessage must be standard-layout for the public ABI.");
  static_assert(ErrorMessage::CAPACITY == 240, "Bumping CAPACITY is a major-version break (§22).");
}

TEST(Error, ErrorIsTriviallyCopyable) {
  static_assert(std::is_trivially_copyable_v<Error>,
                "Error is a public POD; trivial copy is part of the contract.");
  static_assert(std::is_standard_layout_v<Error>,
                "Error must be standard-layout for the public ABI.");
}

TEST(Error, ErrorKindIsUint16) {
  static_assert(std::is_same_v<std::underlying_type_t<ErrorKind>, uint16_t>,
                "ErrorKind underlying type is fixed at uint16_t (§20).");
  static_assert(static_cast<uint16_t>(ErrorKind::Ok) == 0,
                "ErrorKind::Ok == 0 is part of the contract — never renumber.");
}

// -----------------------------------------------------------------------------
// Defaults: a default-constructed Error is the "ok / empty" state. Lets
// callers initialise an Error to fill in later without fearing UB.
// -----------------------------------------------------------------------------
TEST(Error, DefaultIsOkAndEmpty) {
  const Error err;
  EXPECT_EQ(err.kind, ErrorKind::Ok);
  EXPECT_EQ(err.message.size, 0u);
  EXPECT_EQ(err.source.size, 0u);
  EXPECT_TRUE(err.message.View().empty());
  EXPECT_TRUE(err.source.View().empty());
}

// -----------------------------------------------------------------------------
// PYXIS_ERROR macro behaviour.
// -----------------------------------------------------------------------------
TEST(PyxisError, PopulatesKindMessageAndSource) {
  const Error err = PYXIS_ERROR(ErrorKind::FileNotFound, "no such file: %s", "/tmp/missing.usd");
  EXPECT_EQ(err.kind, ErrorKind::FileNotFound);
  EXPECT_EQ(err.message.View(), "no such file: /tmp/missing.usd");

  // Source is "file:line" via __FILE__ / __LINE__. We can't pin the
  // exact text (build-relative paths differ between CI machines), but
  // the colon-line suffix is contractual.
  const std::string_view source = err.source.View();
  EXPECT_FALSE(source.empty());
  const auto colon = source.rfind(':');
  ASSERT_NE(colon, std::string_view::npos);
  EXPECT_GT(source.size() - colon, 1u);  // at least one digit
}

TEST(PyxisError, MessageWithoutFormatArgsWorks) {
  const Error err = PYXIS_ERROR(ErrorKind::InvalidArgument, "plain text");
  EXPECT_EQ(err.kind, ErrorKind::InvalidArgument);
  EXPECT_EQ(err.message.View(), "plain text");
}

// -----------------------------------------------------------------------------
// Truncation — the inline buffer is fixed-capacity, so a long message
// must be truncated with a "..." trailing marker so log readers can
// see truncation happened (§18.3 contract).
// -----------------------------------------------------------------------------
TEST(PyxisError, MessageTruncatesWithTrailingEllipsis) {
  // Build a >CAPACITY-byte message via vsnprintf's `%s` width.
  const std::string oversized(ErrorMessage::CAPACITY * 2, 'x');
  const Error err = PYXIS_ERROR(ErrorKind::FileCorrupt, "%s", oversized.c_str());

  const std::string_view view = err.message.View();
  EXPECT_EQ(view.size(), ErrorMessage::CAPACITY
                             - 1);  // CAPACITY-1 visible bytes (vsnprintf null terminator at end)
  ASSERT_GE(view.size(), 3u);
  EXPECT_EQ(view.substr(view.size() - 3), std::string_view{"..."});
}

// -----------------------------------------------------------------------------
// Expected<T> alias — make sure the std::expected glue actually works
// with the renderer's Error type so callers can rely on the spelling.
// -----------------------------------------------------------------------------
TEST(Expected, HoldsValueByDefault) {
  const Expected<int> success = 42;
  ASSERT_TRUE(success.has_value());
  EXPECT_EQ(*success, 42);
}

TEST(Expected, HoldsErrorOnUnexpected) {
  const Expected<int> failed =
      std::unexpected{PYXIS_ERROR(ErrorKind::OutOfMemoryGpu, "GPU OOM at %u MiB", 4096u)};
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().kind, ErrorKind::OutOfMemoryGpu);
  EXPECT_EQ(failed.error().message.View(), "GPU OOM at 4096 MiB");
}

TEST(Expected, VoidSpecialisationCompiles) {
  const Expected<void> success;
  EXPECT_TRUE(success.has_value());

  const Expected<void> failed = std::unexpected{PYXIS_ERROR(ErrorKind::DeviceLost, "lost")};
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().kind, ErrorKind::DeviceLost);
}

// -----------------------------------------------------------------------------
// PYXIS_TRY — propagates an Expected<void> failure upward unchanged.
// -----------------------------------------------------------------------------
namespace {

Expected<void> AlwaysFails() {
  return std::unexpected{PYXIS_ERROR(ErrorKind::NotImplemented, "stub: %s", "AlwaysFails")};
}

Expected<void> ChainsFailure() {
  PYXIS_TRY(AlwaysFails());
  return {};  // unreachable on the failure path
}

Expected<void> AlwaysOk() {
  return {};
}

Expected<void> ChainsOk() {
  PYXIS_TRY(AlwaysOk());
  return {};
}

}  // namespace

TEST(PyxisTry, PropagatesFailureUnchanged) {
  Expected<void> result = ChainsFailure();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::NotImplemented);
  EXPECT_EQ(result.error().message.View(), "stub: AlwaysFails");
}

TEST(PyxisTry, FallsThroughOnSuccess) {
  const Expected<void> result = ChainsOk();
  EXPECT_TRUE(result.has_value());
}
