# Snow Deformation

An Unreal Engine 5 plugin that simulates a persistent, world-anchored snow
height field on the GPU and lets characters (or anything else) press
footprints into it as they walk. Two compute shader passes run every frame;
a game-thread subsystem drives them and a lightweight actor component turns
foot-bone contacts into GPU input.

Built as a from-scratch companion to the `ComputeShaderV1` learning plugin
in this project - it follows the same low-level RDG / global-shader pattern
(`FGlobalShader` + `SHADER_PARAMETER_STRUCT` + `FComputeShaderUtils::AddPass`
+ `ENQUEUE_RENDER_COMMAND`), taken further into a complete, self-contained
gameplay feature: persistent ping-pong state, a structured buffer of
deformers, a component/subsystem API, and material wiring that needs no
Blueprint at all.

Developed against **UE 5.8**.

## How it works

```
USnowDeformerComponent (per character)
        │  GetActiveContacts() - traces each foot socket to ground,
        │  reports world-space presses this frame
        ▼
USnowDeformationSubsystem::Tick()   (game thread, once per world per frame)
        │  1. moves the simulated region to follow the focus actor
        │  2. collects contacts from every registered component
        │  3. converts them to FSnowDeformerGPU (region-local space)
        │  4. ENQUEUE_RENDER_COMMAND → SnowDeformation::Dispatch_RenderThread
        │  5. pushes SnowRegion into the parameter collection / materials
        ▼
SnowDeformation.usf (render thread, GPU)
        │  Pass 1 SnowAccumulateCS: reproject last frame's height field into
        │          this frame's region, relax it back towards flat, stamp
        │          every deformer (max() keeps the deepest print ever made)
        │  Pass 2 SnowNormalsCS:    height field → RGBA texture the material
        │          samples (R = height, GB = normal.xy)
        ▼
SnowData render target  →  your snow material's WPO / normal input
```

### Why a "region" instead of a world-sized texture

The height field is a fixed-resolution texture (`TextureResolution`, default
1024²) covering a square of world space (`RegionSizeWorld`, default 4096
units) centred on a focus actor (the local player by default). Each frame
the accumulate pass re-samples last frame's buffer at an offset equal to how
far the focus actor moved, so existing tracks stay put in the world while
the window scrolls with the player - the same trick trail/decal systems use
to get effectively unbounded world coverage from a small, cheap texture.
Snow outside the current region simply isn't simulated.

### Persistence and refill

`OutHeightTexture[Pixel] = max(Height, print)` for the core of a print, so a
deeper, more recent press always wins over an older, shallower one at the
same texel - that's what makes a walked trail persist. `RefillRate` slowly
relaxes height back towards zero over time, standing in for fresh snowfall
filling old tracks back in.

### When it doesn't run

The simulation skips its dispatch entirely while the region is stationary,
nothing is pressing into the snow, and the field has finished healing. A
moving region always dispatches - the texture is anchored to the world, so a
move has to be reprojected or every existing track would slide along with
the player. With `RefillRate` at 0 (permanent tracks) an idle scene costs
nothing at all.

## Module layout

```
SnowDeformation/
├── SnowDeformation.uplugin
├── Resources/Icon128.png
├── Shaders/Private/SnowDeformation.usf        - SnowAccumulateCS, SnowNormalsCS
└── Source/SnowDeformation/
    ├── Public/SnowDeformation.h               - module: maps Shaders/ to /SnowDeformationShaders
    ├── Public/SnowDeformationComputePass.h    - FSnowDeformerGPU, dispatch params, shader classes
    ├── Private/SnowDeformationComputePass.cpp - shader registration + Dispatch_RenderThread (RDG)
    ├── Public/SnowDeformationSettings.h       - UDeveloperSettings: tuning + material wiring assets
    ├── Private/SnowDeformationSettings.cpp
    ├── Public/SnowDeformationSubsystem.h      - UTickableWorldSubsystem: owns the sim state
    ├── Private/SnowDeformationSubsystem.cpp
    ├── Public/SnowDeformerComponent.h         - UActorComponent: foot-socket ground traces
    └── Private/SnowDeformerComponent.cpp
```

## Setting it up in a level

