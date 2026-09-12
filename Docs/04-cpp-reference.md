# 4. C++ reference

[← Shader walkthrough](03-shader-walkthrough.md) · [Docs index](README.md) · Next: [Material wiring →](05-material-wiring.md)

File-by-file, what each type owns and why it's that kind of object. The
*shape* of the design is argued in [doc 1](01-architecture.md); this is the
detail.

## File map

| File | Role |
|---|---|
| `SnowDeformation.Build.cs` | Module dependencies |
| `Public/SnowDeformation.h` / `Private/*.cpp` | Module entry point, shader path mapping, log category |
| `Public/SnowDeformationSettings.h` / `.cpp` | Project Settings page |
| `Public/SnowDeformationComputePass.h` / `.cpp` | Shader declarations + RDG dispatch — see [doc 2](02-gpu-compute-pipeline.md) |
| `Public/SnowDeformationSubsystem.h` / `.cpp` | The simulation; game-thread half |
| `Public/SnowDeformerComponent.h` / `.cpp` | Turns feet into contacts |

---

## `SnowDeformation.Build.cs`

```csharp
PublicDependencyModuleNames:  Core, CoreUObject, Engine, DeveloperSettings
PrivateDependencyModuleNames: Projects, RHI, RenderCore
```

**Public vs private matters.** A module that includes our public headers gets
our public dependencies transitively. `USnowDeformationSettings` derives from
`UDeveloperSettings`, so that has to be public or a dependent module can't
compile. `RenderCore` is private because nothing in our public headers exposes
an RDG type that a consumer must resolve.

