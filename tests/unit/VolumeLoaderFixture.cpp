// Pyxis V2.A.5 — VolumeLoader fixture. Authors a tiny synthetic .vdb
// at test setup time via openvdb's authoring API, then runs the
// VolumeLoader against it and asserts on dimensions / active count /
// dense layout indices. No checked-in .vdb binary — the file is built
// fresh per test run, kept identical bit-for-bit on each run since
// openvdb's writer is deterministic for the same input.

#include "VolumeLoader.h"

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

// Build a 4³ FloatGrid with two active voxels and write it to a
// temp .vdb. The two active voxels (1.0 at index (1,1,1), 2.0 at
// index (2,2,2)) make the active bbox a 2³ box from (1,1,1) to
// (2,2,2) — the loader should report dimensions = (2,2,2) and a
// dense buffer of 8 floats with active values placed accordingly.
fs::path WriteSyntheticVdb()
{
  openvdb::initialize();

  const openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(/*background*/ 0.0f);
  grid->setName("density");
  openvdb::FloatGrid::Accessor accessor = grid->getAccessor();  // NOLINT(misc-const-correctness)
  accessor.setValue(openvdb::Coord(1, 1, 1), 1.0f);
  accessor.setValue(openvdb::Coord(2, 2, 2), 2.0f);
  grid->setTransform(openvdb::math::Transform::createLinearTransform(0.1));  // 10cm voxels

  const fs::path tempPath =
      fs::temp_directory_path() / "pyxis_volumeloader_test.vdb";
  openvdb::io::File file{tempPath.generic_string()};
  openvdb::GridPtrVec grids;
  grids.push_back(grid);
  file.write(grids);
  file.close();
  return tempPath;
}

}  // namespace

TEST(VolumeLoaderFixture, LoadsSyntheticGrid)
{
  const fs::path vdbPath = WriteSyntheticVdb();
  ASSERT_TRUE(fs::exists(vdbPath));

  auto result = pyxis::usd_ingest::LoadVdbGrid(vdbPath.generic_string());
  ASSERT_TRUE(result.has_value()) << "LoadVdbGrid failed: "
                                  << (result ? std::string{} : result.error());

  const pyxis::usd_ingest::LoadedVolume& vol = *result;
  EXPECT_EQ(vol.gridName, "density");
  EXPECT_EQ(vol.activeVoxelCount, 2u);
  EXPECT_EQ(vol.dimensions[0], 2u);
  EXPECT_EQ(vol.dimensions[1], 2u);
  EXPECT_EQ(vol.dimensions[2], 2u);
  EXPECT_EQ(vol.voxels.size(), 8u);

  // Active bbox should be (1,1,1) -> (2,2,2).
  EXPECT_FLOAT_EQ(vol.bboxMinIndex[0], 1.0f);
  EXPECT_FLOAT_EQ(vol.bboxMinIndex[1], 1.0f);
  EXPECT_FLOAT_EQ(vol.bboxMinIndex[2], 1.0f);
  EXPECT_FLOAT_EQ(vol.bboxMaxIndex[0], 2.0f);
  EXPECT_FLOAT_EQ(vol.bboxMaxIndex[1], 2.0f);
  EXPECT_FLOAT_EQ(vol.bboxMaxIndex[2], 2.0f);

  // Dense indexing: voxels[z * dx*dy + y * dx + x] with (dx,dy,dz)=2.
  //   (0,0,0)_local == (1,1,1)_grid → index 0 → value 1.0
  //   (1,1,1)_local == (2,2,2)_grid → index 1 + 2 + 4 = 7 → value 2.0
  EXPECT_FLOAT_EQ(vol.voxels[0], 1.0f);
  EXPECT_FLOAT_EQ(vol.voxels[7], 2.0f);
  // The remaining 6 voxels are inactive, zero-initialised.
  EXPECT_FLOAT_EQ(vol.voxels[1], 0.0f);
  EXPECT_FLOAT_EQ(vol.voxels[2], 0.0f);
  EXPECT_FLOAT_EQ(vol.voxels[3], 0.0f);
  EXPECT_FLOAT_EQ(vol.voxels[4], 0.0f);
  EXPECT_FLOAT_EQ(vol.voxels[5], 0.0f);
  EXPECT_FLOAT_EQ(vol.voxels[6], 0.0f);

  // Cleanup the temp file so re-runs start fresh; ignore errors.
  std::error_code errorCode;
  fs::remove(vdbPath, errorCode);
}

TEST(VolumeLoaderFixture, EmptyPathReturnsError)
{
  auto result = pyxis::usd_ingest::LoadVdbGrid({});
  EXPECT_FALSE(result.has_value());
}

TEST(VolumeLoaderFixture, MissingFileReturnsError)
{
  auto result = pyxis::usd_ingest::LoadVdbGrid("c:/this/path/does/not/exist.vdb");
  EXPECT_FALSE(result.has_value());
}
