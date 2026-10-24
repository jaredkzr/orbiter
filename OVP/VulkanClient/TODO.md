# Vulkan client: open items

Known gaps and limitations, to be addressed later.

## Atmosphere
- Precomputed transmittance and multiple-scattering tables (Hillaire 2020);
  the single scattering is still ray-marched per pixel (16 steps). A
  sky-view table and an aerial-perspective volume would make it cheaper.
  Ozone only for Earth. The 1-exp(-L) sky tonemapping (AtmTonemap) is
  legacy; with auto-exposure the sky could be linear radiance.

## Particle streams
- Reentry plasma is implemented but has not been verified visually at peak
  heating.

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
- Reflections show the sky and surface of the planet near the camera
  (analytic environment); other vessels, bases and clouds are not reflected,
  and there is no glossy blur of the planet texture for rough surfaces.
- Material configuration files: only SPECULAR, SMOOTHNESS, METALNESS and
  REFLECT are used (FRESNEL and other entries are ignored).
- Texture maps (_norm/_bump, _rghn, _metal, _spec, _refl, _emis, _transl,
  _transm) are loaded next to mesh textures. Normal maps are taken as
  DirectX convention (green towards +v). Tested with generated maps only:
  stock Orbiter ships none for vessels. DXT5nm-packed normal maps are not
  detected. _heat is not used.

## Shadows
- Cloud shadows use the level-4 cloud textures only (tens of km per texel):
  soft and coarse, not yet checked visually over broken cloud cover. Using
  the cloud quadtree tiles would sharpen them.
- Terrain casts shadows only within the cascades (up to 4 km around the
  camera, 16 km towards the sun); distant mountain shadows are missing.
  Not yet verified on rugged terrain at low sun.
- Saturn's rings cast no shadow on the planet.

## Water
- Sun glitter and wave normals not yet checked in a view facing the sun.

## Legacy planets and rings
- Bodies without tiles use only level 4 of their .tex file (512x256 map);
  the patch levels 5+ are not read.
- Legacy cloud layers (Venus, Titan) are drawn opaque and only from above.
- Rings: no ring shadow on the planet, no forward scattering when lit from
  behind, and the unlit side shows only ambient light. The planet's shadow
  on the rings comes from the atmosphere transmittance (bodies without an
  atmosphere would not get it). High-resolution <name>_ring_N.dds not used.

## Deferred effects
- Engine heat shimmer: needs a screen-space distortion pass (a copy of the
  scene colour taken mid-frame, or a distortion buffer depth-tested against
  the multisampled depth). Deferred as a subtle effect for its cost.
- Lens flare ghosts are discs from the bloom image, not aperture-shaped.

## Exposure
- Auto-exposure (ORBITER_VK_AUTOEXPOSURE=0 disables it): the bloom and
  lens-flare thresholds are in scene units, not relative to the adapted
  exposure, so at night slightly dimmer lights bloom than by day.

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
  smearing on fast relative motion). Mesh animations are not in the motion
  vectors (only whole-object motion). No sharpening pass yet.

## Volumetric clouds
- Ray-marched within 60 km when the camera is below 40 km (ORBITER_VK_CLOUDS=0
  disables). Shell = 2D layer altitude +-1.5 km; coverage from the cloud tiles
  (alpha stretched by smoothstep 0.15..0.85), shape from 3D noise (6 km).
- Transition: the 2D layer fades in from 45 to 60 km; seen edge-on from below
  an overcast deck it shows as a light band at the horizon.
- Half-resolution, 64 steps + 6 light steps, no temporal reprojection of its
  own (relies on TAA/FSR); cost not yet profiled. No cloud shadows from the
  volumes (the 2D cloud-shadow approximation remains). No lightning, no rain.
- Cloud types are not distinguished (all cumulus-like); the layer alpha
  carries no height information.
