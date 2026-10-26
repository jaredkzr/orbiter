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

**In the simulation:** `M` toggles the 3D map view with manoeuvre nodes;
`F5` opens replay. A gamepad works once `JoystickIndex` is set in
`Orbiter.cfg`.

## Configuration

`Orbiter.cfg` in the build directory (Orbiter writes it on exit):

```ini
PlanetTexDir = /path/to/Orbiter-2024/Textures\    ; planet tiles and .tex files (trailing backslash)
TerrainDetail = TRUE           ; procedural terrain detail finer than the elevation data
GroundScatter = TRUE           ; rocks, boulders, trees, cacti (large ones are obstacles)
PlanetResolutionBias = 0       ; +1: terrain tiles half the size on screen (finer, slower)
```

**Per planet** (`Config/<Planet>.cfg`): `TerrainDetail`,
`TerrainDetailHurst`, `TerrainDetailCraters`, `TerrainDetailCraterDepth`,
`TerrainDetailPebbles`, `TerrainDetailLevel` and `SurfaceSaturation`. They
are documented in `Src/Orbiter/TerrainDetail.h`.

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
| `ORBITER_VK_VIDEO=<dir>`, `ORBITER_VK_VIDEO_FRAMES=<n>` | write `n` frames as PNG files |
| `ORBITER_VK_VALIDATION=1` | Vulkan validation layer (messages in `Orbiter.log`) |
| `ORBITER_VK_PROFILE=1` | GPU time per render pass, and other statistics, in `Orbiter.log` |
| `ORBITER_VK_TILE_DEBUG=1` | terrain tile statistics; mesh against core elevation |
| `ORBITER_VK_MESH_CHECK=1` | compare GPU-built tile meshes with the CPU path |
| `ORBITER_VK_RT=0` | no ray tracing (shadow maps, SSAO, screen-space reflections) |
| `ORBITER_VK_RT_LIGHTING=0`, `ORBITER_VK_RT_REFLECT=0` | ray-traced lighting / reflections off |
| `ORBITER_VK_GPU_MESH=0` | build terrain meshes on the CPU |
| `ORBITER_VK_RENDER_SCALE=<f>` | 3D render resolution factor (upscaled by FSR 3) |
| `ORBITER_VK_UPSCALER=…`, `ORBITER_VK_TAA=0`, `ORBITER_VK_MSAA=<n>` | anti-aliasing and upscaling |
| `ORBITER_VK_SCATTER_GALLERY=1\|rock\|grass\|shrub\|conifer\|broadleaf` | lay out the scatter assets next to the focus vessel |
| `ORBITER_NO_TERRAIN_DETAIL=1`, `ORBITER_NO_SCATTER=1` | core: no procedural detail / no ground scatter |
| `ORBITER_VK_GG_TESTFLASH=<m>` | gas giants: a steady lightning flash `m` metres ahead of the camera |

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

## Tests

```bash
cd out/build/linux-x64-headless-release
ctest
```

- **Physics and scripting tests:** ground contact, collisions, docking,
  payloads, manoeuvres, replay, terrain detail and ground scatter. These run
  headless.
- **Vulkan tests:** `Launchpad.Cycle`, `Input.MapView` and `Input.Replay`
  render offscreen and need a Vulkan device.
- **Test files:** add new test scenarios to `Scenarios/Tests/` and their
  scripts to `Script/Tests/`. The build copies them into the build tree.

## Limitations

- The Windows build hasn't been compiled after these changes. Windows keeps
  its own launchpad and the D3D9 client.
- `Src/Plugin` (TransX, scenario editor and others) isn't ported to Linux
  yet.
- Vessel add-ons must be built for Linux from source. Windows `.dll`
  modules don't load, and the log names the bodies and modules that fall
  back.
