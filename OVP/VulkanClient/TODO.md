# Vulkan client: open items

Known gaps and limitations, to be addressed later.

## Atmosphere
- Compared with photographs (2026-10-24): sky radiance scale 6 (was 16),
  daylight exposure cap 1.3, Mars dust (butterscotch sky; blue about the
  sun from a narrower forward lobe for blue light, AtmMiePhase), airglow, aurora
  (ORBITER_VK_AURORA scales it; fixed oval, no activity model), cloud-top
  relief from the cloud density. The DG's HUD glass shows strongly green
  at high night exposure.
- Procedural surface detail (surfdetail.glsl, ORBITER_VK_SURFACE_DETAIL=0):
  craters/rocks/grain; rocks read as pits without cast shadows.
- Ground tracks (GroundTracks.cpp, tile_main.glsl's TrackShading): the
  first three touchdown points of vessels on bare terrain within 3 km leave
  grooves with raised edges and tread (shading only, not in the meshes or
  physics), imprints where they touch down; 4096 segments per session, not
  saved. Track size from the vessel's size and mass. KSC's elevation data has 10-20 m bumps (canopy)
  that look like hills from a few metres up.
- Precomputed transmittance and multiple-scattering tables (Hillaire 2020);
  the single scattering is still ray-marched per pixel (16 steps). A
  sky-view table and an aerial-perspective volume would make it cheaper.
  Ozone only for Earth. The 1-exp(-L) sky tonemapping (AtmTonemap) is
  legacy; with auto-exposure the sky could be linear radiance.

## Engine plumes
- Ray-marched plume volumes (Plumes.cpp, plume.frag; ORBITER_VK_PLUMES=0
  restores the flame billboards). Engine classes are inferred from Isp and
  thrust, overridable in Config/VK/Plumes/<class>.cfg (see README.txt);
  ORBITER_VK_PLUME_TYPE forces a class for testing.
- Plumes write no motion vectors or reactive mask: TAA/FSR may smear them
  over moving backgrounds (planned with the GPU particles).
- Airflow bends plumes along a jet-in-crossflow trajectory (momentum flux
  = thrust over exit area against the crossflow's dynamic pressure); a
  headwind shortens and widens them. Haze applies at the plume's
  emission-weighted distance. Reflection rays (SSR, RT hybrid) add the
  emission of plumes they pass (coarse march, no smoke, no haze).
- Surfaces: the jet axis and six edge lines are ray cast on the CPU (8
  segments each; mesh BVHs) against the
  terrain (elevation sampling, vertical normal), bases and other vessels
  (static meshes, no animations); the blocked fraction of the jet spreads
  along the surface, the rest continues dimmed. CPU cost about 0.1-0.4 ms
  for a few plumes (ORBITER_VK_PROFILE logs it).
  Dust, scorching and light on the surface are still missing (particles,
  engine light).
- Engine light: each plume is a point light at its flame (where the jet
  meets a surface, near that spot) with the irradiance of a glowing
  cylinder (pi L near it, L A / d^2 farther away); ray traced shadows
  when among the first four lights. Smoke particles take local light
  unshadowed. ORBITER_VK_ENGINE_LIGHT=0 disables it. A long flame is a
  single point (no line light).
- Heat shimmer (shimmer.frag, ORBITER_VK_SHIMMER=0): hot exhaust per pixel
  from the plume buffer (10 steps per plume), the background displaced by
  drifting noise; drawn before plumes and particles; single-sampled scenes
  only. Not for smoke or particles' own heat.
- Not in the environment probe.
- Lens flare: only from the sun, while its disc is visible (was every
  very bright pixel, which gave ghosts of engine flames).
- Smoke is lit by sun and ambient light without self-shadowing.
- Bloom: threshold relative to the exposure (1.2 displayed), strength 0.2,
  wider levels falling off by 0.6 (was 0.35 with all levels equal, which
  hid bright flames in glare).

## Particle streams
- GPU particles (Particles.cpp, particle_*.comp, gpuparticle.*): each
  stream owns a ring in a pool of 262144; emission counts are decided on
  the CPU as before (PARTICLESTREAMSPEC), compute passes emit, move (air
  drag, turbulence from the noise volumes, camera-relative positions) and
  light them (atmosphere transmittance, planet shadow, RT vessel shadows).
  ORBITER_VK_PARTICLES=0 hides them, ORBITER_VK_PARTICLE_DENSITY (default
  4) sets how many smaller particles replace one.
- Smoke rendering (ORBITER_VK_SMOKE): sprites (lit, eroded, sorted puffs),
  volume (default: large smoke in a froxel volume, ~0.7 ms more), screen
  (blurred thickness buffer: smooth but blobby, and its half-resolution
  blur bleeds over nearby vessels). Not in reflections or the probe.
- Exhaust streams get a drag of at least 15/s * sqrt(rho/1.225) in air, so
  they sweep back with the airflow like the plumes.
- Engine smoke (VVessel::EmitSmoke): each plume emits smoke of its class
  where the flame ends, or spreads it along the surface the jet meets (pad
  clouds), with buoyancy; colours and densities after launch photographs.
  No water deluge steam.
- Ground interaction (VVessel::EmitDust): a ray along each jet (reach
  2.5 m * sqrt(thrust/kN)) finds the ground; bare terrain throws up dust in
  the body's colour (ballistic under gravity; in vacuum low-angle streaks
  that vanish where they land), water on Earth (elevation <= 0) spray.
  Surface bases (pads, runways) get no dust; base ground meshes over
  terrain count as bases.
- Scorch marks (PlumeRenderer::Scorch, scorch.*): heat where hot jets meet
  the ground or a pad accumulates into up to 128 marks per session (body
  frame), drawn as decals multiplying the lit opaque scene. Not saved with
  the scenario; they darken emission too.
- RCS: vapour puffs (few, fast-growing, faint), also in vacuum.
- Smoke collides with the terrain (RT: ray along each particle's motion,
  terrain only, from above; without RT: a sphere at the ground height under
  the camera). Bases and vessels are left out (smoke would be trapped in
  flame trenches).
- Smoke self-shadowing: a sun-aligned grid (192x192x64, 2.4 x 3 km around
  the camera) of smoke optical depth, summed towards the sun; two-lobe
  attenuation for multiple scattering. Smoke farther away is unshadowed;
  smoke casts no shadows on the ground or vessels.
- Reentry plasma not verified visually at peak heating.

## Surface bases
- Hull heating glow uses a stagnation-point flux estimate (1 m nose radius)
  for the whole vessel; no heat soak after the flux drops.
- Brighton Beach (Moon): part of the landing pad is hidden by terrain when
  seen from the ground camera. Possibly genuine (no Elev_mod flattening for
  the Moon in the basic pack), not yet compared with the D3D9 client.

## Platforms
- Windows build of the client: deferred. Needs a Windows test loop. On
  Windows the core reads input from its own render window (DirectInput and
  window messages), which has to be reconciled with the SDL window.

## Materials and reflections
- Reflections: environment cube probe (128^2, 6 prefiltered levels) at the
  focus vessel, one face per frame (ORBITER_VK_PROBE=0: analytic sky and
  ground only). Other vessels, bases, terrain and 2D clouds are reflected;
  not the focus vessel itself, stars, exhaust, volumetric clouds. Single
  probe without parallax correction: reflections of nearby large objects
  (e.g. the ISS seen from a docked shuttle) are positioned approximately.
  Disabled with MSAA (ORBITER_VK_TAA=0).
- Screen-space reflections (SSR.cpp; ORBITER_VK_SSR=0 off,
  ORBITER_VK_SSR_DEBUG=1 shows them alone, blue where none): one GGX ray per
  reflective pixel through a depth pyramid, ratio-estimator resolve, temporal
  accumulation; radiance from the previous frame. Usual limits: surfaces not
  on screen (undersides seen in water) are missing and fall back to the
  probe/sky, and where the depth test passes behind a visible surface
  (assumed thickness 3% of depth) the camera-facing side is reflected
  instead. Water reflections are traced with the smooth surface and only
  displaced by the waves. Transparent groups and the probe faces get none.
  Off with MSAA. About 0.3 ms at 1080p (ORBITER_VK_PROFILE=1 logs GPU times).
- Ray traced reflections (RayTracing.cpp, rt.glsl; with VK_KHR_ray_query,
  ORBITER_VK_RT=0 or ORBITER_VK_RT_REFLECT=0 off): each reflection ray is
  also traced against the opaque vessel and base groups of the prepass;
  the screen-space hit is kept where it agrees, otherwise the traced hit is
  shaded (texture, sun with traced shadow, ambient, emission, probe). Not in
  the scene: terrain, transparent groups, particles. Hits get no local
  lights, normal maps or aerial perspective. About +0.2 ms.
- Material configuration files: only SPECULAR, SMOOTHNESS, METALNESS and
  REFLECT are used (FRESNEL and other entries are ignored).
- Texture maps (_norm/_bump, _rghn, _metal, _spec, _refl, _emis, _transl,
  _transm) are loaded next to mesh textures. Normal maps are taken as
  DirectX convention (green towards +v). Tested with generated maps only:
  stock Orbiter ships none for vessels. DXT5nm-packed normal maps are not
  detected. _heat is not used.

## Ray traced lighting
- With hardware ray queries (RTLighting.cpp, rtlight_*.comp;
  ORBITER_VK_RT_LIGHTING=0 falls back to shadow maps and screen-space AO):
  per prepass pixel one ray each for the sun (disc sampled: soft, contact
  hardening), ambient occlusion (vessels and bases only, 4 m), planet light
  visibility, and up to 4 local lights; accumulated over frames (depth
  tested reprojection) and filtered. Terrain tiles drawn into the prepass
  are in the scene (casting and receiving). About 1.0-1.4 ms at 1080p.
- Shadow maps are not rendered then: glass groups and the probe faces get
  no sun shadows; transparent geometry has no traced visibility.
- Local light shadows are traced from the light; surfaces the ray leaves
  from inside do not block it (emitters inside hulls). Spot lights start
  where their axis leaves the vessel's meshes (CPU ray cast, cached per
  emitter; static mesh, no animation). Light shadow edges keep some grain.
