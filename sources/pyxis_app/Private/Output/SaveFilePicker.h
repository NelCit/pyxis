// Pyxis app — shared Windows save-file picker.
//
// Wraps IFileSaveDialog (Shell COM) once; reused by:
//   • EditorPanel "Save current AOV..." button — EXR output (M7)
//   • Performance panel "Save profile JSON..." button — JSON output (M11)
//
// The dialog is modal; the call blocks until the user accepts /
// cancels. On non-Windows the function returns the empty string —
// pyxis_app is Windows-only per §5.c, but the no-op fallback keeps
// callers tidy in case viewer-mode ever ports.

#pragma once

#include <string>
#include <string_view>

namespace pyxis::app {

struct SaveFilePickerSpec {
  // Window title (wide string for the COM dialog).
  std::wstring_view title;
  // File-type filter label + glob pair (e.g. {L"OpenEXR (*.exr)", L"*.exr"}).
  std::wstring_view filterLabel;
  std::wstring_view filterGlob;
  // Default extension applied when the user types a bare filename.
  std::wstring_view defaultExtension;
  // Suggested filename pre-filled in the dialog (no path).
  std::wstring_view suggestedFileName;
};

// Modal save-file picker. Returns the UTF-8 path the user picked,
// empty on cancel / error / non-Windows.
[[nodiscard]] std::string SaveFilePickerDialog(const SaveFilePickerSpec& spec) noexcept;

}  // namespace pyxis::app
