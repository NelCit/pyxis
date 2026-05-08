// Pyxis app — M3 hardcoded cube fixture.

#include "Render/HardcodedCubeScene.h"

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <hlsl++.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace pyxis::app {

namespace {

// Unit cube centered at the origin: 8 vertices (±0.5 along each
// axis), 12 triangles via 36 indices. Triangle winding is
// arbitrary — the M3 closesthit shader ignores normals and only
// emits barycentric colour, so back-face culling isn't engaged on
// either side. `static const` (not constexpr) because hlslpp::float3
// has user-defined SIMD ctors and isn't a literal type.
const std::array<hlslpp::float3, 8> CUBE_POSITIONS = {
    hlslpp::float3{-0.5f, -0.5f, -0.5f},  // 0  back-bottom-left
    hlslpp::float3{ 0.5f, -0.5f, -0.5f},  // 1  back-bottom-right
    hlslpp::float3{ 0.5f,  0.5f, -0.5f},  // 2  back-top-right
    hlslpp::float3{-0.5f,  0.5f, -0.5f},  // 3  back-top-left
    hlslpp::float3{-0.5f, -0.5f,  0.5f},  // 4  front-bottom-left
    hlslpp::float3{ 0.5f, -0.5f,  0.5f},  // 5  front-bottom-right
    hlslpp::float3{ 0.5f,  0.5f,  0.5f},  // 6  front-top-right
    hlslpp::float3{-0.5f,  0.5f,  0.5f},  // 7  front-top-left
};

constexpr std::array<std::uint32_t, 36> CUBE_INDICES = {
    // back  (-z)
    0, 1, 2,  0, 2, 3,
    // front (+z)
    4, 6, 5,  4, 7, 6,
    // left  (-x)
    0, 3, 7,  0, 7, 4,
    // right (+x)
    1, 5, 6,  1, 6, 2,
    // bottom (-y)
    0, 4, 5,  0, 5, 1,
    // top (+y)
    3, 2, 6,  3, 6, 7,
};

// Build a row-major + row-vector view-matrix for a camera placed at
// (0, 0, 3) looking down the -Z axis with up = +Y. In row-vector
// convention with translation in the last row, that's:
//   [ 1  0  0  0 ]
//   [ 0  1  0  0 ]
//   [ 0  0  1  0 ]
//   [ 0  0 -3  1 ]
// because pos_view = pos_world * V puts a world point at (0,0,0) at
// view-space (0, 0, -3) — i.e. 3 units in front of the camera along
// the camera's -Z forward axis.
hlslpp::float4x4 BuildViewMatrix() noexcept {
    return hlslpp::float4x4(
        hlslpp::float4(1.0f, 0.0f, 0.0f, 0.0f),
        hlslpp::float4(0.0f, 1.0f, 0.0f, 0.0f),
        hlslpp::float4(0.0f, 0.0f, 1.0f, 0.0f),
        hlslpp::float4(0.0f, 0.0f, -3.0f, 1.0f));
}

// Build a row-major + row-vector Vulkan perspective projection. Y is
// flipped (pre-multiplied -1 in the second row) so the M3 image is
// upright; Vulkan's NDC has Y-down by default.
//
// Derivation (column-vector convention, then transposed):
//   col-vec P (Vulkan, depth [0,1]):
//     [ f/aspect 0  0           0       ]
//     [ 0       -f  0           0       ]   (-f flips Y)
//     [ 0        0  far/(far-n) 1       ]
//     [ 0        0  -n*far/(far-n) 0    ]
//   row-vec is the transpose of the above:
//     [ f/aspect 0  0           0 ]
//     [ 0       -f  0           0 ]
//     [ 0        0  far/(far-n) 1 ]
//     [ 0        0 -n*far/(far-n) 0 ]
hlslpp::float4x4 BuildProjMatrix(std::uint32_t renderWidth,
                                 std::uint32_t renderHeight) noexcept {
    const float fovY   = 1.0472f;   // 60 degrees in radians.
    const float aspect = renderHeight == 0
                            ? 1.0f
                            : float(renderWidth) / float(renderHeight);
    const float focal  = 1.0f / std::tan(fovY * 0.5f);
    const float nearZ  = 0.1f;
    const float farZ   = 100.0f;
    const float zSpan  = farZ - nearZ;

    return hlslpp::float4x4(
        hlslpp::float4(focal / aspect, 0.0f,   0.0f,                     0.0f),
        hlslpp::float4(0.0f,           -focal, 0.0f,                     0.0f),
        hlslpp::float4(0.0f,           0.0f,   farZ / zSpan,             1.0f),
        hlslpp::float4(0.0f,           0.0f,   -nearZ * farZ / zSpan,    0.0f));
}

}  // namespace

std::expected<void, std::string>
BuildHardcodedCubeScene(GpuScene& scene, std::uint32_t renderWidth, std::uint32_t renderHeight) noexcept {
    // ---- Mesh -------------------------------------------------------------
    MeshDesc cubeDesc;
    cubeDesc.positions = CUBE_POSITIONS;
    cubeDesc.indices   = CUBE_INDICES;
    cubeDesc.debugName = "m3.hardcoded-cube";
    auto meshResult = scene.CreateMesh(cubeDesc);
    if (!meshResult) {
        return std::unexpected{
            std::string{"BuildHardcodedCubeScene: CreateMesh failed: "}
            + std::string{meshResult.error().message.View()}};
    }
    const MeshHandle cubeMesh = *meshResult;

    // ---- Instance ---------------------------------------------------------
    // Identity transform — the cube sits at the origin. M5+ is when
    // non-identity instance transforms get exercised end-to-end (and
    // GpuScene's CommitResources gains the row-vector→column-vector
    // transpose helper for the TLAS instance-desc affine layout).
    InstanceDesc cubeInstance;
    cubeInstance.mesh           = cubeMesh;
    cubeInstance.material       = MaterialHandle::Invalid;   // M5+ wires materials.
    cubeInstance.worldFromLocal = hlslpp::float4x4(
        hlslpp::float4(1.0f, 0.0f, 0.0f, 0.0f),
        hlslpp::float4(0.0f, 1.0f, 0.0f, 0.0f),
        hlslpp::float4(0.0f, 0.0f, 1.0f, 0.0f),
        hlslpp::float4(0.0f, 0.0f, 0.0f, 1.0f));
    cubeInstance.visible        = true;
    cubeInstance.debugName      = "m3.hardcoded-cube.instance";
    auto instanceResult = scene.AppendInstance(cubeInstance);
    if (!instanceResult) {
        return std::unexpected{
            std::string{"BuildHardcodedCubeScene: AppendInstance failed: "}
            + std::string{instanceResult.error().message.View()}};
    }

    // ---- Camera -----------------------------------------------------------
    CameraDesc camera;
    camera.viewFromWorld = BuildViewMatrix();
    camera.projFromView  = BuildProjMatrix(renderWidth, renderHeight);
    camera.focalLengthMm = 35.0f;
    camera.apertureFStop = 0.0f;       // pinhole.
    camera.focusDistance = 3.0f;
    camera.nearClip      = 0.1f;
    camera.farClip       = 100.0f;
    scene.SetCamera(camera);

    // ---- Distant sun ------------------------------------------------------
    // Stored but not yet consumed in the shader — the M3 closesthit
    // returns barycentric colour, ignoring lighting. M7 brings NEE
    // and the light shows up in the rendered image.
    LightDesc sun;
    sun.kind      = LightDesc::Kind::Distant;
    sun.color     = hlslpp::float3(1.0f, 0.95f, 0.9f);
    sun.intensity = 5.0f;
    sun.direction = hlslpp::float3(-0.3f, -1.0f, -0.2f);
    // The handle is discarded — we don't update or remove the light
    // for the M3 fixture. Capture explicitly to satisfy AddLight's
    // [[nodiscard]] attribute.
    [[maybe_unused]] const LightHandle sunHandle = scene.AddLight(sun);

    return {};
}

}  // namespace pyxis::app
