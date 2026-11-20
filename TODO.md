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
  the line of sight at full detail to 280 km). With FSR 3 and with FSR 4 a
  small planet's disc by day has a faint dotted halo about it (not with
  multisampling: something in what the upscalers are given, not theirs); Saturn's rings through the sky's veil are still unseen
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
- **FSR 4** (the default upscaler where it starts; `ORBITER_VK_UPSCALER=fsr3`
  for FSR 3: `Fsr4.h`): AMD's Windows DLLs
  run through a loader that is weeks old, by one author - to be replaced by
  AMD's own Linux library or source when there is one. Looked at so far: a
  static scene at full and half render scale, a launch (smoke, plume), the
  validation layer; not yet long flights, the cockpit, resizing the window,
  FSR 4 Lite, frame generation. The loader's own library is not checked
  against a hash (it is built here), AMD's DLLs are.

## Showcase review (2026-11-19)

Ten ray-traced showcase screenshots reviewed (`out/showcase-shots`, build
tree only). Done: the Shuttle's SSME nozzles gimbal per engine in pitch and
yaw as the thrust does, their exhausts leave each bell's exit along its axis
(they started inside the bells, along the thrust: the top engine's 4 deg off
its bell), and the gimbal starts as angles (the SRBs began with an 8 deg roll
gimbal at lift-off); a ship near a ringed planet is lit by the rings
(`VPlanet::RingLight`, in the planet-light term); a night launch as in
photographs (a solid motor's flame light gold, `PlumeClass::lightTint`; at
night the exposure capped by the visible flames' radiance, FLAME_DISPLAY:
the sky black, the flames blown out; the flame's core whiter). Left over:

- **Jupiter's cloud towers**: a fixed grain at the cloud buffer's (half
  resolution) pixel scale on the domes. Not the march's jitter (frames
  correlate 0.92 with the history off), the history (its tolerance, edge
  handling), the noise filtering (a coarser lodBias smooths the outlines
  only), nor the upscaler (multisampling shows it as blocks). Next: the
  light march's sampling, and the deck shading under the domes.
- **Sea horizon**: a row of dots some 2 px above it (40-50 m apart at 6 km).
  Not the small waves, nor the swell's displacement (zeroed: still there);
  `ORBITER_VK_SWELL=0` hid them, maybe by the camera's height.
- **Plume light**: the SRBs' flames (300 times a sunlit surface) light the
  tank's aft dome and the orbiter's base to white in daylight - physically
  about right (an alumina flame at some 2400 K); the dome reads as a flat
  peach disc from behind. Framing: the side view.
- **Ring light**: some 0.3% of sunlight at 4 Saturn radii, under the ambient
  level (1%), which lights a ship in the planet's shadow evenly all round.
- **Night launch**: the exhaust cloud is lit about evenly across its width
  (the flame's light points sit along the flame, deep in the trench; a
  steeper fall-off through the smoke, tried in `LightTransmittance`, was
  undone by the exposure); the drawn SRB flame is short, the long bright
  column is lit smoke.
- Not yet looked at: the reentry plasma's radial streaks and hard lower
  edge; the night launch's blocky smoke edges, dotted catenary wires and
  shard-like reflections; the Delta-glider's virtual cockpit HUD combiner (an
  opaque green panel at night); the Delta-glider's black nozzle interiors;
  the Shuttle's evenly beaded shock diamonds.
