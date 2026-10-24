# Vulkan client: open items

Known gaps and limitations, to be addressed later.

## Atmosphere
- Multiple scattering is a cheap isotropic approximation; precomputed
  scattering tables would be more accurate (twilight colours in particular).

## Particle streams
- Reentry plasma is implemented but has not been verified visually at peak
  heating.

## Surface bases
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
  REFLECT are used (FRESNEL and other entries are ignored). Normal,
  roughness or metalness texture maps are not supported yet.

## Shadows
- Cloud shadows use the level-4 cloud textures only (tens of km per texel):
  soft and coarse, not yet checked visually over broken cloud cover. Using
  the cloud quadtree tiles would sharpen them.
- No terrain self-shadowing (mountains at low sun).
