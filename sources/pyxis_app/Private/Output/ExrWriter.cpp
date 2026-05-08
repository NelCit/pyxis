// Pyxis app — EXR writer implementation.

#include "Output/ExrWriter.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <tinyexr.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pyxis::app {

namespace {

namespace fs = std::filesystem;

// BGRA8 -> RGBA float, normalising 0..255 to 0..1. The underlying
// surface is SBGRA8_UNORM (per VkDeviceManagerHeadless's
// _renderTarget); we treat the bytes as straight unorm and write them
// as linear float for now. M3+'s real colour-management work will
// promote to RGBA16F / OCIO once the path tracer is producing HDR
// values; for the M2 hardcoded triangle the shipped colours are the
// shader's interpolated vertex colours and look correct as-is.
void Bgra8ToRgbaFloat(uint32_t          width,
                      uint32_t          height,
                      const void*       bgra8Pixels,
                      std::size_t       rowPitchBytes,
                      std::vector<float>& outRgba) {
    outRgba.resize(static_cast<std::size_t>(width) * height * 4u);
    const auto* src = static_cast<const uint8_t*>(bgra8Pixels);
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* srcRow = src + (static_cast<std::size_t>(row) * rowPitchBytes);
        float*         dstRow = outRgba.data() + (static_cast<std::size_t>(row) * width * 4u);
        for (uint32_t col = 0; col < width; ++col) {
            const uint8_t blue  = srcRow[col * 4 + 0];
            const uint8_t green = srcRow[col * 4 + 1];
            const uint8_t red   = srcRow[col * 4 + 2];
            const uint8_t alpha = srcRow[col * 4 + 3];
            dstRow[col * 4 + 0] = static_cast<float>(red)   / 255.0f;
            dstRow[col * 4 + 1] = static_cast<float>(green) / 255.0f;
            dstRow[col * 4 + 2] = static_cast<float>(blue)  / 255.0f;
            dstRow[col * 4 + 3] = static_cast<float>(alpha) / 255.0f;
        }
    }
}

}  // namespace

std::expected<void, std::string>
WriteExrBgra8(std::string_view filePath,
              uint32_t          width,
              uint32_t          height,
              const void*       bgra8Pixels,
              std::size_t       rowPitchBytes) noexcept {
    if (filePath.empty())                     return std::unexpected{std::string{"WriteExrBgra8: empty path"}};
    if (bgra8Pixels == nullptr)               return std::unexpected{std::string{"WriteExrBgra8: null source"}};
    if (width == 0 || height == 0)            return std::unexpected{std::string{"WriteExrBgra8: zero dim"}};
    if (rowPitchBytes < static_cast<std::size_t>(width) * 4u) {
        return std::unexpected{std::string{"WriteExrBgra8: rowPitch < width*4"}};
    }

    const fs::path target{ std::string{filePath} };
    if (target.has_parent_path()) {
        std::error_code errorCode;
        fs::create_directories(target.parent_path(), errorCode);
        // Ignore errorCode — the directory may already exist and that's fine.
    }

    std::vector<float> rgba;
    Bgra8ToRgbaFloat(width, height, bgra8Pixels, rowPitchBytes, rgba);

    const std::string targetUtf8 = target.generic_string();
    const char* tinyExrError = nullptr;
    const int saveResult = SaveEXR(rgba.data(),
                                   static_cast<int>(width),
                                   static_cast<int>(height),
                                   /*components*/ 4,
                                   /*save_as_fp16*/ 0,
                                   targetUtf8.c_str(),
                                   &tinyExrError);
    if (saveResult != TINYEXR_SUCCESS) {
        std::string message = "SaveEXR(\"" + targetUtf8 + "\") failed: ";
        message += tinyExrError ? tinyExrError : "unknown";
        if (tinyExrError) FreeEXRErrorMessage(tinyExrError);
        return std::unexpected{message};
    }
    Logging::Get().Info(log::APP, "WriteExr: " + targetUtf8 +
                                  " (" + std::to_string(width) + "x" +
                                  std::to_string(height) + " RGBA32F)");
    return {};
}

}  // namespace pyxis::app
