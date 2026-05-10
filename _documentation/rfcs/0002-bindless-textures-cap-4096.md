# RFC 0002: Bindless texture array cap of 4 096 for v1

- Status: Accepted
- Author(s): Pyxis renderer team
- Created: 2026-05-10
- Last updated: 2026-05-10
- Implementation PRs: milestone/m8a-world-lobby (`ShaderInterop.slang:BINDLESS_TEXTURES_CAP`)

## Summary

Bind material textures via a fixed-size SRV array of capacity
**4 096** at PathTracePass binding 28, instead of the plan §5
`createBindlessLayout` path with `~80 000` slots. v1 bindless is a
pragmatic implementation; the §5 capacity remains the post-v1
target.

## Motivation

Plan §5 specifies a single bindless layout with `Texture_SRV(space=2)`
~80 000 slots via NVRHI's `createBindlessLayout` + `DescriptorTable`.
The M8a UV+sampling pipeline needed bindless textures urgently to
unblock the lobby (169 textures) without the multi-week investment
in true descriptor-table management; the chosen path was a
fixed-size `BindingLayoutItem::Texture_SRV(28).setSize(4096)` in the
existing `set=0` layout.

This works because NVRHI's Vulkan backend applies
`vk::DescriptorBindingFlagBits::ePartiallyBound` to every layout
binding (`vulkan-resource-bindings.cpp:184`), so unbound array
elements are safe as long as the shader doesn't access them. The
closesthit's `mat.flags & MATERIAL_FLAG_HAS_BASE_COLOR_MAP` gate
prevents access; cap-checking `mat.baseColorTex < BINDLESS_TEXTURES_CAP`
is a defence-in-depth.

Sizing rationale:
- v1 §41 milestones target Bistro (~2K textures).
- Lobby has 169 textures.
- Production game-engine scenes (UE marketplace assemblies) typically run 500–3 000 textures.
- 4 096 covers all of the above with comfortable headroom.

## Detailed design

```slang
// resources/shaders/ShaderInterop.slang
static const uint BINDLESS_TEXTURES_CAP = 4096u;
```

```cpp
// sources/pyxis_renderer/Private/Passes/PathTracePass.cpp
nvrhi::BindingLayoutItem::Texture_SRV(28)
    .setSize(shaderinterop::BINDLESS_TEXTURES_CAP),
```

Per-frame `GetOrCreateBindingSet` walks the scene's live textures
and pushes one `Texture_SRV(28, tex).setArrayElement(slot)` per
entry. Slot 0 is bound to the magenta missing-texture fallback. The
M8a free-list slot-recycle path means bindless slot pointers can
churn without the array length changing — `_lastBindlessTextureFingerprint`
in PathTracePass uses an FNV1a-64 over `(count, every live ITexture*)`
to invalidate cached binding sets correctly.

## Alternatives considered

1. **True createBindlessLayout + DescriptorTable (plan §5 canonical)** —
   correct long-term path but requires a separate descriptor set,
   per-frame `writeDescriptorTable` calls instead of binding-set
   rebuilds, and the renderer-internal `SceneResources` accessor
   refactor (RFC 0003). 2–3 weeks of work and gates on the §5
   80K-slot capacity that v1 doesn't need. Rejected for v1; revisit
   when a scene actually demands >4 096 textures.
2. **8 192 cap** — ~2× the 4 096 cost (extra binding-set memory + a
   slightly larger descriptor set layout). No v1 scene needs it.
3. **2 048 cap** — would block Bistro after M9 polish lands texture
   variants. Too tight.

## Drawbacks / risks

- Hard cap. A scene with >4 096 unique textures drops textures past
  the cap; the affected materials fall back to scalar baseColor (the
  closesthit's `mat.baseColorTex < BINDLESS_TEXTURES_CAP` cap check
  prevents OOB sampling).
- Per-frame binding-set walk costs O(textures) on cache miss. Cache
  invalidation triggers on count + slot pointer change; steady-state
  is one `_lastBindings` compare per frame.

## Migration & impact

- No public API change.
- Affected milestones: M8a (within budget), M8b (Bistro: ~2K textures
  fits), M9 (texture variants might push some scenes over — flag for
  re-evaluation if a fixture authors >3 000 textures).
- Post-v1: the §5 80K-cap landing requires this RFC be marked
  "Superseded by NNNN" with the createBindlessLayout RFC.

## Open questions

- Once true bindless lands, should `BINDLESS_TEXTURES_CAP` (the
  shader-side declaration) drop entirely in favour of the bindless
  array's runtime capacity? Likely yes — the cap exists because the
  shader needs to know the array size at compile time, and a true
  bindless layout uses unbounded `Texture2D[]`.
