# Building the Pyxis Omniverse integration (RFC 0004)

**Can you build the deliverables just by cloning this repo? No — and that's
expected.** Both deliverables depend on the **NVIDIA Omniverse Kit SDK + nv-usd
25.11**, which are multi-GB, license-gated, and distributed via NVIDIA's
**Packman** feed. No Kit extension ever vendors the SDK in-repo. These scripts
acquire it non-interactively and build on top.

What lives in *this* repo: the delegate sources
(`sources/pyxis_hydra_omni/`), the Pyxis-side GPU-interop (`pyxis_platform`),
the nv-usd version pin (`deps/nv-usd.packman.xml`), and these scripts.

---

## Deliverable 1 — `omni.hydra.pyxis` (the Hydra render-delegate extension)

Two commands, run once-per-machine for setup then per-build:

```powershell
# 1. Acquire the SDK (multi-GB, one-time). Clones kit-app-template, bootstraps
#    Packman, pulls nv-usd 25.11 + Python 3.12, writes usd-deps/paths.ps1.
_tools/omniverse/setup.ps1            # -ExternalDir D:\pyxis_external (default)

# 2. Build the delegate against nv-usd and stage it into the Kit extension.
_tools/omniverse/build.ps1            # -Config Release (default)
```

`build.ps1` produces `build/omni/pyxis_hydra_omni.dll` + `resources/plugInfo.json`
and stages them into
`<ExternalDir>/kit-app-template/source/extensions/omni.hydra.pyxis/bin/`.

**Verified:** the delegate compiles + links against nv-usd 25.11 with clang-cl,
and `build.ps1` runs clean from a wiped `build/omni`. (See RFC 0004
"Implementation status".)

### Remaining wiring for Kit to *load* it (C2)

A Hydra delegate DLL must be on `PXR_PLUGINPATH_NAME` for Kit/usdview to discover
it. The extension advertises its plugin dir from its `omni::ext::IExt` startup
(or via `extension.toml` env). Add to `omni.hydra.pyxis`:

- in the C++ `IExt::onStartup`: prepend `<ext>/bin` to `PXR_PLUGINPATH_NAME`
  (or call `PlugRegistry::GetInstance().RegisterPlugins(<ext>/bin)`), so the
  staged `bin/resources/plugInfo.json` is found;
- then "Pyxis" appears in the viewport's renderer (Hydra engine) list.

This is the C2 step in RFC 0004's phase plan; the DLL + plugInfo it needs are
already produced + staged by `build.ps1`.

---

## Deliverable 2 — a packaged Omniverse editor with Pyxis built in

A turnkey "Pyxis Viewer" = the `kit_base_editor` app + the `omni.hydra.pyxis`
extension, packaged into a distributable. The base editor is a Kit *app*
template (the dev host); we embed Pyxis as its renderer and package it.

```powershell
# In <ExternalDir>\kit-app-template :
repo.bat template new          # Application -> kit_base_editor, name: pyxis.viewer
#   (interactive wizard; for CI use `repo.bat template new --generate-playback
#    <file>` once, then `repo.bat template replay <file>`)

# Add the renderer extension as a dependency of the app + set it default:
#   - source/apps/pyxis.viewer.kit  ->  [dependencies] "omni.hydra.pyxis" = {}
#   - set the default Hydra engine / renderer to "Pyxis"

_tools/omniverse/build.ps1     # (re)build + stage the delegate into the ext
repo.bat build --config release
repo.bat package               # produces a distributable package of the editor
```

The app template creation is the one interactive step Kit doesn't expose a
non-interactive flag for (only `--generate-playback`/`replay`). Everything
downstream — building the delegate, staging it, building + packaging the app —
is scriptable. A committed playback file can remove that last manual step for CI.

---

## Version pins

| Component | Pin | Where |
|---|---|---|
| Kit SDK | 110.1.1 | `setup.ps1` (`kit-app-template` tag) |
| nv-usd | `usd.py312.windows-x86_64.stock.release` `0.25.11.kit.2-gl.19811` | `deps/nv-usd.packman.xml` |
| Python | 3.12.x (Kit-bundled) | located by `setup.ps1` |

The full Kit `dev/all-deps.packman.xml` cannot be pulled wholesale (some pins,
e.g. `abseil`, are on internal-only feeds). A Hydra delegate only needs the USD
package, which *is* on the public feed — hence the single-dependency pull.
