// Pyxis app — FlyCameraController implementation.

#include "Camera/FlyCameraController.h"

#include <Pyxis/Platform/Window/InputEvent.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pyxis::app {

namespace {

// GLFW key codes hard-coded per the §30.3 rule: pyxis_app must not
// pull <GLFW/glfw3.h> (glfw is PRIVATE-linked from pyxis_platform).
// Codes have been stable since GLFW 3.0 and match USB HID / ASCII
// for printable keys.
constexpr int KEY_W = 87;
constexpr int KEY_A = 65;
constexpr int KEY_S = 83;
constexpr int KEY_D = 68;
constexpr int KEY_Z = 90;        // AZERTY: Z is where W is on QWERTY.
constexpr int KEY_Q = 81;        // AZERTY: Q is where A is on QWERTY.
constexpr int KEY_SPACE = 32;    // World up.
constexpr int KEY_LSHIFT = 340;  // World down.

constexpr int MOUSE_BUTTON_LEFT = 0;

// Pitch clamp — short of ±π/2 to avoid the basis vectors collapsing
// onto each other (camera "up" becomes parallel to "forward" at
// exactly ±90°, breaking the worldFromCamera 3x3 orthonormality).
constexpr float PITCH_LIMIT = std::numbers::pi_v<float> * 0.5f - 0.01f;

uint8_t Bit(FlyCameraController::InputDirection direction) noexcept {
  return static_cast<uint8_t>(direction);
}

// Extract yaw (CW from above) and pitch (positive = look up) from
// the rotation portion of an authored worldFromCamera matrix. The
// inverse of the BuildWorldFromCamera() construction below — used
// once at seed time so the controller picks up where the scene's
// camera was pointing.
//
// The 3x3 rotation block of T(p) * RotateY(-yaw) * RotateX(pitch) is:
//   [  cosYaw   -sinYaw*sinPitch   -sinYaw*cosPitch ]
//   [   0        cosPitch           -sinPitch       ]
//   [  sinYaw    cosYaw*sinPitch    cosYaw*cosPitch ]
// so we recover sin/cos by reading the corresponding entries.
void ExtractYawPitch(const hlslpp::float4x4& worldFromCamera, float& outYaw,
                     float& outPitch) noexcept {
  // hlslpp::store(dst, matrix) writes row-major: storage[row*4 + col].
  float storage[16];
  hlslpp::store(storage, worldFromCamera);
  const float row1col2 = storage[1 * 4 + 2];  // -sinPitch
  const float row0col0 = storage[0 * 4 + 0];  // cosYaw
  const float row2col0 = storage[2 * 4 + 0];  // sinYaw

  outPitch = std::asin(std::clamp(-row1col2, -1.0f, 1.0f));
  outYaw = std::atan2(row2col0, row0col0);
}

}  // namespace

void FlyCameraController::HandleEvent(const InputEvent& event) noexcept {
  switch (event.kind)
  {
    case InputEventKind::KeyDown:
    case InputEventKind::KeyUp: {
      const bool down = (event.kind == InputEventKind::KeyDown);
      const auto setBit = [&](InputDirection direction) {
        if (down)
          _heldKeys |= Bit(direction);
        else
          _heldKeys &= ~Bit(direction);
      };
      switch (event.key)
      {
        case KEY_W:
        case KEY_Z: setBit(InputDirection::Forward); break;
        case KEY_S: setBit(InputDirection::Back); break;
        case KEY_A:
        case KEY_Q: setBit(InputDirection::Left); break;
        case KEY_D: setBit(InputDirection::Right); break;
        case KEY_SPACE: setBit(InputDirection::Up); break;
        case KEY_LSHIFT: setBit(InputDirection::Down); break;
        default: break;
      }
      break;
    }
    case InputEventKind::MouseButtonDown:
      if (event.key == MOUSE_BUTTON_LEFT)
      {
        _lookActive = true;
        _hasLastCursor = false;  // Drop stale delta on press.
      }
      break;
    case InputEventKind::MouseButtonUp:
      if (event.key == MOUSE_BUTTON_LEFT)
      {
        _lookActive = false;
        _hasLastCursor = false;
      }
      break;
    case InputEventKind::MouseMove:
      if (_lookActive)
      {
        if (_hasLastCursor)
        {
          const auto mouseDx = static_cast<float>(event.mouseX - _lastCursorX);
          const auto mouseDy = static_cast<float>(event.mouseY - _lastCursorY);
          // Mouse-right (mouseDx > 0) → camera turns right (yaw +).
          // Mouse-down (mouseDy > 0) → camera looks down (pitch -).
          _yaw += mouseDx * _lookSensitivity;
          _pitch -= mouseDy * _lookSensitivity;
          _pitch = std::clamp(_pitch, -PITCH_LIMIT, PITCH_LIMIT);
        }
        _lastCursorX = event.mouseX;
        _lastCursorY = event.mouseY;
        _hasLastCursor = true;
      }
      break;
    default:
      break;
  }
}