- Planet light: Lambert sphere by phase with tabulated albedos (Earth,
  Moon, Mars, Mercury, Venus, Jupiter, Saturn, Titan; else 0.25); the
  albedo is not taken from the surface below.
- Shadow and AO accumulation lags fast-moving shadows by a few frames.

## Shadows
- Cloud shadows: within the volumetric clouds' range (60 km, camera below
  40 km) the shell's density along the sun ray (6 samples of coverage map
  and shape noise, without the detail noise); farther away the level-4
  cloud textures (tens of km per texel, soft).
- Terrain shadows: with ray tracing, every terrain tile within the horizon
  is in the scene (sun rays to 1000 km), so distant mountains cast
  shadows. The shadow-map fallback covers only the cascades (4 km around
  the camera, 16 km towards the sun).

## Water
- Sun glitter and wave normals not yet checked in a view facing the sun.

## Legacy planets and rings
- Bodies without tiles use only level 4 of their .tex file (512x256 map);
  the patch levels 5+ are not read.
- Legacy cloud layers (Venus, Titan) are drawn opaque and only from above.
- Rings (ring.frag, ringshadow.frag): a particle slab with single
  scattering (reflected on the lit side, transmitted to the unlit side),
  optical depth derived from the texture's brightness (no optical depth
  data); the planet's shadow on them and their shadow on the planet are
  geometric (spheres, no oblateness). <name>_ring_<size>.dds radial
  profiles are used when present (assumed to span the ring radii). No
  planetshine on the rings, no ring shadow on tiled planets.

