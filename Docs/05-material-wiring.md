# 5. Material wiring

[← C++ reference](04-cpp-reference.md) · [Docs index](README.md) · Next: [Build it yourself →](06-build-it-yourself.md)

The compute passes produce a texture. Getting it into a material turns out to
be the fiddliest part of the whole plugin, for a reason that isn't obvious
until you hit it.

## Why this is hard

**A material cannot reference an object that only exists at runtime.**

Material asset references are baked when the asset is saved. The simulation's
render target is created with `NewObject` while the game is running — it has
no asset path, so there is nothing for a Texture Sample node to point at. You
can't drag it into a material graph because it doesn't exist until you press
Play.

The second half of the problem: the texture has **no inherent world
position**. It's 1024×1024 texels representing *a square that moves*. A
material sampling it must be told where that square currently is, every frame,
or it has no way to turn a world position into a UV.

So two things must reach the material: a texture that doesn't exist at edit
time, and a vector that changes every frame.

## What gets sent

### The `SnowData` texture

| Channel | Meaning |
|---|---|
| `R` | Height. `0` undisturbed, `1` fully pressed to `MaxDepthWorld`, **negative** = raised rim |
| `G`, `B` | World-space normal X and Y. Z is reconstructed in the material |
| `A` | Unused (written as 1) |

### The `SnowRegion` vector

| Channel | Meaning |
|---|---|
| `R`, `G` | Region centre, world XY |
| `B` | Region size, world units |
| `A` | Max depth, world units |

Four values in one vector rather than four separate parameters: one collection
entry, one `SetVectorParameterValue` per frame, and they're always mutually
consistent because they're written together. Built by
[`GetSnowRegionParameter`](04-cpp-reference.md#pushmaterialparameters).

## The three routes

### Route 1 — assets in Project Settings (recommended)

Sidesteps the runtime-texture problem by making the texture *not* be runtime.
You create a render target **asset**; the simulation renders into that instead
of making its own. Now it has an asset path, and a material can reference it
normally.

1. Create a **Texture Render Target 2D**. Format **RGBA16f**, tick **Can Create
   UAV**. Its size sets the simulation resolution.
2. Create a **Material Parameter Collection** with one **vector** parameter
   named `SnowRegion`.
3. Assign both in *Project Settings → Plugins → Snow Deformation → Material
   Wiring*.

The material then uses a plain Texture Sample and a Collection Parameter node.
No Blueprint anywhere.

Why RGBA16f specifically: heights are **signed** (the rim is negative), so a
fixed-point format clips the rim to zero and you lose the ridge around every
print. The plugin warns if the format is wrong and force-enables the UAV flag
if you forget it — [`PrepareOutputAsset`](04-cpp-reference.md#prepareoutputasset).

### Route 2 — a dynamic material instance

If you'd rather not add assets, hand a MID to the subsystem and it keeps the
parameters current:

```cpp
Subsystem->RegisterSnowMaterial(MyDynamicMaterialInstance);
```

The material needs a **texture parameter** named `SnowData` and a **vector
parameter** named `SnowRegion` (both names configurable in settings). Works
against the transient target, so no assets and no configuration at all.

MIDs are held **weakly**, so a garbage-collected instance drops out on its own
rather than keeping itself alive or dangling.

### Route 3 — read it yourself

`GetSnowDataRenderTarget`, `GetSnowRegionParameter`, `GetRegionCenter`,
`GetRegionSize`, `GetMaxDepth` are all Blueprint-pure. Use these if you're
driving something that isn't a standard material.

## The material graph

```
UV       = (AbsoluteWorldPosition.xy - SnowRegion.rg) / SnowRegion.b + 0.5
Snow     = TextureSample(SnowData, UV)        // Clamp sampler
Height   = Snow.r
NormalXY = Snow.gb
```

That UV line is the inverse of the shader's
[texel → world mapping](03-shader-walkthrough.md#texel--world-position): the
shader goes `UV → world`, the material goes `world → UV`.

### Fade at the region edge

```
Fade = saturate(min(UV, 1 - UV) * 16)
Mask = Fade.x * Fade.y
```

Multiply `Height` and `NormalXY` by `Mask`. Without it, the clamped sampler
smears the last row of texels outward forever and you get streaks running to
the horizon at the region boundary.

### The normal gotcha

```
Normal = float3(NormalXY, sqrt(saturate(1 - dot(NormalXY, NormalXY))))
```

Z is reconstructed because the shader only stores X and Y — for a height field
Z is always positive, so no information is lost.

**These are world-space normals.** X and Y are world axes, Z is world up. You
must **untick *Tangent Space Normal*** in the material's details panel.
Leave it ticked and the values get interpreted in the mesh's tangent basis,
which for a ground plane rotates them roughly 90° — prints look lit from the
wrong direction, or vanish.

This catches everyone exactly once. If your prints show in base colour but the
shading looks wrong, this is it.

### World Position Offset needs geometry

```
WPO = float3(0, 0, -Height * SnowRegion.a)
```

Negative because [height is positive-means-down](03-shader-walkthrough.md#the-height-convention),
and `SnowRegion.a` is `MaxDepthWorld`.

**WPO moves vertices that already exist. It does not create them.** A default
`/Engine/BasicShapes/Plane` is four vertices — there is nothing between the
corners to push down, so it will not visibly dent no matter what you set.
Options:

- a subdivided grid mesh (simplest; a 64×64 plane is plenty),
- a Nanite mesh with displacement,
- or just accept shading-only, which reads surprisingly well for footprints
  because the normals do most of the visual work.

Shading-only is a legitimate choice, not a fallback. Start there, add geometry
when you want silhouettes.

### All of it in one Custom node

If you'd rather not build fifteen nodes — inputs `SnowData` (Texture Object),
`SnowRegion` (float4), `WorldPos` (float3), with an *Additional Output*
`Normal` of type float3:

```hlsl
float2 UV   = (WorldPos.xy - SnowRegion.xy) / SnowRegion.z + 0.5f;
float2 Fade = saturate(min(UV, 1.0f - UV) * 16.0f);
float  Mask = Fade.x * Fade.y;
float4 Snow = Texture2DSample(SnowData, SnowDataSampler, saturate(UV));
Normal      = float3(Snow.gb * Mask, 0.0f);
Normal.z    = sqrt(saturate(1.0f - dot(Normal.xy, Normal.xy)));
return Snow.r * Mask;   // height, 0..1
```

## Making prints read more strongly

Ordered by how much difference they make — full table in
[doc 7](07-tuning-and-troubleshooting.md).

1. **`RefillRate` → 0.** Tracks stop healing. Biggest single change.
2. **`RegionSizeWorld` down.** At 4096 over 1024 texels you get 4 cm per texel
   and a footprint spans ~11 texels. Halving the region doubles the density
   and is the difference between mushy and crisp.
3. **`NormalStrength` up.** Exaggerates the slope; pure look control.
4. **`MaxDepthWorld` up.** Feeds the normal gradient, so it deepens shading
   even with no displacement.
5. **Contrast in the material.** Multiply height by 2–3 before any `saturate`
   that drives colour.

---

Next: [Build it yourself →](06-build-it-yourself.md)