void FlyCameraController::SeedFromScene(GpuScene& scene) noexcept {
  if (_seeded || !scene.HasCamera())
    return;
  SnapToCamera(scene.GetCamera());
}

void FlyCameraController::SnapToCamera(const CameraDesc& camera) noexcept {
  // viewFromWorld is the inverse of worldFromCamera. For pose-
  // extraction purposes we want worldFromCamera; invert the authored
  // viewFromWorld. hlslpp's float4x4 has an inverse() free function.
  const hlslpp::float4x4 worldFromCamera = hlslpp::inverse(camera.viewFromWorld);

  ExtractYawPitch(worldFromCamera, _yaw, _pitch);

  // Translation = last column of worldFromCamera (column-vector
  // convention, §10). hlslpp::store(dst, matrix) writes row-major.
  float storage[16];
  hlslpp::store(storage, worldFromCamera);
  _position = hlslpp::float3{storage[0 * 4 + 3], storage[1 * 4 + 3], storage[2 * 4 + 3]};

  // Mark seeded so subsequent SeedFromScene calls become no-ops —
  // the snap IS the seed for downstream Update() ticks.
  _seeded = true;
}

hlslpp::float4 FlyCameraController::OrientationQuat() const noexcept {
  // Quaternion = qYaw(-yaw, world Y) * qPitch(pitch, local X). yaw>0 =
  // look right (CW from above) so the rotation around +Y is by -yaw,
  // matching BuildWorldFromCamera's convention. Half-angles per the
  // standard quaternion construction. Composed in the order BWC
  // applies to a vector: pitch first (acts on the post-yaw camera),
  // then yaw — which means the quaternion product is qYaw * qPitch.
  const float halfYaw   = -_yaw  * 0.5f;
  const float halfPitch =  _pitch * 0.5f;
  const float cosY = std::cos(halfYaw);
  const float sinY = std::sin(halfYaw);
  const float cosP = std::cos(halfPitch);
  const float sinP = std::sin(halfPitch);
  // qYaw   = (0,    sinY, 0,    cosY)   axis +Y
  // qPitch = (sinP, 0,    0,    cosP)   axis +X
  // q = qYaw * qPitch (Hamilton product, x/y/z imaginary, w real).
  return hlslpp::float4{
      cosY * sinP,           // x
      sinY * cosP,           // y
      -sinY * sinP,          // z
      cosY * cosP};          // w
}

void FlyCameraController::Reset() noexcept {
  _position      = hlslpp::float3{0.0f, 0.0f, 0.0f};
  _yaw           = 0.0f;
  _pitch         = 0.0f;
  _heldKeys      = 0;
  _lookActive    = false;
  _hasLastCursor = false;
  _seeded        = false;
  // _moveSpeed + _lookSensitivity intentionally preserved — those
  // are user preferences that survive a scene swap.
}

void FlyCameraController::SetMoveSpeed(float metresPerSecond) noexcept {
  // Clamp into a sane range. Below ~0.05 m/s keys feel unresponsive;
  // above ~200 m/s the user overshoots scene bounds in one tap on
  // most hardware-typical mouse-DPI ranges.
  if (metresPerSecond < 0.05f)
    metresPerSecond = 0.05f;
  if (metresPerSecond > 200.0f)
    metresPerSecond = 200.0f;
  _moveSpeed = metresPerSecond;
}

