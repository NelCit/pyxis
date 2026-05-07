# Pyxis Reviewer Checklist

Plan §45.1 normative reviewer checklist — this is the artefact the plan keeps
referencing as "reviewers reject". Each bullet links a recurring failure mode
to the section that forbids it.

## Header & include discipline (§30.3)

- [ ] No `#include <pxr/...>` in `sources/pyxis_renderer/` (the renderer is
      USD-free; only the ingest adapters and `pyxis_material_translation` may
      see USD headers).
- [ ] No `<windows.h>` in any `Public/` header.
- [ ] No NVRHI implementation types in any `Public/` header beyond opaque
      forward-declared interface pointers (`nvrhi::IDevice*`,
      `nvrhi::ICommandList*`).
- [ ] No transitive third-party headers leak through `Public/`.
- [ ] Include order respected: matching header → Pyxis public → Pyxis
      private → NVRHI/Vulkan → third-party → standard library.
- [ ] `#pragma once` only; no include guards.

## ABI & public surface (§18 / §22)

- [ ] No `std::string` / `std::vector` / `std::map` / any STL container in
      any `Public/` POD or any public method signature. Inputs use
      `std::span<const T>` / `std::string_view` (borrowed); outputs that own
      a string use `pyxis::ErrorMessage` / `pyxis::FrameProfile::ScopeName`.
- [ ] `std::span` / `std::string_view` only in *input-only* parameters; never
      stored, never returned, never persisted past the call.
- [ ] Public POD layout is byte-frozen — adding a field consumes a trailing
      `_reserved*` slot, otherwise it's a §22.1 MAJOR break and `version.txt`
      is bumped.
- [ ] Strong-handle enums use the §19.7 24-bit slot + 8-bit generation
      layout.
- [ ] `[[nodiscard]]` on every factory and every `Expected<T>`-returning
      function.
- [ ] `version.txt` bumped per §22 if the PR touches `Public/`.
- [ ] Symbol-export golden file (`_tools/golden_exports.txt`) updated if the
      diff exports / un-exports symbols from `pyxis_renderer.dll`.

## Flecs conventions (§30.11) — `sources/pyxis_renderer/Private/Scene/`

- [ ] No `world.query_builder<...>()` inside a per-frame system body; queries
      are cached at registration time in `Queries/QueryCache.h`. Per-frame
      construction defeats archetype caching and allocates.
- [ ] Components are POD structs (no `std::vector`, no `std::string`, no
      virtuals); variable-length data lives in `Private/GpuScene/` tables.
- [ ] `Dirty<T>` is a zero-size tag component; cleared in
      `System_ClearDirtyFlags` after each phase's work.
- [ ] Systems are free functions named `System_VerbObject` in
      `Private/Scene/Systems/`.
- [ ] Custom `PYXIS_PHASE_*` pipeline used; `flecs::OnUpdate` etc. are not
      used. Reordering phases or inserting between them needs an RFC.
- [ ] Mutation is single-writer: only the render thread calls
      `world.entity()`/`set()`/`destruct()`. Ingest threads push
      `MutationCommand` records onto the `moodycamel::ConcurrentQueue`.
- [ ] No observer-as-system abuse: observers are reserved for cross-component
      invariants (refcount drops, `DeletionQueue` scheduling).

## Profiling & instrumentation (§34)

- [ ] Every new pass implements `IRenderPass::Name()` and the constructor
      registers via `RenderGraph::AddPass` (graph framework wraps it with
      `Profiler::CpuScope` + `Profiler::GpuScope` automatically).
- [ ] Hot CPU functions outside passes carry an explicit `Profiler::CpuScope`
      with the §31 dotted-prefix naming
      (`ingest.*`, `render.*`, `assets.*`, `app.*`, `ingest.shared.*`).
- [ ] No new "quality knob" added directly to a pass — knobs go through
      `RenderSettings` (§21).

## Error handling (§20 / §30.6)

- [ ] No raw `bool` returns for failure paths.
- [ ] Recoverable failures use `Expected<T, Error>` returned and propagated;
      construction via `PYXIS_ERROR(kind, fmt, …)`.
- [ ] Programmer errors use `PYXIS_ASSERT` / `PYXIS_VERIFY`; fatal hardware /
      driver paths use `PYXIS_FATAL` (logs flushed, then `std::terminate`).
- [ ] No silent failures — every `Expected` failure is logged at the boundary
      that decides to handle it, with category + sdfPath + cause.

## Threading (§31 / §32)

- [ ] No GPU calls (`nvrhi::IDevice::*`, `cl->*`) outside the render thread.
- [ ] No `spdlog::default_logger()` / `SPDLOG_INFO(...)` outside
      `pyxis_platform/Private/`; other modules go through
      `pyxis::Logging::Get()` (§33.10 single-registry rule).
- [ ] ImGui call sites are render-thread-only and gated behind
      `PYXIS_DEBUG_TOOLS`.

## License audit (§48.3)

- [ ] If a third-party dep is added/removed, both
      `_tools/license_audit.py`'s `COMPONENTS` table and the shipped `NOTICE`
      are updated; CI asserts `NOTICE` byte-equals `NOTICE.generated`.
- [ ] No GPL / AGPL / SSPL component introduced (the audit script enforces).

## Tests / fixtures (§35 / §36)

- [ ] PR adding a new public-API verb or a new MaterialX coverage path
      ships a fixture under `tests/fixtures/...` (§35 fixture-intent table).
- [ ] No silent `@disabled` tests; failing tests carry a tracking issue + a
      target re-enable date (§45.3).