Note what is *absent*: **`Renderer`**. Everything used here (RDG,
`FGlobalShader`, `FComputeShaderUtils`, `CreateRenderTarget`) lives in
`RenderCore`. An earlier version depended on `Renderer` and added
`Renderer/Private` to the include path; that was unnecessary and a portability
trap. [Detail](02-gpu-compute-pipeline.md#bringing-persistent-textures-into-the-graph).

## `FSnowDeformationModule`

The whole module does one thing — map the plugin's `Shaders/` folder to the
virtual path `/SnowDeformationShaders` so the shader compiler can find the
`.usf`. Covered in
[doc 2](02-gpu-compute-pipeline.md#getting-a-shader-file-compiled-at-all),
including why `LoadingPhase` must be `PostConfigInit` and why `ShutdownModule`
deliberately does nothing.

It also defines `LogSnowDeformation`, so anything the plugin reports is
filterable in the output log.

---

## `USnowDeformationSettings`

```cpp
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Snow Deformation"))
class SNOWDEFORMATION_API USnowDeformationSettings : public UDeveloperSettings
```

Exists out of necessity, not taste: **a world subsystem has no details panel
anywhere in the editor**, so `UPROPERTY(EditAnywhere)` on the subsystem is
unreachable. Before this class existed the simulation could not be tuned at
all without writing Blueprint.

- `config = Game` → persists to `DefaultGame.ini`.
- `defaultconfig` → writes to the project default rather than per-user, so the
  settings are shared and source-controlled.
- `GetCategoryName()` returns `Plugins`, which places the page under Project
  Settings → Plugins.

The two asset references are `TSoftObjectPtr`, so a project that doesn't use
them never loads them. They're resolved exactly once, in the subsystem's
`Initialize` — see [`PrepareOutputAsset`](#prepareoutputasset).

---

## `USnowDeformationSubsystem`

`UTickableWorldSubsystem`. Owns the render targets, the region, the
registration lists, and the per-frame dispatch.

### Lifecycle

`Initialize` seeds runtime values from the settings and resolves the wiring
assets. Runtime properties stay `BlueprintReadWrite` so a game can override
them mid-play.

`Deinitialize` clears both registration lists, releases render targets, and
drops the resolved assets. Render targets are `UPROPERTY(Transient)` so GC
owns them; nulling is enough.

`DoesSupportWorldType` restricts to `Game` and `PIE`. Without it the subsystem
would spin up for editor preview worlds, asset thumbnails and inactive worlds
— ticking simulations nobody asked for.

### `EnsureRenderTargets`

Called every tick; returns immediately when the current targets already match
what's requested. It reallocates when resolution changes or targets go
missing.

```cpp
auto MakeRenderTarget = [this](ETextureRenderTargetFormat Format) -> UTextureRenderTarget2D*
{
    UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(this);
    RT->RenderTargetFormat = Format;
    RT->bCanCreateUAV = true;          // required for a compute shader to write it
    RT->InitAutoFormat(TextureResolution.X, TextureResolution.Y);
    RT->UpdateResourceImmediate(true);
    return RT;
};
```

Formats: the two height buffers are **`RTF_R32f`** — one channel, full float
precision, because they're accumulated across frames and precision loss
compounds. The output is **`RTF_RGBA16f`**; half precision is plenty for one
frame of consumption, and it halves the bandwidth the material pays.

`NewObject` is called with **no name**. It used to pass `TEXT("SnowHeightA")`,
which collides on reallocation — the outgoing object still holds that name
until GC runs, and `NewObject` with a duplicate name renames or asserts.

### `PrepareOutputAsset`

When the settings name a render target asset, the simulation writes into it
instead of a transient target — that's what lets a material reference it (see
[doc 5](05-material-wiring.md)). Two things get validated, **once**, at
`Initialize`:

```cpp
if (Asset->RenderTargetFormat != RTF_RGBA16f)  // warn: signed heights get clipped
if (!Asset->bCanCreateUAV) { Asset->bCanCreateUAV = true; Asset->UpdateResource(); }
```

The UAV flag is force-enabled because without it UAV creation fails and the
field stays silently blank — a bad failure mode to leave to chance. The format
is only warned about, because overriding it would stomp a deliberate choice.

Doing this at `Initialize` rather than inside `EnsureRenderTargets` is not
cosmetic: `EnsureRenderTargets` runs every frame, so a mis-configured asset
would log the same warning sixty times a second.

A supplied asset also **decides the simulation resolution** (its size wins over
`TextureResolution`) rather than being resized to match. Silently resizing
someone's asset is a worse surprise than ignoring a setting.

### Tick order, and why it is this order

```
1. EnsureRenderTargets()
2. resolve focus actor → NewRegionCenter          (not yet committed)
3. gather contacts from every registered component
4. update IdleRelaxRemaining
5. decide bNeedsDispatch — early out if false
6. check render target resources are live — early out if not
7. commit RegionCenterWorld, convert contacts to region-local
8. ENQUEUE_RENDER_COMMAND, flip ping-pong index
9. PushMaterialParameters()
```

The subtlety is that **`RegionCenterWorld` is only committed when we actually
dispatch** (step 7). If we bail at step 5 or 6, the centre stays where the
texture is still anchored, so next frame's `RegionOffsetFromPrev` is measured
from the right place and no motion is lost. Committing it earlier and then
bailing would desynchronise the texture from its own centre.

### The idle skip

```cpp
const bool bRegionMoved = !NewRegionCenter.Equals(RegionCenterWorld, 0.01);
const bool bNeedsDispatch = bFirstFrame || bRegionMoved || NumPresses > 0 || IdleRelaxRemaining > 0.0f;
```

Skipping the dispatch is only safe when *nothing can change*:

- **`bRegionMoved`** forces a dispatch even with no contacts. The texture is
  world-anchored; a move must be reprojected or every track slides with the
  player. This is the condition it would be easiest to get wrong.
- **`IdleRelaxRemaining`** keeps the simulation running after the last footfall
  so tracks finish healing:

```cpp
IdleRelaxRemaining = RefillRate > KINDA_SMALL_NUMBER ? (1.0f / RefillRate) : 0.0f;
```

Self-tuning rather than another setting: `1 / RefillRate` is exactly how long
a full-depth print takes to relax to flat. With `RefillRate = 0` tracks are
permanent, so once the region stops moving there is genuinely nothing left to
compute and the cost drops to zero.

### `StampSnowAt`, and why it banks world space

```cpp
void USnowDeformationSubsystem::StampSnowAt(FVector WorldLocation, ...)
{
    Deformer.LocalCenter = FVector2f(FVector2D(WorldLocation));   // world, not local
    PendingOneShotDeformers.Add(Deformer);
}
```

then, in `Tick`, after the centre is committed:

```cpp
const FVector2f RegionCenterFloat(RegionCenterWorld);
for (FSnowDeformerGPU& OneShot : PendingOneShotDeformers)
{
    OneShot.LocalCenter -= RegionCenterFloat;
    GPUDeformers.Add(OneShot);
}
```

This is worth understanding because the original code got it wrong. It
converted to region-local space *at call time*, using whatever centre was left
over from the previous tick. Then `Tick` moved the region and handed the
already-converted value to a shader that interprets it relative to the **new**
centre — so every explosion and vehicle stamp landed offset by exactly how far
the player had moved that frame. Banking world space and rebasing once, at the
moment the centre is known, is the fix. It's the same push-vs-pull hazard
described in [doc 1](01-architecture.md#why-each-piece-is-the-kind-of-object-it-is).

### `PushMaterialParameters`

Runs every frame **including frames where the dispatch is skipped** — a
material instance created this frame still needs its parameters. Pushes the
`SnowRegion` vector into the parameter collection (only if the collection
actually declares that parameter, checked once at `Initialize`, otherwise
you'd get an engine error every frame), then refreshes every registered
dynamic material instance, pruning any that have been garbage collected.

```cpp
FLinearColor GetSnowRegionParameter() const
{
    return FLinearColor(RegionCenterWorld.X, RegionCenterWorld.Y, RegionSizeWorld, MaxDepthWorld);
}
```

Four values packed into one vector — one collection entry instead of four, and
one `SetVectorParameterValue` per frame instead of several. See
[doc 5](05-material-wiring.md#the-snowregion-vector).

---

## `USnowDeformerComponent`

Answers one question per frame: *where are this actor's feet touching the
ground right now?*

It does **not** tick (`PrimaryComponentTick.bCanEverTick = false`); the
subsystem pulls from it. Rationale in
[doc 1](01-architecture.md#why-each-piece-is-the-kind-of-object-it-is).

### Contact detection

```cpp
const FVector SocketLoc = SkelMesh->GetSocketLocation(Socket);
const FVector TraceEnd  = SocketLoc - FVector(0, 0, TraceDownDistance);

if (World->LineTraceSingleByChannel(Hit, SocketLoc, TraceEnd, TraceChannel, QueryParams))
{
    const float HeightAboveGround = SocketLoc.Z - Hit.Location.Z;
    if (HeightAboveGround <= ContactHeight)   // planted
        /* emit a contact at Hit.Location */
}
```

A downward line trace from each named socket. The print lands at the **hit
location**, not the socket — so it sits on the ground surface rather than
floating at ankle height.

> **`ContactHeight` is the setting people get bitten by.** Foot bones sit at the
> ankle, typically 10–15 cm above the sole, so a threshold below that means the
> foot is *never* considered planted on flat ground and you get prints only in
> odd cases (stepping off a slope, landing from a jump). If nothing is
> printing, raise this first — see
> [troubleshooting](07-tuning-and-troubleshooting.md#nothing-prints-on-flat-ground).

### Speed scaling

```cpp
const float SpeedAlpha = FMath::Clamp(Speed / MinSpeedForFullStrength, 0.0f, 1.0f);
const float Strength   = FMath::Max(SpeedAlpha, 0.35f);
```

A running stride presses harder than a shuffle. The `0.35` floor is a
judgement call: a foot planted while standing still should still leave a mark,
just a lighter one. Without the floor, standing still produces nothing, which
looks broken.

### The fallback path

If no skeletal mesh or none of the named sockets exist, and
`SimpleContactFallbackRadius > 0`, it stamps one print under the actor's
bounds instead. That covers vehicles, simple pawns and props with no foot rig.
It's opt-in (defaults to 0) because silently stamping a capsule-sized circle
under anything that has the component would be surprising.

---

Next: [Material wiring →](05-material-wiring.md)
