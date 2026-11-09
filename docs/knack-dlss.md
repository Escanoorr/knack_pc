# Knack: DLSS at the game's own resolution (experimental)

NVIDIA DLSS (DLAA: the render size is not lowered, so this makes the image cleaner, not faster) for
Knack, switched on from the in-game menu (Insert, Image tab, "DLSS (experimental)"). It is a module of its
own (`renderer_vulkan/vk_knack_dlss.*`, `host_shaders/knack_dlss.comp`) with a handful of hook lines in the
rasterizer; none of the God of War III upscaler (`vk_temporal_upscaler.*`, `docs/upscaler.md`) is used.

## What the game gives

Found in frame captures and in the dumped G-buffer shaders of 1.10 (the same engine in 1.00):

* The G-buffer pass has six colour targets and the scene depth (`D32S8`, normal depth: near 0, far 1).
  Colour target 5 is `R16G16F` and holds the game's velocity: every G-buffer pixel shader writes
  `current - previous` clip position (x/w, y/w, NDC), clamped to the half-float range. In the pixel shader the
  interpolated attribute at location 5 (the vertex shader output with the same inputs as `gl_Position`, so the
  current position) is added and the one at location 6 (made from a second set of transforms: the previous
  frame's) is subtracted. The vertex shaders use the previous view and the previous object transforms
  (skinning included), so animated objects are in it.
* The HDR scene colour is the `R16G16B16A16F` target drawn with that depth (sky, lighting, forward and
  transparent passes), full size.
* The post-processing chain follows: its first compute dispatch is shader `526df215` (once per frame,
  the same in every capture), then bloom/DoF-style computes, tone mapping to `R8G8B8A8`, the game's SMAA
  and the UI.

## What runs

* **Detection** (`OnTargets`, per draw): six or more colour targets with the sixth `R16G16F` and a depth is
  the G-buffer pass (scene depth and velocity); a draw into a full-size `RGBA16F` target with that depth is the
  scene colour. The scene depth is remembered between frames by guest address and size (image ids can be
  reused); everything else is per frame.
* **Jitter** (`DrawJitter`): a sub-pixel viewport shift (Halton 2,3, 8 phases) on every geometry draw
  (more than 6 indices or instanced) that uses the scene depth, from the Z pre-pass on, until DLSS has run:
  Z pre-pass, G-buffer, decals, sky, forward and transparent passes. Shadow maps, half-size depth, full-screen
  composites, post-processing and the UI are not shifted. The pre-pass and the G-buffer share the depth, so
  the jitter must be the same for both: the scene depth is known from the *previous* frame, and the first
  frame of a scene (or a new depth buffer) is left alone. The shift is a viewport shift, so the game's
  clip-space velocities stay jitter free.
* **Motion** (`knack_dlss.comp`, `MOTION_PASS`): the velocity target becomes DLSS motion vectors. DLSS wants
  render pixels, *previous minus current* (the programming guide: added to the pixel's position it gives where
  the pixel was), x right, y down; the game has *current minus previous* in NDC with y up, so the scale is
  `(-w/2, +h/2)`. Pixels at clear depth get 0.
* **DLSS** runs in front of the dispatch whose shader hash is `GOW3_KNACK_DLSS_BEFORE_CS` (default
  `526df215`), through the same bridge as the other upscaler (`vk_dlss.*`, `gpu/dlss_bridge`), at scale 1.0 with
  HDR input and auto exposure. **Render preset**: `knack_dlss_preset` / menu (K = 11 default, J = 10, 0 = the
  driver's default), passed on to NGX for every quality mode.
* **Merge** (`MERGE_PASS`): DLSS's colour replaces the RGB of the scene colour, its alpha stays; the game's
  post-processing, SMAA and UI then follow as always.

Known limits of this first stage: bloom and the half-size effects that read the scene colour before the
trigger see the aliased image; sky and particles have no velocity of their own (the sky is zero), so
fast camera turns can leave a trace on them; there is no reactive mask; no scene-cut detection (a gap of more
than 250 ms restarts the history).

## Settings

`gow3.ini` / environment (the environment wins at start): `knack_dlss` (`GOW3_KNACK_DLSS`, 0 or 1),
`knack_dlss_preset` (`GOW3_KNACK_DLSS_PRESET`: 0, 10 or 11). Environment only, for testing:

* `GOW3_KNACK_DLSS_BEFORE_CS=<hex hash>`: another compute dispatch to run in front of.
* `GOW3_KNACK_DLSS_FLIP_MV=1`, or `_FLIP_MV_X=1` / `_FLIP_MV_Y=1`: motion vectors with the other sign.
* `GOW3_KNACK_DLSS_FLIP_JITTER=1`, or `_FLIP_JITTER_X=1` / `_FLIP_JITTER_Y=1`: the jitter handed to DLSS with the
  other sign (the rendering itself is not changed).
* `GOW3_KNACK_DLSS_DEBUG=1`: shows the motion vectors DLSS gets instead of the image, signed (grey = none, redder =
  to the right, cyan = left, greener = down, magenta = up, 16 px = full); `=2` shows the magnitude.
* `GOW3_KNACK_DLSS_TEST=nojitter|nomotion|nodlss`: no jitter at all, zero motion vectors, or the jitter and the motion
  conversion without running DLSS (the image is the jittered frame as it is): tells which input is the problem.

## Log lines (`last_run.log`)

`DLSS: ready` (the bridge and NVIDIA's DLL were found; `DLSS ... preset 11` follows when the feature is made),
`KnackDlss: scene found, WxH`, `KnackDlss: running, WxH, preset N`, `KnackDlss: the 3D scene is drawn but
the compute dispatch ... never came`, `KnackDlss: the scene images do not fit: ...`, `KnackDlss: DLSS failed`.
The menu shows the same as "DLSS status".

## Runtime files and building

DLSS needs `gow3_dlss.dll` (the bridge, MSVC) and NVIDIA's `nvngx_dlss.dll` next to `gow3-probe.exe`
(`bin/` in the Windows package). `package.sh` refuses to ship NVIDIA's DLL, so the workflow **Knack DLSS
runtime** (`.github/workflows/knack-dlss-runtime.yml`) builds both on a Windows runner and keeps them as an
artifact, for personal use (NVIDIA's licence). The package itself is the regular **Windows zip** workflow.

## Carrying it to a newer project version

The change is small and separate: new files `vk_knack_dlss.*`, `knack_dlss.comp`, this document and the workflow;
and a few lines in `vk_rasterizer.{h,cpp}` (include, member, construction, one call at the end of
`PrepareRenderState`, the display-pass check, the jitter in the two draw paths, the dispatch), `vk_dlss.{h,cpp}`
(the preset), `gow3_settings.{h,cpp}` (three fields, two keys), `gow3_overlay.cpp` (the Image tab section),
`gpu/CMakeLists.txt` (two shader lines). `git diff` of the commit shows exactly those; the shader hashes are
the game's, so they stay valid across versions of the port.

## Diagnosing with NVIDIA's debug overlay

The SDK's *dev* library (`lib/Windows_x86_64/dev/nvngx_dlss.dll`, kept in the runtime artifact under `dev-debug/`;
it draws a watermark) has an on-screen overlay and hotkeys (DLSS Programming Guide, section 8): Ctrl+Alt+F12
cycles the view (input colour, motion vectors, depth, jitter plot), Ctrl+Alt+F9 tries the jitter sign / axis
configurations, Ctrl+Alt+F6 is the accumulation test for a still scene. Put it over `nvngx_dlss.dll` only for the test.
