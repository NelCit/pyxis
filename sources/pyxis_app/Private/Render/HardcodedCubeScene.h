// Pyxis app — M3 hardcoded cube fixture.
//
// Plan §41 M3. Both viewer and headless drive the same path-trace
// box render at this milestone — a single cube at the origin, a
// pinhole camera framing it, one distant light. This helper does the
// `GpuScene::CreateMesh / AppendInstance / SetCamera / AddLight`
// dance so HeadlessMode + ViewerMode don't duplicate the geometry +
// camera math.
//
// M3.5 + M4 replace this with the real USD-loaded scene; until then
// the cube is what the renderer renders with no input.

#pragma once

#include <Pyxis/Renderer/Forward.h>

#include <cstdint>
#include <expected>
#include <string>

namespace pyxis {
class GpuScene;
}

namespace pyxis::app {

// Populate the supplied GpuScene with the M3 hardcoded cube fixture.
// `renderWidth` / `renderHeight` drive the camera aspect ratio so
// the cube isn't stretched at non-square aspect ratios.
[[nodiscard]] std::expected<void, std::string> BuildHardcodedCubeScene(
    GpuScene& scene, std::uint32_t renderWidth, std::uint32_t renderHeight) noexcept;

}  // namespace pyxis::app
