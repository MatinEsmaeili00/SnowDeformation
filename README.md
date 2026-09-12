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
deformers, and a component/subsystem API instead of a single one-shot pass.

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
        ▼
SnowDeformation.usf (render thread, GPU)
        │  Pass 1 SnowAccumulateCS: reproject last frame's height field into
        │          this frame's region, relax it back towards flat, stamp
        │          every deformer (max() keeps the deepest print ever made)
        │  Pass 2 SnowNormalsCS:    height field → RGBA texture the material
        │          samples (R = height, GB = normal.xy)
        ▼
GetSnowDataRenderTarget()  →  your snow material's WPO / normal input
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

## Module layout

```
SnowDeformation/
├── SnowDeformation.uplugin
├── Shaders/Private/SnowDeformation.usf        - SnowAccumulateCS, SnowNormalsCS
└── Source/SnowDeformation/
    ├── Public/SnowDeformation.h               - module: maps Shaders/ to /SnowDeformationShaders
    ├── Public/SnowDeformationComputePass.h     - FSnowDeformerGPU, dispatch params, shader classes
    ├── Private/SnowDeformationComputePass.cpp  - shader registration + Dispatch_RenderThread (RDG)
    ├── Public/SnowDeformationSubsystem.h       - UTickableWorldSubsystem: owns the sim state
    ├── Private/SnowDeformationSubsystem.cpp
    ├── Public/SnowDeformerComponent.h          - UActorComponent: foot-socket ground traces
    └── Private/SnowDeformerComponent.cpp
```

## Setting it up in a level

1. Add the plugin to your project (drop this folder into `<Project>/Plugins/`)
   and enable it in the Plugins browser, or add it to your `.uproject`'s
   `Plugins` array.
2. Add a **Snow Deformer** component to your character Blueprint/class. Set
   `FootSocketNames` to your skeleton's actual foot bone/socket names (default
   `foot_l` / `foot_r`). For a pawn with no skeletal mesh, set
   `SimpleContactFallbackRadius` instead and it'll stamp a single print under
   the actor's bounds while moving.
3. Get `USnowDeformationSubsystem` from the world (native "Get World
   Subsystem" Blueprint node, class `Snow Deformation Subsystem`) and read
   `GetSnowDataRenderTarget()`, `GetRegionCenter()`, `GetRegionSize()`,
   `GetMaxDepth()`.
4. In your snow material, sample the render target with:
   ```
   UV = (AbsoluteWorldPosition.xy - RegionCenter) / RegionSize + 0.5
   Height = tex.r          // 0 flat .. 1 fully pressed, negative = rim
   NormalXY = tex.gb
   ```
   Drive **World Position Offset** with `-Height * MaxDepth` on Z, and blend
   `NormalXY` into your surface normal. Push `RegionCenter` / `RegionSize` in
   as a Material Parameter Collection or per-instance dynamic parameters
   updated once a frame (e.g. via a small tick on the same actor that owns
   the subsystem query), since they change as the player moves.
5. Optional: call `StampSnowAt(WorldLocation, Radius, Depth, ...)` on the
   subsystem for anything that isn't a character - explosions, vehicle
   wheels, dropped props.

## Tuning

All of the knobs below live on `USnowDeformationSubsystem` (simulation-wide)
or `USnowDeformerComponent` (per-character):

| Property | Effect |
|---|---|
| `TextureResolution` | Height-field texel density. Higher = crisper prints, more GPU cost. |
| `RegionSizeWorld` | World-space area covered. Bigger = trails persist further from the player, but each texel covers more ground. |
| `MaxDepthWorld` | World units a fully-pressed footprint sinks. |
| `RefillRate` | How fast tracks fill back in (snow "healing"). 0 = permanent tracks. |
| `FootprintRadius` / `Depth` / `Falloff` | Shape of the pressed core of a print. |
| `RimWidth` / `RimHeight` | Ring of snow pushed up around the print's edge. |
| `MinSpeedForFullStrength` | Speed at which prints reach full depth; slower still presses, just lighter. |

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
- Material wiring (WPO / normal sampling) is left to the target project's
  material graph rather than shipped as a fixed asset, since every project's
  snow shading is different.

## License

Copyright Matin. All rights reserved.
