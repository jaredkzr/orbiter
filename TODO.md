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
canals and ponds, nor wet sand along them (the same open-water test); White
Sands' lakebed runways marked out on the gypsum (`MARKINGS lakebed`: dark lines
blended over the terrain, placed on the runways the hi-res surface tiles show);
the sky's light in front of bodies beyond the air (the Moon by day: its dark
side lost in the sky; `ORBITER_VK_SKY_VEIL=0` as before), also in front of
their cloud layers and rings. Then (2026-11-19): bare earth on slopes (the
ground filter takes the lowest tenth over the samples' plane where it slopes:
`GroundFilter.h`), enabled at Alcântara and Svobodniy; Edwards' lakebed runways
18L/36R, 18R/36L and 15/33; surf and wet sand only along water open over some
100 m (a second probe of the shore field: KSC's barge canal clear of surf); the
launchpad's scenario builder (its Build page: `ScenarioBuilder.h`,
`Launchpad.Builder`); scripts' `proc.skip()` waits for a frame of the
simulation whatever the machine's load (the interpreter's turn: the landing
autopilot came down 500 m short while a video was encoded). Then, the same
day: all of Rogers Dry Lake's marked runways at Edwards (18L/C/R, 15/33, 05L/R,
30L/R, 06/24, the south lakebed's 07/25 and 09/27, the lakebed beyond the paved
22L and 25: from OpenStreetMap, which agrees with the tiles within 5 m); bare
earth at steps (cliffs, banks: the ground of the samples on the node's side,
`ORBITER_BARE_STEP`); zoomed views from the ground no longer exhaust the GPU's
memory (small tiles bounded by their own elevations, not their data tile's
range: the ground under a line of sight into the sky was "in view"); a far
planet's cloud layer through the sky's veil takes the air in its shader (drawn
before the veil, the upscaler left black streaks about it: seen once the zoom
worked); a build copies changed body and base files; the scenario builder's
systems, base directories, docked starts, fuel, gear, orbits over a base,
stations and launch sites. Left over:

- **Bare earth**: decided against at Plesetsk (unbroken taiga: no ground to
  find) and Havana (hilly, built up: the cities' own levelling holds there).
  At steps the rim still comes out some 4 m lower than flat ground (Alcântara:
  10 m under the smoothed data against 5 m; it was 16 m): the nodes are 220 m
  apart and the ground between them is interpolated across the edge.
- **Zoomed views**: a view of half a degree along the ground settles at two
  to three times a normal view's GPU memory (7 GB at Habana: every tile on
  the line of sight at full detail to 280 km). With the FSR upscaler a small
  planet's disc by day has a faint dotted halo about it (not with
  multisampling); Saturn's rings through the sky's veil are still unseen
  close up.
- **Edwards' lakebed runways**: 17/35 is not in OpenStreetMap and is not
  marked out; the runways' lighter strips are the surface tiles' own colour
  where the grade keeps the terrain detail off. The Shuttle's aim-point marks
  at White Sands are only in the hi-res tiles.
- **Scenario builder**: gear and docking lines only for the stock classes
  (others: "Further lines"); fuel is one level for all tanks; base
  directories of a period or a context are left out; the launch sites are a
  built-in list for Earth (approximate places, no pads of their own); no
  attached payloads, no Windows launchpad page.
