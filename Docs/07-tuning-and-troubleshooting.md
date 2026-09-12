# 7. Tuning & troubleshooting

[← Build it yourself](06-build-it-yourself.md) · [Docs index](README.md)

What each knob actually controls, and symptom → cause for the things that go
wrong.

## Where the settings live

- **Simulation-wide** → *Project Settings → Plugins → Snow Deformation*.
  Also writable at runtime on the subsystem from Blueprint.
- **Per-character** → the **Snow Deformer** component's details panel.

## Simulation settings

| Setting | Default | What it really does |
|---|---|---|
| `TextureResolution` | 1024² | Texel density, with `RegionSizeWorld`. **Ignored when a render target asset is assigned** — that asset's size wins ([why](04-cpp-reference.md#prepareoutputasset)). |
| `RegionSizeWorld` | 4096 | World units the texture covers. The most under-used knob — see below. |
| `MaxDepthWorld` | 20 | World units a fully-pressed print sinks. Also feeds the normal gradient, so it deepens shading even with no displacement. |
| `RefillRate` | 0.02 | Normalised height recovered per second. `0` = permanent tracks **and** a free idle scene ([why](04-cpp-reference.md#the-idle-skip)). |
| `NormalStrength` | 1.5 | Scales XY of the normal before normalising. Pure look control; doesn't touch the height field. |

### `RegionSizeWorld` deserves its own paragraph

Texel size is `RegionSizeWorld / TextureResolution`. At the defaults that's
4096 / 1024 = **4 cm per texel**, so a 44 cm footprint spans about 11 texels —
enough to see, not enough to look sharp.

| Region | Texels per footprint | Trails persist within |
|---|---|---|
| 4096 | ~11 | 20 m of the player |
| 2048 | ~22 | 10 m |
| 1024 | ~44 | 5 m |

Halving the region doubles the sharpness and halves how far behind you tracks
survive. Raising `TextureResolution` to 2048 instead costs 4× the memory and
bandwidth for the same gain. **Shrink the region before you grow the texture.**

## Per-character settings

| Setting | Default | What it really does |
|---|---|---|
| `FootSocketNames` | `foot_l`, `foot_r` | Sockets/bones to trace from. |
| `TraceDownDistance` | 40 | How far below the socket to look for ground. |
| `ContactHeight` | 6 | How close the socket must be to count as planted. **The one that bites** — see below. |
| `FootprintRadius` | 22 | Radius of the pressed core, world units. |
| `FootprintDepth` | 1.0 | 0..1, scaled by `MaxDepthWorld`. |
| `FootprintFalloff` | 2.2 | Edge sharpness exponent. Higher = smaller, crisper print. |
| `RimWidth` / `RimHeight` | 9 / 0.3 | The ridge of displaced snow outside the core. |
| `MinSpeedForFullStrength` | 80 | Speed (cm/s) for a full-strength print. Never fades below 0.35. |
| `SimpleContactFallbackRadius` | 0 | Single print under the actor's bounds when there's no foot rig. 0 disables. |
| `TraceChannel` | `WorldStatic` | Channel the ground trace uses. |

---

# Troubleshooting

## Nothing prints on flat ground

**Only get prints stepping off a slope, or landing from a jump.**

`ContactHeight` is too low. The test is:

```cpp
SocketLoc.Z - Hit.Location.Z <= ContactHeight
```

Foot bones sit at the **ankle**, typically 10–15 cm above the sole. With the
default `6`, that distance is already ~10–12 on flat ground, so the foot is
never considered planted. Stepping off a slope briefly brings it under the
threshold — which is exactly the symptom.

**Fix:** `ContactHeight` → **20**, `TraceDownDistance` → **60**.

## Nothing prints at all

Work down this list:

1. Is there a **Snow Deformer** component on the character?
2. Does the log show `Snow height field allocated at 1024x1024`? No line means
   the subsystem never ticked — check you're in Game/PIE, not an editor
   preview world (`DoesSupportWorldType` excludes those).
3. Do `FootSocketNames` match your skeleton's actual bone names?
4. Does the ground block `TraceChannel` (`WorldStatic` by default)?
5. `ContactHeight`, as above.

## Prints appear but the snow looks flat

Almost always **World Position Offset with nothing to displace**. A
`/Engine/BasicShapes/Plane` is four vertices; WPO moves vertices, it doesn't
create them ([detail](05-material-wiring.md#world-position-offset-needs-geometry)).

Either use a subdivided mesh, or lean on shading: raise `NormalStrength` and
`MaxDepthWorld`. Shading-only reads well for footprints.

## Shading looks wrong / lit from the wrong side

***Tangent Space Normal* is still ticked.** The shader outputs **world-space**
normals. Untick it in the material's details panel
([detail](05-material-wiring.md#the-normal-gotcha)).

## Tracks slide along with the player

The reprojection offset is wrong or not being applied. Either the sign of
`RegionOffsetFromPrev` is flipped, or the dispatch is being skipped on a frame
where the region moved — the region must **always** dispatch when it moves
([detail](04-cpp-reference.md#the-idle-skip)).

## The raised rim around prints never shows

The output render target isn't a float format. Heights are **signed** and the
rim is negative, so a fixed-point target clips it to zero. Set the asset to
**RGBA16f**. The log warns about this at startup.

## "Can Create UAV" warning in the log

Expected if you made the render target asset without ticking it. The plugin
force-enables it for the session, because otherwise UAV creation fails and the
field is silently blank. Tick **Can Create UAV** on the asset to silence it.

## Prints land offset from where they should be

If the offset is roughly how far you moved that frame, something converted
world → region-local space using a stale region centre. That's the bug
described in
[`StampSnowAt`](04-cpp-reference.md#stampsnowat-and-why-it-banks-world-space).

## Streaks running off to the horizon

No edge mask in the material. The clamped sampler repeats the boundary texels
outward forever. Add the fade
([detail](05-material-wiring.md#fade-at-the-region-edge)).

## A shader parameter reads as zero on the GPU

The name in `BEGIN_SHADER_PARAMETER_STRUCT` doesn't match the HLSL global
exactly. There's no error for this — check spelling first, always
([detail](02-gpu-compute-pipeline.md#parameter-structs)).

Also check you used `FVector2f`/`FIntPoint` rather than `FVector2D`, which is
double-precision in UE5 and won't match a `float2`.

## Part of the texture never updates

Thread group size and dispatch group count disagree. They're driven from one
shared constant (`SnowDeformation::ThreadGroupSize`) specifically to make this
impossible — if you've forked the code, check both uses
([detail](02-gpu-compute-pipeline.md#declaring-a-compute-shader)).

## Editing the .usf changes nothing

Shaders are cached. `r.ShaderDevelopmentMode=1` and the `recompileshaders
changed` console command, or restart the editor.

Remember that **UBT does not validate HLSL** — a C++ build succeeding says
nothing about your shader
([how to check](06-build-it-yourself.md#a-verification-habit-worth-keeping)).

---

[← Back to the docs index](README.md)
