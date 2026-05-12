# Pyxis regression tests

Image-regression fixtures consumed by [`_tools/run_regression.py`](../../_tools/run_regression.py).
Each subdirectory is one regression test. Plan §35 / §41 M10.

## Layout

```
tests/regression/
    <test_name>/
        regression.json      # tolerances + paths to config / scene / baseline
        baseline.exr         # ground-truth render
```

`regression.json` schema:

| field | required | meaning |
|---|---|---|
| `name` | yes | ctest-visible name (used as the test label) |
| `description` | no | one-line scene / camera description |
| `config` | yes | path to a `pyxis.exe --config` JSON (relative to repo root or the fixture dir) |
| `scene` | no | path to a `.usd*` scene; omitted ⇒ pyxis resolves through `--config`'s default-scene chain (§29.4.a) |
| `baseline` | yes | EXR ground truth (relative paths resolved as above) |
| `tolerance.rmse` | yes | maximum allowed RMSE across all channels |
| `tolerance.mae` | yes | maximum allowed mean-absolute-error |
| `tolerance.maxAbsoluteDelta` | yes | maximum single-pixel absolute difference (clamps the long tail of an otherwise-low-RMSE diff) |

The harness writes a `diff.exr` (`|produced − baseline|` per channel) to the
test's output directory on failure. Open it in usdview's image viewer / Nuke /
`oiiotool --show` to triage.

## Capturing a new baseline

```pwsh
.\build\dev\bin\Release\pyxis.exe --headless `
    --config tests\fixtures\<name>.json `
    --scene  <scene-path-or-omit> `
    --output tests\regression\<test_name>\baseline.exr
```

After capture: commit the EXR alongside its `regression.json`. The baseline
file is the contract — bump it only when the scene / shaders intentionally
change.

## §36.5 strict-vs-tolerant determinism

Today every fixture is tolerant-mode (per-test floats). The strict-mode
matrix (RTX 4080 + pinned driver + pinned Vulkan SDK + Win 11 23H2/24H2 →
byte-identical EXR required) is wired in via `_tools/regression_matrix.json`
in the M10 follow-up that adds nightly CI. Local developer runs always use
tolerant mode.
