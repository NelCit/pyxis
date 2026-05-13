# Pyxis golden tests — expected baselines

Output side of the golden-test suite. Inputs (USD fixtures + render
configs) live under [tests/golden-tests-data/](../golden-tests-data/).

```
tests/golden-tests-data/<test-name>/
    fixture.usda       # the scene
    regression.json    # tolerances + optional --frame override

tests/golden-tests-expected/<test-name>/
    baseline.exr       # checked-in expected pixels
```

A test passes iff the rendered EXR matches `baseline.exr` byte-for-byte.
Default tolerances target byte-equal under the §33.7 / §36.5 strict-mode
contract (RTX 4070 Laptop + pinned NVIDIA driver + pinned Vulkan SDK +
Win 11 24H2).

The set covers every visual feature shipped to date:

| Test | Feature | Plan |
|------|---------|------|
| `geom_triangle_mesh`        | basic UsdGeomMesh                              | M3 / M5 |
| `geom_subdiv_catmark`       | OpenSubdiv catmullClark refinement              | V2.A.1 / M12 |
| `geom_holes`                | UsdGeomMesh::holeIndices                        | M12 |
| `geom_lefthanded`           | UsdGeomMesh::orientation = "leftHanded"         | M12 |
| `geom_visibility_filter`    | UsdGeomImageable::visibility / purpose          | V2.A.2 / M12 |
| `geom_displaycolor_fallback`| primvars:displayColor → fallback material       | V2.A.33 / M12 |
| `geom_analytic_primitives`  | Sphere/Cube/Cylinder/Cone/Capsule tessellation  | V2.A.21 / M13 |
| `geom_basis_curves`         | UsdGeomBasisCurves ribbon                       | V2.A.3 / M13 |
| `geom_points`               | UsdGeomPoints billboards                        | V2.A.3 / M13 |
| `geom_nurbs_bezier`         | NURBS cubic Bezier patch                        | V2.A.4 / M20 |
| `xform_advanced_ops`        | xformOp:transform / orient / rotateXYZ / !invert! | V2.A.6 / M14 |
| `xform_inactive_filter`     | active=false                                    | V2.A.17 / M14 |
| `mat_usd_preview`           | UsdPreviewSurface                               | M5 |
| `mat_materialx_openpbr`     | MaterialX `ND_open_pbr_surface_*`               | V2.A.8 |
| `mat_materialx_standard`    | MaterialX `ND_standard_surface_*`               | V2.A.8 |
| `mat_mdl_omnipbr`           | MDL `mdl::OmniPBR`                              | V2.A.23 |
| `light_dome`                | UsdLuxDomeLight                                 | M7 |
| `light_distant_lambert`     | UsdLuxDistantLight + N·L                        | M7 |
| `light_rect`                | UsdLuxRectLight                                 | M7 |
| `instancer_basic`           | UsdGeomPointInstancer                            | M6 |
| `anim_skel_frame0`          | UsdSkel CPU skinning at t=0                     | V2.A.4 |
| `anim_skel_frame24`         | UsdSkel CPU skinning at t=24                    | V2.A.4 |
| `anim_xform_frame0`         | xformOp:translate.timeSamples at t=0            | V2.A.13 |
| `anim_xform_frame100`       | xformOp:translate.timeSamples at t=100          | V2.A.13 |

## Run

```
ctest --preset dev -L golden
```

## Rebake (after a deliberate visual change)

```
python _tools/run_goldens.py --rebake
```

## Adding a new feature

Drop a fixture under [../golden-tests-data/<short-name>/](../golden-tests-data/)
and a sibling baseline dir here. The convention is short-name dir,
single `fixture.usda`, single `regression.json`, single `baseline.exr`.
Multi-frame animation features get one sub-test per sampled frame
(e.g. `anim_skel_frame0` / `anim_skel_frame24`).
