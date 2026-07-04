---
name: slang-debugging-reference
description: Reference distilled from the official shader-slang/slang CLAUDE.md — Slang compiler debugging flags, IR inspection, and the upstream team's root-cause philosophy. Use when debugging Slang compilation errors, inspecting generated SPIR-V/IR, investigating slangc miscompiles, or filing upstream Slang issues from Pyxis shader work.
---

# Slang Debugging Reference (from upstream CLAUDE.md)

Source: `https://github.com/shader-slang/slang/blob/master/CLAUDE.md` (fetched 2026-07-04).
This is the Slang team's own agent guidance — use it when Pyxis shader builds (§10/§23) hit compiler-side problems.

## Debugging the compiler pipeline

- **Inspect IR after passes**: `slangc -dump-ir -target spirv-asm -o tmp.spv shader.slang`
  Compare the IR before/after suspicious passes; most emit/codegen bugs originate upstream
  (an IR pass, type legalization, specialization, or lowering), not in the emitter.
- **Trace instruction origin**: Slang's `InstTrace` machinery traces where a problematic
  IR instruction was created — use when a bad instruction appears in output.
- **Assertions on Windows**: the `SLANG_ASSERT` environment variable controls assertion
  behavior — enable when hunting nondeterministic compiler crashes.
- **GPU-free testing**: `slang-test` supports `-cpu` / interpreter targets — reproduce
  compiler issues without touching the Vulkan device.

## Upstream philosophy (mirror it in bug reports and workarounds)

> "Fix root causes, not symptoms. When a bug appears in emit/codegen, the cause is usually
> upstream... Always question whether a particular input representation is correct before
> writing code to handle it."

Red flags the Slang team rejects (avoid these in Pyxis-side shader workarounds too):
- consumer-side patching of malformed structures,
- rebuilding syntax from semantic data,
- special-case equivalence helpers where two forms should be modeled identically.

## Filing upstream issues / PRs

Their PR template requires: motivation with a concrete repro, principled solution
justification, file-by-file change summary, a vocabulary glossary, and a process report
answering *"is this input shape correct, or should its producer be fixed instead?"*
Structure Pyxis-originated Slang bug reports the same way — they get triaged faster.

## Their build/test quick commands (for repro-in-upstream work)

- Build: `cmake --preset debug` / `cmake --build --preset debug`
- Test: `./build/Release/bin/slang-test` from repo root
- Format before committing: `./extras/formatting.sh`
- Public API (`include/`) is ABI-frozen: never reorder/insert enum values or virtual
  methods mid-interface — append only (same discipline as Pyxis §22).
