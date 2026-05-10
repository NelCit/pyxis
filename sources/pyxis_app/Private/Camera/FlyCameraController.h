// Pyxis app — viewer-side fly camera controller.
//
// Plan §29.2 keyboard navigation. Owns yaw / pitch / world position
// driven by:
//   - WASD (QWERTY) and ZQSD (AZERTY) for forward / left / back / right
//   - Space / LShift for world-up / world-down
//   - LMB-drag for mouse-look (pitch clamped to ±89°)
//
// Lifetime: lives on the viewer's main thread next to the frame loop.
// HandleEvent runs in the GLFW callback context (same thread on
// Windows). Update reads/writes GpuScene's camera once per frame —
// the SetCamera call is the documented per-frame mutation surface
// (§18.5) and is safe from the render thread.
//
// Initial state seeds itself from GpuScene::GetCamera() the first
// time Update() is called: yaw / pitch are extracted from the
// authored worldFromCamera so the user's camera-look starts where
// the loaded scene's camera was pointing. Subsequent frames build a
// fresh worldFromCamera from accumulated yaw / pitch / position.

#pragma once

#include <Pyxis/Renderer/Descs/CameraDesc.h>

#include <hlsl++.h>

#include <cstdint>

namespace pyxis {
class GpuScene;
struct InputEvent;
}  // namespace pyxis

namespace pyxis::app {

class FlyCameraController final {
 public:
  FlyCameraController() = default;
  ~FlyCameraController() = default;

  FlyCameraController(const FlyCameraController&) = delete;
  FlyCameraController& operator=(const FlyCameraController&) = delete;

  // Forwarded by the viewer's window event sink. Tracks key state
  // (held / released) and mouse state (LMB held + last cursor pos).
  // Cheap — pure bookkeeping, no GpuScene access.
  void HandleEvent(const InputEvent& event) noexcept;

  // Per-frame: integrate held-key movement over `dtSeconds` and
  // accumulated mouse-look delta into yaw / pitch / position, then
  // call `scene.SetCamera(...)` with the updated viewFromWorld.
  // Preserves the existing scene camera's projection + lens
  // parameters (focalLengthMm, fStop, focus, near, far) — only the
  // view transform updates.
  void Update(float dtSeconds, GpuScene& scene) noexcept;

  // Snap to a specific authored camera. Re-seeds yaw / pitch /
  // position from the supplied desc so the controller's next Update
  // continues from there. Used by the editor's Scene-Camera combo
  // (and ViewerMode after a scene reload) to jump the FlyCam onto
  // an authored hero camera.
  void SnapToCamera(const CameraDesc& camera) noexcept;

  // Linear-translation speed (the WASD/ZQSD pace). Angular look
  // sensitivity is intentionally NOT exposed — mouse drag is in
  // pixels and stays at the §29.2 default.
  [[nodiscard]] float MoveSpeed() const noexcept { return _moveSpeed; }
  void SetMoveSpeed(float metresPerSecond) noexcept;

  // Drop the seeded-from-scene flag so the next Update re-seeds from
  // whatever camera the (possibly newly-loaded) scene authors. Used
  // by ViewerMode after a scene reload — without it the FlyCam
  // silently keeps the old scene's pose. Preserves move speed
  // (the user's slider value carries across reloads).
  void Reset() noexcept;

 private:
  // Public so the .cpp's anonymous-namespace Bit() helper can name
  // it — it's still an implementation detail (only the .cpp uses it).
 public:
  enum class InputDirection : uint8_t {
    Forward = 1u << 0,
    Back = 1u << 1,
    Left = 1u << 2,
    Right = 1u << 3,
    Up = 1u << 4,
    Down = 1u << 5,
  };

 private:
  void SeedFromScene(GpuScene& scene) noexcept;
  [[nodiscard]] hlslpp::float4x4 BuildWorldFromCamera() const noexcept;
  [[nodiscard]] hlslpp::float4x4 BuildViewFromWorld() const noexcept;

  // Pose. Yaw measured CW-from-above (FPS convention: yaw>0 = look
  // right); pitch positive = look up, clamped to ±π/2 - 0.01 to
  // dodge gimbal flip.
  hlslpp::float3 _position{0.0f, 0.0f, 0.0f};
  float _yaw = 0.0f;    // radians
  float _pitch = 0.0f;  // radians

  // Tunables — exposed as fields rather than constants so a future
  // §29.3 viewer panel can edit them at runtime without recompile.
  float _moveSpeed = 2.0f;          // metres per second
  float _lookSensitivity = 0.003f;  // radians per pixel of mouse delta

  // Held-key state. Bitmask layout matches the InputDirection enum
  // values above — a single uint8_t handles every meaningful combo.
  uint8_t _heldKeys = 0;

  // LMB-drag state.
  bool _lookActive = false;
  bool _hasLastCursor = false;
  double _lastCursorX = 0.0;
  double _lastCursorY = 0.0;

  // Lazy seeding — Update() seeds yaw/pitch/position from the
  // scene's camera the first time it sees a HasCamera()=true scene,
  // so the user's first frame matches what the loaded .usd specified.
  bool _seeded = false;
};

}  // namespace pyxis::app
