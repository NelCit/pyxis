# Contributing to Pyxis

This document is the canonical contribution policy for Pyxis (plan §45.1).
PR reviewers consult this list; a checklist failure rejects the PR.

The engineering plan in [plan_final.md](plan_final.md) is normative — coding
rules in §30, the public API contract in §18, the ABI rules in §22, and the
§42 "do not build yet" list are PR-blocking. When in doubt, cite a section.

---

## Build from source

Bootstrap a fresh Windows dev box:

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
ctest --preset dev
```

The full walkthrough lives in `_documentation/getting_started.md` (post-M11
deliverable, plan §50). Until then, the install script + this README are the
contract.

---

## Run the regression suite

The Python regression harness invokes `pyxis.exe --headless` against fixture
configs and image-diffs the output against baselines (plan §35).

```pwsh
python _tools/run_regression.py
```

Per §36.5 the harness picks **strict** mode (byte-identity) when the host GPU /
driver / Vulkan SDK / OS matches the v1 reference matrix
(`_tools/regression_matrix.json`); otherwise it uses **tolerant** mode with
per-test RMSE / max-absolute-delta floats from the fixture's
`parameters.json`.

---

## Code style

- [`.clang-format`](.clang-format) — repo-root, hand-tuned for this codebase.
- [`.clang-tidy`](.clang-tidy) — curated checks per plan §30 + §36.2.
- Plan §30 is the normative coding-rule reference (§30.2 naming, §30.3 headers,
  §30.4 ownership, §30.6 error handling, §30.9 concurrency, §30.10 memory,
  §30.11 Flecs conventions).

CI runs `_tools/run_clang_format.py --check` and `clang-tidy` with
`WarningsAsErrors: '*'` against every `sources/pyxis_*` file.

---

## Commit-message convention

Pyxis follows [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/).

```
<type>(<scope>): <subject>

[body — optional]

[BREAKING CHANGE: <description>  — required for any §22.1 MAJOR diff]
```

`type` ∈
`{feat, fix, perf, refactor, test, docs, build, ci, chore, revert}`.

`scope` ∈
`{platform, renderer, hydra, usd_ingest, material, app, shaders, build, ci, docs, rfc}`.

CI lints commit messages via `_tools/check_commit_messages.py`. A breaking
change footer **plus** a `version.txt` major bump are required for any change
that breaks `Public/` source compatibility (plan §22.1).

---

## Branch policy

- `main` is protected; PRs only.
- Squash-merge is the default. Linear history; no merge commits on `main`.
- Force-pushes to `main` are forbidden.
- Force-pushes to a feature branch are allowed only by the branch's author.
- Hooks (`--no-verify`, `--no-gpg-sign`) **must not** be skipped on commit
  unless the user has explicitly approved a one-off bypass.

---

## PR checklist (mirrored in `.github/pull_request_template.md`)

- [ ] `clang-format` clean (`python _tools/run_clang_format.py --check`).
- [ ] `clang-tidy` no new warnings.
- [ ] Unit tests added/updated where relevant.
- [ ] Regression fixtures updated where the PR touches §35's rule (a new
      public-API verb or a new MaterialX coverage path **must** ship a
      fixture).
- [ ] If the PR touches `sources/pyxis_renderer/Public/`, `version.txt` is
      bumped per §22 and the symbol-export golden file
      (`_tools/golden_exports.txt`) is updated.
- [ ] If the PR adds a new third-party dep, both `vcpkg.json` baseline and
      `_cmake/Thirdparty.cmake` SHA are updated, **and** the dep is added to
      `_tools/license_audit.py`'s `COMPONENTS` table + the shipped `NOTICE`
      file (CI asserts byte-equality).
- [ ] If the PR violates a §30 rule, link the RFC under
      `_documentation/rfcs/` that approved it.
- [ ] No `[[deprecated]]` removals before the §22.3 two-minor window expires.

## Reviewer checklist (`.github/REVIEW_CHECKLIST.md`)

Reviewers verify:

- **§30.3 header discipline**: no `pxr/...` in `pyxis_renderer/...`; no STL
  containers in any `Public/` POD; no transitive third-party headers leak
  through `Public/`.
- **§30.11 Flecs**: no per-frame `world.query_builder<...>()` construction;
  no observer-as-system abuse; `Dirty<T>` cleared in `System_ClearDirtyFlags`;
  components are POD with fixed layout.
- **§18.9 ABI**: no STL containers in any new `Desc`; offset / padding
  reviewed for any new public POD; trailing `_reserved*` slots consumed
  before adding new fields.
- **Profiler scopes**: every new pass and every hot CPU function carries one
  `Profiler::CpuScope` and one `Profiler::GpuScope` per the §34 dotted-name
  convention.
- **Tracy / spdlog category prefix** follows §31 (`ingest.*`, `render.*`,
  `assets.*`, `app.*`, `ingest.shared.*`).
- **Error catalogue** §20 is used; no raw `bool` returns for failure paths.

---

## RFCs

Non-trivial design and process changes go through an RFC under
`_documentation/rfcs/` before any code lands (plan §44). What needs an RFC:
public API surface changes (§44.1), Flecs convention changes (§30.11), adding
back a §42 deferred feature, third-party MAJOR bumps, BLAS / TLAS flag policy
changes, bindless capacity changes, weakening any §30 normative rule.

What doesn't need an RFC: bug fixes within RMSE tolerance, internal refactors
inside a single `Private/` folder, new fixtures, new profiler scopes, new
spdlog categories, CI tightening.

Lifecycle (§44.4): minimum 7 calendar days from PR open to merge; one
code-owner approval (two for public-API RFCs); status transitions to
`Accepted` and the RFC merges *before* any implementation PR.

---

## Triage / on-call (§45.3)

| Severity | Trigger | Response |
|---|---|---|
| **S1** | Main fails to build, or headless smoke crashes | Revert-or-fix within 24 h; main blocked until green. |
| **S2** | Nightly subset-Moana RMSE > 2× tolerance, or peak GPU > +20 % baseline | Tracking issue + assignment within 48 h. |
| **S3** | Flaky test, perf jitter inside ±10 % budget | Logged; addressed in next maintenance sprint. |

A failing test is **never** `@disabled` without a tracking issue + a target
re-enable date.

S1 incidents get a postmortem under `_documentation/postmortems/YYYY-MM-DD-<slug>.md`
— no blame, focus on the detection-and-recovery loop.
