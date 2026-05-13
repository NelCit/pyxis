# Pyxis golden test data (USD fixtures + render configs)

Inputs to the golden-test suite. Outputs (the `baseline.png` per
test) live under [../golden-tests-expected/<test-name>/](../golden-tests-expected/).
See that directory's README for the feature-coverage table + run /
rebake instructions.

```
tests/golden-tests-data/<test-name>/
    fixture.usda       # the scene
    regression.json    # tolerances + optional --frame override

tests/golden-tests-expected/<test-name>/
    baseline.png       # checked-in expected pixels
```

The shared default render config lives at
[`_shared/config.json`](_shared/config.json) — 256×256, seed=42, FIF=3
for byte-equal under §33.7. Per-test overrides live next to the fixture.
