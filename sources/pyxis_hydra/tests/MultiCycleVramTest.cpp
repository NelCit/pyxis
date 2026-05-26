// RFC 0004 — headless proof that the Pyxis delegate fully releases its Vulkan
// device + GpuScene VRAM when destroyed, so that switching renderers in Kit
// (which destroys/recreates the pxr-engine delegate) doesn't leak VRAM across
// switches. This is the prerequisite for "exclusive renderer activation": when
// Kit tears our delegate down, the GPU memory must come back.
//
// It creates a PyxisHydraHost (delegate + PyxisEngine + own Vulkan device),
// loads a (heavy) USD stage, renders, then destroys everything — repeated N
// times. If teardown leaked the device or its VRAM, the 2nd/3rd cycle would fail
// to create a device or OOM on texture upload. Exit 0 = clean release each cycle.
//
//   MultiCycleVramTest <scene.usd> [cycles=3]

#include "PyxisEngine.h"
#include "PyxisHydraHost.h"

#include <pxr/usd/usd/stage.h>

#include <cstdio>
#include <cstdlib>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: MultiCycleVramTest <scene.usd> [cycles]\n");
        return 2;
    }
    // This test validates that delegate teardown RELEASES the Vulkan device +
    // VRAM each cycle. RFC 0006's persistent-engine feature deliberately KEEPS
    // the engine resident across delegate destruction (the pxr switch-back
    // optimisation), which would defeat the per-cycle release this test proves.
    // Force persistence OFF so the owned-engine teardown path is exercised.
#if defined(_WIN32)
    _putenv("PYXIS_OMNI_NO_PERSIST=1");
#else
    setenv("PYXIS_OMNI_NO_PERSIST", "1", 1);
#endif
    const int cycles = (argc > 2) ? std::atoi(argv[2]) : 3;

    UsdStageRefPtr stage = UsdStage::Open(argv[1]);
    if (!stage)
    {
        std::printf("FAIL: could not open stage %s\n", argv[1]);
        return 3;
    }

    for (int i = 1; i <= cycles; ++i)
    {
        std::printf("=== cycle %d / %d ===\n", i, cycles);
        // Scoped so the host (and thus PyxisEngine + Vulkan device) is fully
        // destroyed before the next cycle begins.
        PyxisHydraHost host(1280, 720);
        if (host.Engine() == nullptr || !host.IsValid())
        {
            std::printf("FAIL cycle %d: host/engine did not initialise "
                        "(prior cycle leaked the device/VRAM?)\n",
                        i);
            return 4;
        }
        std::printf("cycle %d: host valid, calling SetStage...\n", i);
        std::fflush(stdout);
        host.SetStage(stage);
        // Render many frames on the SAME engine to mimic Kit's continuous render
        // and exercise the profiler timer-query recycling (BeginFrame/EndFrame).
        // A query leak would exhaust the NVRHI pool ("Insufficient query pool
        // space") within a few hundred frames.
        constexpr int kFramesPerCycle = 250;
        for (int f = 0; f < kFramesPerCycle; ++f)
        {
            if (!host.Render())
            {
                std::printf("FAIL cycle %d: Render() returned false at frame %d\n", i, f);
                return 5;
            }
        }
        std::printf("cycle %d: %d frames rendered\n", i, kFramesPerCycle);
        std::fflush(stdout);
        std::printf("cycle %d: instances=%llu meshes=%llu materials=%llu — OK\n", i,
                    static_cast<unsigned long long>(host.Engine()->LastInstanceCount()),
                    static_cast<unsigned long long>(host.Engine()->LastMeshCount()),
                    static_cast<unsigned long long>(host.Engine()->LastMaterialCount()));
    }

    std::printf("PASS: %d create/render/destroy cycles with no VRAM leak "
                "(device released cleanly each time)\n",
                cycles);
    return 0;
}
