# 3. Shader walkthrough

[← GPU compute pipeline](02-gpu-compute-pipeline.md) · [Docs index](README.md) · Next: [C++ reference →](04-cpp-reference.md)

Source: [`Shaders/Private/SnowDeformation.usf`](../Shaders/Private/SnowDeformation.usf).
This is the actual algorithm; everything else in the repo exists to feed it.

## The height convention

Decide this first, because every line depends on it:

```
 Height = +1   pressed all the way down, by MaxDepthWorld world units
 Height =  0   undisturbed snow
 Height < 0    the rim — snow pushed UP around the edge of a print
```

Positive means *down*. That reads backwards at first, but it makes the common
case — "how pressed is this texel?" — a plain 0..1 value, and it lets the
accumulate pass use `max()` for persistence (see below). The sign flip happens
once, in the material's World Position Offset.

The consequence to remember: **height is signed**, so the output texture must
be a float format. An 8-bit target clips every negative value and you lose the
rim entirely. That's why [`PrepareOutputAsset`](04-cpp-reference.md#prepareoutputasset)
warns about non-RGBA16f targets.

## The struct contract

```hlsl
struct FSnowDeformerGPU
{
    float2 LocalCenter;  // XY relative to the region centre, world units
    float  Radius;       // world units
    float  Depth;        // 0..1, how far down this presses
    float  Falloff;      // edge sharpness exponent
    float  RimWidth;     // world units of displaced ring outside Radius
    float  RimHeight;    // 0..1, how high the rim piles up
    float  Strength;     // 0..1 master multiplier
};
```

