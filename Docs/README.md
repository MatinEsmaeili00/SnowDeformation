# Snow Deformation — the long explanation

These documents exist so you can **rebuild this plugin from nothing and
understand every decision in it**, not just read what it does. Wherever there
was a choice, the docs say what the alternatives were and why this one won.

If you only want to *use* the plugin, the [top-level README](../README.md) is
enough. Come here to learn how it works.

## Read in this order

| # | Document | What you get out of it |
|---|---|---|
| 1 | [Architecture](01-architecture.md) | The shape of the whole system, and the one idea (a scrolling, world-anchored region) everything else follows from. |
| 2 | [The GPU compute pipeline](02-gpu-compute-pipeline.md) | How a compute shader actually gets declared, compiled, bound and dispatched in UE5. Global shaders, RDG, UAVs, structured buffers, thread groups. |
| 3 | [Shader walkthrough](03-shader-walkthrough.md) | `SnowDeformation.usf` line by line, including the reprojection and normal-reconstruction maths derived from scratch. |
| 4 | [C++ reference](04-cpp-reference.md) | Every class and file, what it owns, and why it's that kind of object. |
| 5 | [Material wiring](05-material-wiring.md) | Getting GPU output into a material — and why that is genuinely the hardest part. |
| 6 | [Build it yourself](06-build-it-yourself.md) | Empty plugin → working footprints, in order, with the mistakes called out in advance. |
| 7 | [Tuning & troubleshooting](07-tuning-and-troubleshooting.md) | Every knob, what it really controls, and symptom → cause for the things that go wrong. |

## The one-paragraph version

A compute shader maintains a 1024×1024 texture representing a square of snow
around the player. Each frame it copies last frame's texture into this frame's
position (so tracks stay put in the world while the window follows you),
relaxes it slightly back toward flat, then stamps a circle for every foot
currently touching the ground. A second compute pass turns that height field
into normals. A material samples the result and shades the prints. Everything
else in this repo is plumbing around those two passes.

## A note on how to read the code

The interesting parts, in order of how much they'll teach you:

1. [`SnowDeformation.usf`](../Shaders/Private/SnowDeformation.usf) — the actual
   algorithm, ~140 lines. Start here. Explained in [doc 3](03-shader-walkthrough.md).
2. [`SnowDeformationComputePass.cpp`](../Source/SnowDeformation/Private/SnowDeformationComputePass.cpp)
   — how those shaders get run. Explained in [doc 2](02-gpu-compute-pipeline.md).
3. [`SnowDeformationSubsystem.cpp`](../Source/SnowDeformation/Private/SnowDeformationSubsystem.cpp)
   — the game-thread half. Explained in [doc 4](04-cpp-reference.md).

Everything else is configuration, registration, or convenience.
