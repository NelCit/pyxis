# Conventions (plan §30 — reviewers reject violations)

Naming: `PascalCase` types/functions, `_camelCase` private fields, `camelCase` POD-public fields/locals, `UPPER_SNAKE_CASE` compile-time constants (no k/s prefixes), `PascalCase` enum-class values (`MeshHandle::Invalid`), `PYXIS_SCREAM` macros, flat `pyxis::` namespace. Acronyms count as words: `BlasCache`, not `BLASCache`.

Braces: Allman for control statements (if/for/while), attached for functions/classes/namespaces.

Errors, three tiers: `PYXIS_ASSERT`/`PYXIS_VERIFY` (programmer error) → `Expected<T, Error>` + `PYXIS_ERROR(kind, fmt, ...)` + `PYXIS_TRY` (recoverable) → `PYXIS_FATAL`. `[[nodiscard]]` on Expected-returning methods; void verbs silently drop stale handles (counted in `FrameStats::staleHandleDrops`).

Forbidden: exceptions/RTTI in renderer+platform, STL streams, raw new/delete outside RAII, singletons (except Logging + Tracy), allocations in pass `Execute()` (preallocate in `Declare()`/on-resize), `<windows.h>` or `pxr/` in public headers.

Public classes: PIMPL (`struct Impl; std::unique_ptr<Impl>`); large Impls split per-verb-group files with bodies as Impl member functions.

Flecs (§30.11, `Private/Scene|GpuScene` only): POD components (no vector/string — side tables by handle); `Dirty<T>` zero-size tags cleared in `System_ClearDirtyFlags`; systems = free functions `System_VerbObject` in `Private/Scene/Systems/`; queries cached at registration (query built in per-frame body = PR-blocking); prefer pair relationships `(Instance, MaterialOf, mat)`; single-writer world (ingest single-threaded v1; MutationCommand queue deferred).

Shader interop (§23): 16-byte cbuffer alignment, `static_assert(sizeof % 16 == 0)` mandatory; permutations via ShaderMake `-D NAME={0,1}`.

Prefer canonical shading ops (ACES, Lambert NdotL, real BRDF terms) over ad-hoc approximations even in stubs.
