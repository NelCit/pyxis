// Pyxis USD ingest — OpenVDB grid loader implementation.

#include "VolumeLoader.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <mutex>

namespace pyxis::usd_ingest {

namespace {

// openvdb::initialize() must be called once per process before any
// I/O. The call is internally guarded but cheaper to gate ourselves
// + we get a single canonical log line.
void EnsureOpenVdbInitialized() noexcept
{
  static std::once_flag initFlag;
  std::call_once(initFlag, []() {
    try
    {
      openvdb::initialize();
    }
    catch (const std::exception& exc)
    {
      Logging::Get().Error(log::APP, std::string{"openvdb::initialize() threw: "} + exc.what());
    }
  });
}

// Pick the float grid with the most active voxels. Returns nullptr
// if no float grid is present. Other grid types (Vec3SGrid, BoolGrid,
// Int32Grid, etc.) are ignored in v1; multi-channel volumes are post-v1.
openvdb::FloatGrid::Ptr PickLargestFloatGrid(const openvdb::GridPtrVecPtr& grids) noexcept
{
  openvdb::FloatGrid::Ptr best;
  openvdb::Index64        bestActive = 0;
  for (const openvdb::GridBase::Ptr& base : *grids)
  {
    auto floatGrid = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
    if (!floatGrid)
      continue;
    const openvdb::Index64 active = floatGrid->activeVoxelCount();
    if (active > bestActive)
    {
      bestActive = active;
      best       = floatGrid;
    }
  }
  return best;
}

}  // namespace

std::expected<LoadedVolume, std::string> LoadVdbGrid(const std::string& filePath) noexcept
{
  EnsureOpenVdbInitialized();

  if (filePath.empty())
    return std::unexpected{std::string{"VolumeLoader: empty file path"}};

  openvdb::GridPtrVecPtr grids;
  std::string            errorMessage;
  try
  {
    openvdb::io::File file{filePath};
    file.open();
    grids = file.getGrids();
    file.close();
  }
  catch (const std::exception& exc)
  {
    errorMessage = "VolumeLoader: openvdb open failed for '" + filePath + "': " + exc.what();
  }
  if (!errorMessage.empty())
    return std::unexpected{std::move(errorMessage)};
  if (!grids || grids->empty())
    return std::unexpected{std::string{"VolumeLoader: '"} + filePath + "' contains no grids"};

  openvdb::FloatGrid::Ptr grid = PickLargestFloatGrid(grids);
  if (!grid)
    return std::unexpected{std::string{"VolumeLoader: '"} + filePath + "' has no float grid"};

  // openvdb::evalActiveVoxelBoundingBox() returns the inclusive
  // CoordBBox over active voxels — empty if the grid was authored
  // but never written, in which case the dense buffer is trivially
  // zero-size and we surface a friendly error.
  const openvdb::CoordBBox activeBbox = grid->evalActiveVoxelBoundingBox();
  if (activeBbox.empty())
  {
    return std::unexpected{std::string{"VolumeLoader: '"} + filePath
                           + "' grid '" + grid->getName() + "' has no active voxels"};
  }

  const openvdb::Coord bboxMin = activeBbox.min();
  const openvdb::Coord bboxMax = activeBbox.max();
  // CoordBBox dims are inclusive: a 1-voxel box has dim == 1, not 0.
  const openvdb::Coord dim = activeBbox.dim();
  const auto           voxelsPerAxisX = static_cast<uint32_t>(dim.x());
  const auto           voxelsPerAxisY = static_cast<uint32_t>(dim.y());
  const auto           voxelsPerAxisZ = static_cast<uint32_t>(dim.z());
  const size_t         voxelCount     = static_cast<size_t>(voxelsPerAxisX)
                                 * voxelsPerAxisY
                                 * voxelsPerAxisZ;
  if (voxelCount == 0)
  {
    return std::unexpected{std::string{"VolumeLoader: '"} + filePath
                           + "' active bbox has zero voxels"};
  }

  LoadedVolume out;
  try
  {
    out.voxels.assign(voxelCount, 0.0f);
  }
  catch (const std::bad_alloc&)
  {
    return std::unexpected{std::string{"VolumeLoader: bad_alloc allocating "}
                           + std::to_string(voxelCount) + " voxels for '" + filePath + "'"};
  }

  // Sparse-tree walk: dense buffer indexed (z * dx*dy + y * dx + x)
  // with x,y,z in [0, dim). Each active voxel writes its value;
  // inactive voxels keep the zero-init from std::vector::assign.
  // Accessor pattern (cached) is the canonical openvdb fast read.
  const openvdb::FloatGrid::ConstAccessor accessor = grid->getConstAccessor();
  const int                         minXAxis = bboxMin.x();
  const int                         minYAxis = bboxMin.y();
  const int                         minZAxis = bboxMin.z();
  for (uint32_t voxelZ = 0; voxelZ < voxelsPerAxisZ; ++voxelZ)
  {
    for (uint32_t voxelY = 0; voxelY < voxelsPerAxisY; ++voxelY)
    {
      for (uint32_t voxelX = 0; voxelX < voxelsPerAxisX; ++voxelX)
      {
        const openvdb::Coord coord(static_cast<int>(voxelX) + minXAxis,
                                   static_cast<int>(voxelY) + minYAxis,
                                   static_cast<int>(voxelZ) + minZAxis);
        const size_t flatIdx = (static_cast<size_t>(voxelZ) * voxelsPerAxisY * voxelsPerAxisX)
                               + (static_cast<size_t>(voxelY) * voxelsPerAxisX)
                               + voxelX;
        out.voxels[flatIdx] = accessor.getValue(coord);
      }
    }
  }

  out.dimensions  = {voxelsPerAxisX, voxelsPerAxisY, voxelsPerAxisZ};
  out.bboxMinIndex = {static_cast<float>(bboxMin.x()),
                      static_cast<float>(bboxMin.y()),
                      static_cast<float>(bboxMin.z())};
  out.bboxMaxIndex = {static_cast<float>(bboxMax.x()),
                      static_cast<float>(bboxMax.y()),
                      static_cast<float>(bboxMax.z())};

  // openvdb::math::Transform returns the affine portion as a Mat4d.
  // Row-major copy matches Pyxis convention (§10 column-vector math
  // with row-major storage). Mat4d::asPointer() yields 16 doubles in
  // row-major order.
  const openvdb::math::Transform& xform = grid->transform();
  const openvdb::math::Mat4d      matrix = xform.baseMap()->getAffineMap()->getMat4();
  const double*                   matPtr = matrix.asPointer();
  for (int row = 0; row < 16; ++row)
    out.indexToWorld[static_cast<size_t>(row)] = static_cast<float>(matPtr[row]);

  out.gridName         = grid->getName();
  out.activeVoxelCount = static_cast<uint64_t>(grid->activeVoxelCount());

  return out;
}

}  // namespace pyxis::usd_ingest
