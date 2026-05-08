// Pyxis renderer — FrameProfile snapshot.
//
// Plan §18.4. Opaque-by-design snapshot returned by
// PyxisRenderer::LastFrameProfile() / Profiler::LastFrameProfile().
// Inline-owning ScopeName POD so the snapshot is self-contained — it
// stays valid indefinitely, never points into Profiler-owned storage
// that rotates on the next EndFrame().

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
        uint8_t                    size = 0;
        [[nodiscard]] std::string_view View() const noexcept {
            return { data.data(), size };
        }
    };

    struct PassTiming {
        ScopeName name;          // e.g. "pass.Triangle", "render.commitResources"
        ScopeKind kind = ScopeKind::Cpu;
        double    durationMs = 0.0;
        uint32_t  depth      = 0;
    };

    // Borrowed view onto the Profiler's frame-timing ring. Valid until
    // the next Profiler::EndFrame(). Callers that need the data past
    // the next frame copy what they need.
    std::span<const PassTiming> passes;

    double   cpuFrameMs = 0.0;
    double   gpuFrameMs = 0.0;
    uint64_t frameIndex = 0;
};

}  // namespace pyxis
