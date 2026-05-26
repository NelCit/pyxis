# Pyxis × Omniverse Kit (RFC 0004 / RFC 0007)

Pyxis as a **Hydra render delegate** for NVIDIA Omniverse Kit (110.1.1 / nv-usd
25.11). **RFC 0007** retired the separate `pyxis_hydra_omni` C++ module: since
both `build/dev` and Kit are nv-usd 25.11, the SINGLE delegate `pyxis_hydra.dll`
(built in `build/dev`) loads directly in Kit, and the Kit extension carries no
compiled C++ (pure Python). The Kit SDK + nv-usd live **under
`<repo>/build/omniverse`** (gitignored). The only thing outside the repo is the
Packman **package cache** (`PM_PACKAGES_ROOT`, analogous to a vcpkg/npm cache).

**Can you build it from a clean clone? No** — and that's expected of every Kit
extension: the Kit SDK + nv-usd are multi-GB, license-gated, Packman-delivered,
and never vendored. These scripts acquire them non-interactively and build on top.

## Scripts (`_tools/omniverse/`)

| Script | What it does |
|---|---|
| `check.ps1` | Verify every prerequisite (clang-cl, cmake, ninja, GPU, Vulkan + external-memory, Kit SDK, nv-usd, Python 3.12, prebuilt renderer). Prints OK/MISSING + hints. |
| `setup.ps1` | One-time: clone kit-app-template, bootstrap Packman, pull nv-usd 25.11 + Python 3.12 into `build/omniverse`. Writes `usd-deps/paths.ps1`. |
| `build.ps1` | Build `pyxis_hydra` (delegate + headless tests) in `build/dev`; assemble + stage the `omni.hydra.pyxis` Python extension (delegate DLL + plugInfo + runtime DLLs + shaders) into the Kit app. |
| `run_smoke.ps1` / `run_lobby.ps1` / `run_parity.ps1` | Headless tests (exes in `build/dev/bin/Release`): HdEngine smoke, World Lobby render, and the §25.O.3 ingest-parity check. |
| `deps/nv-usd.packman.xml` | Pins the nv-usd 25.11 package. |

## Build deliverable 1 — the extension

```powershell
_tools/omniverse/setup.ps1     # one-time SDK acquisition (multi-GB)
_tools/omniverse/check.ps1     # confirm prerequisites
_tools/omniverse/build.ps1     # build pyxis_hydra (build/dev) + stage omni.hydra.pyxis
```

The extension carries **no compiled C++** (RFC 0007): it ships the prebuilt
delegate `pyxis_hydra.dll` (built in `build/dev`) + its `plugInfo.json` + the
Pyxis runtime DLLs, plus a pure-Python module (`omni/hydra/pyxis/__init__.py`)
that on startup (1) registers the delegate's `plugInfo` with USD's `PlugRegistry`
(`Plug.Registry().RegisterPlugins`, replacing the former native Carbonite IExt),
(2) switches `omni.hydra.pxr` to Pyxis via `set_hd_engine`, and (3) adds the
Pyxis section to the Render Settings window.

## Run / debug from VS Code

`.vscode/launch.json` has two configurations, each with a prebuild:

- **Omniverse: Pyxis Editor (prebuild + launch)** — builds the delegate
  (`pyxis_hydra.dll`, `build/dev`), stages the Python extension, then launches
  `kit.exe` on `pyxis.editor.kit`. `cppvsdbg` so you can breakpoint inside
  `pyxis_hydra.dll` once Kit loads it.
- **Omniverse: HdEngine smoke (debug delegate)** — prebuilds + launches the
  headless smoke harness (drives a USD stage through the delegate via HdEngine
  and renders Pyxis into the host AOV). Fast iteration with full breakpoints, no
  Kit. Sets the nv-usd-first `PATH` + `PXR_PLUGINPATH_NAME` the harness needs.

Prebuild tasks live in `.vscode/tasks.json` (`omni:prebuild`,
`omni:prebuild-smoke`, …) and can also be run standalone from the Command
Palette → *Tasks: Run Task*.

## Tests

- **gtest (`build/dev`, CI):** GPU interop suite (`GpuInteropRoundTrip`,
  `GpuInteropExport`, `GpuInteropImport` — 10 cases) + `PyxisRenderToExportedImage`
  (renderer → exportable image). Skip cleanly on CPU-only CI.
- **ctest (`build/dev`, GPU-only — label `requires_gpu`, excluded from CI):**
  `HdEngineSmoke` (drives a USD stage through the delegate via `HdEngine`;
  asserts ingest counts ≥ 1, the frame renders, and it composites into the host
  `HdRenderBuffer`) + the §25.O.3 ingest-parity suite (`Parity.WorldLobby` + 21
  golden fixtures, delegate vs `pyxis.exe --ingest usd_direct`):
  ```
  ctest --test-dir build/dev -C Release -L parity --output-on-failure
  ctest --test-dir build/dev -C Release -L requires_gpu --output-on-failure  # full GPU set
  ```

## Status (verified on RTX 5000)

The delegate is a **complete, host-agnostic Hydra renderer**: it ingests
geometry/camera/material/light from a USD stage, renders via Pyxis, and presents
into the host's bound color AOV — all proven headlessly. It **loads + registers**
in a live Kit editor (`type discoverable = True`) and is **enumerated** alongside
Pixar Storm.

**Kit-viewport path (RFC 0007):** Kit can't register an *external* Kit viewport
engine (`registerHydraEngineFactory(IHydraEngineFactory)` is NVIDIA-internal,
forward-declared only). Instead Pyxis rides **`omni.hydra.pxr`** — NVIDIA's
"Pixar Hydra" engine that hosts ANY registered USD `HdRendererPlugin` — exactly
as Aurora / V-Ray / Octane / Synopsys AVxcelerate do. The Python `__init__.py`
registers the delegate and uses `set_hd_engine("pxr", "HdPyxisRendererPlugin")`
to switch the live `pxr` viewport to Pyxis (the closed engine boots on Storm, so
a single second activation is required — see the module docstring). The earlier
RFC-0004 custom `omni.ui` viewport panel is **dropped** (RFC 0007): "select Pyxis
+ edit its render settings" is what `omni.hydra.pxr` already provides. usdview and
the decoupled headless workflow work today; the live viewport switch needs Kit
verification (see RFC 0007 open questions).

## Deliverable 2 — packaged editor app

The `pyxis.editor` Kit app (kit_base_editor + the extension) builds + launches;
package via `repo.bat package` in `build/omniverse/kit-app-template`. See
RFC 0004 for the full design + findings.
