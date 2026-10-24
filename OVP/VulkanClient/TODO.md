# Vulkan client: open items

Known gaps and limitations, to be addressed later.

## Atmosphere
- Multiple scattering is a cheap isotropic approximation; precomputed
  scattering tables would be more accurate (twilight colours in particular).

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
- No terrain self-shadowing (mountains at low sun).

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
