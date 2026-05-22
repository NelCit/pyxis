// Pyxis renderer — mesh UV tight-packing helper implementation.

#include "GpuScene/MeshUvPack.h"

namespace pyxis::gpuscene_detail {

void AppendTightMeshUvs(const hlslpp::float2* uv0,
                        std::size_t            uv0Count,
                        std::uint32_t          vertexCount,
                        std::vector<PackedUv>& out)
{
  for (std::size_t i = 0; i < uv0Count; ++i)
    out.push_back(PackedUv{static_cast<float>(uv0[i].x),
                           static_cast<float>(uv0[i].y)});
  if (uv0Count < vertexCount)
  {
    const std::size_t padCount = vertexCount - uv0Count;
    out.insert(out.end(), padCount, PackedUv{0.0f, 0.0f});
  }
}

}  // namespace pyxis::gpuscene_detail