## Deferred effects
- Lens flare ghosts are discs from the bloom image, not aperture-shaped.

## Exposure
- Auto-exposure (ORBITER_VK_AUTOEXPOSURE=0 disables it). The bloom
  threshold and the sun's lens flare (visibility test, ghost brightness)
  are relative to the adapted exposure. With the camera in sunlight the
  exposure is capped at 1.3 (daylight camera).

## Ambient occlusion
- Screen-space, horizon based, from a single-sample depth prepass of
  vessels, bases and terrain (ORBITER_VK_AO=0 disables it;
  ORBITER_VK_AO_STRENGTH, ORBITER_VK_AO_DIRECT tune it). 4 m radius, fades
  out by 2 km. 60% of the occlusion is also applied to direct light (an
  artistic choice for visible contact darkening in sunlight). Spatial blur
  only: no temporal accumulation yet (would come with TAA).
- The prepass draws the geometry a second time.

## Temporal anti-aliasing and upscaling
- AMD FSR 3.1 (FidelityFX SDK v1.1.4, fetched as a sparse checkout at
  configure time; CMake option ORBITER_VK_FSR) is the default; our own TAA
  is the fallback (ORBITER_VK_UPSCALER=taa) and is used when the SDK is not
  built. ORBITER_VK_RENDER_SCALE (e.g. 0.67, 0.5) upscales with either.
  ORBITER_VK_FSR_SHARPNESS (default 0.5), ORBITER_VK_FSR_DEBUG=1 shows
  FSR's debug panels.
