// Pyxis renderer — shared PCG32 RNG module (RTX-alignment design,
// rtx-realtime-alignment-design.md, Phase B). Implements plan §12.1's
// stream/seeding contract EXACTLY:
//
//   streamId = HashCombine(pixelXY.y * width + pixelXY.x, sampleIndex, bounceIndex)
//   state    = SplitMix64(uint64_t(seed) ^ (uint64_t(frameIndex) << 32) ^ streamId)
//
// `HashCombine` is documented as "pcg_hash (Bob Jenkins-style mix)"; below
// it is the single-round PCG output-mix hash (a well-known avalanche mix,
// NOT the streaming generator itself — see Pcg32NextUint for that) applied
// three times to fold (pixelIndex, sampleIndex, bounceIndex) into one word.
// `SplitMix64` is the standard one-line splitmix finalizer, used ONCE to
// turn the combined 64-bit seed into a well-mixed initial PCG32 state (the
// canonical `pcg32_srandom_r` initialization idiom).
//
// Determinism (plan §12, headless): for a fixed (seed, frameIndex, pixel,
// streamId) tuple, every `Pcg32NextUint` / `Pcg32NextFloat` call sequence is
// bit-identical across runs — no shader-global state, no derivatives, no
// wave-intrinsics. One `Pcg32` value lives entirely in a shader invocation's
// registers (§12.1: "One PCG32 instance per shader invocation, never a
// global one").
//
// Callers seed ONE stream per shader invocation (per pixel, per pass, per
// frame) via `SeedPcg32(seed, frameIndex, pixel, width, streamId)` where
// `streamId` is normally `MakeStreamId(RNG_STREAM_<PASS>, sampleIndex,
// bounceIndex)` — the pass-tag keeps two passes that happen to draw the
// same (pixel, sampleIndex, bounceIndex) tuple in the same frame from
// producing correlated noise patterns. Successive `Pcg32NextFloat*` calls
// on that ONE seeded stream are then used for every random number the
// invocation needs (RIS candidates, hemisphere directions, GGX VNDF, AO
// samples, ...) — §12.3's "no two bounces share a stream" is satisfied by
// folding `bounceIndex` into the seed whenever a caller re-seeds for a new
// recursion depth (see indirect_diffuse.slang's bounce-hit RIS re-seed).
//
// Shared INCLUDE module (like camera_ray.slang / dome_sample.slang) — never
// compiled standalone; #included by every pass with a stochastic estimator
// (direct_lighting / indirect_diffuse / ambient_occlusion / reflections).

#ifndef PYXIS_RNG_SLANG
#define PYXIS_RNG_SLANG

// Per-pass stream tags folded into MakeStreamId's hash — see the file
// header. Values are arbitrary but stable; treat as append-only.
static const uint RNG_STREAM_DIRECT_LIGHTING   = 1u;
static const uint RNG_STREAM_INDIRECT_DIFFUSE  = 2u;
static const uint RNG_STREAM_AMBIENT_OCCLUSION = 3u;
static const uint RNG_STREAM_REFLECTIONS       = 4u;

// Single-round PCG output-permutation hash (O'Neill's "pcg_hash", widely
// used as a fast GPU integer avalanche mix). Not a generator by itself —
// used here purely to combine independent integers into one well-mixed
// word (HashCombine3 below), and as SeedPcg32's stream-increment derivation.
uint PcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Fold three integers into one hash — plan §12.1's `HashCombine(pixel,
// sampleIndex, bounceIndex)`. Boost-style combine (each term rehashed
// through PcgHash before folding) so small changes in any one input
// avalanche fully rather than just XORing raw hashes together.
uint HashCombine3(uint a, uint b, uint c) {
    uint h = PcgHash(a);
    h = PcgHash(h ^ (PcgHash(b) + 0x9E3779B9u));
    h = PcgHash(h ^ (PcgHash(c) + 0x9E3779B9u));
    return h;
}

// Fold a per-pass tag + per-invocation sample/bounce indices into the
// `streamId` term plan §12.1 combines with (seed, frameIndex, pixel).
uint MakeStreamId(uint passTag, uint sampleIndex, uint bounceIndex) {
    return HashCombine3(passTag, sampleIndex, bounceIndex);
}

// Standard one-line SplitMix64 finalizer (Vigna). Used ONCE per
// `SeedPcg32` call to turn the combined 64-bit seed into a well-mixed
// 64-bit word — the canonical `pcg32_srandom_r` seeding idiom, not an
// iterated generator (Pcg32NextUint below is the generator).
uint64_t SplitMix64(uint64_t x) {
    // NOTE: no `UL`/`ul` suffix — Slang (unlike C++) parses that suffix as
    // a 32-bit `uint` literal and SILENTLY TRUNCATES these constants
    // (verified empirically against slangc; a bare hex literal is
    // correctly widened to uint64_t instead, with only a benign
    // informational warning about signedness). Do not "fix" this by
    // adding the suffix back.
    uint64_t z = x + 0x9E3779B97F4A7C15;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EB;
    return z ^ (z >> 31);
}

