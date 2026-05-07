<!--
  Pyxis PR template — plan §45.1.
  Reviewers walk this checklist; a failure rejects the PR.
-->

## Summary

<!-- One paragraph. What changes, and why now. -->

## Motivation

<!-- Optional. Customer pain, regression caught, RFC link, etc. -->

## Plan references

<!-- Section numbers in plan_final.md this PR touches.  Examples: §18.5 (GpuScene), §30.11 (Flecs). -->

- §

## RFC link (if applicable)

<!--
  Required if the PR adds / changes:
    - Public/Pyxis/* surface (§18 / §44.1).
    - sources/pyxis_renderer/Private/Scene/ Flecs convention (§30.11).
    - Anything from the §42 "do not build yet" list.
    - Third-party dep MAJOR bump.
    - BLAS/TLAS flag policy or bindless capacity (§5 / §16).
-->

`_documentation/rfcs/NNNN-<slug>.md`

---

## PR checklist

- [ ] `clang-format` clean (`python _tools/run_clang_format.py --check`).
- [ ] `clang-tidy` no new warnings (CI runs with `WarningsAsErrors: '*'`).
- [ ] Unit tests added/updated where relevant.
- [ ] Regression fixtures updated where the PR touches §35's rule.
- [ ] If the PR touches `sources/pyxis_renderer/Public/`, `version.txt` is
      bumped per §22, the symbol-export golden file
      (`_tools/golden_exports.txt`) is updated, and the body carries a
      `BREAKING CHANGE:` footer for any §22.1 MAJOR diff.
- [ ] If the PR adds a new third-party dep, both `vcpkg.json` baseline and
      `_cmake/Thirdparty.cmake` SHA are updated, **and** the dep is added to
      `_tools/license_audit.py`'s `COMPONENTS` table + the shipped `NOTICE`
      file (CI asserts `NOTICE` byte-equals `NOTICE.generated`).
- [ ] If the PR violates a §30 rule, the linked RFC under
      `_documentation/rfcs/` is `Accepted`.
- [ ] No `[[deprecated]]` removals before the §22.3 two-minor window expires.
- [ ] No silent `@disabled` tests; failing tests have a tracking issue + a
      target re-enable date (§45.3).

## Reviewer checklist (§45.1 — reviewers verify before approving)

- [ ] **§30.3 header discipline**: no `pxr/...` in `pyxis_renderer/...`; no
      STL containers in any `Public/` POD; no transitive third-party headers
      leak through `Public/`.
- [ ] **§30.11 Flecs**: no per-frame `world.query_builder<...>()` construction;
      no observer-as-system abuse; `Dirty<T>` cleared in
      `System_ClearDirtyFlags`; components are POD with fixed layout.
- [ ] **§18.9 ABI**: no STL containers in any new `Desc`; offset / padding
      reviewed; trailing `_reserved*` slots consumed before new fields.
- [ ] **Profiler scopes** present on every new pass / hot CPU function (§34);
      Tracy / spdlog category prefix matches §31 (`ingest.*`, `render.*`,
      `assets.*`, `app.*`, `ingest.shared.*`).
- [ ] **Error catalogue** §20 is used; no raw `bool` returns for failure paths.

## Test plan

<!-- Bulleted markdown checklist of TODOs for testing the pull request. -->

- [ ]
