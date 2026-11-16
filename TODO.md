# To do

Noted for later; not yet started.

## Scenery: Cape Canaveral's terrain and the Shuttle Landing Facility's runway

Seen in the landing videos (2026-11-15: `Tests/Audio/LandingVideo.scn`, a delta-glider landing on
the SLF, runway 33, rendered by the Vulkan client):

- **The terrain around the Cape is spiky.** The ground near the runway and along the horizon shows
  steep, jagged hills where the real Cape is flat coastal land (a few metres above sea level).
  Suspect the elevation data or its interpolation at this location (spikes or noise in the tiles,
  or the procedural detail added on top). It also upsets anything flying low over it: a landing
  autopilot reading its height above the ground porpoised over it (the approach now starts over
  the runway to avoid it).
- **The runway has no proper surface.** Its definition (`Src/Celbody/Vsop87/Earth/Config/Earth/Base/Canaveral.cfg`,
  the `RUNWAY` with `END1 -1150 0 -2880`, `END2 -5600 0 -5435`, `WIDTH 100`, `RWTEX Runway2`) has
  `NRWSEG 0`, its 15 textured segments commented out. So no runway is drawn: what is seen is the
  satellite texture under it, blurred at low height, without markings. Wanted: a runway mesh and
  texture at the real dimensions (4,572 m x 91 m) with its markings (threshold, designators,
  touchdown zone, centreline, edges), crisp from the cockpit at touchdown; and checking how the
  Vulkan client draws base runways (`RUNWAY` segments) once they are enabled.
