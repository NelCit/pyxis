# Commands (Windows / PowerShell)

Configure + build (from repo root):
- `cmake --preset dev` — configure (clang-cl + vcpkg; first run populates vcpkg, slow)
- `cmake --build --preset dev` — Debug-ish dev build; `--preset dev-release` for Release
- `cmake --build --preset ci` — CI config (no viewer)

Test:
- `ctest --preset dev` — unit tests + regression suite
- Direct: `build\dev\<config>\pyxis_unit_tests.exe`
- Regression (image diff) spawns `pyxis.exe --headless --config <json>` and compares EXRs; deterministic only within the pinned §33.7 matrix (RTX 4080, pinned driver), else RMSE tolerance.

Run:
- Viewer: `build\dev\<config>\pyxis.exe`
- Headless: `pyxis.exe --headless --config <cfg.json>` (seed must be non-zero; writes EXR)

Hygiene:
- clang-format via repo `.clang-format` (format before commit); `.clang-tidy` is single source of truth for checks (clangd reads it natively)
- Public API changes: run `_tools\check_exports.py` (dumpbin golden diff)

Windows shell notes: PowerShell 5.1 default (`&&` unsupported — use `;`); Git Bash available; `rtk <cmd>` proxies/compresses common commands (auto-hook on Bash tool).
