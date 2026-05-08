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
    MeshHandle        mesh           = MeshHandle::Invalid;
    MaterialHandle    material       = MaterialHandle::Invalid;
    hlslpp::float4x4  worldFromLocal{};   // row-major + column-vector (§10)
    bool              visible        = true;
    std::string_view  debugName;
};

}  // namespace pyxis
