---
name: dlss-streamline-integration
description: Pyxis's optional DLSS integration via NVIDIA Streamline — licensing boundaries, SDK staging, the denoiser/dlssExecMode settings surface, the DlssProvider probe, two-resolution pipeline architecture, and a debugging table for "why is effective=Builtin". Invoke for anything touching DLSS, Streamline, sl.interposer, NGX, Ray Reconstruction (DLSS-D/RR), Super Resolution (DLSS-SR), the denoiser render setting, PYXIS_DLSS_PATH, _tools/setup_dlss.py, or probe/downgrade log lines.
---

# DLSS / Streamline integration (Pyxis)

Authoritative design + status: `_documentation/rtx-realtime-alignment-design.md`
→ "DLSS — corrected stance", "DLSS scope includes upscaling", and
"DLSS (optional) — implementation status". Read those before changing
anything here.

## Reference routing (NVIDIA's own docs — trust these over blog posts)

Once the SDK is staged (`_local/dlss/sdk/`), the bundled guides are the
integration bible; read the relevant one BEFORE editing integration code:

| Topic | Read |
|---|---|
| slInit, manual hooking, resource tagging, per-frame Constants, threading | `_local/dlss/sdk/docs/ProgrammingGuide.md` |
| Super Resolution: optimal settings, jitter, mvec conventions, quality modes | `_local/dlss/sdk/docs/ProgrammingGuideDLSS.md` |
| Ray Reconstruction: guide-buffer contract, denoiser replacement semantics | `_local/dlss/sdk/docs/ProgrammingGuideDLSS_RR.md` |
| Debugging: development DLLs, sl.imgui overlay, logging | `_local/dlss/sdk/docs/Debugging.md` (if present in the staged version) |

No public agent skill exists for engine-side Streamline integration
(verified 2026-07: Streamline/DLSS/ovrtx repos and GitHub search — NVIDIA's
ovrtx `skills/` covers only their Python API, where DLSS is renderer-
internal). This skill + those guides are the reference set.

## Licensing boundaries (non-negotiable)

- **Streamline SOURCE (headers, interposer source, plugin manager) is MIT**
  — vendorable into the repo (`thirdparty/streamline/` with its MIT license
  text alongside).
- **The prebuilt binaries are proprietary** (NVIDIA RTX SDKs License):
  `sl.interposer.dll`, `sl.common.dll`, `sl.dlss*.dll`, `nvngx_dlss*.dll`.
  NEVER commit them. They live in the untracked `_local/dlss/` (gitignored)
  and are staged next to `pyxis.exe` (build dirs are untracked) or pointed
  at via the `PYXIS_DLSS_PATH` env var.
- An Apache-2.0 app USING DLSS as an optional runtime component is fine;
  relicensing/absorbing SDK materials or shipping the SDK standalone is not.
- Always keep the builtin fallback (ReLAX/SIGMA chain + TAA) working — DLSS
  must degrade gracefully on any machine, and headless/golden determinism
  (§33.7) pins the Builtin path.

## Getting the SDK

`_tools/setup_dlss.py --accept-nvidia-license` (prints the NVIDIA license
notice first). If its URL guess goes stale, the working manual recipe:

```powershell
gh api repos/NVIDIAGameWorks/Streamline/releases --jq '.[0].tag_name'
gh release download <tag> -R NVIDIAGameWorks/Streamline -p "streamline-sdk-*.zip" -D _local\dlss
Expand-Archive _local\dlss\streamline-sdk-*.zip -DestinationPath _local\dlss\sdk
```

SDK layout that matters: `include/` (MIT headers: sl.h, sl_dlss.h,
sl_dlss_d.h, sl_consts.h), `bin/x64/` (release DLL set — use these, not
`bin/x64/development/`, unless you want the debug overlay/validation),
`docs/ProgrammingGuide*.md` (the integration bible — init, manual hooking,
resource tagging, per-feature requirements; trust these over blog posts).
`nvngx_dlss.dll` = Super Resolution snippet, `nvngx_dlssd.dll` = Ray
Reconstruction snippet, loaded by `sl.dlss.dll` / `sl.dlss_d.dll`.

## Pyxis surface

