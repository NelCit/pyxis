# Tech stack

- C++23, clang-cl 17+ (`/std:c++latest /W4 /WX /permissive-`), MSVC STL; renderer/platform `/EHs-c-` (no exceptions) + `/GR-` (no RTTI); hydra `/EHsc` (USD requires).
- Build: CMake presets (Ninja Multi-Config) + vcpkg. Presets: `dev` (clang-cl + vcpkg, viewer), `ci` (no viewer). Build dirs `build/dev`, `build/ci`; `compile_commands.json` exported there (root `.clangd` points at `build/dev`).
- GPU: Vulkan via NVRHI (opaque `IDevice*`/`ICommandList*` in Public/); RTXMU manages BLAS memory/compaction (`NVRHI_WITH_RTXMU=ON`); TLAS is plain NVRHI.
- Shaders: Slang → SPIR-V via ShaderMake (`-matrix-layout-row-major -O3 -profile sm_6_6 -emit-spirv-directly`). `ShaderInterop.slang` = only C++/shader shared file. Row-major storage, column-vector math (`mul(M, v)`, translation in last column).
- ECS: Flecs (PRIVATE-linked, never in Public/), custom phase pipeline `PYXIS_PHASE_*` (not flecs::OnUpdate), world owned by `GpuScene::Impl`.
- Scene I/O: OpenUSD (Hydra 2.0 Scene Indices; legacy UsdImagingDelegate banned).
- Materials: everything converts to OpenPBR (`OpenPBRMaterialDesc` → XXH3 dedup → `OpenPBRMaterialGPU`); one generic closesthit, branchless on MaterialFlag.
- Math interop: hlslpp aliases inside `PYXIS_INTEROP_STRUCT`.
- Test: gtest (`pyxis_unit_tests`), python EXR-diff regression (ctest); Tracy + in-process Profiler (§34, dotted zone prefixes `render.*`, `ingest.hydra.*`, …).
- Logging: spdlog via `pyxis::Logging::Get()` (only allowed singleton besides Tracy).
- Version: SemVer, exports golden-file `_tools/check_exports.py` gate.
