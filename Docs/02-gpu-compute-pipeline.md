# 2. The GPU compute pipeline

[← Architecture](01-architecture.md) · [Docs index](README.md) · Next: [Shader walkthrough →](03-shader-walkthrough.md)

This is the part most tutorials hand-wave. Everything here refers to
[`SnowDeformationComputePass.h`](../Source/SnowDeformation/Public/SnowDeformationComputePass.h)
and [`.cpp`](../Source/SnowDeformation/Private/SnowDeformationComputePass.cpp).

## Why a compute shader at all

Unreal gives you easier ways to write into a render target:

| Approach | Why not here |
|---|---|
| `DrawMaterialToRenderTarget` | A material can't read its own previous output and write it in the same draw, and you get no random-access writes. Reprojection needs both. |
| Canvas / `DrawLine` etc. | CPU-driven, one draw call per print, no per-texel logic. Fine for decals, useless for a simulation. |
| Niagara | Can do this, but it's a big dependency to carry for two kernels, and you lose direct control of the dispatch. |
| **Compute shader** | Arbitrary read of any texel, arbitrary write to any texel, one thread per texel, runs as a plain dispatch. |

The deciding factor is **random-access write** (`RWTexture2D`). The accumulate
pass reads texel *A* and writes texel *B* — a pixel shader fundamentally can't
do that, it only writes the fragment it was invoked for.

## The three threads

Getting this wrong is the most common source of crashes in render code.

```
GAME THREAD                RENDER THREAD              GPU
───────────                ─────────────              ───
Subsystem::Tick()
  gather contacts
  build Params ──────┐
                     │ ENQUEUE_RENDER_COMMAND
                     └──►  Dispatch_RenderThread()
                             build RDG graph ──────►  SnowAccumulateCS
                             Execute()                SnowNormalsCS
```

The game thread **never** touches RHI resources. It builds a plain struct of
values and hands it over. The render thread runs roughly a frame behind, which
is exactly why this matters:

```cpp
// SnowDeformationSubsystem.cpp — resolve on the RENDER thread, not here
FTextureRenderTargetResource* PrevResource = HeightRenderTargets[...]->GameThread_GetRenderTargetResource();
...
ENQUEUE_RENDER_COMMAND(SnowDeformationDispatch)(
    [Params, PrevResource, NextResource, DataResource](FRHICommandListImmediate& RHICmdList) mutable
    {
        Params.PrevHeightTexture = PrevResource->GetRenderTargetTexture();
        ...
    });
```

We capture the **resource**, not the `FRHITexture*`. A resource can swap the
texture underneath it — a resolution change, a device reset — between the game
thread reading it and the command actually running. Resolving inside the
lambda means we always get the texture that is valid *now*.

`Params` is captured **by value**, so the render thread owns its own copy of
the deformer array and the game thread is free to reuse its memory
immediately. The lambda is `mutable` only so it can fill in those three
pointers.

## Getting a shader file compiled at all

A plugin's `.usf` lives outside the engine's shader directory, so the compiler
can't find it until you map a virtual path. That's the module's entire job:

```cpp
// SnowDeformation.cpp
const FString PluginShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
AddShaderSourceDirectoryMapping(TEXT("/SnowDeformationShaders"), PluginShaderDir);
```

Two things make this fragile if you get them wrong:

- **Loading phase must be `PostConfigInit`.** Shader directory mappings have to
  exist before the shader compiler starts, which is earlier than `Default`.
  That's set in [`SnowDeformation.uplugin`](../SnowDeformation.uplugin).
- **The mapping is process-global and never unregistered.** `ShutdownModule`
  deliberately does nothing; other modules may still be walking the mapping
  table during shutdown.

## Declaring a compute shader

```cpp
class FSnowAccumulateCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FSnowAccumulateCS);
    SHADER_USE_PARAMETER_STRUCT(FSnowAccumulateCS, FGlobalShader);

    using FParameters = FSnowAccumulateParams;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters,
                                             FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        SET_SHADER_DEFINE(OutEnvironment, THREADS_X, SnowDeformation::ThreadGroupSize);
        SET_SHADER_DEFINE(OutEnvironment, THREADS_Y, SnowDeformation::ThreadGroupSize);
    }
};
```

**Global shader, not material shader.** A *material* shader is compiled once
per material that uses it and is driven by the material graph. A *global*
shader is a single instance owned by the engine, compiled once per platform,
with parameters you bind from C++. A simulation kernel with no artist-facing
inputs is squarely the second kind.

**`ShouldCompilePermutation`** gates which platforms get the shader compiled
at all. Restricting to SM5+ means mobile/ES31 targets don't try (and fail) to
build a shader they can't run.