- **Settings** (`RenderSettings::RealTimeQuality`): `denoiser` —
  `DENOISER_DLSS` (0, DEFAULT) / `DENOISER_BUILTIN` (1) / `DENOISER_OFF`
  (2); `dlssExecMode` — Auto/Quality/Balanced/Performance/DLAA (mirrors
  `omni:rtx:post:dlss:execMode`). JSON accepts strings
  (`"dlss"|"builtin"|"off"`, `"auto"|...|"dlaa"`) or ints; unknown strings
  are hard errors (Configuration.cpp `ReadDenoiserField`).
- **Probe**: `Private/Dlss/DlssProvider.{h,cpp}` — DLL discovery
  (`PYXIS_DLSS_PATH` → exe dir) + symbol resolution + (Stage 2) slInit /
  feature support. Public status POD: `Descs/DlssStatus.h` via
  `PyxisRenderer::GetDlssStatus()`; EditorPanel shows requested/effective +
  reason.
- **Resolution & gating**: `PyxisRenderer::RenderFrame` resolves
  `{requested, effective}` per frame, logs on change
  (`denoiser: requested=Dlss effective=Builtin (reason: ...)`), and masks
  `PASS_MASK_DENOISE|PASS_MASK_TAA` out of the settings copy in
  `PassContext` when effective ≠ Builtin. The denoiser/TAA passes self-gate
  on those bits — never gate them by editing pass files.
- **Two-resolution pipeline** (Stage 2): G-buffer + signal passes +
  denoise + composite at `renderResolution` (from
  `slDLSSGetOptimalSettings(displayRes, execMode)`); `DlssPass` (between
  Composite and AutoExposure) evaluates SR/RR to `displayResolution`;
  AutoExposure/Tonemap/Blit run at display res. Jitter (the §12.4/TAA
  sequence) MUST be active when DLSS is — an unjittered DLSS input
  produces a soft, shimmering image.
- **Ray Reconstruction guide buffers** map 1:1 to Phase-A targets:
  diffuse albedo = `gAlbedo`, normal+roughness = `gNormalRoughness`,
  motion vectors = `gMotionVector` (check units: sl expects pixel-space
  unless `mvecScale` says otherwise), depth = `gViewZ`, specular hit
  distance = `gReflections.a`. RR replaces the builtin chain (raw signals
  in); SR-only mode keeps the builtin chain at render res.

## Debugging: "why is effective=Builtin?"

| Log reason / symptom | Cause | Fix |
|---|---|---|
| `sl.interposer.dll not found (checked ...)` | DLLs not staged | copy `_local/dlss/sdk/bin/x64/*.dll` next to pyxis.exe or set `PYXIS_DLSS_PATH` |
| symbol resolution failed | wrong/old SDK zip, dev-vs-release mismatch | re-stage a matching complete DLL set from one SDK version |
| slInit failed | manual-hooking preferences wrong, missing Vulkan extensions/features at device creation | check `slGetFeatureRequirements` list vs `VkDeviceManager` extension set |
| feature unsupported | `nvngx_dlss*.dll` missing next to `sl.dlss*.dll`, non-RTX GPU, old driver | stage the nvngx snippets too; driver 535+; RTX GPU |
| effective=Dlss but image black/garbage | resource states/layouts around `slEvaluateFeature`, wrong tag extents (renderRes vs displayRes) | match the ProgrammingGuide's state table; tag color-in at renderRes, color-out at displayRes |
| ghosting/smearing | motion-vector scale or sign mismatch | verify `mvecScale` against gMotionVector's pixel-space convention |
| soft/shimmering output | jitter not applied or jitter offsets not passed in `sl::Constants` | enable the TAA jitter path when DLSS active; pass per-frame offsets |
| goldens flaky with DLSS on | DLSS is not byte-deterministic across drivers | goldens/headless determinism pin Builtin — never bake goldens with DLSS |

## House rules

- Ownership: one `DlssProvider` (constructor-injected, no singleton §30.10);
  all Streamline calls behind it — passes never include sl headers.
- The Streamline logging callback is process-global; install once.
- Fallback verification is part of any DLSS change: rename
  `sl.interposer.dll`, render, expect a clean `effective=Builtin` line, no
  crash, restore.
