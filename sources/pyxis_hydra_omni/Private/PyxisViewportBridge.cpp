// RFC 0004 Option A — C++23 implementation of the panel boundary. See
// PyxisViewportBridge.h. This TU includes the Pyxis render core (via
// PyxisHydraHost) + nv-usd; it must NOT include carb / omni.ui headers.

#include "PyxisViewportBridge.h"

#include "PyxisEngine.h"
#include "PyxisHydraHost.h"

#include <Pyxis/Platform/Interop/GpuInteropExporter.h>

#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdUtils/stageCache.h>

#include <cstring>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace pyxis_omni {

namespace {

float HalfToFloat(uint16_t half)
{
    const uint32_t sign = (half >> 15) & 1u, exp = (half >> 10) & 0x1Fu, mant = half & 0x3FFu;
    uint32_t bits;
    if (exp == 0)
    {
        bits = mant ? 0u : (sign << 31);
    }
    else if (exp == 0x1F)
    {
        bits = (sign << 31) | (0xFFu << 23) | (mant << 13);
    }
    else
    {
        bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

// First UsdGeomCamera in the stage (Kit authors persp/top/etc. cameras), so the
// view is framed even before we sync the active viewport camera.
SdfPath FindFirstCamera(const UsdStageRefPtr& stage)
{
    for (const UsdPrim& prim : stage->Traverse())
    {
        if (prim.IsA<UsdGeomCamera>())
        {
            return prim.GetPath();
        }
    }
    return SdfPath();
}

class PyxisViewportBridgeImpl final : public PyxisViewportBridge
{
public:
    explicit PyxisViewportBridgeImpl(uint32_t width, uint32_t height)
        : _host(std::make_unique<PyxisHydraHost>(width, height))
    {
    }

    bool IsValid() const { return _host && _host->IsValid(); }

    bool RenderCurrentStage(const char* cameraPath) override
    {
        if (!IsValid())
        {
            return false;
        }

        // Kit publishes its edited stage into USD's global stage cache; pick the
        // most-recently-inserted (last) entry.
        const std::vector<UsdStageRefPtr> stages = UsdUtilsStageCache::Get().GetAllStages();
        UsdStageRefPtr stage;
        for (auto it = stages.rbegin(); it != stages.rend(); ++it)
        {
            if (*it)
            {
                stage = *it;
                break;
            }
        }
        if (!stage)
        {
            return false;  // no live stage yet.
        }

        if (stage != _lastStage)
        {
            _host->SetStage(stage);
            _lastStage = stage;
        }

        SdfPath camera;
        if (cameraPath && cameraPath[0] != '\0')
        {
            camera = SdfPath(cameraPath);
        }
        else
        {
            camera = FindFirstCamera(stage);
        }

        if (!_host->Render(UsdTimeCode::Default(), camera))
        {
            return false;
        }

        CopyAovToRgba8();
        return true;
    }

    const uint8_t* PixelsRgba8() const override
    {
        return _rgba8.empty() ? nullptr : _rgba8.data();
    }

    ViewportSharedTexture SharedTexture() const override
    {
        ViewportSharedTexture tex;
        if (!IsValid())
        {
            return tex;
        }
        const pyxis::ExportedImage& img = _host->Engine()->ExportedColor();
        const pyxis::ExportedSemaphore& sem = _host->Engine()->Timeline();
        tex.memoryHandle = img.memoryHandle;
        tex.semaphoreHandle = sem.handle;
        tex.allocationSize = img.allocationSize;
        tex.lastSignaledValue = _host->Engine()->LastSignaledValue();
        tex.width = img.width;
        tex.height = img.height;
        tex.vkFormat = img.vkFormat;
        // deviceUuid wired in Stage 4 (needs a PyxisEngine accessor).
        return tex;
    }

    uint32_t Width() const override { return _host ? _host->Width() : 0; }
    uint32_t Height() const override { return _host ? _host->Height() : 0; }

    uint64_t LastInstanceCount() const override
    {
        return IsValid() ? _host->Engine()->LastInstanceCount() : 0;
    }

private:
    // Composited RGBA16F AOV -> tonemapped-as-is RGBA8 (clamp 0..1), top-down.
    void CopyAovToRgba8()
    {
        HdRenderBuffer* buffer = _host->ColorBuffer();
        if (!buffer)
        {
            return;
        }
        const uint32_t w = _host->Width(), h = _host->Height();
        const auto* src = static_cast<const uint16_t*>(buffer->Map());
        if (!src)
        {
            return;
        }
        _rgba8.resize(size_t(w) * h * 4);
        const uint64_t total = uint64_t(w) * h;
        for (uint64_t p = 0; p < total; ++p)
        {
            for (int c = 0; c < 4; ++c)
            {
                float v = (c < 3) ? HalfToFloat(src[p * 4 + c]) : 1.0f;  // opaque alpha.
                v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
                _rgba8[p * 4 + c] = static_cast<uint8_t>(v * 255.f + 0.5f);
            }
        }
        buffer->Unmap();
    }

    std::unique_ptr<PyxisHydraHost> _host;
    UsdStageRefPtr _lastStage;
    std::vector<uint8_t> _rgba8;
};

} // namespace

PyxisViewportBridge* PyxisViewportBridge::Create(uint32_t width, uint32_t height)
{
    auto impl = std::make_unique<PyxisViewportBridgeImpl>(width, height);
    if (!impl->IsValid())
    {
        return nullptr;  // no GPU / interop.
    }
    return impl.release();
}

} // namespace pyxis_omni
