# Orbiter on Linux (Vulkan client)

On Linux, Orbiter builds as a native program: the simulation core, Lua
scripting, the vessel modules and the **Vulkan graphics client**
(`OVP/VulkanClient`, SDL3 windowing and input). The launchpad is an ImGui
dialog inside the client's window. Without the Vulkan client, Orbiter still
builds and runs headless: the simulation core only, for scripts and tests.

These instructions were tested on Ubuntu with GCC, CMake and Ninja, on an
AMD Radeon RX 9070 XT with Mesa RADV.

## Requirements

**Packages (Ubuntu/Debian names)**

```bash
sudo apt install build-essential cmake ninja-build git \
    libvulkan-dev glslang-tools libsdl3-dev \
    vulkan-validationlayers   # optional: Vulkan validation (ORBITER_VK_VALIDATION=1)
```

- **SDL3:** if `libsdl3-dev` isn't available, CMake downloads and builds SDL
  3.2.10 itself.
- **glslang:** if `glslangValidator` isn't installed, CMake builds glslang
  from source to compile the shaders.
- **Other libraries:** Vulkan Memory Allocator, Dear ImGui/ImPlot and the
  AMD FidelityFX SDK (FSR 3) are fetched during configuration, so the first
  configure needs network access.

**GPU:** a Vulkan 1.3 driver. Recommended, and used when present:

| Device feature | Used for |
| --- | --- |
| `bufferDeviceAddress` | tile meshes built on the GPU, ray tracing |
| `VK_KHR_ray_query` + `VK_KHR_acceleration_structure` | ray-traced shadows, ambient occlusion, reflections |

Without these features the client falls back to shadow maps, screen-space
effects and terrain meshes built on the CPU. Shaders use single precision
only: the terrain detail works from tile-relative coordinates, so GPUs with
slow or missing 64-bit float support run it at full speed.

