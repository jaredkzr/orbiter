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
