# To do

Noted for later; not yet started.

## Scenery: after the bases' ground and runways (2026-11-17)

Done: bare earth (`BareEarth` in base files) at Cape Canaveral, Wallops Island,
Matagorda, Sriharikota, Peenemünde and Kourou; every paved runway of Earth's
bases drawn with its markings (`MARKINGS` in the base files, drawn by the Vulkan
client: `runway.glsl`; the SLF and the Skid Strip surveyed); tiled pavement
textures with normal and roughness maps; landing pads restyled (`pad.glsl`:
markings, per-body pavement, night lights); a
fine grain on base ground meshes; no tyre tracks on paved ground; no surf on
canals and ponds. Left over:

- **Bare earth elsewhere**: surveyed, not enabled (radial profiles to 15 km,
  with and without):
  - Alcântara and Svobodniy: the ground filter (the lowest tenth within
    600 m) cut real relief - Alcântara's plateau by its cliffs, Svobodniy's
    hills (mean drops of 10 and 14 m). They need a filter that tells canopy
    from slopes (e.g. the lowest tenth of the slope-detrended data).
  - Plesetsk: unbroken taiga - no ground within 600 m to find; little change.
  - Havana: built up; the cities' own levelling (`CityMap::Ground`) is off
    near bases (their fade) - decide which of the two should hold there.
- **Runway designators** not given (`DESIG`) come from the true heading; real
  designators are magnetic (a few are one off). Most of Earth's runways are
  placeholders (3000 x 60 m at round headings); real ones can be surveyed (the
  FAA's or the AIP's ends) and given their designators, overruns and surfaces.
- **Runway and pad markings in other clients**: only the Vulkan client draws
  them (others show the tiled pavement; `STYLE classic` keeps a pad's old
  texture); nor do ray traced reflections show them.
- **After the base asset overhaul** (all phases done): the Cape's pad slopes are greyer than the
  cream of the photos; both LC-39 pads share one surface painting (and the VAB's east and west
  faces); the trench, roofs and sheds still use tiled textures; the VAB's low bay is one box
  (the real one is stepped). `SOLARPLANT` (now a fixed array, exported) has no test: no stock
  base uses it (checked at a temporary plant at Edwards: drawn, shadowed, collides).
- **Scenario builder in the main menu** (Launchpad): build a scenario without editing files -
  pick the focus vessel and its class, where it starts (landed at a base: its pad or runway;
  on a planet's surface by longitude/latitude and heading; in orbit, by elements or relative
  to another object), add other ships with their own states and locations, set the date and
  the camera; preview it, then save it as a `.scn` under `Scenarios/` and launch it.
- **The Moon by day**: near new moon it shows as a dark disc in Earth's daytime sky (seen
  from Habana, MJD 51985.7) - the sky's light in front of it missing.
- **Classic pads' curvature**: `STYLE classic` pads (and other base objects
  kilometres from their base's centre) are still planes at the curvature of
  their centre, tilted against the ground by up to centimetres; the marked
  pads follow it (`Lpad::ExportMarkedGroup`).
- **White Sands' lakebed runways** (width 0, the gypsum) still use `Runway2`:
  nothing is drawn; they could get painted lines on the bare ground.
- **Wet sand along canals**: the shore field's wet-sand band (`coastPrm.x`)
  still runs along narrow water, like the surf did; the same open-water test
  (`OpenWater`, tile_main.glsl) could scale it.
- **`ORBITER_VK_VIDEO_FILE` flights are not repeatable**: the landing autopilot
  (`Script/Tests/LandingVideo.lua`) touches down 560-740 m in with it, 1099 m
  without, every time; so do real-time runs. Its control loop seems to miss
  frames when they are paced by the encoder or the clock.