**Planet data:** the repository has no planet textures. Use the textures of
the Orbiter 2024 release (or the Orbiter 2016 high-resolution packs): the
`Textures/<Planet>/Archive/*.tree` archives, plus the legacy
`Textures/<Planet>.tex` files for bodies without tiles. Point Orbiter at
them in `Orbiter.cfg` (see [Configuration](#configuration)).

**Sky data:** the simulated sky's star catalogue and dust map
(`Config/CSphere/Sky/stars.bin` and `dust.bin`, 190 MB) are too large for
git. Download them once, from the repository's root, before building (or
build them from their sources with `Utils/SkyKit`):

```bash
python3 Utils/SkyKit/download.py
```

## Building

```bash
git clone --recursive https://github.com/orbitersim/orbiter.git
cd orbiter
cmake --preset linux-x64-headless-release -DORBITER_SPICE_CELBODIES=OFF
cmake --build --preset linux-x64-headless-release
```

- **Build tree:** `out/build/linux-x64-headless-release/`. That directory is
  the Orbiter installation: the executable `Orbiter`, `Modules/`, the
  Vulkan client `Modules/Plugin/VulkanClient.so`, and `Config/`, `Meshes/`,
  `Textures/` and `Scenarios/` copied from the source tree.
- **Debug build:** use the `linux-x64-headless-debug` preset.
- **SPICE ephemerides:** `-DORBITER_SPICE_CELBODIES=OFF` builds the
  VSOP87/ELP82 modules instead. SPICE needs the planetary kernels (`*.bsp`,
  sources listed in `Kernels/spk_sources.txt`) in `Kernels/`.
- **The Vulkan client** is built when Vulkan and SDL3 are found
  (`-DORBITER_BUILD_VULKANCLIENT=ON/OFF` overrides this). FSR 3 can be left
  out with `-DORBITER_VK_FSR=OFF`.
- **New files:** after adding source files, run `cmake .` in the build
  directory before building again.
- **Copied data:** each build copies `Config/`, `Meshes/`, `Textures/`,
  `Scenarios/` and `Script/` into the build tree. Changes made only in the
  build tree are overwritten.

## Running

Run from the build directory, which is the Orbiter root:

```bash
cd out/build/linux-x64-headless-release
./Orbiter -p VulkanClient                         # the launchpad, in the client's window
./Orbiter -p VulkanClient --scenariox="Delta-glider/Brighton Beach"   # a scenario directly
./Orbiter --scenariox=Tests/ContactTest --fixedstep=0.02             # headless: no graphics
```

**Command-line options:** `./Orbiter --help` lists them all. The most useful:

| Option | Effect |
| --- | --- |
| `-p <plugin>` | load a plugin module (`VulkanClient` for graphics) |
| `--scenario=<scn>` / `-S` | start a scenario (`Scenarios/<scn>.scn`) |
| `--scenariox=<scn>` / `-s` | start a scenario and exit when it closes |
| `--fixedstep=<s>` | fixed time step [s]: repeatable, not bound to real time |
| `--maxframes=<n>` | end the session after `n` frames |
| `--maxsystime=<t>` | end the session after `t` seconds |

**The launchpad:** without a scenario on the command line, Orbiter opens
the launchpad over a live scene (a session of its own, never saved). It
lists the scenarios with their preview images (`<scenario>.jpg` beside the
scenario file, rendered with `Utils/ScenarioShots/render.py`), the
recently launched ones first. Enter launches the selected scenario, Ctrl+Q
quits.

**In the simulation:** `M` toggles the 3D map view with manoeuvre nodes;
`F5` opens replay. A gamepad works once `JoystickIndex` is set in
`Orbiter.cfg`.

**Playtesting:** `Utils/Playtest/orbctl.py` plays Orbiter from the
command line (keys, clicks on buttons by their labels, screenshots, the
flight's state), for a person or an agent; see `Utils/Playtest/README.md`.

**External camera:** `F1` switches between the cockpit and the outside
view; `F2` cycles the camera modes (`Shift+F2` back), and the Camera dialog
(`Ctrl+F1`) lists them:

| Mode | The camera |
| --- | --- |
| Auto | Free near a planet (in its atmosphere, or low over it), Orbital higher up |
| Free | orbits about the local vertical; the horizon stays level and the view stays put as the vessel turns |
| Orbital | turns with the orbit (up: the orbit's normal), so the planet stays put on screen |
| Chase | behind the direction of travel, horizon level, with a little lag |
| Locked | fixed to the vessel's axes; turns all the way round it |
| Flyby | watches the vessel pass from a point ahead on its path, then moves ahead again |

Right-drag or `Ctrl`+arrows turn the camera about its target (Free, Orbital
and Chase stop just short of straight overhead), the wheel or
`PgUp`/`PgDn` change the distance, `Ctrl`+wheel the field of view; `Home`
puts the camera behind the target (Flyby: ahead again). Scenarios with the
classic track modes open in the closest new mode: Target relative in
Locked, the others in Free with the camera where they put it; saving writes
the new mode.

## Configuration

`Orbiter.cfg` in the build directory (Orbiter writes it on exit):

```ini
PlanetTexDir = /path/to/Orbiter-2024/Textures\    ; planet tiles and .tex files (trailing backslash)
TerrainDetail = TRUE           ; procedural terrain detail finer than the elevation data
GroundScatter = TRUE           ; rocks, boulders, trees, cacti (large ones are obstacles)
PlanetResolutionBias = 0       ; +1: terrain tiles half the size on screen (finer, slower)
UIScale = 1                    ; interface size, on top of the scaling with the window's height (1 at 1080 lines)
LaunchpadBackdrop = TRUE       ; a live scene behind the launchpad (FALSE: a plain background)
```

The scene behind the launchpad is `Config/Launchpad/Backdrop.scn`;
`ORBITER_LAUNCHPAD_BACKDROP=0` turns it off for one run.

**Graphics** (the Settings screen's Graphics page, on the launchpad and in
flight with F6): the Vulkan client keeps its settings in
`Config/VK/Graphics.cfg` (anti-aliasing, render resolution, ray tracing,
effects) and the sky's display mode and parts in `Config/VK/Sky.cfg`. A
quality preset (Low, Medium, High, Ultra) sets the lighting, effects and sky
at once. Some settings take effect when a scenario is launched; they are
marked as such in flight. An `ORBITER_VK_*` variable below still overrides a
setting, which the page then shows locked. Offscreen runs (tests,
benchmarks, scenario images) leave both files alone and use the defaults,
unless `ORBITER_VK_GRAPHICS_CFG=<file>` names a file for all the graphics
settings.

**Per planet** (`Config/<Planet>.cfg`): `TerrainDetail`,
`TerrainDetailHurst`, `TerrainDetailCraters`, `TerrainDetailCraterDepth`,
`TerrainDetailPebbles`, `TerrainDetailLevel` and `SurfaceSaturation`. They
are documented in `Src/Orbiter/TerrainDetail.h`.

**Per base** (`Config/<Planet>/Base/<Base>.cfg`): `BareEarth = <radius> [<blend>]`
(metres; documented in `Src/Orbiter/Base.h`) levels the terrain within `radius` of
the base to the ground under the elevation data's trees and buildings (the
data are surface models), blended back over `blend` (3000). Its pads and runways
are graded on that ground. Areas to leave alone (ground built up on purpose, the
base's meshes set on the data there) are listed as `x z radius` lines between
`BEGIN_BAREEARTH_KEEP` and `END_BAREEARTH_KEEP`: Cape Canaveral's launch complex 39
pads. Also on at Wallops Island, Matagorda, Sriharikota, Peenemünde and Kourou
(flat, wooded ground); not where the filter would cut real relief (`TODO.md`). Cape Canaveral has `BareEarth = 40000`: the canopy
stood 5-10 m over its flat coastal land, as spiky hills about the runways.
`Scenario.CapeGroundTest` checks the ground there.

**Runway surfaces:** a base file's `RUNWAY` with `MARKINGS precision|nonprecision|visual|none`
(and `DESIG <d1> <d2>`, `OVERRUN <m1> [<m2>]`, `SURFACE concrete|asphalt [<m>]`;
documented in `Src/Orbiter/Baseobj.h`) gets a pavement texture of 25 ft slabs
(`RunwayConcrete`, `RunwayAsphalt`: an atlas of 4 x 4 different slabs) instead of
an `RWSEG` layout, and its markings are drawn by the Vulkan client at their real
size from that description (`Shaders/runway.glsl`: FAA threshold stripes,
designators, touchdown zone, aiming point, centreline, side stripes, overrun
chevrons; slabs' shade, stains, rubber, transverse grooves): sharp at any
distance, no texture of their own. Each slab of a runway takes one of the
atlas's slabs, turned or mirrored one of 8 ways, by a hash of its place (other
clients tile the atlas: it repeats every 100 ft). The core
hands the client the runways with `oapiGetBaseRunways`; their mesh groups carry
`MESHGRP_RUNWAY`. All of Earth's paved runways have them: Cape Canaveral's Shuttle
Landing Facility and Skid Strip with their ends from the FAA, the rest with their
ends and widths as they were, designators from their headings (Edwards, Baikonur
and Havana: the real ones). Base ground meshes (runways, pads) also get a fine
grain below their textures' texels (`mesh.frag` `GroundDetail`), and wheels
leave no tracks on paved ground (`oapiSurfacePaved`: runways with their overruns
and shoulders, pads, decks). The pavement textures:

```bash
python3 Utils/runway/make_pavement.py [--preview <dir>]
```

**Landing pads** (`LPAD1`, `LPAD2`, `LPAD2A`, all bodies) are drawn the same way
(`Shaders/pad.glsl`, `oapiGetBasePadSpec`, `MESHGRP_PAD`), after heliports and
rocket landing zones: a hazard band at the edge (the octagon's raised rim), a white
perimeter stripe, a yellow touchdown circle, the pad's number large in white, the
blast's scorch at the centre; concrete slabs on Earth, slabs of sintered regolith
elsewhere (`PadPavers`, tinted to the ground about the base: `SURFACE concrete|pavers`
overrides), green perimeter and yellow touchdown lights at night. `STYLE classic` in
an `LPADx` block keeps its old texture. The ground under a pad is levelled over
all of it (a square's corners too), pads side by side to one level, and the
pad's mesh follows the planet's curvature across it (pads kilometres from the
base's centre were tilted against the ground, partly under it).

**Base buildings and kits** (the overhaul of the bases' assets, in progress):
- Generic buildings (`BLOCK`, `HANGAR`, `HANGAR2`, `HANGAR3`, `TANK`) are drawn by the Vulkan
  client from their description (`oapiGetBaseBuildings`; groups flagged `MESHGRP_BUILDING`),
  with the city kit's shapes and shaders (`CityKit` `MakeBase`, `building.frag`): facades with
  real-size windows and doors by their old textures' look (`Fcd01`-`15`, `Wall01`, ...), hangar
  doors, round roofs and arches, tanks; off-world (under 10 kPa) pressurised modules with
  portholes and airlocks, long halls and arches under regolith. The core's geometry stays the
  collision shape and other clients' look. `ORBITER_VK_BASE_BUILDINGS=0`: the core's, for A/B.
- The kits' meshes (`Meshes/BaseKit`, off-world so far) are built in Blender:
  `blender -b --python Utils/BaseKit/blender_kit.py -- offworld <asset|all> --check`
  (`kitlib.py`: shapes, world-scale texturing, groups by material and role). Their textures are
  tiling materials at real size (`Utils/BaseKit/kittex.py` → `Textures/BaseKit/kit_*`, with
  `_norm`, `_rghn`, `_metal`, `_night`). `--check` runs `Utils/BlenderMsh/msh_check.py
  --bounds-json Utils/BaseKit/footprints.json`: footprints and heights as the layouts expect.
- Mesh group flags: `MESHGRP_NOCOLLIDE` (0x100: detail out of the collision hulls),
  `MESHGRP_COLLISION` (0x200, with hidden 0x2: a collision proxy; a mesh with any collides with
  those alone). Hidden groups are tested by their user flag (they were tested by the texture-wrap
  flags).
- Night-only emission: a texture's `<name>_night.dds` (and the legacy `<name>_n.dds` night
  textures of the generic buildings) are lit as the base's sun sets.
- Kit lights (aviation reds, door lamps): `Utils/BaseKit/kit_lights.json` (written by
  `blender_kit.py`), placed by `python3 Utils/BaseKit/kitlights.py` as `LIGHT x h z r g b size`
  lines (`LIGHTABS` on a floating base) in `Config/VK/Bases/<Name>.cfg`.
- The kits' ground (roads, tracks, aprons: `Meshes/BaseKit/<Base>_ground.msh`) is built from
  its description (`Utils/BaseKit/ground/<Base>.json`) by `python3 Utils/BaseKit/ground.py build
  [Base]`: roads go around the bases' pads (and off-world pads' blast berms), aprons have holes
  where pads stand (the ground mesh lies a few cm over the pads: it hid them). It writes the
  roads' and aprons' lights too (the `groundlights` block of `Config/VK/Bases/<Name>.cfg`: edge
  markers along tracks and taxiways, streetlights, apron masts and off-world work lights along
  the tracks about a base's core, with `FLOODLIGHTH` - a floodlight's height above the ground
  there). The glows (`LIGHT`) light nothing; only floodlights do (the nearest 64 in a frame). `basekit.py`/`offworld.py layout` go through it.
- The client-drawn buildings have lamps at night (`CityKit` `Kit::BaseLights`): over hangar
  doors (and a floodlight on the apron before them), airlocks and doors, amber markers along
  covered tunnels, red obstruction lights on roofs 40 m and higher and on pressurised hubs.
- Before/after screenshots of the showcase bases: `python3 Utils/BaseKit/shots.py scenes`
  (`Scenarios/VkTest/base_*.scn`), `shots.py render --label L [--only X] [--validate]
  [--env ORBITER_VK_BASE_BUILDINGS=0]`, `shots.py compare A B` (`out/baseshots/`);
  `python3 Utils/BaseKit/shotview.py` serves them on the LAN (port 8766: side by side, wipe,
  flip; phones too).

**Ground scatter:** layers and plants are in `Config/Scatter/<Body>.cfg`
(documented in `Src/Orbiter/GroundScatter.h`). The rock meshes and textures
are made with Blender:

```bash
blender -b --python Utils/ScatterAssets/rocks.py -- <Moon|Mars|Earth|Ice|Io|Venus|Titan> .
```

## Environment variables

These are for testing and tuning (`1` turns a feature on, `0` off).

| Variable | Effect |
| --- | --- |
| `ORBITER_VK_OFFSCREEN=1` | render without a window (servers, automated tests) |
| `ORBITER_VK_SCREENSHOT=<file.png>` | save a screenshot … |
| `ORBITER_VK_SCREENSHOT_FRAME=<n>` | … at frame `n` |
| `ORBITER_VK_SCREENSHOT_EXIT=1` | … and exit afterwards |
| `ORBITER_VK_SCREENSHOT_NOUI=1` | … without the user interface, HUD and panels (scenario previews: `Utils/ScenarioShots/render.py`) |
| `ORBITER_VK_FRAMETIME=<s>` | a fixed frame time for exposure, sky adaptation and the upscaler: with `--fixedstep`, images that do not depend on how fast the frames were (A/B comparisons) |
| `ORBITER_VK_VIDEO=<dir>`, `ORBITER_VK_VIDEO_FRAMES=<n>` | write `n` frames as PNG files |
| `ORBITER_MIN_WARP=<w>` | time acceleration allowed down to `w` (below the usual 0.1): deep slow motion in renders, e.g. `Tests/Audio/TrailerTides.scn` (a black hole's break-up at 0.015) |
| `ORBITER_VK_VIDEO_FILE=<file.mp4>` | stream every frame into ffmpeg, encoded to that file (H.264, on the GPU with VAAPI, else x264; `ORBITER_VK_VIDEO_CODEC=x264` forces it): no frames on disk, and offscreen (`ORBITER_VK_OFFSCREEN=1`) no screen needed. `ORBITER_VK_VIDEO_FPS` its frame rate (30). With `--fixedstep=0.0333333` a frame a step, and `ORBITER_AUDIO_RENDER` the game's sound alongside, to join to it |
| `ORBITER_VK_VALIDATION=1` | Vulkan validation layer (messages in `Orbiter.log`) |
| `ORBITER_VK_PROFILE=1` | GPU time per render pass, and other statistics, in `Orbiter.log` |
| `ORBITER_VK_FRAMETIMES=<file>` | a line per frame: frame, sim time, wall time, GPU time (with `ORBITER_VK_PROFILE`, per pass), the client's CPU time and its wait on the GPU (`Tests/Bench/analyze.py`) |
| `ORBITER_VK_TILE_DEBUG=1` | terrain tile statistics; mesh against core elevation |
| `ORBITER_VK_MESH_CHECK=1` | compare GPU-built tile meshes with the CPU path |
| `ORBITER_VK_RT=0` | no ray tracing (shadow maps, SSAO, screen-space reflections) |
| `ORBITER_VK_RT_LIGHTING=0`, `ORBITER_VK_RT_REFLECT=0` | ray-traced lighting off / no ray tracing in the scene (the hardware's ray queries stay enabled) |
| `ORBITER_VK_GRAPHICS_CFG=<file>` | read and write the graphics settings in this file (offscreen runs use none by default) |
| `ORBITER_VK_GPU_MESH=0` | build terrain meshes on the CPU |
| `ORBITER_VK_RENDER_SCALE=<f>` | 3D render resolution factor (upscaled by FSR 3) |
| `ORBITER_VK_UPSCALER=…`, `ORBITER_VK_TAA=0`, `ORBITER_VK_MSAA=<n>` | anti-aliasing and upscaling |
| `ORBITER_VK_SCATTER_GALLERY=1\|rock\|grass\|shrub\|conifer\|broadleaf` | lay out the scatter assets next to the focus vessel |
| `ORBITER_NO_TERRAIN_DETAIL=1`, `ORBITER_NO_SCATTER=1` | core: no procedural detail / no ground scatter |
| `ORBITER_VK_GG_TESTFLASH=<m>` | gas giants: a steady lightning flash `m` metres ahead of the camera |
| `ORBITER_VK_SHAKE=<f>` | scale of the cockpit view's jolts from turbulence and buffeting (`0` off) |
| `ORBITER_AUDIO_RENDER=<file.wav>` | mix the sound offline into a WAV file (32-bit float, 48 kHz), one time step of sound per time step: with `--fixedstep`, repeatable renders (`Tests/Audio/analyze.py`) |
| `ORBITER_AUDIO=0`, `ORBITER_AUDIO=1` | no sound / sound on the output device even headless or offscreen (by default only with a graphics client, and not offscreen) |
| `ORBITER_AUDIO_REALISM=physical\|cinematic` | sound in space as set, whatever the settings say (tests) |
| `ORBITER_AUDIO_TEST_TONE=<vessel>[:<Hz>[:<level>]]` | a sine on a vessel, heard as any sound source: travel time, Doppler, shock cone (tests) |
| `ORBITER_AUDIO_LOG=1` | what is heard of each sound source (travel time, Doppler, levels), twice a second in `Orbiter.log` (`2`: silent ones too); sonic booms also on the console (`events`: the moments only - callouts, booms, the drive - not each source's state) |
| `ORBITER_AUDIO_VESSELS=0` | no sounds of the vessels themselves (engines, thrusters, air): only test tones |
| `ORBITER_AUDIO_ADAPT=1`, `ORBITER_AUDIO_ADAPT=0` | the adapting listening level on / off, whatever the settings' "Dynamic range" says (renders: off by default, the physical levels) |
| `ORBITER_AUDIO_RADIO=0` | no radio: chatter, the ground's replies (tests) |
| `ORBITER_AUDIO_MUSIC=1`, `ORBITER_AUDIO_MUSIC=0` | the music on / off, whatever the settings say (renders: off by default) |
| `ORBITER_AUDIO_HRTF=1`, `ORBITER_AUDIO_HRTF=0` | binaural sound (headphones, `Sound/HRTF/kemar.hrtf`) on / off, whatever the settings' "Headphones" says |
| `ORBITER_AUDIO_SOLO=<buses>` | renders: only these buses heard (`outside`, `cockpit`, `voice`, `radio`, `music`, `interface`; comma-separated) - the mix's balance measured |
| `ORBITER_AUDIO_RENDER_VOLUMES=1` | renders: mixed at the settings' volumes (master, buses), not at full - listening reels |
| `ORBITER_AUDIO_VOICES=<n>` | the voice cap: continuous sources playing at most (default 32; the rest, and those not heard, stopped) |
| `ORBITER_AUDIO_AMBIENCE=0` | no ambience: the cabin's air handling, the wind and the sea about the listener (tests) |
| `ORBITER_AUDIO_CONTACTS=0` | the vessels' contact sounds off (tests; on by default). They are recorded takes: `Sound/Contacts/<layer>/*.flac`, listed in `Sound/Contacts/contacts.txt` (`<file> <layer> <tier> <energy>`; without it silent; a layer missing: silent, one warning), cut from Freesound recordings credited in `Sound/Contacts/SOURCES.md` and `Sound/CREDITS.md`: the wheels' touchdown thumps (`impact_gear`, one take per wheel as it touches, by the blow's energy), a tyre's chirp (`chirp`: a wheel touching down sliding over 15 m/s, once per touchdown, played as long as the wheel takes to spin up, as loud as the friction's work - in the cockpit a dull zip, -25 dB), rolling (`rolling`: steady takes played as random crossfading stretches, as v^1.5 sqrt(load), its pitch with speed; in the cockpit a rumble below 400 Hz) and braking and the tyres' scrub (`brake`: as the root of their friction power); the hull's, legs' and obstacles' blows (`impact_metal`, tiered as the gear's), their scraping (`scrape`: a third bed, as the root of the friction power, higher and brighter the faster it slides, louder at its first bite; in the cockpit a grind below 600 Hz), a part torn off (`tear`, and `clatter` 0.1-0.3 s on: as loud as the piece's energy at the speed it was struck, heavier lower) and a crash (`crash`, with tears staggered over 0.3 s and the clatter: 1e-4 of the kinetic energy) - these from where they happened on the planet, heard out after the vessel is gone (nothing across vacuum). Docking: the ports' blow, the capture `latch`, a ripple of 4-8 `hook`s 80-160 ms apart, the `seal` within 2 s; undocking: the `release`, 2-4 hooks, the push-off - in the cabins (the docked vessels' structure) at fixed levels (0.1-0.35 Pa), outside 15 dB less, in vacuum nothing. Animations' motors (`motor`: looped takes, two at once): in the cabin 0.04-0.09 Pa by kind (servo, door, gear), a little higher the faster, wobbling; outside 12 dB less; stopped at an end, the lock (a `latch` take), the gear's thud, a door's `seal`. With `ORBITER_AUDIO_LOG=1`: `Audio: contact of <vessel>: chirp wheel 1 at ...`, `tear at ...`, `crash heard: ...`, `docking heard: ...`, `Audio: motor of <vessel>: animation 13 (door) starts at ...` and, twice a second, the beds' and motors' levels. Docking and animations to hear: `Tests/Audio/DockVideo.scn` (`DOCK_VIEW=cockpit|outside`), `Tests/Audio/AnimVideo.scn` (`ANIM_VIEW=cockpit|ground`). Landings to hear them in: `Tests/Audio/LandingVideo.scn`, `LandingVideoBelly.scn` (`LV_GEAR=up`), `LandingVideoCrash.scn` (`LV_GEAR=up LV_CRASH=1 ORBITER_DAMAGE=1`) (`Script/Tests/LandingVideo.lua` flies the approach, flare and rollout; `LV_VIEW=ground|cockpit`, `LV_SINK=<m/s>`, `LV_TD=<m>`: the ground camera's place past the threshold; `LV_SIDE=<m>`: the approach that far right of the centreline). The ground: paved (a base's runways, pads, decks), regolith (an airless body), else by the surface texture as the ground scatter reads it - soil (green), rock (not green: bare, arid, dry lakes), snow (bright, grey, not warm, and only above 50 degrees of latitude or 1500 m), the water mask counting as wet soil - each with its own takes where it has them, else the paved ones (one note): on soil and rock `rolling_soil` and `scrape_soil`, a `spray` of dirt instead of the chirp (1e-6 of the spin-up's work) and at the hull's blows (1e-5 of their energy); on snow the ground's rolling and scrape, no chirp; on regolith the gear's and hull's `crunch` at every blow, through the structure only (1e-5), the rolling quieter (-6 dB) and, there being no air, heard only inside. Off any runway: `Tests/Audio/LandingVideoDesert.scn` (Rogers Dry Lake: rock), on the Moon in daylight `Tests/Audio/LandingVideoMoon.scn` (`CONTACTS_VIEW=cockpit|outside`, `CONTACTS_SPEED=<m/s>`). Their moments are logged with `ORBITER_AUDIO_LOG` whether on or not: `Audio: contact of <vessel>: impact wheel 1 at 1.280 s: 2.54 m/s, 2.974e+04 J, paved` (a point touching: which, its closing speed, the blow's energy, the ground - paved, soil, rock, snow, regolith; `spray wheel ...` for a chirp on soil), `dock`/`undock` (closing speed, reduced mass), `break` (a part torn off: its mass, the speed it was struck at), `crash` The aftermath of a break-up (Audio/Effects.cpp, laws in Audio/Contacts.h) from the same recordings: a blast's boom (`explosion` takes, heard softly no louder than 16 Pa RMS, with an echo 0.6-1.2 s on, dulled), each burning piece's flames (a looped `fire` bed instead of the synthesized fire), then its smoulder (the bed 30 dB down, dulled, 45 s) and hot metal ticking as it cools (`tick`, sparser over a minute), each piece landing (`debris_land`, else `clatter`, then `impact_metal`; at most 3 in 0.1 s). With `ORBITER_AUDIO_LOG`: `Audio: boom heard ...`, `Audio: fire of <piece> ...`, `Audio: debris <piece> lands ...`, `Audio: tick of <piece> ...`. |
| `ORBITER_AUDIO_CALLOUTS=0`, `ORBITER_AUDIO_CALLOUTS=1` | the voice's callouts off / on, whatever the settings say (tests); with `ORBITER_AUDIO_LOG`, each callout on the console |
| `ORBITER_AUDIO_VOICE=orbiter`, `ORBITER_AUDIO_VOICE=xrsound` | the callouts in Orbiter's voice (`Sound/Voice`, also for XRSound's default lines in XR vessels and add-ons) / XRSound's recordings, whatever the "Callout voice" setting says |
| `ORBITER_AUDIO_CAPCOM=1`, `ORBITER_AUDIO_CAPCOM=2` | Mission Control's replies always from this CAPCOM, whatever the shift (by default they take turns, 8-hour shifts by the sim clock from 06:00 UTC) |
| `ORBITER_AUDIO_ASYNC=1` | renders: sound files decoded in the background, as on the device (each sound starts when its file is there, a step or so later; `Audio.*Async`) |
| `ORBITER_AUDIO_FLIGHT_TRACE=<file>` | the focus vessel's flight as the sound follows it (a sample a step: `Src/Orbiter/Audio/FlightState.h`), for `Tests/Audio/*.trace` |
| `ORBITER_XR_PA=<cockpit>,<close>` | XRSound's volume 1 as pressures [Pa], in the cockpit and close on a vessel (default `63.2,15.8`: full scale; listening tests) |
| `ORBITER_LOG=<file>` | the log written there, not `Orbiter.log` (the tests: each its own, `Testing/logs/<test>.log`) |
| `ORBITER_NO_SAVE=1` | nothing saved at a session's start or end: the settings, "(Current state)", the MFD windows (the tests: they run side by side in one folder) |
| `ORBITER_RADIO_CODEC=0` | the radio degrades with distance without its low-rate codec (only the narrower band, the fading, the dropouts; listening tests) |
| `ORBITER_RADIO_REEL=<dir>` | `Audio.Synth "[radio-reel]"` (a hidden test): Mission Control's lines through the radio near Earth, at the Moon and further, as WAVs in `<dir>` (`ORBITER_RADIO_REEL_FILES`: which lines) |

Example: an offscreen screenshot of a test scene, validated and profiled:

```bash
ORBITER_VK_OFFSCREEN=1 ORBITER_VK_VALIDATION=1 ORBITER_VK_PROFILE=1 \
ORBITER_VK_SCREENSHOT=/tmp/shot.png ORBITER_VK_SCREENSHOT_FRAME=400 ORBITER_VK_SCREENSHOT_EXIT=1 \
./Orbiter -p VulkanClient --fixedstep=0.016667 --scenariox=VkTest/cine_Moon_close
```

**Test scenes** in `Scenarios/VkTest`:
- `body_<Name>`: a delta-glider landed on each of the 28 bodies with a surface
- `cine_*`: ground-level cameras on the Moon, Mercury and Mars
- `field_*`: forest, desert and Moon scatter
- `gallery_*`: the scatter assets of Earth, Moon and Mars
- `orbit_Venus`, `orbit_Titan`: Venus and Titan from orbit
- `jupiter_*`, `saturn_*`, `uranus_*`, `neptune_*`: the gas giants' cloud
  decks (`_high` above them, `_storm` at a storm, `_plunge` a descent
  through them)

**Memory:** dense forest scenes can map more than 8 GB of virtual memory
for the GPU, although only about 0.5 GB is resident. A `ulimit -v` cap
below about 12 GB makes them fail, for example when saving screenshots.

## Regenerating the voices and sounds

The voices (`Sound/Voice`, `Sound/Voice/Ground`) and the generated sounds
(`Sound/Generated`) are made offline with local models and committed; no
model runs in the game. Each file can be made again exactly: the lines
files keep the kept seed of every line (`lines.takes.json`), and
`Sound/Generated/prompts.txt` the prompt and seed of every sound. The
models, revisions and licences: `Sound/CREDITS.md`.

They need a GPU with about 16 GB (here: ROCm on an RX 9070 XT) and Python
3.12 venvs outside the repository, with PyTorch for the GPU:

- voices: `qwen-tts` and `faster-whisper` (Qwen3-TTS-12Hz-1.7B-Base, Whisper
  `small.en` as a gate);
- sounds: the MOSS-SoundEffect v2.0 pipeline (`moss_soundeffect_v2`).

Stop anything else holding the GPU's memory first (a local LLM server).
On ROCm, `MIOPEN_FIND_MODE=FAST` (otherwise about 50 s a line) and, for
the sounds, `TORCHDYNAMO_DISABLE=1`.

```bash
export MIOPEN_FIND_MODE=FAST
# the onboard voice (crew, commander): every line without its file
<voice-venv>/bin/python Utils/Audio/make_voice.py --engine qwen3
# Mission Control (both CAPCOMs): endings that fall, the voice held
<voice-venv>/bin/python Utils/Audio/make_voice.py --engine qwen3 --lines Sound/Voice/Ground/lines.txt \
    --max-rise -1.0 --end-stop -24 --min-voice-sim 0.945
# a line again, new takes to choose from, then the one chosen kept
<voice-venv>/bin/python Utils/Audio/make_voice.py --engine qwen3 --only "Gear Down" --candidates 3 --review /tmp/review
<voice-venv>/bin/python Utils/Audio/make_voice.py --engine qwen3 --review /tmp/review --keep "Gear Down=7"
# the sounds
TORCHDYNAMO_DISABLE=1 <sfx-venv>/bin/python Utils/Audio/make_sfx.py [--only <name>...] [--force]
# every callout the game can say has its file
python3 Utils/Audio/check_voice.py
```

`--force` remakes files from their kept seeds (the same audio on the same
model). The scripts' own help has the rest: the gates that reject a take
(off script, cut off, a rising ending, a voice drifting from its
reference), the shaping, and the post-processing of the sounds. A take
the gates accept is still listened to; the gates never choose between
good takes.

## Tests

```bash
cd out/build/linux-x64-headless-release
ctest
```

- **Physics and scripting tests:** ground contact, collisions, docking,
  payloads, manoeuvres, replay, terrain detail and ground scatter. These run
  headless.
- **Vulkan tests:** `Launchpad.Cycle` and the `Input.*` tests (map view,
  replay, MFDs, camera modes) render offscreen and need a Vulkan device.
- **Unit tests:** transfer conics and routes, relativity, Kerr geodesics,
  collision geometry, the camera's geometry (`Camera.Rig`) and the audio
  engine (`Audio.Engine`).
- **Audio tests:** sessions rendered to WAV files and checked with
  `Tests/Audio/analyze.py` (levels per band and time, frequencies, stereo
  balance, clipping, clicks; `--png` draws a spectrogram): `Audio.Render`
  (silence), `Audio.Flyby*` (a tone flown past a listener: travel time,
  Doppler, the shock cone of a supersonic pass), `Audio.Cockpit*` (the
  cockpit and the vacuum, physical and cinematic, time acceleration),
  `Audio.Takeoff` and `Audio.MoonLiftoff` (a vessel's own engines, on Earth
  and on the Moon), `Audio.Contacts*` (touchdown, rolling, a belly's
  scrape), `Audio.Docking`, `Audio.Api` (the sound API: files on vessels and
  the interface, an animation's motor; `Audio.ApiAsync`: their files decoded
  in the background). Unit tests: `Audio.Engine`, `Audio.Propagation`,
  `Audio.Synth`, `Audio.World` (the world without Orbiter: fly-bys, the
  cone, the cockpit, vacuum, handles, a recording's travel time),
  `Audio.Flight` (the flight's events from recorded traces and tables; the
  crew's lines' table; the environment's options), `Audio.Churn` (the audio
  thread's rules: no allocation while rendering, sources made and destroyed
  while a real audio thread renders them, a pause holding the sounds, levels
  ramped over a step, every blow struck; `Audio.ChurnTSan`: the same under
  ThreadSanitizer).
- **The crew's lines:** `Sound/Voice/callouts.cfg` (each line's file,
  priority, wait, level and cooldown; when a line is said is the code's).
  XRSound's cfgs and a vessel's code may give a line a file of their own.
  Mission Control's replies: `Sound/Voice/Ground/replies.cfg` (the moments
  the ground answers, and with which takes).
- **Repeatable runs:** with `--fixedstep`, everything that moves with the
  frame - the camera's transitions, the time acceleration's ramp, the MFDs'
  and planners' updates - goes by the fixed step (`TimeData::UiT1`), not the
  wall clock; each test has its own log and saves nothing (`ORBITER_LOG`,
  `ORBITER_NO_SAVE`). Scripts' `oapi.get_systime ()` stays the wall clock:
  watchdogs only. The tests read the build's `Orbiter.cfg` (the planet
  textures' folder: the high-resolution coasts and terrain).
- **Sound of vessel classes:** `Config/Sound/README.txt` (tuning, motors of
  animations); the API in `OrbiterAPI.h` (`oapiAudio*`) and Lua's `audio`
  table.
- **Test files:** add new test scenarios to `Scenarios/Tests/` and their
  scripts to `Script/Tests/`. The build copies them into the build tree.

## Limitations

- **Sound:** the voice and generated-sound overhaul (the cast, Mission
  Control, the generated effects) started from what XRSound already had:
  its callout list, its sound slots and cfgs, its default files as the
  fallback, and the behaviour built around them. It re-voices and extends
  that design rather than rethinking it. A full audit of Orbiter's sound -
  what is heard, when and why, and which of XRSound's choices to keep - and
  a wider overhaul is recommended.
- The Windows build hasn't been compiled after these changes. Windows keeps
  its own launchpad and the D3D9 client.
- `Src/Plugin` (TransX, scenario editor and others) isn't ported to Linux
  yet.
- Vessel add-ons must be built for Linux from source. Windows `.dll`
  modules don't load, and the log names the bodies and modules that fall
  back.
