# 6. Build it yourself

[← Material wiring](05-material-wiring.md) · [Docs index](README.md) · Next: [Tuning & troubleshooting →](07-tuning-and-troubleshooting.md)

The order below is the order that lets you **verify something at every step**,
rather than writing the whole thing and then debugging a black screen. Each
stage ends with a way to prove it works.

Reference implementation: every file named here exists in this repo, so you
can diff against it when something doesn't line up.

---

## Stage 0 — an empty plugin that loads

Editor → Plugins → New Plugin → **Blank**. Then:

**`SnowDeformation.uplugin`** — the one non-obvious field:

```json
"Modules": [{ "Name": "SnowDeformation", "Type": "Runtime", "LoadingPhase": "PostConfigInit" }]
```

`PostConfigInit`, not `Default`. Shader directory mappings must be registered
before the shader compiler initialises, which happens earlier than `Default`.
Get this wrong and your `.usf` isn't found, with a confusing error.

**`SnowDeformation.Build.cs`** — start with `Core`, `CoreUObject`, `Engine`,
`Projects`, `RHI`, `RenderCore`. Resist adding `Renderer`; you won't need it,
and reaching into its private headers is a trap
([why](04-cpp-reference.md#snowdeformationbuildcs)).

✅ **Verify:** the editor compiles and the plugin appears in the Plugins browser.

---

## Stage 1 — map the shader directory

```cpp
void FSnowDeformationModule::StartupModule()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("SnowDeformation"));
    if (!Plugin.IsValid()) return;
    const FString Dir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
    if (!AllShaderSourceDirectoryMappings().Contains(TEXT("/SnowDeformationShaders")))
        AddShaderSourceDirectoryMapping(TEXT("/SnowDeformationShaders"), Dir);
}
```

Leave `ShutdownModule` empty — mappings are process-global and other modules
may still read the table during shutdown.

Create `Shaders/Private/SnowDeformation.usf` with a trivial kernel that writes
a constant.

✅ **Verify:** restart the editor, search the log for your shader name under
`LogShaderCompilers`. If it isn't there, the mapping or the loading phase is
wrong. **Do this before writing any real shader code** — it's the step most
likely to waste an hour, and it's much easier to debug in isolation.

---

## Stage 2 — one compute pass writing a visible pattern

Now the C++ shader plumbing, covered in full in
[doc 2](02-gpu-compute-pipeline.md):

1. A parameter struct (`BEGIN_SHADER_PARAMETER_STRUCT`) with a single
   `SHADER_PARAMETER_RDG_TEXTURE_UAV` output.
2. A class deriving `FGlobalShader` with `DECLARE_GLOBAL_SHADER` +
   `SHADER_USE_PARAMETER_STRUCT`.
3. `IMPLEMENT_GLOBAL_SHADER` pointing at the virtual path and entry point.
4. A `Dispatch_RenderThread` that builds an `FRDGBuilder`, registers an
   external texture, creates a UAV, adds one pass, executes.

Have the kernel write something obviously wrong-looking — UVs as colour:

```hlsl
OutTexture[Pixel] = float4(UV, 0, 1);
```

Drive it from a temporary `UFUNCTION(BlueprintCallable)` that takes a render
target, and view that target in the editor.

✅ **Verify:** a red/green gradient in the render target. You now have the
entire GPU path working, and everything after this is algorithm rather than
plumbing.

**Two traps here:**

- Parameter names in the struct must match the HLSL globals **exactly**.
  Mismatches don't error — the value is silently zero.
- Start every kernel with `if (any(Pixel >= TextureSize)) return;`
  ([why](03-shader-walkthrough.md#the-bounds-check)).

---

## Stage 3 — persistent state and ping-pong

Add the second height texture and swap them each frame
([why two](02-gpu-compute-pipeline.md#ping-pong-why-two-height-textures)).
Make the kernel read `PrevHeightTexture` and write `OutHeightTexture`, with no
reprojection yet — just copy, and subtract a little each frame.

Use `RTF_R32f` for the height buffers: they accumulate across frames, so
precision loss compounds.

✅ **Verify:** write a blob into the buffer once (a hard-coded circle at the
centre), then watch it fade over several seconds. If it flickers or shows
garbage, your ping-pong indices are swapped or you're reading and writing the
same texture.

---

## Stage 4 — the region and reprojection

This is the actual idea ([doc 1](01-architecture.md#the-central-idea-a-world-anchored-scrolling-region)).

Add `RegionSizeWorld`, a region centre that follows an actor, and
`RegionOffsetFromPrev`. Then the one line everything depends on:

```hlsl
const float2 PrevUV = (LocalXY + RegionOffsetFromPrev) / RegionSize + 0.5f;
```

Derived step by step in
[doc 3](03-shader-walkthrough.md#reprojection-the-heart-of-the-thing).

Sample it with a **bilinear, clamped** sampler and `SampleLevel` (compute
shaders have no implicit derivatives). Treat UVs outside 0..1 as fresh snow.

✅ **Verify:** stamp a hard-coded blob, then walk away from it. The blob must
stay **on the ground** while the texture window moves. If it follows you, your
offset sign is flipped — swap `C_now - C_prev`.

This is the single most important thing to verify in the whole project. Get it
working before adding anything else.

---

## Stage 5 — real deformers

Add the structured buffer
([layout rules](02-gpu-compute-pipeline.md#the-deformer-structured-buffer)):

- Mirror the struct exactly in C++ and HLSL, with a `static_assert` on its size.
- Handle the empty case — a structured buffer can't be zero-sized; upload one
  inert entry and pass `NumDeformers = 0`.
- Use `max()` to combine, so deeper always wins and order doesn't matter
  ([why](03-shader-walkthrough.md#the-pressed-core)).

✅ **Verify:** a `StampSnowAt`-style Blueprint call leaves a print at exactly
the world position you asked for. If prints land offset by roughly your
movement speed, you converted to region-local space at the wrong moment —
[this exact bug](04-cpp-reference.md#stampsnowat-and-why-it-banks-world-space).

---

## Stage 6 — normals

Second pass, central differences, tangent cross product. Full derivation in
[doc 3](03-shader-walkthrough.md#from-heights-to-a-normal).

Use per-axis texel size, clamp the neighbour reads at the border, and `Load`
rather than `Sample`.

✅ **Verify:** view the output target — normals should read as a smooth
blue-ish field with visible shading around each print.

---

## Stage 7 — feet

The [component](04-cpp-reference.md#usnowdeformercomponent): trace down from
each foot socket, emit a contact where it hits.

Make the subsystem **pull** from registered components during its own tick
rather than having components push
([why](01-architecture.md#why-each-piece-is-the-kind-of-object-it-is)).

✅ **Verify:** walk around and leave footprints.

⚠️ **Set `ContactHeight` to ~20, not 6.** Foot bones sit at the ankle, 10–15 cm
above the sole, so a low threshold means the foot is never "planted" on flat
ground and you'll only get prints in odd cases. This is the single most common
"it doesn't work" — see
[troubleshooting](07-tuning-and-troubleshooting.md#nothing-prints-on-flat-ground).

---

## Stage 8 — the material

Covered in [doc 5](05-material-wiring.md), including why a material can't
reference a runtime texture and the three ways around it.

✅ **Verify:** prints visible in-game. Untick *Tangent Space Normal*
([why](05-material-wiring.md#the-normal-gotcha)), and don't expect a 4-vertex
plane to physically dent
([why](05-material-wiring.md#world-position-offset-needs-geometry)).

---

## Stage 9 — the things that make it usable

Worth doing, in roughly this order:

- **`UDeveloperSettings`.** Not optional in practice: a world subsystem has no
  details panel, so without this your simulation cannot be tuned at all
  ([detail](04-cpp-reference.md#usnowdeformationsettings)).
- **Resolve RHI textures on the render thread**, not the game thread
  ([why](02-gpu-compute-pipeline.md#the-three-threads)).
- **Skip the dispatch when idle** — but never while the region is moving
  ([why](04-cpp-reference.md#the-idle-skip)).
- **Weak pointers** for registered components and materials.
- **A log category**, so failures are greppable.

## A verification habit worth keeping

`UnrealBuildTool` does **not** validate HLSL. A broken `.usf` compiles clean
and fails at runtime. To check shaders without opening the editor:

```
UnrealEditor-Cmd.exe <project>.uproject -nosplash -unattended -nopause -AbsLog=<path>
```

then grep the log for your shader names and for `Failed to compile`. Cheap,
and it catches shader errors in the same loop as C++ errors.

---

Next: [Tuning & troubleshooting →](07-tuning-and-troubleshooting.md)
