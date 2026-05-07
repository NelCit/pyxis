# RFCs

Non-trivial design and process changes go through an RFC under this folder
before any code lands. Plan §44 is the canonical lifecycle reference; this
README is an index of merged RFCs (one line per RFC).

| Number | Title | Status |
|---|---|---|
| 0000 | Template (do not number) | Template |

## Lifecycle (§44.4)

1. PR opens against `_documentation/rfcs/NNNN-<slug>.md` with `Status: Draft`.
2. Minimum **7 calendar days** between PR open and merge (24 h only for
   security fixes by code-owner consensus).
3. At least one code-owner approval for the area being changed; **two** for
   any RFC that touches the public API (§18).
4. Status transitions to `Accepted` and the RFC merges *before* any
   implementation PR. The implementation PR's description links the RFC.
5. Rejected RFCs merge with `Status: Rejected` plus a "Why rejected" section.
6. Superseded RFCs link forward (`Superseded by NNNN`).

## What needs an RFC (§44.1)

- Anything that changes the public API surface (§18) in a non-trivial way:
  new method, new public POD, new handle, new error kind, new feature flag.
- Anything that changes the `pyxis_renderer/Private/Scene/` Flecs conventions
  (§30.11): new phase, reordered phases, new component category, observer
  policy change.
- Adding back any item from the §42 deferred-features list.
- Bumping a third-party dependency to a new MAJOR (e.g. OpenUSD v25 → v26).
- Changing the BLAS / TLAS flag policy (§16) or the bindless capacity (§5).
- Anything that removes or weakens a normative rule in §30.

## What doesn't need an RFC (§44.2)

- Bug fixes within the per-test RMSE tolerance.
- Internal refactors strictly inside one `Private/` folder.
- New `Private/` files, internal helpers, fixtures.
- New `Profiler` scopes, new spdlog categories.
- New CI checks that strictly tighten existing rules.
