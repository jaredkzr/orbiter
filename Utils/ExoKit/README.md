# ExoKit

The exoplanets of Orbiter's neighbouring systems (`Config/Systems`), from the
[NASA Exoplanet Archive](https://exoplanetarchive.ipac.caltech.edu/)'s
Planetary Systems Composite Parameters table (`pscomppars.csv`, cached here).

    python3 exokit.py           # write the planet files from the cached table
    python3 exokit.py --fetch   # download the table again first

For each planet of Alpha Centauri (Proxima), Barnard's Star, Epsilon Eridani
and TRAPPIST-1 it writes `Config/Systems/<system>/<planet>.cfg` and the
`PlanetN` lines of the system file. Orbiter itself never goes online: the
files are committed.

## What the files hold

- **Orbit as seen from the Sun** (`SkyOrbit = P T e i node w`: period [d],
  epoch [JD], eccentricity, inclination, node and argument of periastron
  [deg]; `SkyOrbitEpoch = Conjunction` when the epoch is a transit or inferior
  conjunction, else `Periapsis`). The core turns it into elements in the
  ecliptic frame at the host star's place on the sky, its size from the
  period and the masses, so that the periods are the measured ones and the
  transiting planets transit.
- **Mass and radius**: measured where they are; for planets found by radial
  velocity the mass is M sin i / sin i and the radius R = M^0.27 (rocky).
- **Tidal locking** (`TidalLock = TRUE`): the M dwarfs' close planets turn
  once per orbit, meridian 0 to the star.
- **The look** (the `LOOKS` table): `SurfaceClass` (Rock, Ocean, Ice, Desert,
  Lava, Gas), relief, seas, craters, open water about the substellar point,
  colours; atmospheres and clouds for the two temperate ocean worlds
  (TRAPPIST-1 e, Proxima b). The surfaces are made at run time (core:
  `Src/Orbiter/ProcTerrain`, the elevation; Vulkan client: `ProcSurface`,
  the colours, the water and the clouds).

## Assumed

Everything the archive does not give is filled in and marked "assumed" in
the files: the orbits' nodes (and the inclinations of the radial velocity
planets: 60 deg, the median of random orientations), radii of the radial
velocity planets, tidal locking, and all of the surfaces, atmospheres and
clouds. JWST's results are followed where there are any: TRAPPIST-1 b and c
dark bare rock, d without an Earth-like atmosphere.
