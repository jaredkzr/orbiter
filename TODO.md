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
- **Night launch smoke** (2026-11-20, `out/showcase-shots/plume`): done -
  the stripes over the smoke (the erosion noise's trilinear lattice, now a
  mip coarser), the SRB flame drawn 120 m at sea level as in photographs,
  the smoke taking only the flame's drawn share of its light (it was lit 60
  times brighter than the flame: white for 250 m), billows of some 30 m
  shaded towards the flame at night, no shock cells in solid motors' flames
  and the SSMEs' fading after the first disc, the plume march skipping
  empty space (the long flame meeting the pad: 2.4 -> 0.9 ms). Left over:
  stars behind a launch exposed for its flames; a
  herringbone pattern on the pad's ground under the smoke (not the smoke's
  noise; shown since the smoke is no longer white); the SSMEs' faint blue
  column at night (lost in the SRBs' glow and steam in photographs).
- **Smoke and plume detail** (2026-11-20, `out/showcase-shots/detail`):
  done - the smoke march stepped by the pixels' footprint and accumulated
  over frames (rays offset in their pixels, reprojected history clamped to
  its neighbours), the erosion noise without mips and of even variance,
  finer octaves kept; a narrower erosion threshold and twice the extinction
  (VOL_SIGMA); the warp halved; the flames' noise stretched along the flow,
  their edge ragged, their highlights rolled off (plume.frag: colour and
  structure, not flat white). Tried, no gain: inverted Worley billows in
  dense smoke, full-resolution plumes (+0.4-0.6 ms), a lower night
  exposure (the flames drawn some 2x brighter than FlameRadiance says).
  Left over: billows still softer and less round than photographs', and
  brown where the pad's deluge steam is white; the froxels' fixed depth
  range (1 m - 20 km, 8 % a slice) not fitted to the smoke; ghosting of
  the march's history under fast camera moves not yet looked at.
  Later flight: the SRB smoke as wide as the flared flame's end (it grew by
  the square root, its opacity down by the square: thin behind a wide
  flame) - kept to 35 km it hid the flame from the ground at 40 km, fade
  as before; lit from the flame's end in thin air (a smoke-only light, the
  whole flame's area: from afar a glowing column under the flame, not a
  dull ribbon); engine smoke thinning as it grows past twice its size (the
  SRBs' wide puffs, hundreds of metres across, were dark discs behind the
  lit column); the smoke leaving at 500 m/s, the flames'
  noise flowing at 150 (solid) and 200 (hydrolox) exit radii/s; the ground
  layer of a jet meeting a surface sized by the flame's hot part (the 120 m
  SRB flame at ignition: a glowing rectangle around the pad). Seen then: a
  bright arc on the ground below the Shuttle at 20 km (night, track camera
  some 700 m off; not the flame's light - it stays with ORBITER_VK_ENGINE_LIGHT=0:
  the terrain's or the city lights' detail around the camera?).
  Then (2026-11-21) the jet's smoke popping in behind the flame: frame by
  frame, the SRBs' smoke at 30 km was a chain of round puffs each a frame's
  step on (the stream passes the vessel at some 1.2 km/s), billows forming
  and vanishing in them. Four causes, all fixed: two or three puffs a frame
  (spaced by the airspeed, not the jet's speed on top: now eight a size,
  each the thinner); their erosion core taken from their opacity, which
  thin air lowers (eroded to their centres: blobs); the erosion's noise at
  rest in the air while young smoke crosses it at hundreds of m/s (its
  billows re-formed every frame: each octave faded where it would slide
  visibly in a frame, by the speed of the dominant particle carried below
  the core in its accumulator, VolCoreSpeedPack - a smooth stream from the
  flame, its large lobes showing as it slows, the fine billows last);
  eddies scaled by the puff's size (75 m puffs thrown about at some 200
  m/s: capped at the noise's 12 m). Seen from further out, the flame ended
  in a ball of smoke several times its width (born at full size, 2.5 times
  the flame's end, and scattered by half that): born as wide as the
  flame's end, growing to that size, denser while narrow (PF_JET). Cost:
  some 0.3 ms at 30 km close behind (more particles, narrower young ones).
  Seen then: a fine honeycomb of dots over darker smoke (in the v4 video
  too: the march's offsets or its upsampling?). Then, the same day: from
  the ground 25 km off the trail was half as wide as the flame's end -
  beyond the volume (16-20 km) engine smoke is sprites, whose eroded puffs
  cover the middle half of their squares (the volume's overlapping puffs
  their whole extent): drawn 1.8 times larger, as wide as in the volume;
  and the jet's smoke born 1.5 times as wide as the flame's end (its glow
  reads two or three times wider from afar). Then (2026-11-22, the v7
  video reviewed): the SRBs' column under the flame dark at low altitude,
  the SSMEs' steam a lit streak beside it (the flame's end light lit
  smoke in thin air only, the SRB smoke born 75 m past the last light
  point: half of it in dense air, faded in by the height over the ground
  of where the flame would end unhindered - gated by the jet's ray cast,
  which reaches 100 m past the flame, the pad's cloud brightened in a
  step per booster, and by the end the surface cuts it at, in steps as
  the rays met the deck or the flame trench; where the surface cuts a jet
  now moves at most twice as far as its nozzle a frame - it jumped 5 m,
  the flame's end and glow with it; hydrolox
  free-jet smoke an eighth, no long trail); a row of dark blobs down a trail beyond some 10 km (the
  splat's kernel half a froxel wide: a particle thinner than a depth
  slice at a slice's centre took all of it, at its edge none - a clump
  per slice, 1 km apart at 15 km: now a froxel each side, weighted by as
  much less); the old trail kilometres behind the flames drawn over them
  (sprites after the plumes: now under a flame where farther than its
  light-weighted distance, a third half-size plume image); city lights
  through the flared exhaust's glow (a flame hides what is behind it by
  its light before the roll-off: 1 - exp(-6 L)). Left over: the volume's
  smoke behind a flame still drawn over it (only the sprites are put
  under); a few dark froxel-sized cells in the young jet smoke under the
  flame seen from 5 km (its 400 m depth slices: smoke spread into the
  next is lit from there); the pad's cloud flickering by 2-3 % from one
  frame to the next as the vehicle climbs (in v7 too, and with the end
  light off and the clip unlimited; not the exposure, a camera cut or
  jolt, the flames' flicker, a ring reallocated, one class of particles
  or the volume's shadow points: the smoke's temporal accumulation,
  half-resolution march or the upscaler under the camera's pan?).
- Not yet looked at: the reentry plasma's radial streaks and hard lower
  edge; the night launch's dotted catenary wires and shard-like
  reflections; the Delta-glider's virtual cockpit HUD combiner (an opaque
  green panel at night); the Delta-glider's black nozzle interiors.
