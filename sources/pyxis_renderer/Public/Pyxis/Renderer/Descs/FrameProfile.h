// Pyxis renderer — FrameProfile snapshot.
//
// Plan §18.4. Snapshot of the most recently drained per-frame slot,
// returned by PyxisRenderer::LastFrameProfile() /
// Profiler::LastFrameProfile(). Pass *names* are inline-owning
// (`ScopeName`) so each PassTiming entry stays meaningful even if
// the consumer copies it elsewhere. The `passes` field itself is a
// `std::span<const PassTiming>` pointing into Profiler-owned
// storage stable until the next BeginFrame() — see the §18.9
// carve-out and the field comment below for the lifetime rule.

#pragma once

#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace pyxis {

struct FrameProfile {
  enum class ScopeKind : uint8_t { Cpu, Gpu };

  // Inline owning name buffer — same ABI rationale as ErrorMessage
  // (§18.3, §18.9): crosses DLL boundaries safely without an STL
  // container persisted past the call.
  struct ScopeName {
    static constexpr std::size_t CAPACITY = 56;
    std::array<char, CAPACITY> data{};
    uint8_t size = 0;
    [[nodiscard]] std::string_view View() const noexcept { return {data.data(), size}; }
  };

  struct PassTiming {
    ScopeName name;  // e.g. "pass.PathTrace", "render.frame.cpu"
    ScopeKind kind = ScopeKind::Cpu;
    double durationMs = 0.0;
    uint32_t depth = 0;
  };

  // Borrowed view onto the Profiler's just-drained slot. Valid until
  // the next Profiler::BeginFrame(), where the next slot rotation
  // moves a different vector into Impl::lastFrame. Callers that need
  // the data past that boundary copy what they need.
  //
  // frameIndex below identifies the frame that produced these passes
  // (i.e. the slot we just drained), NOT the live frame the consumer
  // is in. With FIF=1 + SLOT_COUNT=4 that's typically 3 frames behind
  // the live counter — fine for human-paced FPS readouts.
  std::span<const PassTiming> passes;

  double cpuFrameMs = 0.0;
  double gpuFrameMs = 0.0;
  uint64_t frameIndex = 0;
};

}  // namespace pyxis
