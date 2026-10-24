# Vulkan client: open items

Known gaps and limitations, to be addressed later.

## Atmosphere
- The sky just above the horizon is too orange/yellow. The scattering model
  is single-scattering only; a multiple-scattering approximation (or
  precomputed scattering tables) would whiten the horizon.
- Vessels are not affected by the atmosphere: no aerial perspective (haze)
  and no attenuation of sunlight, unlike terrain and clouds.

## Particle streams
- Reentry plasma is implemented but has not been verified visually at peak
  heating.

## Surface bases
- Brighton Beach (Moon): part of the landing pad is hidden by terrain when
  seen from the ground camera. Possibly genuine (no Elev_mod flattening for
  the Moon in the basic pack), not yet compared with the D3D9 client.