hlslpp::float4x4 FlyCameraController::BuildWorldFromCamera() const noexcept {
  const float cosYaw = std::cos(_yaw);
  const float sinYaw = std::sin(_yaw);
  const float cosPitch = std::cos(_pitch);
  const float sinPitch = std::sin(_pitch);

  // T(_position) * RotateY(-_yaw) * RotateX(_pitch). yaw>0 = look
  // right (CW from above) so we apply -_yaw to the standard CCW
  // rotation matrix; pitch>0 = look up. Result in row-major +
  // column-vector form (§10): translation in last column.
  return hlslpp::float4x4(
      hlslpp::float4{cosYaw, -sinYaw * sinPitch, -sinYaw * cosPitch, _position.x},
      hlslpp::float4{0.0f, cosPitch, -sinPitch, _position.y},
      hlslpp::float4{sinYaw, cosYaw * sinPitch, cosYaw * cosPitch, _position.z},
      hlslpp::float4{0.0f, 0.0f, 0.0f, 1.0f});
}

hlslpp::float4x4 FlyCameraController::BuildViewFromWorld() const noexcept {
  // viewFromWorld is the inverse of worldFromCamera. For an
  // orthonormal-rotation + translation matrix, the inverse is the
  // transpose of the 3x3 rotation block plus a transformed
  // translation: viewFromWorld = R^T * translate(-position) which
  // in expanded form is R^T with translation column = -R^T *
  // position. hlslpp's inverse() handles arbitrary 4x4s and is
  // cheap on the per-frame path.
  return hlslpp::inverse(BuildWorldFromCamera());
}

void FlyCameraController::Update(float dtSeconds, GpuScene& scene) noexcept {
  SeedFromScene(scene);
  if (!_seeded)
    return;

  const float cosYaw = std::cos(_yaw);
  const float sinYaw = std::sin(_yaw);
  const float cosPitch = std::cos(_pitch);
  const float sinPitch = std::sin(_pitch);

  // Camera basis in world space (columns of worldFromCamera 3x3 — see
  // BuildWorldFromCamera() for derivation). Movement uses these
  // directly so W/S follow the camera's pitched forward; A/D follow
  // the world-horizontal right (no pitched roll on strafe — the
  // standard FPS feel).
  const hlslpp::float3 forward{sinYaw * cosPitch, sinPitch, -cosYaw * cosPitch};
  const hlslpp::float3 right{cosYaw, 0.0f, sinYaw};
  const hlslpp::float3 worldUp{0.0f, 1.0f, 0.0f};

  hlslpp::float3 move{0.0f, 0.0f, 0.0f};
  if (_heldKeys & Bit(InputDirection::Forward))
    move = move + forward;
  if (_heldKeys & Bit(InputDirection::Back))
    move = move - forward;
  if (_heldKeys & Bit(InputDirection::Right))
    move = move + right;
  if (_heldKeys & Bit(InputDirection::Left))
    move = move - right;
  if (_heldKeys & Bit(InputDirection::Up))
    move = move + worldUp;
  if (_heldKeys & Bit(InputDirection::Down))
    move = move - worldUp;

  // Normalise so diagonal movement isn't faster than axis-aligned;
  // hlslpp::length returns a float1 scalar.
  const float lengthSquared = static_cast<float>(hlslpp::dot(move, move));
  if (lengthSquared > 1.0e-6f)
  {
    move = move * (1.0f / std::sqrt(lengthSquared));
    _position = _position + move * (_moveSpeed * dtSeconds);
  }

  // Re-publish the camera with the new view matrix; preserve the
  // scene's authored projection + lens parameters so the M4-loaded
  // .usd's framing (focal length, fStop, etc.) survives.
  CameraDesc updated = scene.GetCamera();
  updated.viewFromWorld = BuildViewFromWorld();
  scene.SetCamera(updated);
}

}  // namespace pyxis::app
