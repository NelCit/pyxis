// Pyxis platform — small path helper.
//
// std::filesystem is fine inside the platform implementation, but it doesn't
// cross DLL boundaries cleanly (locale + iterator stability under different
// SCL settings). This thin wrapper takes string_view inputs, returns owned
// PODs, and routes through std::filesystem internally.

#pragma once

#include <Pyxis/Platform/PlatformApi.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace pyxis {

class PYXIS_PLATFORM_API Path final {
public:
    static constexpr std::size_t CAPACITY = 1024;  // > MAX_PATH; tolerates long paths.

    Path() = default;
    explicit Path(std::string_view utf8);

    [[nodiscard]] std::string_view View()      const noexcept;
    [[nodiscard]] bool             Exists()    const noexcept;
    [[nodiscard]] bool             IsDirectory() const noexcept;
    [[nodiscard]] bool             IsAbsolute() const noexcept;

    // Replaces the path's contents from a UTF-8 view. Truncates to CAPACITY.
    void Assign(std::string_view utf8) noexcept;

    // Joins `child` onto the current path with the platform separator.
    [[nodiscard]] Path Join(std::string_view child) const noexcept;

    // Returns the parent directory (everything up to the last separator).
    // Returns an empty Path for a path with no separator.
    [[nodiscard]] Path Parent() const noexcept;

    // Ensures every directory along the path exists; no-op if already
    // present. Returns false on OS-level failure (logged once).
    [[nodiscard]] bool EnsureDirectoryExists() const noexcept;

private:
    std::array<char, CAPACITY> _buf{};
    uint16_t                    _size = 0;
};

}  // namespace pyxis
