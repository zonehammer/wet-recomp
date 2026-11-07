# Performance on handhelds (ROG Ally / Steam Deck class)

> Linux / Proton support is experimental and not actively worked on; these are
> the results as measured, provided as is.

Measured on an ASUS ROG Ally X (Ryzen Z1 Extreme, Bazzite, Proton, Direct3D 12)
with WET Recomp 0.2.78, November 2026. Figures are 10-second averages from
`wet.log`, in the same levels on each run.

## Recommended settings

| Setting | Value | Why |
|---|---|---|
| FPS cap | **60** | The Ally can't reach 120 in levels; a steady 60 beats 45-80 jumping around |
| Resolution scale | 1 (720p) with NIS upscaling | Anything higher costs too much GPU time |
| Motion blur, depth of field | Off | Removes some of the screen-effect passes |
| Antialiasing | Off | A little GPU time back |
| Power limit (TDP) | 25-30 W if you can | The bottleneck is mostly the CPU, which runs faster with more power |
| Proton | Experimental or GE-Proton are worth a try | Newer Direct3D 12 translation |

With these, levels run at about **55-60 FPS**, dipping into the 40s in the
heaviest fights. Cutscenes run at the game's own 30.

## Why the frame rate is limited

Only half of the game is recompiled. Its own code (gameplay, physics, AI) is
translated to native PC code and runs fast. What it sends to the graphics chip
is still emulated: the game writes raw command lists for the Xbox 360's Xenos
GPU, not PC Direct3D calls, so every frame Xenia's GPU emulation has to:

- decode those commands and translate render states and shaders;
- emulate the 360's 10 MB of EDRAM, which is why every screen effect is copied
  out and loaded back as a texture (the "resolves" and "texture loads" below);
- untile and convert textures from the 360's memory layout.

Nearly all of this runs on one CPU thread (Xenia's command processor), so a
faster core helps and more cores don't. A desktop CPU is fast enough to reach
240 FPS; a handheld's low-power cores make it the ceiling. Under Proton,
Direct3D 12 is translated to Vulkan on top, which costs more CPU time.

The game was also built for a console where the CPU and GPU share memory and
the GPU answers in microseconds, so it often waits for the GPU (fences, swaps,
the flip flag). In emulation the GPU side can be a frame behind, and the game
thread sits waiting: the idle time in the logs.

Recomps that run fastest on handhelds (Unleashed Recompiled, for example)
don't emulate the GPU at all: they intercept the game's own graphics calls and
render natively, with shaders translated ahead of time and render targets as
ordinary PC textures. That removes this whole layer, but means rebuilding
every rendering feature the game uses by hand.

## Where the time goes

A typical level frame on the Ally at 720p:

| Part of the frame | Time |
|---|---|
| GPU drawing | 5-7 ms |
| Texture loads | ~1.8 ms |
| Resolves (copying drawn images out of the emulated video memory) | ~1.6 ms |
| GPU idle, waiting for the next frame | 5-9 ms |

The GPU itself could manage around 80 FPS. What holds it back is the CPU side:
the GPU emulation (Xenia's command processor, then Proton's Direct3D 12 to
Vulkan translation) preparing each frame. The game's render thread and the GPU
emulation spend much of each frame waiting on each other. On a desktop CPU
this is fast enough not to matter.

About 90% of the texture reloads are the game's own screen effects: each
frame it draws into hidden full-, half- and tiny-size images and reads them
back as textures. Only 2-3% of reloads were avoidable.

## Things that were tried and didn't help

| Experiment | Result |
|---|---|
| Xenia's Vulkan renderer instead of Direct3D 12 (skips Proton's translation) | Characters don't draw and the frame rate collapses; not usable for WET |
| Turning off the game's own `x360-antialiasing` engine setting | No measurable difference |
| Handing the GPU each frame in parts (splitting command submissions every 2 ms) | No difference; the idle time comes from the game and the emulation waiting on each other, not from how work is submitted |
| Resolve-to-texture shortcut (filling screen-effect textures by copying straight from where they were drawn) | Texture loads dropped from 1.8 to 1.2 ms, but copying out of render targets on AMD graphics made drawing ~2.5 ms slower: 43-46 FPS against 50-60 without it |
| Sampling profiler under Proton | Crashes (suspending threads isn't supported under Wine); it now refuses to start there |

All of these were removed again; the code is as it was before them.

## What could still help (bigger jobs)

- Letting the game run a frame ahead of the GPU emulation, to fill the idle
  time. It changes the game's frame timing, so it needs a working profiler on
  the handheld and careful testing.
- Finding which textures reload without changing (the remaining 2-3%).
