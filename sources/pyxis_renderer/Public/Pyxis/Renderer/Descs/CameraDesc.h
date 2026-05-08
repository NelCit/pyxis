// Pyxis renderer — public camera descriptor.
//
// Plan §18.4. Input to GpuScene::SetCamera. Both matrices are
// row-major (§10): `posView = mul(posWorld, viewFromWorld)`,
// `posClip = mul(posView, projFromView)`.
//
// `apertureFStop = 0.0f` is the pinhole sentinel — DOF stays off (§43.3
// reserves the runtime path for M9+). `focusDistance` is in scene
// units (meters by default). `nearClip` / `farClip` follow the usual
// reverse-Z-friendly convention: keep `near` small and positive.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <hlsl++.h>

namespace pyxis {

struct CameraDesc {
    hlslpp::float4x4 viewFromWorld{};
    hlslpp::float4x4 projFromView{};
    float            focalLengthMm = 35.0f;
    float            apertureFStop = 0.0f;     // 0 = pinhole
    float            focusDistance = 1.0f;
    float            nearClip      = 0.01f;
    float            farClip       = 10000.0f;
};

}  // namespace pyxis
