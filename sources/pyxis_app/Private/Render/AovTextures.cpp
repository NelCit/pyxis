// Pyxis app — caller-side AOV texture ownership.

#include "Render/AovTextures.h"

namespace pyxis::app {

std::expected<AovTextures, std::string>
AovTextures::Create(nvrhi::IDevice* device, uint32_t width, uint32_t height) noexcept {
    if (device == nullptr) {
        return std::unexpected{std::string{"AovTextures::Create: null device"}};
    }
    if (width == 0 || height == 0) {
        return std::unexpected{std::string{"AovTextures::Create: zero dim"}};
    }

    // initialState=RenderTarget + keepInitialState=true matches what the
    // viewer swapchain wrap and the previous headless offscreen RT used,
    // so NVRHI's tracker observes identical first-use transitions across
    // modes. Without keepInitialState NVRHI would assume the texture
    // starts in Common state and inject an extra barrier on first use,
    // which would invalidate the §33.7 byte-equal EXR contract.
    nvrhi::TextureDesc desc;
    desc.format            = nvrhi::Format::SBGRA8_UNORM;
    desc.width             = width;
    desc.height            = height;
    desc.dimension         = nvrhi::TextureDimension::Texture2D;
    desc.isRenderTarget    = true;
    desc.isUAV             = true;     // M3 PathTracePass writes via RWTexture2D<float4>.
    desc.debugName         = "aov.color";
    desc.initialState      = nvrhi::ResourceStates::RenderTarget;
    desc.keepInitialState  = true;

    AovTextures result;
    result.color = device->createTexture(desc);
    if (!result.color) {
        return std::unexpected{
            std::string{"AovTextures::Create: createTexture(aov.color) failed"}};
    }
    result.width  = width;
    result.height = height;
    return result;
}

}  // namespace pyxis::app
