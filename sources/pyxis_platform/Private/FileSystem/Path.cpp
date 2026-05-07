// Pyxis platform — Path implementation.

#include <Pyxis/Platform/FileSystem/Path.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace pyxis {

namespace fs = std::filesystem;

Path::Path(std::string_view utf8) {
    Assign(utf8);
}

void Path::Assign(std::string_view utf8) noexcept {
    const std::size_t n = (utf8.size() < CAPACITY - 1) ? utf8.size() : CAPACITY - 1;
    std::memcpy(_buf.data(), utf8.data(), n);
    _buf[n] = '\0';
    _size   = static_cast<uint16_t>(n);
}

std::string_view Path::View() const noexcept {
    return { _buf.data(), _size };
}

bool Path::Exists() const noexcept {
    if (_size == 0) return false;
    std::error_code ec;
    return fs::exists(fs::path(View()), ec);
}

bool Path::IsDirectory() const noexcept {
    if (_size == 0) return false;
    std::error_code ec;
    return fs::is_directory(fs::path(View()), ec);
}

bool Path::IsAbsolute() const noexcept {
    if (_size == 0) return false;
    return fs::path(View()).is_absolute();
}

Path Path::Join(std::string_view child) const noexcept {
    const fs::path p = fs::path(View()) / fs::path(child);
    Path out;
    out.Assign(p.generic_string());
    return out;
}

Path Path::Parent() const noexcept {
    const fs::path p = fs::path(View()).parent_path();
    Path out;
    out.Assign(p.generic_string());
    return out;
}

bool Path::EnsureDirectoryExists() const noexcept {
    if (_size == 0) return false;
    std::error_code ec;
    fs::create_directories(fs::path(View()), ec);
    return !ec;
}

}  // namespace pyxis
