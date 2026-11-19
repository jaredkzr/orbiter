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
autopilot came down 500 m short while a video was encoded). Left over:

- **Bare earth, decided against**: Plesetsk (unbroken taiga: no ground to find,
  and its ravines' banks cut by 16 m even over the samples' plane) and Havana
  (hilly, built up: the cities' own levelling holds there, 2 m on the mean
  against the bare earth's 5 m). At Alcântara the plateau's cliff edges still
  come out some 5 m lower than its flat ground (a step is no plane).
- **Narrow fields of view from the ground exhaust the GPU's memory**: a ground
  observer's aperture set below about 2 degrees (`oapi.set_cameraaperture`)
  loads terrain until the heap limit stops Orbiter (6-7 GB within 20 s at
  Habana). It also kept the rings' share of the sky's veil from being looked at
  close up (Saturn by day): checked only as far as the planet's disc (Jupiter's
  pixel as bright as the sky).
- **Edwards' lakebed runways**: their axes and north ends are from the level 17
  surface tiles, their far ends not (the lakebed is saturated in the imagery:
  18L/R to where they leave the lake, 15/33 its nominal length); the lake's
  other runways (17/35, 05/23, 06/24, 07/25, 09/27, 12/30) are not marked out.
  The Shuttle's aim-point marks at White Sands are only in the hi-res tiles.
- **Scenario builder**: no docked starts, no fuel or gear states, orbits only
  by elements over the body's equator (not relative to a base's overflight),
  the bodies of `Sol` only, bases from `Config/<body>/Base` only (not a
  planet's other base directories), no Windows launchpad page.
- **A build does not copy changed base files** (`Src/Celbody/*/Config/Earth/Base`)
  into the build tree unless something else there changed: copied by hand.