**`ModifyCompilationEnvironment`** injects preprocessor defines into the HLSL.
`THREADS_X`/`THREADS_Y` become the `numthreads` values in the shader. They
come from one shared constant:

```cpp
namespace SnowDeformation { static constexpr int32 ThreadGroupSize = 8; }
```

That constant feeds both the define here *and* `GetGroupCount` at dispatch
time. If those two ever disagree you silently under- or over-dispatch and part
of the texture goes unwritten — a genuinely nasty bug, so the constant exists
purely to make it impossible.

Finally the shader is bound to a file and entry point:

```cpp
IMPLEMENT_GLOBAL_SHADER(FSnowAccumulateCS,
    "/SnowDeformationShaders/Private/SnowDeformation.usf", "SnowAccumulateCS", SF_Compute);
```

Both passes live in the same `.usf` with different entry points — they share
the `TextureSize` / `RegionSize` declarations, and one file is easier to read
than two.

## Parameter structs

```cpp
BEGIN_SHADER_PARAMETER_STRUCT(FSnowAccumulateParams, )
    SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, PrevHeightTexture)
    SHADER_PARAMETER_SAMPLER(SamplerState, PrevHeightSampler)
    SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, OutHeightTexture)
    SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FSnowDeformerGPU>, Deformers)
    SHADER_PARAMETER(FIntPoint, TextureSize)
    SHADER_PARAMETER(float, RegionSize)
    SHADER_PARAMETER(FVector2f, RegionOffsetFromPrev)
    SHADER_PARAMETER(int32, NumDeformers)
    SHADER_PARAMETER(float, RefillRate)
    SHADER_PARAMETER(float, DeltaTime)
    SHADER_PARAMETER(float, ClearMask)
END_SHADER_PARAMETER_STRUCT()
```

This macro generates a struct whose members are matched **by name** to the
global declarations in the `.usf`. Get a name wrong and the parameter silently
stays zero — no error. That's the single most common way to waste an afternoon
here, so when a value reads as 0 on the GPU, check the spelling first.

Note the type distinctions, which are what RDG uses to work out dependencies:

- `SHADER_PARAMETER_RDG_TEXTURE` — read-only (an SRV). `Texture2D<float>`.
- `SHADER_PARAMETER_RDG_TEXTURE_UAV` — writable. `RWTexture2D<float>`.
- `SHADER_PARAMETER_RDG_BUFFER_SRV` — read-only structured buffer.
- `SHADER_PARAMETER` — a loose constant, packed into a uniform buffer for you.

Use `FVector2f` / `FIntPoint`, never `FVector2D`. `FVector2D` is
double-precision in UE5 and will not match a `float2`.

## RDG — the Render Dependency Graph

You could call `RHICmdList.DispatchComputeShader` directly. RDG exists because
doing that correctly is harder than it looks: every resource has to be
transitioned into the right state (write → read) at the right time, and
getting a barrier wrong gives you corruption or a hang that only reproduces on
one vendor's driver.

RDG makes you *declare* what each pass reads and writes, then:

- inserts every resource transition and barrier for you,
- knows pass 2 reads what pass 1 wrote, so it orders and barriers them,
- allocates transient resources from a pool and reuses memory,
- culls passes whose output nothing consumes,
- gives you named events that show up in RenderDoc / Unreal Insights.

You build a graph, then execute it:

```cpp
FRDGBuilder GraphBuilder(RHICmdList);
/* ... add passes ... */
GraphBuilder.Execute();
```

Nothing runs until `Execute()`. Everything before it is recording intent.

### Bringing persistent textures into the graph

RDG normally owns transient resources. Ours must survive between frames, so
they're `UTextureRenderTarget2D`s that we *register* into the graph each frame:

```cpp
const FRDGTextureRef PrevHeightRDG = RegisterExternalTexture(GraphBuilder, Params.PrevHeightTexture, TEXT("SnowPrevHeight"));
```

This wraps an existing `FRHITexture` in an `FRDGTexture` so passes can
reference it, without RDG trying to manage its lifetime. The names are what
you'll see in a GPU capture.

> Historical note worth keeping: this originally used
> `GraphBuilder.RegisterExternalTexture(CreateRenderTarget(...))` and added
> `Renderer/Private` to the module's include paths to find `CreateRenderTarget`.
> That was wrong — the function is in RenderCore's **public**
> `PooledRenderTarget.h`, and reaching into another module's private headers
> breaks installed-engine and packaged builds. The public helper in
> `RenderGraphUtils.h` does both steps.

### UAVs

```cpp
const FRDGTextureUAVRef NextHeightUAV = GraphBuilder.CreateUAV(NextHeightRDG);
```

