# 1. Architecture

[← Docs index](README.md) · Next: [The GPU compute pipeline →](02-gpu-compute-pipeline.md)

## The problem

You want a character to leave footprints in snow, and you want those prints to
stay there while the character walks away. Naively that means storing, for
every point of ground in the level, how deep the snow is pressed. That's a
per-world-position value that changes over time — in other words, a texture
covering the whole level.

For a 1 km² level at any useful resolution that texture is enormous, and
almost all of it is empty almost all of the time. So the entire design hinges
on one question: **how do you get the appearance of unbounded coverage out of
a small, fixed-size texture?**

## The central idea: a world-anchored scrolling region

Keep one modest texture — 1024×1024 by default — and say it covers a square of
world space centred on the player, `RegionSizeWorld` units across (4096 by
default). Snow outside that square isn't simulated at all.

The trick is what happens when the player moves. If the texture simply moved
with the player, every track in it would slide along too, glued to the screen
instead of the ground. So each frame, before doing anything else, the shader
**re-samples last frame's texture at an offset equal to how far the player
moved**. A track that was 3 metres behind you is still 3 metres behind you in
world terms; it has just shifted a few texels within the buffer.

```
        frame N                      frame N+1  (player moved right)

   ┌───────────────────┐          ┌───────────────────┐
   │        ▲          │          │     ▲             │
   │     footprint     │   ──►    │  footprint        │   same world position,
   │        @          │          │     @             │   different texel
   └───────────────────┘          └───────────────────┘
   centred on player              centred on player (now further right)
```

This is the same technique trail systems, decal buffers and some virtual
texture schemes use. The cost is fixed no matter how big your level is, and
the only thing you give up is snow memory beyond `RegionSizeWorld / 2` from
the player — which nobody can see anyway.

The derivation of the reprojection maths is in
[doc 3](03-shader-walkthrough.md#reprojection-the-heart-of-the-thing).

## Consequences of that choice

Almost every other design decision falls out of this one.

**The texture must persist between frames.** You're reading last frame's
result to produce this frame's. That means two render targets, ping-ponged —
you cannot read and write the same texture in one compute dispatch. See
[ping-pong buffers](02-gpu-compute-pipeline.md#ping-pong-why-two-height-textures).

**The material needs to know where the region is.** The texture has no
inherent world position; a material sampling it has to be told the current
centre and size to convert world position into UVs. That's the `SnowRegion`
vector, and it's why [material wiring](05-material-wiring.md) is more involved
than "plug in a texture".

**A moving region always has to be reprojected.** The simulation can skip work
when nothing is pressing into the snow, but *not* when the player is moving —
skipping a frame there would leave the texture anchored to a stale centre and
every track would slide. That's the exact condition in
[`Tick`](04-cpp-reference.md#the-idle-skip).

**Work in region-local space, not world space.** A level built 500,000 units
from the origin would push world coordinates into the range where `float`
precision gets coarse, and footprints would visibly quantise. Everything
inside the shader is relative to the region centre, so values stay small
regardless of where the level sits.

## The pieces

```
USnowDeformerComponent (on each character)          ── game thread
        │  "where are my feet touching the ground right now?"
        │  line-traces each foot socket down, reports world-space contacts
        ▼
USnowDeformationSubsystem::Tick()                   ── game thread
        │  1. move the region to follow the focus actor
        │  2. ask every registered component for contacts
        │  3. convert world contacts → region-local FSnowDeformerGPU
        │  4. hand it all to the render thread
        │  5. push SnowRegion to the material layer
        ▼
SnowDeformation::Dispatch_RenderThread()            ── render thread
        │  builds an RDG graph: register textures, upload the deformer
        │  buffer, add two compute passes, execute
        ▼
SnowDeformation.usf                                 ── GPU
        │  Pass 1  SnowAccumulateCS : reproject → relax → stamp
        │  Pass 2  SnowNormalsCS    : height → height + normals
        ▼
SnowData render target → your material
```

Three threads of execution are involved and the boundaries matter. Detail in
[doc 2](02-gpu-compute-pipeline.md#the-three-threads).

## Why each piece is the kind of object it is

**`USnowDeformationSubsystem` is a `UTickableWorldSubsystem`.** The simulation
is per-world (PIE spinning up a second world must not share state with the
editor world), needs to tick, and should exist without anyone placing an actor
in the level. A world subsystem is exactly that: automatic lifetime tied to
the world, no actor to forget to place. An actor would have worked but would
have to be placed in every level; a `GameInstanceSubsystem` would wrongly
share one height field across level loads.

**`USnowDeformerComponent` is a `UActorComponent` that does not tick.** It
deliberately has `PrimaryComponentTick.bCanEverTick = false`. Instead of each
component pushing contacts into the subsystem on its own tick, the subsystem
*pulls* from every registered component during its own tick.

That's worth dwelling on, because it's a real design decision:

- **Pull** gives one deterministic point in the frame where all contacts are
  gathered, so they all land in the same dispatch with the same region centre.
- **Push** would have components writing into the subsystem at unpredictable
  points relative to the subsystem's tick, so some contacts would be rebased
  against the previous frame's centre and land offset. *(This is exactly the
  bug that `StampSnowAt` originally had — see
  [doc 4](04-cpp-reference.md#stampsnowat-and-why-it-banks-world-space).)*
- Pull also means an idle component costs nothing at all.

**`USnowDeformationSettings` is a `UDeveloperSettings`.** Not an aesthetic
choice — a necessity. A world subsystem has **no details panel anywhere in the
editor**, so `UPROPERTY(EditAnywhere)` on it is unreachable; you literally
cannot tune it. `UDeveloperSettings` auto-registers a page under Project
Settings and persists to config. The subsystem seeds its runtime values from
it on `Initialize`.

**The deformer list holds weak pointers.** A character can be destroyed
without `EndPlay` unregistering cleanly in every path. `TWeakObjectPtr` plus
pruning stale entries during iteration means a destroyed actor can never leave
a dangling pointer behind.

## What this design does *not* do

Stated plainly, because knowing the boundaries is part of understanding it:

- **One region, one focus actor per world.** Splitscreen would need several.
- **No replication.** Deformation is cosmetic, so each client traces its own
  local pawn. Replicating a height field would be absurd bandwidth for
  something nobody's gameplay depends on.
- **No persistence across level load.** The height field is transient.
- **Snow is a displacement of existing geometry.** The plugin produces a height
  field; it does not create mesh. A flat two-triangle plane will shade
  correctly but not physically dent — see
  [doc 5](05-material-wiring.md#world-position-offset-needs-geometry).

---

Next: [The GPU compute pipeline →](02-gpu-compute-pipeline.md)
