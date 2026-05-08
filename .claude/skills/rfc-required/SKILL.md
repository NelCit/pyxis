---
name: rfc-required
description: Decide whether a Pyxis change requires an RFC under §44 before code lands, and verify the RFC lifecycle (Draft → Accepted → implementation PR linked) is being followed. Invoke when a PR proposes a change to the public API surface, the §30.11 Flecs conventions, the §42 deferred list, the BLAS/TLAS flag policy, the bindless capacity, or any §30 normative rule. Reports whether an RFC is needed and how to file it.
---

# rfc-required

§44 codifies the RFC process. Non-trivial design and process changes are proposed via short markdown documents under `_documentation/rfcs/` **before any code lands**. This skill answers "does this PR need an RFC?" and, if yes, walks the user through the lifecycle.

## What needs an RFC (§44.1)

- Anything that changes the public API surface (§18) in a non-trivial way:
  - new method
  - new public POD
  - new handle
  - new error kind
  - new feature flag
- Anything that changes `_renderer/Private/Scene/` Flecs conventions (§30.11):
  - new phase
  - reordered phases
  - new component category
  - observer policy change
- Adding back any item from the §42 "Do Not Build Yet" list (subdivision, volumes, curves, displacement, animation, texture compression, multi-hitgroup material specialisation, …).
- Bumping a third-party dependency to a new MAJOR (e.g. OpenUSD v25 → v26).
- Changing the BLAS / TLAS flag policy (§16).
- Changing the bindless capacity (§5).
- Anything that removes or weakens a normative rule in §30.

## What does NOT need an RFC (§44.2)

- Bug fixes that don't change behaviour outside per-test RMSE tolerance.
- Internal refactors strictly inside one `Private/` folder.
- New `Private/` files, new internal helpers, new fixtures.
- New `Profiler` scopes, new spdlog categories.
- New CI checks that strictly tighten existing rules.

## Decision flow

1. Read the PR diff.
2. Walk the §44.1 list. For each item, answer yes/no with a file:line citation.
3. If any §44.1 trigger fires → an RFC is required.
4. If yes, check the PR description for `RFC <NNNN>` reference. If absent → block.
5. If a reference exists, locate `_documentation/rfcs/NNNN-<slug>.md`:
   - **Status: Accepted**? If Draft / Review / Rejected — block (the RFC must merge first, §44.4).
   - The implementation PR's description must link the RFC number.
6. For RFCs touching the public API: **two** code-owner approvals required (§44.4).

## Lifecycle (§44.4)

If an RFC is required and the user is filing it:

1. PR opens against `_documentation/rfcs/NNNN-<slug>.md` with `Status: Draft`.
2. **Minimum 7 calendar days** between PR open and merge (shortened to 24 h only for security fixes by code-owner consensus).
3. **At least one code-owner approval** for the area (§45). Public API changes need two.
4. Status transitions to `Accepted` and the RFC merges *before* any implementation PR.
5. Rejected RFCs merge with `Status: Rejected` plus a "Why rejected" section. They stay searchable.
6. Superseded RFCs link forward (`Superseded by NNNN`).
7. `_documentation/rfcs/README.md` lists every RFC with one-line status.

## Template (§44.3)

`_documentation/rfcs/0000-template.md`:

```markdown
# RFC NNNN: <short title>

- Status: Draft | Review | Accepted | Rejected | Superseded by NNNN
- Author(s):
- Created:
- Last updated:
- Implementation PRs:

## Summary
One paragraph.

## Motivation
What problem, why now, who's blocked.

## Detailed design
The actual change. Code sketches. POD diffs. Phase-pipeline diffs.
Cross-reference plan_final.md sections.

## Alternatives considered
At least two; explain why rejected.

## Drawbacks / risks
Honest list, including ABI / regression-image impact.

## Migration & impact
Who must do what, and on what timeline. List affected milestones (§38/§41).

## Open questions
Questions blocking acceptance.
```

## Patterns to grep for in the diff

| Pattern | Trigger |
|---|---|
| New file under `sources/pyxis_renderer/Public/` | §18 surface change |
| New `enum class` value or new method in any header under `Public/` | §18 |
| Changes to `sources/pyxis_renderer/Private/Scene/Phases.h` | §30.11 phase pipeline |
| New observer in `Private/Scene/Observers/` that isn't a refcount/deletion-queue/BLAS-release case | §30.11 observer policy |
| Edits to `vcpkg.json` baseline that change the major of a dep | §44.1 |
| Edits touching BLAS / TLAS build flags in `Private/GpuScene/Blas*.cpp` or `Tlas*.cpp` | §16 policy |
| Bindless slot constants increased | §5 |
| Anything from §42 "Do Not Build Yet" being implemented (search for animation, subdivision, volumes, curves, displacement, motion blur, DoF, LUT tone-map, light linking) | §44.1 |

## Output

```
## RFC required? YES
- Diff adds `pyxis::PyxisRenderer::PickAt(int, int)` to Public/Pyxis/Renderer/PyxisRenderer.h:88 — new public method (§18)
- Diff bumps OpenUSD baseline 25.x → 26.0 in vcpkg.json — third-party MAJOR (§44.1)

## RFC linked in PR description? NO

## Action
- Block PR.
- File _documentation/rfcs/NNNN-pickat-and-usd-26.md from §44.3 template.
- Wait for Accepted status (≥ 7 days; 2 approvals because public API).
- Then open implementation PR linking RFC NNNN.
```

Or, when no RFC is needed:

```
## RFC required? NO
- Diff is internal refactor strictly inside sources/pyxis_renderer/Private/Passes/ (§44.2)
- No public surface change, no Flecs convention change, no §42 item revived.
```

Don't auto-file the RFC — it's a deliberative document the user must author.