This must match the C++ struct byte for byte — see
[the structured buffer section](02-gpu-compute-pipeline.md#the-deformer-structured-buffer)
for why, and what the `static_assert` protects you from.

---

# Pass 1: `SnowAccumulateCS`

Three jobs in one kernel: reproject, relax, stamp.

## The bounds check

```hlsl
const int2 Pixel = int2(DTid.xy);
if (any(Pixel >= TextureSize)) { return; }
```

`SV_DispatchThreadID` is the global thread index. Because group count is a
*ceiling* divide, the last group can extend past the texture. Without this,
those threads write out of bounds. It's two lines and it's not optional.

## Texel → world position

```hlsl
const float2 UV = (float2(Pixel) + 0.5f) / float2(TextureSize);
const float2 LocalXY = (UV - 0.5f) * RegionSize;
```

The `+ 0.5` gives the **centre** of the texel rather than its corner. Without
it everything is offset by half a texel, which is invisible until you compare
against another system and find a persistent slight shift.

`LocalXY` is this texel's position **relative to the region centre**, in world
units — so it spans `-RegionSize/2 .. +RegionSize/2`.

Working in local space rather than absolute world coordinates is deliberate.
A level built far from the origin (say 500,000 units out) pushes `float` into
a range where consecutive representable values are more than a centimetre
apart, and footprints visibly quantise. Region-local values stay small no
matter where the level sits.

## Reprojection: the heart of the thing

```hlsl
const float2 PrevUV = (LocalXY + RegionOffsetFromPrev) / RegionSize + 0.5f;
```

Derivation. Let `C_now` be this frame's region centre and `C_prev` last
frame's, so `RegionOffsetFromPrev = C_now - C_prev`.

```
This texel's world position:     W        = C_now + LocalXY

Where that same world position
sat in last frame's local space: L_prev   = W - C_prev
                                          = C_now + LocalXY - C_prev
                                          = LocalXY + (C_now - C_prev)
                                          = LocalXY + RegionOffsetFromPrev

As a UV in last frame's buffer:  PrevUV   = L_prev / RegionSize + 0.5
```

That's it. **This single line is what makes tracks stay on the ground instead
of sliding with the player.** Everything in [doc 1](01-architecture.md) follows
from it.

```hlsl
float Height = 0.0f;
if (all(PrevUV > 0.0f) && all(PrevUV < 1.0f))
{
    Height = PrevHeightTexture.SampleLevel(PrevHeightSampler, PrevUV, 0).r * ClearMask;
}
```

- **Outside 0..1** means this world position wasn't covered by last frame's
  region — it just scrolled in. Fresh, undisturbed snow, so `0`.
- **`SampleLevel` with a bilinear clamped sampler**, not `Load`. The offset is
  almost never a whole number of texels, so we need filtering between them.
  `SampleLevel` (rather than `Sample`) because compute shaders have no implicit
  derivatives to pick a mip from — you must state the mip explicitly.
- **`ClearMask`** is 0 on the first frame and 1 afterwards. Multiplying by it
  wipes the buffer once at startup without needing a separate clear pass. A
  branch would work equally well; this just costs less than a pass.

The bilinear resample does mean tracks blur very slightly each frame that the
region moves. That's the standing cost of this technique. Higher resolution or
a smaller `RegionSizeWorld` both reduce it.

## Relaxation (refill)

```hlsl
const float Refill = RefillRate * DeltaTime;
Height = (Height > 0.0f) ? max(Height - Refill, 0.0f) : min(Height + Refill, 0.0f);
```

Pull the value toward zero by a fixed amount per second, from whichever side
it's on, clamped so it never overshoots past flat. Models fresh snowfall
filling old tracks in. `RefillRate = 0` makes tracks permanent — and, because
the CPU side knows nothing can change once the region stops moving, also makes
an idle scene free. See [the idle skip](04-cpp-reference.md#the-idle-skip).

## Stamping

```hlsl
for (int i = 0; i < NumDeformers; ++i)
{
    const FSnowDeformerGPU D = Deformers[i];
    const float Dist = length(LocalXY - D.LocalCenter);
    if (Dist > D.Radius + D.RimWidth) { continue; }
```

Every texel loops over every deformer. The early-out means a texel far from
any foot does one subtract, one length and one compare per deformer — cheap
enough that the loop doesn't dominate at realistic deformer counts.

### The pressed core

```hlsl
    if (Dist <= D.Radius)
    {
        float Core = 1.0f - saturate(Dist / max(D.Radius, 0.001f));
        Core = pow(Core, max(D.Falloff, 0.001f));
        Height = max(Height, Core * D.Depth * D.Strength);
    }
```

`Core` is 1 at the centre falling linearly to 0 at `Radius`; `pow` reshapes
that curve. Higher `Falloff` pulls the profile toward the centre — a smaller,
sharper print with crisper walls. `Falloff = 1` is a plain cone.

The `max(..., 0.001f)` guards are there because a designer *will* eventually
type 0 into one of those fields, and division by zero or `pow(x, 0)` produces
NaNs that then propagate into the persistent buffer and never wash out.

**`max()` is what makes trails persist.** The new value only wins where it is
deeper than what's already there, so:

- walking over your own tracks deepens them but never erases them,
- a lighter print can't overwrite a heavier one at the same texel,
- and the result is order-independent, which matters because the loop order
  across deformers is arbitrary.

### The displaced rim

```hlsl
    else if (D.RimHeight > 0.0f && Height <= 0.0f)
    {
        const float R = saturate((Dist - D.Radius) / max(D.RimWidth, 0.001f));
        const float Ring = sin(R * PI);
        Height = min(Height, -Ring * D.RimHeight * D.Strength);
    }
```

`R` runs 0..1 across the band between `Radius` and `Radius + RimWidth`.
`sin(R * PI)` rises from 0 to 1 and back to 0 — a smooth bump peaking in the
middle of the band, with no hard seam at either edge. (A triangle or smoothstep
would also work; `sin` is one instruction and has zero-derivative ends.)

Two details:

- **`min`, not `max`**, because the rim is negative and "taller" means *more*
  negative.
- **`Height <= 0.0f`** means the rim is only written onto undisturbed snow. A
  foot landing next to an existing print can't push a ridge through the middle
  of it. It also means that within a single loop, a deformer that already wrote
  a core at this texel is safe from a later deformer's rim.

```hlsl
OutHeightTexture[Pixel] = Height;
```

One write, to this thread's own texel.

---

# Pass 2: `SnowNormalsCS`

Turns the height field into what the material samples:
`R = height, GB = normal.xy`.

## Sampling the neighbourhood

```hlsl
const int2 MaxPixel = TextureSize - 1;
const float HL = HeightTexture.Load(int3(clamp(Pixel.x - 1, 0, MaxPixel.x), Pixel.y, 0));
const float HR = HeightTexture.Load(int3(clamp(Pixel.x + 1, 0, MaxPixel.x), Pixel.y, 0));
const float HU = HeightTexture.Load(int3(Pixel.x, clamp(Pixel.y - 1, 0, MaxPixel.y), 0));
const float HD = HeightTexture.Load(int3(Pixel.x, clamp(Pixel.y + 1, 0, MaxPixel.y), 0));
const float H  = HeightTexture.Load(int3(Pixel, 0));
```

`Load` rather than `Sample`: we want exact texels, no filtering, so integer
coordinates are correct and cheaper. The `clamp` handles the texture border —
without it you read out of bounds (returns 0 in HLSL, which would fake a cliff
at the edge of the region).

This is a **central difference**: comparing the neighbours either side rather
than the texel and one neighbour. It's symmetric, so it doesn't bias the
normal half a texel in one direction.

## From heights to a normal

```hlsl
const float2 TexelWorldSize = RegionSize / float2(TextureSize);
const float DzDx = -(HR - HL) * MaxDepth / (2.0f * TexelWorldSize.x);
const float DzDy = -(HD - HU) * MaxDepth / (2.0f * TexelWorldSize.y);
```

Step by step:

1. Actual surface height in world units is `Z = -Height * MaxDepth` (the sign
   flip from our convention — pressed down is negative Z).
2. So `dZ/dx = -(dHeight/dx) * MaxDepth`.
3. `dHeight/dx ≈ (HR - HL) / (2 texels)`, and each texel is
   `TexelWorldSize.x` world units wide — hence the denominator.

Using **per-axis** texel size matters on a non-square height field; deriving
both gradients from `TextureSize.x` skews the normals. (It did, until it was
fixed.)

```hlsl
const float3 Normal = normalize(float3(-DzDx * NormalStrength, -DzDy * NormalStrength, 1.0f));
```

For a height field `z = f(x, y)`, the two surface tangents are
`Tx = (1, 0, dz/dx)` and `Ty = (0, 1, dz/dy)`, and

```
Tx × Ty = (-dz/dx, -dz/dy, 1)
```

which is the (unnormalised) surface normal — that's where the form comes from.

`NormalStrength` scales only the XY before normalising, which exaggerates the
apparent slope without changing the height field itself. It's a pure look
control: raise it when prints read too flat.

```hlsl
OutSnowDataTexture[Pixel] = float4(H, Normal.x, Normal.y, 1.0f);
```

Only X and Y are stored. Z is always positive for a height field, so the
material reconstructs it as `sqrt(saturate(1 - x² - y²))` — the standard
two-channel normal trick, and it frees a channel.

**These are world-space normals** (X and Y are world axes, Z is up), *not*
tangent space. The material must have *Tangent Space Normal* unticked or they
will be interpreted in the mesh's tangent basis and look wrong. This catches
everyone once — see [doc 5](05-material-wiring.md#the-normal-gotcha).

---

Next: [C++ reference →](04-cpp-reference.md)
