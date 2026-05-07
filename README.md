# Pyxis

A C++23 real-time path tracer (NVRHI/Vulkan, Slang shaders, Windows-only v1)
inspired by Autodesk Aurora. Primary scene target: render the Disney Moana
Island USD scene end-to-end on a single Windows workstation.

> **Pyxis is a previewer, not a production renderer.** It is not an
> Arnold/RenderMan/Cycles competitor. No render farm, no production color
> management, no animation playback in v1. Anything outside that scope goes
> through the [RFC process](_documentation/rfcs/) before being added.

The canonical engineering plan lives in [plan_final.md](plan_final.md). It is
the source of truth for architecture, public API, coding rules, milestones,
and governance. The repo-root [CLAUDE.md](CLAUDE.md) is a section index map
for that plan.

## Status

Pre-release. Following milestones M0..M11 from `plan_final.md` §38 / §41:

- **M0** — Skeleton: build system green, `pyxis.exe` opens a Vulkan device,
  `SceneWorld` Flecs phase pipeline initialised.
- **M3** — Slang path-trace box (single cube, BLAS+TLAS).
- **M8a** — Moana subset render (≤1M tris, nightly regression seed).
- **M11** — Profiling polish (1.0.0).

## Build (target)

Pyxis targets Windows 10/11 x64 with clang-cl 17+, Vulkan SDK 1.3.x, vcpkg
manifest mode, CMake 3.27+. See `_documentation/getting_started.md` (post-M11)
for the canonical walkthrough.

For a fresh dev box, `_tools/required_install.ps1` installs every required
dependency (Visual Studio Build Tools, LLVM, CMake, Vulkan SDK, Python).

```pwsh
# elevated PowerShell
.\_tools\required_install.ps1
```

Then:

```pwsh
git clone <repo>
cd pyxis
cmake --preset dev
cmake --build --preset dev
```

## Layout

| Path | Purpose |
|---|---|
| `plan_final.md` | Canonical design document (read this first) |
| `CLAUDE.md` | Section-index map for the plan (auto-loaded by Claude Code) |
| `_cmake/` | CMake helpers + clang-cl toolchain + custom triplets |
| `_tools/` | Scripts — install, license audit, regression harness, perf bisect, … |
| `_documentation/` | User-facing docs + RFCs + postmortems |
| `_pipelines/` | CI definitions |
| `sources/pyxis_platform/` | Vulkan/NVRHI device, OS, file I/O, logging (SHARED) |
| `sources/pyxis_renderer/` | Renderer core, public API, `SceneWorld` (Flecs), passes |
| `sources/pyxis_material_translation/` | UsdPreviewSurface / MaterialX / RenderMan → OpenPBR |
| `sources/pyxis_hydra/` | Hydra 2.0 render delegate (ingest adapter) |
| `sources/pyxis_usd_ingest/` | Direct USD walker (ingest adapter; no Hydra) |
| `sources/pyxis_app/` | Single executable: viewer + headless |
| `resources/shaders/` | Slang sources |
| `tests/` | gtest unit tests + Python regression harness |
| `thirdparty/` | Vendored or fetched third-party (NVRHI, Slang, OpenUSD, …) |

## Licensing

Pyxis is distributed under the **Apache License, Version 2.0** — see
[LICENSE](LICENSE).

[NOTICE](NOTICE) aggregates third-party attributions. CI asserts
`NOTICE` byte-equals `NOTICE.generated` (produced by
`_tools/license_audit.py`); any drift fails the build. Adding a dep with a
non-Apache-compatible licence (GPL, AGPL, SSPL) fails the same step.

The third-party-licence mix is constrained to MIT / BSD-2 / BSD-3 / Apache-2 /
Zlib / public-domain components only.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for build steps, code-style rules,
commit-message convention (Conventional Commits 1.0.0), and the PR / reviewer
checklists. Non-trivial design changes go through the RFC process under
`_documentation/rfcs/` (plan §44).

## Privacy

Pyxis ships **zero telemetry** (plan §47.3 / §48.1). No phone-home, no
anonymous usage stats, no automatic crash-dump upload. Configuration files
and per-run reports stay on the local machine.
