// Pyxis renderer — public instance-creation descriptor.
//
// Plan §18.4. Input to GpuScene::AppendInstance.
//
// `worldFromLocal` is row-major + column-vector (§10): multiplication
// is `posWorld = mul(worldFromLocal, posLocal)` — matrix on the
// left, translation in the last column. Identity for an instance at
// the origin with no rotation or scale.

#pragma once

#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <hlsl++.h>
#include <string_view>

namespace pyxis {

struct InstanceDesc {
  MeshHandle mesh = MeshHandle::Invalid;
  MaterialHandle material = MaterialHandle::Invalid;
  hlslpp::float4x4 worldFromLocal{};  // row-major + column-vector (§10)
  bool visible = true;
  // V2.A.x — UsdGeomGprim::doubleSided. When true, ray hits from the
  // back face contribute the same as front-face hits (no back-face
  // culling, normal flipped to match the incoming ray side). Common
  // on foliage / cloth / signage. The closesthit branch lives behind
  // the existing MaterialFlag bits — we plumb the per-instance flag
  // through but the shader read is a follow-up; for now the flag
  // propagates onto FrameStats and is queryable via the renderer
  // public surface so other passes can act on it.
  bool doubleSided = false;
  std::string_view debugName;
};

}  // namespace pyxis
