// Pyxis renderer — radiance_cache.slang: a world-space Spatial Hash Radiance
// Cache (SHARC-style), CLEAN-ROOM implementation.
//
// PROVENANCE / LICENSING (important): this is an independent implementation of
// the *published* spatial-hash-radiance-cache algorithm (NVIDIA RTXGI 2.0
// "SHaRC", Ha/Boksansky/et al.). It was written from the public algorithm
// description (the SHaRC Integration guide) — NOT copied from NVIDIA's
// SharcCommon.h / HashGridCommon.h, whose NVIDIA RTX SDK license (§4(e)) is
// incompatible with Pyxis's Apache-2.0 license. Every line here is ours. The
// data layout, hash function, packing, and resolve math are Pyxis-native
// choices, deliberately simpler than the reference (fp32 not fp16, no SH
// encoding, no responsive-lighting, no 64-bit-atomic path) — appropriate for
// Pyxis's static-camera, converged (N-frame accumulation) headless renders.
//
// WHY (rtx-realtime-alignment-design.md, 2026-07-07): ovrtx's RTX-RT indirect
// diffuse GI uses a radiance cache for infinite-bounce lighting; Pyxis's
// IndirectDiffusePass truncates at 2 bounces + dome-ambient. This module gives
// the indirect path an infinite-bounce term via a world-space cache, gated on
// RealTimeQuality::passMask bit 9 (PASS_MASK_SHARC_GI) / SPEC_ID_SHARC_GI so
// the builtin path stays byte-identical when off. It is an OPTIONAL render
// mode — "either our current way, either sharc".
//
// ALGORITHM (3 phases, per the SHaRC design):
//   1. UPDATE  — while path tracing, at each eligible bounce vertex, evaluate
//      local radiance (direct + emission + the cache's own infinite-bounce
//      term at the *next* vertex) and atomically accumulate it into the
//      vertex's voxel cell (this frame's running sum + sample count).
//   2. RESOLVE — a compute pass folds this frame's per-cell sum into a
//      cross-frame running mean (temporal accumulation, capped at
//      SHARC_ACCUM_FRAME_MAX), then clears the per-frame sum.
//   3. QUERY   — at a non-primary bounce hit, read the resolved cell radiance
//      and terminate the path early with it (infinite bounce, cheaply).
//
// GRID: multi-resolution world-space voxels. Cell size grows with distance
// from the camera (constant screen-space footprint), so near geometry gets
// fine cells and far geometry coarse ones. The (quantized position, level)
// tuple is spatially hashed into a fixed-capacity table with a stored 32-bit
// checksum + short linear probe for collision resolution.

#ifndef PYXIS_RADIANCE_CACHE_SLANG
#define PYXIS_RADIANCE_CACHE_SLANG

// ---- tunables ------------------------------------------------------------
// Capacity: number of voxel cells (power of two). 2^22 = 4,194,304 cells.
// Table buffers: hash (4B) + accum (16B) + resolved (16B) = 36B/cell ≈ 150 MiB.
// Raised from 2^21 for the finer grid (GRID_DENSITY 0.5) + trilinear query —
// finer cells occupy more of the table, so more capacity avoids probe-fail
// collisions (which would show as dark specks in the interpolated result).
static const uint  SHARC_CAPACITY        = 1u << 22;
static const uint  SHARC_CAPACITY_MASK   = SHARC_CAPACITY - 1u;
// Linear-probe length on collision before giving up (miss = cache absent).
static const uint  SHARC_PROBE_COUNT     = 8u;
// Radiance is premultiplied by this and accumulated in 32-bit ints per
// component. Sized so scale · clamp(radiance) · (samples/cell/frame) < 2^32.
static const float SHARC_RADIANCE_SCALE  = 256.0;
static const float SHARC_RADIANCE_CLAMP  = 8192.0;   // per-component store cap
// Temporal running-mean cap (larger = higher quality, slower response).
static const float SHARC_ACCUM_FRAME_MAX = 64.0;
// Grid level-of-detail. cellSize(d) = SHARC_VOXEL_BASE · logBase^level, where
// level = floor(log_logBase(d · SHARC_GRID_DENSITY)). Tuned for World Lobby's
// metre-scale interior (a few cm near-camera, growing with depth).
static const float SHARC_LOG_BASE        = 2.0;
// voxelSize ~= VOXEL_BASE·GRID_DENSITY as a fraction of camera distance, stepped
// by powers of LOG_BASE. LOWER GRID_DENSITY => finer cells (0.015·2 = ~3% of
// distance here). Finer = sharper reflected detail (reflections read this cache),
// at the cost of more occupied cells (raise SHARC_CAPACITY if collisions appear).
static const float SHARC_GRID_DENSITY    = 0.25;     // lower = finer cells; trilinear query averages 8 => finer viable
static const float SHARC_VOXEL_BASE      = 0.015;    // metres, finest cell edge
static const int   SHARC_LEVEL_MIN       = 0;
static const int   SHARC_LEVEL_MAX       = 16;
// Sentinel checksum for an empty cell.
static const uint  SHARC_EMPTY           = 0u;

