---
name: coding-rules-enforce
description: Audit Pyxis C++ source against the §30 coding rules — naming, headers, types & ownership, function shape, error tiers, allocations. Invoke before opening a PR or when reviewing diffs in sources/pyxis_renderer/, sources/pyxis_platform/, sources/pyxis_hydra/, sources/pyxis_usd_ingest/, sources/pyxis_app/, or sources/pyxis_material_translation/. Reports violations with §30.x citations.
---

# coding-rules-enforce

§30 is normative — reviewers reject PRs that violate it. This skill scans the diff (or a target directory if specified) for the patterns most likely to trigger rejection.

Scope: the renderer/platform perimeter is **no exceptions, no RTTI, no STL streams**. Hydra is the only target compiled with `/EHsc /GR` because USD needs them.

## What to grep for

Run from the repo root. Limit to files under `sources/`.

### §30.1 — forbidden constructs

| Pattern | Where | Violation |
|---|---|---|
| `<iostream>`, `<sstream>`, `<fstream>`, `std::cout`, `std::cerr`, `std::endl` | anywhere | STL streams forbidden — use spdlog |
| `\bnew\b`, `\bdelete\b` | anywhere outside `Private/.../*Raii*.h` and similar thin RAII wrappers | raw new/delete forbidden |
| `dynamic_cast`, `typeid` | `sources/pyxis_renderer/**`, `sources/pyxis_platform/**` | RTTI is `/GR-` on these targets |
| `throw `, `\btry\b`, `\bcatch\b` | `sources/pyxis_renderer/**`, `sources/pyxis_platform/**` | `/EHs-c-` perimeter — no exceptions |
| `throw `, `try`, `catch` crossing into `Public/` | renderer public headers | exceptions never cross DLL boundary |

### §30.2 — naming

Confirm in changed files:

- Types/classes/structs/enums: `PascalCase` (`GpuScene`, `MeshHandle`).
- Free / member / static functions: `PascalCase` (`AppendInstance`, `BlasCache::EstimateScratchSize`).
- Public POD fields: `camelCase` (`RenderSettings::maxBounces`).
- Private fields: `_camelCase` (`_uploadQueue`).
- Locals: `camelCase`.
- Compile-time constants (incl. `static constexpr` members): `UPPER_SNAKE_CASE` with **no prefix** (`MAX_BINDLESS_TEXTURES`, `HANDLE_SLOT_BITS`).
- Enum-class constants: `PascalCase` with **no prefix** (`MaterialFlag::DoubleSided`, `MeshHandle::Invalid`).
- Macros: `PYXIS_SCREAM` (`PYXIS_DEBUG_TOOLS`, `PYXIS_ASSERT`).
- Namespace: single flat `pyxis::`. No per-module sub-namespace.
- Acronyms count as words: `BlasCache` not `BLASCache`, `GpuScene` not `GPUScene`, `aovId` not `AOVId`. Two-letter acronyms keep both letters cased: `UI`, `IO`.
- No `m_`, `s_`, no Hungarian. Statics live in anonymous namespaces.
- One primary type per header; matching `.cpp`. Helpers get `*Internal.h` only when unavoidable.

### §30.3 — headers and includes

- `#pragma once` only; no include guards.
- Public headers must not include `<windows.h>`, `pxr/...`, or any third-party header beyond opaque `nvrhi::IDevice*` / `nvrhi::ICommandList*` forward decls.
- Include order, blank line between groups: matching header → Pyxis public → Pyxis private → NVRHI/Vulkan → third-party → std.
- `<>` for third-party and Pyxis public; `""` for same-target private.
- No transitive includes — every `.cpp` includes what it uses.

### §30.4 — types and ownership

- `std::unique_ptr` for sole ownership. `std::shared_ptr` only when truly shared (NVRHI uses `RefCountPtr` — that's the library idiom). Never `shared_ptr` to mean "I don't know who owns it".
- **Out-parameters forbidden.** Return by value: `std::optional<T>`, `Expected<T>`, or aggregate.
- `[[nodiscard]]` on every factory and every `Expected<T>`-returning function.
- `final` classes by default if not designed for inheritance.
- Strong-typed handles (`enum class : uint32_t`, `Invalid = 0`) — no bare `uint32_t` indices crossing module boundaries.
- `const` correctness: every non-mutating member function `const`; every non-mutated parameter `const T&` (or `T` if trivially copyable and ≤ 16 bytes).

### §30.5 — functions

- ≤ 50 lines guideline; > 100 lines requires reviewer justification — flag and let the user explain.
- ≤ 5 parameters; group with a `Desc` struct beyond that (`Foo(const FooDesc& desc)`).
- **No default arguments on virtual functions; no default arguments on public API.** This is hard.
- `noexcept` on every move ctor/op, every dtor, and every function inside the renderer/platform no-exceptions perimeter.

### §30.6 — error handling (three tiers)

- Programmer error → `PYXIS_ASSERT(cond, fmt, ...)` (Debug) or `PYXIS_VERIFY(cond, fmt, ...)` (always-on).
- Recoverable runtime → `Expected<T, Error>` constructed via `PYXIS_ERROR(kind, fmt, ...)`; propagate up with `PYXIS_TRY`.
- Fatal hardware/driver → `PYXIS_FATAL` after flushing logs.
- Flag any `bool` returned for a failure path, any silently dropped error, any `Expected<T>` failure that isn't logged at the boundary.

### §30.7 — comments

- Public headers: Doxygen `///` with one paragraph per class (purpose, lifetime, threading model) and one line per function.
- Private files: `//` only when the *why* is non-obvious. Never restate the *what*.
- Untagged TODO: `// TODO(<owner>, <ticket-or-section>): ...`. Untagged TODOs are PR-blocking.

### §30.10 — allocations in render passes

- No allocations in any `IRenderPass::Execute()` body. Preallocate in `Declare()` / on-resize.
- One frame-scoped scratch arena (`FrameAllocator`) for per-frame transient CPU data.
- No `new T[]`; use `std::vector<T>` with `reserve()` or `std::pmr` arenas.

### §30.9 — concurrency

- No global mutable state. Singletons forbidden except `pyxis::Logging::Get()` (§33.10) and Tracy's process-global client.
- Atomics via `std::atomic<T>`; never via `volatile`.

## Output

Group by §30 subsection. For each finding cite file:line and the rule.

```
## §30.1 forbidden constructs (PR-blocking)
- sources/pyxis_renderer/Private/Foo.cpp:42 — raw `new MyThing()`; use unique_ptr/RAII

## §30.2 naming
- sources/pyxis_renderer/Public/Pyxis/Renderer/Descs/MeshDesc.h:18 — m_vertexCount; private fields use _camelCase, public POD fields use camelCase (no prefix)

## §30.5 functions
- sources/pyxis_hydra/Private/MeshAdapter.cpp:204 — DoWork() default arg `bool force = false`; default args forbidden on public API (move to overload or Desc struct)

## §30.10 allocations
- sources/pyxis_renderer/Private/Passes/PathTracePass.cpp:Execute():91 — std::vector<X> tmp; preallocate in Declare()
```

Don't auto-fix — report only. The user picks which to address.