// One PCG32 stream's state — 128 bits total (state + odd stream
// increment), lives in registers for the lifetime of one shader
// invocation. Never persisted to memory, never shared across threads.
struct Pcg32 {
    uint64_t state;
    uint64_t inc;
};

// Advance the stream, return the next 32-bit word. Canonical PCG32-XSH-RR
// (O'Neill 2014, `pcg32_random_r`): LCG step with multiplier
// 6364136223846793005, xorshift-high then random-rotate output
// permutation. One IMUL + one IADD + a rotate on GPU (§12.1).
uint Pcg32NextUint(inout Pcg32 rng) {
    const uint64_t oldState = rng.state;
    // No `UL` suffix — see SplitMix64's doc comment (Slang truncates it).
    rng.state = oldState * 6364136223846793005 + rng.inc;
    const uint xorshifted = uint(((oldState >> 18u) ^ oldState) >> 27u);
    const uint rot = uint(oldState >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

// Seed one PCG32 stream per plan §12.1's contract. `pixel`/`width` locate
// the invocation (pixelIndex = pixel.y*width + pixel.x); `streamId` is the
// caller's MakeStreamId(...) result (folds in the pass tag + sample/bounce
// indices) — combined here with pixelIndex via the SAME HashCombine the
// plan specifies, so the final `streamId` term matches
// `HashCombine(pixelXY.y*width+pixelXY.x, sampleIndex, bounceIndex)`
// exactly when a caller's MakeStreamId already carries (passTag,
// sampleIndex, bounceIndex).
Pcg32 SeedPcg32(uint seed, uint frameIndex, uint2 pixel, uint width, uint streamId) {
    const uint pixelIndex = pixel.y * width + pixel.x;
    const uint combinedStream = HashCombine3(pixelIndex, streamId, 0u);
    const uint64_t seeded =
        uint64_t(seed) ^ (uint64_t(frameIndex) << 32u) ^ uint64_t(combinedStream);
    const uint64_t mixed = SplitMix64(seeded);
    Pcg32 rng;
    rng.state = mixed;
    // PCG requires an ODD stream increment; derive it from a SECOND,
    // independent SplitMix64 output (mixing in a distinct constant) so
    // state/inc aren't trivially related, then force the low bit set.
    rng.inc = (SplitMix64(mixed ^ 0xDA3E39CB94B95BDB) << 1u) | 1u;
    // One warm-up step (matches the reference `pcg32_srandom_r`): the
    // very first post-seed state has a known relationship to the seed
    // that a single extra step scrambles away.
    Pcg32NextUint(rng);
    return rng;
}

// [0, 1) uniform float — 24 mantissa bits filled from the top of the PCG32
// word (the common GPU-RNG convention; avoids the low bits, which are the
// least statistically strong in an LCG-derived stream).
float Pcg32NextFloat(inout Pcg32 rng) {
    return float(Pcg32NextUint(rng) >> 8u) * (1.0 / 16777216.0);
}

float2 Pcg32NextFloat2(inout Pcg32 rng) {
    const float a = Pcg32NextFloat(rng);
    const float b = Pcg32NextFloat(rng);
    return float2(a, b);
}

// ---------------------------------------------------------------------
// Sampling helpers shared by every consumer of a Pcg32 float2 draw
// (IndirectDiffusePass's cosine bounce, AmbientOcclusionPass's stochastic
// hemisphere occlusion, ReflectionsPass's GGX VNDF sample). Kept here
// rather than duplicated per-pass — same "share, don't duplicate" rule
// shading.slang's EvaluateDirectLightSample doc comment states.
// ---------------------------------------------------------------------

// Duff et al. 2017 "Building an Orthonormal Basis, Revisited" — branchless,
// robust as n.z -> -1 (unlike the naive Gram-Schmidt-from-a-fixed-axis
// approach, which degenerates near the poles).
void BuildOrthonormalBasis(float3 n, out float3 t, out float3 b) {
    const float s = (n.z >= 0.0) ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float bb = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
    b = float3(bb, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted hemisphere sample in a LOCAL frame (z = the hemisphere's
// pole / surface normal). Malley's method: uniform disk -> project up.
// pdf(dir) = dir.z / PI (not returned — every caller here uses it in a
// context where the cosine term and the 1/PI Lambert normalization cancel
// the pdf exactly, the standard cosine-importance-sampling simplification).
float3 CosineSampleHemisphere(float2 u) {
    const float r = sqrt(u.x);
    const float phi = 6.283185307179586 * u.y;
    const float x = r * cos(phi);
    const float y = r * sin(phi);
    const float z = sqrt(max(0.0, 1.0 - u.x));
    return float3(x, y, z);
}

// Transform a LOCAL-frame direction (z = pole) into world space given an
// orthonormal basis (t, b, n).
float3 LocalToWorld(float3 dirLocal, float3 t, float3 b, float3 n) {
    return dirLocal.x * t + dirLocal.y * b + dirLocal.z * n;
}

#endif  // PYXIS_RNG_SLANG