A UAV (Unordered Access View) is the "any thread may write any texel" view of
a texture. A render target can only produce one if it was created with
`bCanCreateUAV = true` — which is why the subsystem force-enables that flag on
a user-supplied asset and warns about it. Without it, UAV creation fails and
the field silently stays blank.

### Ping-pong: why two height textures

Pass 1 reads every texel of the previous height field (at an offset) and
writes every texel of the new one. Reading and writing the same texture in one
dispatch is undefined — thread *A* may read a texel that thread *B* has
already overwritten, and there's no ordering guarantee between them.

So there are two, swapped every frame:

```cpp
Params.PrevHeightTexture = HeightRenderTargets[CurrentHeightIndex]...;
Params.NextHeightTexture = HeightRenderTargets[1 - CurrentHeightIndex]...;
/* ...dispatch... */
CurrentHeightIndex = 1 - CurrentHeightIndex;
```

The third target, `SnowData`, is written only by pass 2 and only read by the
material, so it needs no ping-pong.

### The deformer structured buffer

Per-frame variable-length data goes up as a structured buffer:

```cpp
DeformerBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SnowDeformers"),
    sizeof(FSnowDeformerGPU), NumDeformers,
    Params.Deformers.GetData(), sizeof(FSnowDeformerGPU) * NumDeformers);
```

Two subtleties:

**The layout must match the HLSL struct exactly.**

```cpp
struct FSnowDeformerGPU
{
    FVector2f LocalCenter;   // 8
    float Radius, Depth, Falloff, RimWidth, RimHeight, Strength;  // 24
};
static_assert(sizeof(FSnowDeformerGPU) == 32, "...must stay tightly packed...");
```

Every member is 4-byte-aligned and the total is a multiple of 16, so C++ and
HLSL agree without padding. The `static_assert` is there because adding, say,
a `bool` would change the size on the C++ side only, and every field after it
would read as garbage on the GPU — with no error anywhere.

**A structured buffer can't be zero-sized.** On a frame where nobody is
touching the snow we upload one inert entry instead, and pass `NumDeformers =
0` so the loop never reads it:

```cpp
static const FSnowDeformerGPU DummyDeformer;
DeformerBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("SnowDeformersDummy"), ..., 1, &DummyDeformer, ...);
```

## Dispatching

```cpp
TShaderMapRef<FSnowAccumulateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
FComputeShaderUtils::AddPass(
    GraphBuilder,
    RDG_EVENT_NAME("SnowAccumulate(%dx%d, %d deformers)", Params.TextureSize.X, Params.TextureSize.Y, NumDeformers),
    ComputeShader,
    PassParameters,
    FComputeShaderUtils::GetGroupCount(Params.TextureSize,
        FIntPoint(SnowDeformation::ThreadGroupSize, SnowDeformation::ThreadGroupSize)));
```

`GetGroupCount` is a **ceiling** divide: 1024 / 8 = 128 groups per axis
exactly, but at 1000 you'd get 125 groups covering 1000 — and at 1001, 126
groups covering 1008. Those extra 7 columns of threads have no texel to write,
which is why every kernel starts with:

```hlsl
if (any(Pixel >= TextureSize)) { return; }
```

Leave that out and you write out of bounds on any non-multiple-of-8
resolution. Detail in [doc 3](03-shader-walkthrough.md#the-bounds-check).

`RDG_EVENT_NAME` formats a label that appears in GPU captures — it costs
nothing in shipping and makes profiling possible, so it's worth filling in
properly.

## Why 8×8

A thread group of 8×8 = 64 threads. GPU hardware executes in waves of 32
(NVIDIA) or 64 (AMD), so 64 divides cleanly on both and wastes no lanes. It's
also small enough that a partially-covered group at the texture edge wastes
little. 16×16 (256 threads) is the other common choice and is fine here too;
8×8 is the safer default for a kernel with a dynamic inner loop, because
register pressure from the loop limits how many groups can be resident at
once.

## Cost

Two dispatches over 1024² texels per frame — about a million threads each.
Pass 1 does one texture sample plus a loop over the deformers (typically 2,
one per foot); pass 2 does five loads. On any discrete GPU this is a fraction
of a millisecond. The expensive axis isn't resolution, it's **deformer count**,
because every texel loops over every deformer — the early-out on distance
keeps that cheap, but a hundred simultaneous deformers would start to show.

That's also why the subsystem skips the dispatch entirely when nothing is
happening; see [the idle skip](04-cpp-reference.md#the-idle-skip).

---

Next: [Shader walkthrough →](03-shader-walkthrough.md)
