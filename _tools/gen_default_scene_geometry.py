#!/usr/bin/env python3
"""Pyxis — bootstrap helper that emits triangle-list USD primitives.

The bundled `resources/scenes/default.usd` needs concrete UsdGeomMesh
prims (M5+'s StageWalker only handles triangle-list meshes; the M3.5
authoring used UsdGeomSphere parametrics that get skipped). This
script emits points / faceVertexCounts / faceVertexIndices arrays for
a small library of platonic shapes, ready to paste into the .usd.

Run once from the repo root; the output is committed:

    py _tools/gen_default_scene_geometry.py

The script is committed for reproducibility (so a future contributor
who wants a different shape library can regenerate easily).
"""

from __future__ import annotations

import math
import sys
from pathlib import Path


def _cube(size: float) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    s = size * 0.5
    pts = [
        (-s, -s, -s), ( s, -s, -s), ( s,  s, -s), (-s,  s, -s),
        (-s, -s,  s), ( s, -s,  s), ( s,  s,  s), (-s,  s,  s),
    ]
    tris = [
        # back -Z (CW from outside)
        (0, 2, 1), (0, 3, 2),
        # front +Z
        (4, 5, 6), (4, 6, 7),
        # left -X
        (0, 7, 3), (0, 4, 7),
        # right +X
        (1, 2, 6), (1, 6, 5),
        # bottom -Y
        (0, 1, 5), (0, 5, 4),
        # top +Y
        (3, 7, 6), (3, 6, 2),
    ]
    return pts, tris


def _octahedron(size: float) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    s = size
    pts = [
        ( s, 0, 0), (-s, 0, 0),
        (0,  s, 0), (0, -s, 0),
        (0, 0,  s), (0, 0, -s),
    ]
    tris = [
        (0, 2, 4), (2, 1, 4), (1, 3, 4), (3, 0, 4),  # top hemisphere
        (2, 0, 5), (1, 2, 5), (3, 1, 5), (0, 3, 5),  # bottom hemisphere
    ]
    return pts, tris


def _tetrahedron(size: float) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    s = size
    pts = [
        ( s,  s,  s),
        ( s, -s, -s),
        (-s,  s, -s),
        (-s, -s,  s),
    ]
    tris = [
        (0, 1, 2), (0, 2, 3), (0, 3, 1), (1, 3, 2),
    ]
    return pts, tris


def _icosahedron(size: float) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    # Golden-ratio icosahedron, 12 vertices + 20 faces. Vertices live
    # on three orthogonal golden rectangles.
    phi = (1.0 + math.sqrt(5.0)) * 0.5
    raw = [
        (-1,  phi, 0), ( 1,  phi, 0), (-1, -phi, 0), ( 1, -phi, 0),
        (0, -1,  phi), (0,  1,  phi), (0, -1, -phi), (0,  1, -phi),
        ( phi, 0, -1), ( phi, 0,  1), (-phi, 0, -1), (-phi, 0,  1),
    ]
    norm = math.sqrt(1.0 + phi * phi)
    pts = [(p[0] * size / norm, p[1] * size / norm, p[2] * size / norm) for p in raw]
    tris = [
        (0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11),
        (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
        (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9),
        (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1),
    ]
    return pts, tris


def _icosphere(size: float, subdivisions: int
               ) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    """Subdivided icosahedron projected back onto a sphere of radius `size`.

    subdivisions = 0 → 12 verts / 20 tris  (raw icosahedron)
    subdivisions = 1 → 42 verts / 80 tris  (visibly spherical)
    subdivisions = 2 → 162 verts / 320 tris (smooth sphere; ~10 KB inlined)

    Each pass splits every triangle into 4 by adding edge-midpoints,
    then normalises every vertex back to radius `size`. Edge-midpoints
    are deduplicated via a (a, b) → index cache so neighbouring tris
    share their split vertex; without dedup the vert count explodes
    AND the rendered mesh shows seam cracks.
    """
    base_pts, base_tris = _icosahedron(size)
    pts: list[list[float]] = [list(p) for p in base_pts]
    tris: list[tuple[int, int, int]] = list(base_tris)

    def _project(idx: int) -> None:
        x, y, z = pts[idx]
        length = math.sqrt(x * x + y * y + z * z)
        if length > 1e-9:
            scale = size / length
            pts[idx] = [x * scale, y * scale, z * scale]

    for vert_idx in range(len(pts)):
        _project(vert_idx)

    for _step in range(subdivisions):
        midpoint_cache: dict[tuple[int, int], int] = {}

        def _midpoint(vert_a: int, vert_b: int) -> int:
            key = (min(vert_a, vert_b), max(vert_a, vert_b))
            cached = midpoint_cache.get(key)
            if cached is not None:
                return cached
            ax, ay, az = pts[vert_a]
            bx, by, bz = pts[vert_b]
            mid_idx = len(pts)
            pts.append([(ax + bx) * 0.5, (ay + by) * 0.5, (az + bz) * 0.5])
            _project(mid_idx)
            midpoint_cache[key] = mid_idx
            return mid_idx

        new_tris: list[tuple[int, int, int]] = []
        for (vert_a, vert_b, vert_c) in tris:
            mid_ab = _midpoint(vert_a, vert_b)
            mid_bc = _midpoint(vert_b, vert_c)
            mid_ca = _midpoint(vert_c, vert_a)
            new_tris.append((vert_a, mid_ab, mid_ca))
            new_tris.append((vert_b, mid_bc, mid_ab))
            new_tris.append((vert_c, mid_ca, mid_bc))
            new_tris.append((mid_ab, mid_bc, mid_ca))
        tris = new_tris

    return [(p[0], p[1], p[2]) for p in pts], tris


def _format_points(pts: list[tuple[float, float, float]]) -> str:
    return "[" + ", ".join(f"({x:.4f}, {y:.4f}, {z:.4f})" for (x, y, z) in pts) + "]"


def _format_indices(tris: list[tuple[int, int, int]]) -> str:
    flat = []
    for (a, b, c) in tris:
        flat.extend([a, b, c])
    return "[" + ", ".join(str(i) for i in flat) + "]"


def _format_face_counts(num_tris: int) -> str:
    return "[" + ", ".join("3" for _ in range(num_tris)) + "]"


def emit_mesh(name: str, points: list[tuple[float, float, float]],
              tris: list[tuple[int, int, int]]) -> str:
    return (
        f"        # {name}: {len(points)} verts, {len(tris)} triangles\n"
        f"        int[] faceVertexCounts = {_format_face_counts(len(tris))}\n"
        f"        int[] faceVertexIndices = {_format_indices(tris)}\n"
        f"        point3f[] points = {_format_points(points)}\n"
    )


def main() -> int:
    print("# ---- Cube ----")
    pts, tris = _cube(0.6)
    print(emit_mesh("Cube (size=0.6)", pts, tris))

    print("# ---- Octahedron ----")
    pts, tris = _octahedron(0.45)
    print(emit_mesh("Octahedron (radius=0.45)", pts, tris))

    print("# ---- Tetrahedron ----")
    pts, tris = _tetrahedron(0.4)
    print(emit_mesh("Tetrahedron (extent=0.4)", pts, tris))

    print("# ---- Icosphere (subdivision 1) ----")
    pts, tris = _icosphere(0.45, subdivisions=1)
    print(emit_mesh("Icosphere subdiv=1 (radius=0.45)", pts, tris))

    print("# ---- Icosahedron ----")
    pts, tris = _icosahedron(0.45)
    print(emit_mesh("Icosahedron (radius=0.45)", pts, tris))

    return 0


if __name__ == "__main__":
    sys.exit(main())
