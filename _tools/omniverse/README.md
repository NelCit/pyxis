# Pyxis × Omniverse Kit (RFC 0004)

Pyxis as a **Hydra render delegate** for NVIDIA Omniverse Kit (110.1.1 / nv-usd
25.11). All build artifacts live **under `<repo>/build/omniverse`** (gitignored)
— no sibling external folder. The only thing outside the repo is the Packman
**package cache** (`PM_PACKAGES_ROOT`, analogous to a vcpkg/npm cache).

**Can you build it from a clean clone? No** — and that's expected of every Kit
extension: the Kit SDK + nv-usd are multi-GB, license-gated, Packman-delivered,
and never vendored. These scripts acquire them non-interactively and build on top.

## Scripts (`_tools/omniverse/`)

| Script | What it does |
|---|---|
| `check.ps1` | Verify every prerequisite (clang-cl, cmake, ninja, GPU, Vulkan + external-memory, Kit SDK, nv-usd, Python 3.12, prebuilt renderer). Prints OK/MISSING + hints. |
| `setup.ps1` | One-time: clone kit-app-template, bootstrap Packman, pull nv-usd 25.11 + Python 3.12 into `build/omniverse`. Writes `usd-deps/paths.ps1`. |
| `build.ps1` | Configure + compile `pyxis_hydra_omni` against nv-usd; assemble + stage the `omni.hydra.pyxis` extension (delegate DLL + runtime DLLs + shaders) into the Kit app. |
| `run_smoke.ps1` | Headless end-to-end test: drives a USD stage through the delegate via HdEngine; verifies ingest + render + composite into the host buffer. |
| `deps/nv-usd.packman.xml` | Pins the nv-usd 25.11 package. |

## Build deliverable 1 — the extension

```powershell
cmake --build build/dev --config Release --target pyxis_renderer pyxis_platform  # prebuilt renderer
_tools/omniverse/setup.ps1     # one-time SDK acquisition (multi-GB)
_tools/omniverse/check.ps1     # confirm prerequisites
_tools/omniverse/build.ps1     # build + stage omni.hydra.pyxis
```

## Tests

- **gtest (`build/dev`, CI):** GPU interop suite (`GpuInteropRoundTrip`,
  `GpuInteropExport`, `GpuInteropImport` — 10 cases) + `PyxisRenderToExportedImage`
  (renderer → exportable image). Skip cleanly on CPU-only CI.
- **ctest (`build/omni`):** `HdEngineSmoke` — drives a USD stage (mesh + material
  + light + camera) through the delegate via `HdEngine` and asserts
  `meshCount/instanceCount/materialCount/lightCount ≥ 1`, the frame renders, and
  it **composites byte-identically into the host `HdRenderBuffer`**:
  ```
  ctest --test-dir build/omni --output-on-failure
  ```

## Status (verified on RTX 5000)

The delegate is a **complete, host-agnostic Hydra renderer**: it ingests
geometry/camera/material/light from a USD stage, renders via Pyxis, and presents
into the host's bound color AOV — all proven headlessly. It **loads + registers**
in a live Kit editor (`type discoverable = True`) and is **enumerated** alongside
Pixar Storm.

**Kit-viewport caveat (Kit 110):** Kit's viewport renderer menu can't *select* an
external USD Hydra delegate — `createViewport` needs a Kit engine registered via
`registerHydraEngineFactory(IHydraEngineFactory)`, and `IHydraEngineFactory` is
forward-declared only (NVIDIA-internal). So selecting "Pyxis" yields
`unable to find suitable engine for config`. Paths to pixels in a Kit window:
(A) a custom `omni.ui` panel that drives `HdEngine` + the delegate (the host-facing
render path is done — see `HdEngineSmoke`); (B) the closed engine-factory (needs
NVIDIA internal SDK). usdview / the decoupled file workflow work today.

## Deliverable 2 — packaged editor app

The `pyxis.editor` Kit app (kit_base_editor + the extension) builds + launches;
package via `repo.bat package` in `build/omniverse/kit-app-template`. See
RFC 0004 for the full design + findings.