// ---- grid ----------------------------------------------------------------
// Distance-based level: coarser cells further from the camera.
int SharcGridLevel(float3 posWorld, float3 camWorld)
{
    const float dist = max(distance(posWorld, camWorld), 1e-4);
    const float l = log(dist * SHARC_GRID_DENSITY) / log(SHARC_LOG_BASE);
    return clamp(int(floor(l)), SHARC_LEVEL_MIN, SHARC_LEVEL_MAX);
}

float SharcVoxelSize(int level)
{
    return SHARC_VOXEL_BASE * pow(SHARC_LOG_BASE, float(level));
}

// Integer 3D voxel coordinate at a level (signed, biased into unsigned range).
int3 SharcGridCoord(float3 posWorld, int level)
{
    const float inv = 1.0 / SharcVoxelSize(level);
    return int3(floor(posWorld * inv));
}

// 32-bit integer hash (PCG-style finalizer) of the voxel key.
uint SharcHashU32(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Combine the (coord, level) tuple into a 32-bit key, then hash. The raw key
// doubles as the stored checksum so a probed slot can be validated.
// Dominant-axis normal octant (6 directions: +X,-X,+Y,-Y,+Z,-Z). Folded into
// the cell key (NVIDIA SHaRC parity, RTX-alignment 2026-07-10): without a
// normal term, OPPOSITE and PERPENDICULAR faces crossing the same voxel share
// one cache cell, and bright energy leaks around corners/through walls — the
// measured symptom was interior-facing surfaces (the lobby's concrete column
// shadow side) over-filled +0.117 luma vs ovrtx by floor/window cells bleeding
// into them. Keying by normal octant separates them at ZERO layout cost (the
// key/checksum is hashed anyway); capacity pressure (up to 6x key space) is
// absorbed by the 2^22 table.
uint SharcNormalOctant(float3 n)
{
    const float3 a = abs(n);
    if (a.x >= a.y && a.x >= a.z) return (n.x >= 0.0) ? 0u : 1u;
    if (a.y >= a.z)               return (n.y >= 0.0) ? 2u : 3u;
    return (n.z >= 0.0) ? 4u : 5u;
}

uint SharcVoxelKey(int3 c, int level, uint octant)
{
    // Bias coords into unsigned; pack low bits of each axis + level + octant.
    const uint ux = uint(c.x + 0x40000000);
    const uint uy = uint(c.y + 0x40000000);
    const uint uz = uint(c.z + 0x40000000);
    uint k = ux * 0x9e3779b1u;
    k = (k ^ (uy + 0x85ebca6bu)) * 0xc2b2ae35u;
    k = (k ^ (uz + 0x27d4eb2fu)) * 0x165667b1u;
    k = (k ^ (uint(level) * 0x9e3779b1u));
    k = (k ^ ((octant + 1u) * 0x85ebca6bu));
    return k;
}

// ---- buffer access helpers (byte-address, atomic-friendly) ---------------
// hashEntries: 4 bytes/cell  (uint32 checksum; SHARC_EMPTY = free)
// accum      : 16 bytes/cell (uint32 r,g,b scaled-int sum + uint32 sampleCount)
// resolved   : 16 bytes/cell (float32 r,g,b resolved radiance + float32 frameW)

// Locate the slot for a world position + surface orientation; returns
// SHARC_CAPACITY on failure. `insert` = true allocates an empty slot (the
// UPDATE path). `octant` = SharcNormalOctant(surface normal).
uint SharcFindSlot(RWByteAddressBuffer hashEntries, float3 posWorld, float3 camWorld,
                   uint octant, bool insert, out uint outChecksum)
{
    const int level = SharcGridLevel(posWorld, camWorld);
    const int3 coord = SharcGridCoord(posWorld, level);
    uint checksum = SharcVoxelKey(coord, level, octant);
    if (checksum == SHARC_EMPTY) checksum = 1u;  // avoid the empty sentinel
    outChecksum = checksum;

    const uint base = SharcHashU32(checksum) & SHARC_CAPACITY_MASK;
    [loop]
    for (uint i = 0u; i < SHARC_PROBE_COUNT; ++i)
    {
        const uint slot = (base + i) & SHARC_CAPACITY_MASK;
        const uint byteOff = slot * 4u;
        if (insert)
        {
            uint prev;
            hashEntries.InterlockedCompareExchange(byteOff, SHARC_EMPTY, checksum, prev);
            if (prev == SHARC_EMPTY || prev == checksum)
                return slot;  // we claimed it, or it was already ours
        }
        else
        {
            const uint stored = hashEntries.Load(byteOff);
            if (stored == checksum) return slot;
            if (stored == SHARC_EMPTY) return SHARC_CAPACITY;  // empty run ends the probe
        }
    }
    return SHARC_CAPACITY;
}

// UPDATE: accumulate a radiance sample into the cell at (posWorld, normal octant).
void SharcAccumulate(RWByteAddressBuffer hashEntries, RWByteAddressBuffer accum,
                     float3 posWorld, float3 camWorld, float3 normalWorld, float3 radiance)
{
    uint checksum;
    const uint slot = SharcFindSlot(hashEntries, posWorld, camWorld,
                                    SharcNormalOctant(normalWorld), /*insert*/ true, checksum);
    if (slot >= SHARC_CAPACITY) return;
    const float3 r =
        clamp(radiance, float3(0.0, 0.0, 0.0), float3(SHARC_RADIANCE_CLAMP, SHARC_RADIANCE_CLAMP,
                                                      SHARC_RADIANCE_CLAMP))
        * SHARC_RADIANCE_SCALE;
    const uint byteOff = slot * 16u;
    uint prev;
    accum.InterlockedAdd(byteOff,       uint(r.x), prev);
    accum.InterlockedAdd(byteOff + 4u,  uint(r.y), prev);
    accum.InterlockedAdd(byteOff + 8u,  uint(r.z), prev);
    accum.InterlockedAdd(byteOff + 12u, 1u,        prev);
}

// Read ONE cell's resolved radiance by (coord, level). Returns false if the
// cell is empty / unresolved. Buffers are UAVs (RW) so a query pass can share
// the update binding set; UAV .Load reads are valid.
bool SharcReadCellAt(RWByteAddressBuffer hashEntries, RWByteAddressBuffer resolved,
                     int3 coord, int level, uint octant, out float3 radiance)
{
    radiance = float3(0.0, 0.0, 0.0);
    uint checksum = SharcVoxelKey(coord, level, octant);
    if (checksum == SHARC_EMPTY) checksum = 1u;
    const uint base = SharcHashU32(checksum) & SHARC_CAPACITY_MASK;
    [loop]
    for (uint i = 0u; i < SHARC_PROBE_COUNT; ++i)
    {
        const uint slot = (base + i) & SHARC_CAPACITY_MASK;
        const uint stored = hashEntries.Load(slot * 4u);
        if (stored == SHARC_EMPTY) return false;      // empty run ends the probe
        if (stored == checksum)
        {
            const float4 res = asfloat(resolved.Load4(slot * 16u));
            if (res.w < 0.5) return false;            // allocated but not yet resolved
            radiance = res.xyz;
            return true;
        }
    }
    return false;
}

// QUERY: TRILINEARLY-interpolated resolved radiance at posWorld (full-SHARC v2).
// Samples the 8 cells whose CENTERS surround posWorld and blends by proximity,
// removing the "blocky" per-cell steps the single-cell lookup produced in
// reflections (user-reported grain / dropped detail). Empty corners are skipped
// and the remaining weights renormalized, so a partially-populated
// neighbourhood still returns a smooth value. Returns false only if ALL 8
// corners are empty (caller falls back to its own path).
bool SharcQuery(RWByteAddressBuffer hashEntries, RWByteAddressBuffer resolved,
                float3 posWorld, float3 camWorld, float3 normalWorld, out float3 radiance)
{
    radiance = float3(0.0, 0.0, 0.0);
    const int level = SharcGridLevel(posWorld, camWorld);
    const float voxel = SharcVoxelSize(level);
    const uint octant = SharcNormalOctant(normalWorld);
    // Cell-center space: cell C's centre is (C+0.5)*voxel, so shift by -0.5.
    const float3 g = posWorld / voxel - 0.5;
    const int3 baseC = int3(floor(g));
    const float3 f = g - float3(baseC);
    float3 sum = float3(0.0, 0.0, 0.0);
    float  wsum = 0.0;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const int3 o = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const float3 wv = float3(o.x == 1 ? f.x : 1.0 - f.x,
                                 o.y == 1 ? f.y : 1.0 - f.y,
                                 o.z == 1 ? f.z : 1.0 - f.z);
        const float w = wv.x * wv.y * wv.z;
        if (w < 1e-4) continue;
        float3 r;
        if (SharcReadCellAt(hashEntries, resolved, baseC + o, level, octant, r))
        {
            sum += w * r;
            wsum += w;
        }
    }
    if (wsum < 1e-3) return false;
    radiance = sum / wsum;
    return true;
}

#endif  // PYXIS_RADIANCE_CACHE_SLANG