- FSR shaders are built with our generator (tools/ffx_gen_shaders.py) and
  glslang; the SDK is patched at configure time (context size for 4-byte
  wchar_t, rgba16f luma history format) and built without
  auto-vectorisation (misaligned scratch structures). The Lanczos lookup
  table variant is not used: with it the history blurs (LUT texture upload
  through the backend appears broken here); the reference Lanczos is used.
- No reactive / transparency-and-composition masks yet (exhaust, particles
  and glass rely on FSR's own shading-change detection).
- FSR 4 (ML upscaler): revisit when its Vulkan SDK (2.4) is released.
- On by default (ORBITER_VK_TAA=0 for 4x MSAA). Transparent surfaces,
  particles and exhaust take the motion of what is behind them (possible
  smearing on fast relative motion). Animated mesh groups have their own
  motion (last frame's animation transform); vertex lists animated by
  vessels (LOCALVERTEXLIST) and edited vertices do not. No sharpening pass
  yet.

## Volumetric clouds
- Ray-marched within 60 km when the camera is below 40 km (ORBITER_VK_CLOUDS=0
  disables). Shell = 2D layer altitude +-1.5 km; coverage from the cloud tiles
  (alpha stretched by smoothstep 0.15..0.85), shape from 3D noise (6 km).
- Transition: the 2D layer fades in from 45 to 60 km; seen edge-on from below
  an overcast deck it shows as a light band at the horizon.
- Half-resolution, 64 steps + 6 light steps, no temporal reprojection of its
  own (relies on TAA/FSR); cost not yet profiled. No cloud shadows from the
  volumes on themselves beyond the light march; they shadow the ground
  (CloudShadow). No lightning, no rain.
- Cloud types are not distinguished (all cumulus-like); the layer alpha
  carries no height information.

## Gas giants
- Cloud decks below the camera are ray-marched (ggclouds.comp, Clouds.cpp's
  RenderGasGiant): decks, haze and gas per planet in
  Config/VK/GasGiant/<name>.cfg (see README.txt), storm towers with shelves
  and anvils, vortex swirl, lightning flashes and channels. Light fades with
  depth (pressure table from the core). The decks cover a 2000 km march range;
  beyond it the planet map takes over.
- Steps: about 2 percent of the distance in decks and near towers, long
  jumps through clear air, at most 128 per ray; the noise is mip-filtered to
  the pixel footprint. Half resolution, with temporal accumulation, and
  half the 8x8 tiles reused when the camera is still.
- Cost at 1080p (RX 9070 XT): Jupiter 4-10 ms, Saturn and Neptune storm
  views about 13 ms (large towers). Goal: about 3 ms.
- Distant towers vanish in the Rayleigh fog of the deep gas (about 75 km of
  visibility at 6 bar), as they should. Uranus is bland, as Voyager saw it.
- Not done: vessel lighting from flashes with RT shadows, turbulence (camera
  shake, buffeting), comparison with Juno and Cassini images, crush depth
  (no vessel destruction yet).

## Plume and exhaust overhaul (planned, after the current graphics list)
- User request: overhaul plume and exhaust effects. Current state: billboard
  particle streams (direction/spread fixed 2026-10-24), exhaust flame
  sprites. Candidates: volumetric/shaded plume cones that expand with
  ambient pressure (vacuum vs sea level), shock diamonds, heat shimmer,
  soft particles (depth fade), lit/shadowed smoke, reactive mask for FSR.
