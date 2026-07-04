# Definition of done

1. `cmake --build --preset dev` clean (`/W4 /WX` — warnings are errors).
2. `ctest --preset dev` green (unit + regression). If renderer output could change: run the headless EXR regression deliberately and inspect diffs — image is the only regression artefact (§35).
3. clang-format applied; clang-tidy clean per repo `.clang-tidy`.
4. If `sources/pyxis_renderer/Public/` touched: `_tools\check_exports.py` golden diff + §22 version bump reasoning; new public types need an RFC (§44). Project skills `/api-surface-check` and `/rfc-required` audit this.
5. Perf-relevant PRs: profile evidence required (§34 KPIs: `pass.PathTrace < 12ms`, `commitResources < 2ms` @1080p hero cam, RTX 4080) — PRs without measurements get rejected. New passes need Profiler/Tracy scopes with dotted prefixes (`/profiler-scope-lint`).
6. New ECS code: `/flecs-conventions-audit`; milestone-closing PRs: `/milestone-exit-criteria`.
7. Loading milestones (M12+): must ship unit tests + USD fixture + integration smoke (user rule).
