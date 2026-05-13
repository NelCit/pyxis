// Pyxis renderer — LightDesc extended defaults tests.
//
// Plan §18.4 / §22.3. PublicDescLayout::LightDescDefaultsToDistantSun
// pins the original-v0.0.1 fields (kind / intensity / envMap /
// doubleSided). This file pins the §22.3 MINOR-additive trailing block
// — every field added after v0.0.1 is documented in LightDesc.h with a
// version note, and these tests assert the documented default value so
// a stray reorder or default flip is caught at the test layer.

#include <Pyxis/Renderer/Descs/LightDesc.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

using pyxis::LightDesc;

// -----------------------------------------------------------------------------
// Enum surface — the §22 MINOR-additive enum tail. Every value is part
// of the byte-stable contract; never renumber.
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<std::underlying_type_t<LightDesc::Kind>, uint8_t>,
              "LightDesc::Kind underlying type is fixed at uint8_t.");
static_assert(static_cast<uint8_t>(LightDesc::Kind::Distant) == 0,
              "Kind::Distant == 0 — the M3 ship kind.");
static_assert(static_cast<uint8_t>(LightDesc::Kind::Dome) == 1);
static_assert(static_cast<uint8_t>(LightDesc::Kind::Rect) == 2);
// §22 MINOR-additive (loaded by the M8a UsdLux ingest pass).
static_assert(static_cast<uint8_t>(LightDesc::Kind::Cylinder) == 3);
static_assert(static_cast<uint8_t>(LightDesc::Kind::Geometry) == 4);
static_assert(static_cast<uint8_t>(LightDesc::Kind::Portal) == 5);

static_assert(std::is_same_v<std::underlying_type_t<LightDesc::DomeFormat>, uint8_t>,
              "LightDesc::DomeFormat underlying type is fixed at uint8_t.");
static_assert(static_cast<uint8_t>(LightDesc::DomeFormat::LatLong) == 0,
              "DomeFormat::LatLong == 0 — the default + most-common format.");
static_assert(static_cast<uint8_t>(LightDesc::DomeFormat::MirroredBall) == 1);
static_assert(static_cast<uint8_t>(LightDesc::DomeFormat::Angular) == 2);

// -----------------------------------------------------------------------------
// Defaults — the §22.3 trailing-block fields, each documented inline in
// LightDesc.h with a version note. Reorder these and you ship a major-
// version break; flip a default and you change the meaning of every
// existing UsdLux fixture.
// -----------------------------------------------------------------------------
TEST(LightDescExtendedDefaults, DomeFormatDefaultsToLatLong) {
  const LightDesc desc;
  EXPECT_EQ(desc.domeFormat, LightDesc::DomeFormat::LatLong);
}

TEST(LightDescExtendedDefaults, NormalizeDefaultsToFalse) {
  // Per UsdLuxLightAPI: normalize=false is the per-area-multiply path,
  // which is the historical default.
  const LightDesc desc;
  EXPECT_FALSE(desc.normalize);
}

TEST(LightDescExtendedDefaults, DiffuseSpecularContributionsDefaultToOne) {
  const LightDesc desc;
  EXPECT_FLOAT_EQ(desc.diffuse, 1.0f);
  EXPECT_FLOAT_EQ(desc.specular, 1.0f);
}

TEST(LightDescExtendedDefaults, DistantAngleDefaultsToRealSun) {
  // 0.53° = real sun apparent angular diameter (UsdLuxDistantLight spec).
  const LightDesc desc;
  EXPECT_FLOAT_EQ(desc.distantAngle, 0.53f);
}

TEST(LightDescExtendedDefaults, ShapingDefaultsAreInert) {
  // Cone angle 90° + softness 0 + focus 0 + white focus tint = no spot,
  // no IES-style shaping, no focus tint. Matches an unshaped light.
  const LightDesc desc;
  EXPECT_FLOAT_EQ(desc.shapingConeAngle, 90.0f);
  EXPECT_FLOAT_EQ(desc.shapingConeSoftness, 0.0f);
  EXPECT_FLOAT_EQ(desc.shapingFocus, 0.0f);
  EXPECT_FLOAT_EQ(desc.shapingFocusTint.x, 1.0f);
  EXPECT_FLOAT_EQ(desc.shapingFocusTint.y, 1.0f);
  EXPECT_FLOAT_EQ(desc.shapingFocusTint.z, 1.0f);
}

TEST(LightDescExtendedDefaults, DomeRotationYDefaultsToZero) {
  const LightDesc desc;
  EXPECT_FLOAT_EQ(desc.domeRotationY, 0.0f);
}