1. Drop this folder into `<Project>/Plugins/` and enable it in the Plugins
   browser (or add it to your `.uproject`'s `Plugins` array).
2. Add a **Snow Deformer** component to your character Blueprint/class. Set
   `FootSocketNames` to your skeleton's actual foot bone/socket names
   (default `foot_l` / `foot_r`). For a pawn with no skeletal mesh, set
   `SimpleContactFallbackRadius` instead and it'll stamp a single print under
   the actor's bounds while moving.
3. Tune the simulation in **Project Settings → Plugins → Snow Deformation**.
   A world subsystem has no details panel of its own, so that page is where
   the knobs actually live.
4. Wire the output into a material - see below.
5. Optional: call `StampSnowAt(WorldLocation, Radius, Depth, ...)` on the
   subsystem for anything that isn't a character - explosions, vehicle
   wheels, dropped props.

## Material wiring

The simulation produces one RGBA texture:

| Channel | Meaning |
|---|---|
| `R` | Height. `0` = undisturbed, `1` = pressed all the way down by `MaxDepthWorld`, **negative** = the rim of displaced snow pushed up around a print. |
| `G`, `B` | World-space normal X and Y. Z is reconstructed in the material. |

…and one vector, `SnowRegion`, that places that texture in the world:

| Channel | Meaning |
|---|---|
| `R`, `G` | Region centre, world XY |
| `B` | Region size, world units |
| `A` | Max depth, world units |

### Getting them into the material

A material can't reference a texture that only exists at runtime, which is
the whole difficulty here. There are three ways around it, in order of how
little work they need:

**1. Assets in Project Settings (no Blueprint at all).** This is the
recommended route.

- Create a **Texture Render Target 2D** asset. Set *Render Target Format* to
  **RGBA16f** and tick **Can Create UAV**. Its size decides the simulation
  resolution, so 1024×1024 is a good start.
- Create a **Material Parameter Collection** asset with one **vector**
  parameter named `SnowRegion`.
- Assign both under *Project Settings → Plugins → Snow Deformation →
  Material Wiring*.

The subsystem now renders straight into your asset and refreshes
`SnowRegion` every frame. Your material samples the render target with a
plain Texture Sample node and reads `SnowRegion` with a Collection Parameter
node. Nothing else to do.

The signed height is why the format matters: a fixed-point target clips the
negative rim away and you lose the ridge of snow around each print. The
plugin warns in the log if the format is wrong, and force-enables the UAV
flag if you forget it.

**2. A dynamic material instance.** If you'd rather not add assets, call
`Register Snow Material` on the subsystem with your MID and it keeps the
`SnowData` (texture) and `SnowRegion` (vector) parameters current every
frame. `Unregister Snow Material` when you're done; the subsystem holds it
weakly, so a garbage-collected MID drops out on its own.

**3. Read it yourself.** `Get Snow Data Render Target`,
`Get Snow Region Parameter`, `Get Region Center`, `Get Region Size` and
`Get Max Depth` are all Blueprint-pure if you want to drive something custom.

### The material graph

With `SnowData` (texture) and `SnowRegion` (float4) in hand:

```
UV      = (AbsoluteWorldPosition.xy - SnowRegion.rg) / SnowRegion.b + 0.5
Snow    = TextureSample(SnowData, UV)          // use a Clamp sampler
Height  = Snow.r
NormalXY= Snow.gb
```

Then:

- **World Position Offset** ← `float3(0, 0, -Height * SnowRegion.a)`.
  Negative because a height of 1 means *pressed down*. This needs a mesh with
  enough vertices to deform - tessellate your snow plane, or use Nanite with
  displacement.
- **Normal** ← `float3(NormalXY, sqrt(saturate(1 - dot(NormalXY, NormalXY))))`.
  These are **world-space** normals, so untick *Tangent Space Normal* in the
  material's details panel or they'll be interpreted wrongly.

Fade the effect out at the region edge so tracks don't smear where the
texture runs out:

```
Fade = saturate(min(UV, 1 - UV) * 16)
Mask = Fade.x * Fade.y
```

…then multiply `Height` and `NormalXY` by `Mask`.

If you'd rather do it in one node, a **Custom** node with inputs
`SnowData` (Texture Object), `SnowRegion` (float4) and `WorldPos` (float3),
with an *Additional Output* `Normal` of type float3:

```hlsl
float2 UV     = (WorldPos.xy - SnowRegion.xy) / SnowRegion.z + 0.5f;
float2 Fade   = saturate(min(UV, 1.0f - UV) * 16.0f);
float  Mask   = Fade.x * Fade.y;
float4 Snow   = Texture2DSample(SnowData, SnowDataSampler, saturate(UV));
Normal        = float3(Snow.gb * Mask, 0.0f);
Normal.z      = sqrt(saturate(1.0f - dot(Normal.xy, Normal.xy)));
return Snow.r * Mask;   // height, 0..1
```

## Tuning

Simulation-wide settings live in *Project Settings → Plugins → Snow
Deformation* and are also writable at runtime on the subsystem:

| Property | Effect |
|---|---|
| `TextureResolution` | Height-field texel density. Higher = crisper prints, more GPU cost. Ignored when an output render target asset is assigned - that asset's size wins. |
| `RegionSizeWorld` | World-space area covered. Bigger = trails persist further from the player, but each texel covers more ground. |
| `MaxDepthWorld` | World units a fully-pressed footprint sinks. |
| `RefillRate` | How fast tracks fill back in (snow "healing"). 0 = permanent tracks, and a cheaper idle scene. |
| `NormalStrength` | Scales the slope baked into the output normals. |

Per-character settings live on `USnowDeformerComponent`:

| Property | Effect |
|---|---|
| `FootSocketNames` | Which sockets/bones to trace from. |
| `TraceDownDistance` / `ContactHeight` | How far to look for ground, and how close counts as "planted". |
| `FootprintRadius` / `Depth` / `Falloff` | Shape of the pressed core of a print. |
| `RimWidth` / `RimHeight` | Ring of snow pushed up around the print's edge. |
| `MinSpeedForFullStrength` | Speed at which prints reach full depth; slower still presses, just lighter. |
| `SimpleContactFallbackRadius` | Single print under the actor's bounds when there's no foot rig. 0 disables. |
| `TraceChannel` | Collision channel the ground trace uses. |

## Known limitations

- Single region, single focus actor per world - fine for a single-player
  snow area; a splitscreen/large-open-world setup would need multiple
  regions or a much bigger `RegionSizeWorld`.
- No replication: this is a client-side visual effect. For a multiplayer
  game you'd replicate the contact list (or trust each client to trace its
  own local pawn - snow indentation is cosmetic, so this is the recommended
  option that's already how the component is built) rather than the height
  field itself.
- Ground contact for the skeletal path is a straight-down line trace against
  `TraceChannel` (defaults to `WorldStatic`); tune it or add a dedicated
  collision channel if it picks up the wrong geometry.
- Deformation is a displacement of *existing* geometry, so the snow mesh
  needs enough vertices (tessellation or Nanite displacement) for the WPO to
  show. A flat two-triangle plane will not visibly deform.
- The region's height field is not saved: tracks are rebuilt from scratch on
  level load.

## License

Copyright Matin. All rights reserved.
