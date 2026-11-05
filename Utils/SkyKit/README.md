# SkyKit: data for the simulated sky

Offline tools that build the data of the Vulkan client's simulated sky
(`Config/CSphere/Sky/`). Nothing here runs with Orbiter; the client reads
only the files these tools write. The client requires them: without them it
does not start (`ORBITER_VK_SKY=0` leaves the sky out, for tests). Requires Python 3 with numpy, scipy and
pandas.

The built `stars.bin` and `dust.bin` are not in git (190 MB): download
them with `python3 Utils/SkyKit/download.py` (from the repository's root;
a GitHub release's assets, checked by SHA-256), or build them as below.

```
python3 fetch.py [--cache DIR] [dust] [gaia] [hip] [teff] [harris] [iau]   # download the sources (default cache: ~/orbiter-data/skykit)
python3 build_dust.py --out Config/CSphere/Sky/dust.bin
python3 build_clusters.py --out Config/CSphere/Sky/globulars.csv
python3 build_stars.py --dust Config/CSphere/Sky/dust.bin --out Config/CSphere/Sky/stars.bin
python3 build_names.py --ref Config/CSphere/Sky/stars_ref.csv --out Config/CSphere/Sky/stars_named.csv
```

`build_names.py` names the stars (the targets of interstellar travel) from
`build_stars.py`'s full-precision reference table (`stars_ref.csv`, written
beside `stars.bin`). Orbiter's core reads `stars_named.csv`; without it the
only interstellar target is the Galactic Centre.

`build_stars.py` needs the dust map first: it dereddens the stars with the
extinction the client computes (`skykit/galdust.py` mirrors the client's
`GalDust`), so that the client shows them from the Sun as observed. It also
places the members of the Magellanic Clouds (`galaxies.cfg`, read by
`skykit/galaxies.py` as the client does) and of the globular clusters at
their systems' distances.

## Outputs

| file | made by | contents |
|------|---------|----------|
| `Config/CSphere/Sky/galaxy.cfg` | hand-written | model of the Galaxy: stellar components, spiral arms, dust beyond the map, luminosity functions |
| `Config/CSphere/Sky/dust.bin` | `build_dust.py` | 3D dust extinction near the Sun: three nested grids (2, 4, 8 pc cells; 50 MB) |
| `Config/CSphere/Sky/stars.bin` | `build_stars.py` | 3D star catalogue: 4.8 million stars with position, absolute magnitude, temperature, velocity, identifier (140 MB; format in `Orbitersdk/include/StarCatalogue.h`) |
| `Config/CSphere/Sky/galaxies.cfg` | hand-written | models of the Local Group's brightest galaxies (LMC, SMC, M31, M32, M110, M33): distance, orientation, luminosity, components, dust; proper motions of the Clouds' members |
| `Config/CSphere/Sky/globulars.csv` | `build_clusters.py` | the Milky Way's globular clusters: position, M_V, Plummer and tidal radii, [Fe/H] (154) |
| `Config/CSphere/Sky/stars_ref.csv` | `build_stars.py` | full-precision positions and velocities of the Hipparcos stars and those within 25 pc (by index in `stars.bin`) |
| `Config/CSphere/Sky/stars_named.csv` | `build_names.py` | named stars: IAU names and `curated_names.csv`, with their positions and velocities (424) |
| `Utils/SkyKit/curated_nearby.csv` | hand-written | parallaxes of nearby stars where the catalogues' are poor (a system's components at one distance) |
| `Utils/SkyKit/curated_names.csv` | hand-written | common names of well-known stars the IAU leaves unnamed (Tau Ceti, 61 Cygni, ...) |

## Sources and licences

| source | used for | licence / terms |
|--------|----------|-----------------|
| Edenhofer, Zucker, Frank et al. 2024, *A parsec-scale Galactic 3D dust map out to 1.25 kpc from the Sun*, A&A 685, A82 ([arXiv:2308.01295](https://arxiv.org/abs/2308.01295)); data: [Zenodo 10658339](https://zenodo.org/records/10658339) (posterior mean, 1.25 kpc and 2 kpc maps) | `dust.bin` | CC BY 4.0 |
| Gaia DR3 (Gaia Collaboration, Vallenari et al. 2023), via the [GAVO data centre](https://dc.g-vo.org) mirror and the [ESA Gaia archive](https://gea.esac.esa.int/archive/) | `stars.bin` | CC BY-NC 3.0 IGO: "Credit: ESA/Gaia/DPAC" ([terms](https://www.cosmos.esa.int/web/gaia-users/license)) |
| Hipparcos (ESA 1997) and its new reduction (van Leeuwen 2007), via the ESA Gaia archive | `stars.bin`: the brightest stars | CC BY-NC 3.0 IGO: "Credit: ESA" ([terms](https://www.cosmos.esa.int/web/hipparcos/catalogues)) |
| Bailer-Jones et al. 2021, AJ 161, 147: distances of the Gaia EDR3 stars (GAVO's `gedr3dist`) | `stars.bin`: distance posteriors | published catalogue (cite) |
| Harris 1996, AJ 112, 1487 (2010 edition), [mwgc.dat](https://physics.mcmaster.ca/~harris/mwgc.dat) | `globulars.csv`; cluster members in `stars.bin` | free; whoever passes it on must refer to the source, free of charge |
| McConnachie 2012, AJ 144, 4; van der Marel & Kallivayalil 2014; Kallivayalil et al. 2013 (and others in `galaxies.cfg`) | `galaxies.cfg` | published values |
| IAU Catalog of Star Names (IAU Division C Working Group on Star Names), [text version](https://www.pas.rochester.edu/~emamajek/WGSN/IAU-CSN.txt) | `stars_named.csv` | CC BY 4.0 (IAU): credit the IAU WGSN |
| Pecaut & Mamajek 2013, ApJS 208, 9, [table version 2022.04.16](https://www.pas.rochester.edu/~emamajek/EEM_dwarf_UBVIJHK_colors_Teff.txt) | `stars.bin`: colour -> temperature, G - V | published table (cite) |

Orbiter already ships ESA-derived star data (`star.bin`) under the same
terms; see the user manual's credits.

Model parameters in `galaxy.cfg` (published values, not data files):

- spiral arms: Reid et al. 2019, ApJ 885, 131, table 2 ([arXiv:1910.03357](https://arxiv.org/abs/1910.03357)); arm width growth: Reid et al. 2014
- R0 = 8.15 kpc: Reid et al. 2019; the Sun's height above the plane: Bennett & Bovy 2019
- boxy bulge (G2): Dwek et al. 1995; bar angle ~27 deg
- extinction curve: Cardelli, Clayton & Mathis 1989 (R_V = 3.1)
- dust units: the map's E (Zhang, Green & Rix 2023) times 2.8 gives A_V (Edenhofer et al. 2024)
- nebulae: N_H / A_V = 2.2e21 cm^-2 (Gueaver & Oezel 2009); case B recombination (Osterbrock & Ferland 2006); ionising photons by temperature after Martins et al. 2005 (O stars) and Sternberg et al. 2003 (B stars); dust albedo and asymmetry after Draine 2003; hot-star temperatures by spectral type after Pecaut & Mamajek 2013 (dwarfs), cooler for supergiants (Martins et al. 2005, Crowther et al. 2006)
- luminosity functions: rounded local values (Wielen/Jahreiss-like main sequence, giants), tuned so that the sky's brightness at the Galactic poles is ~40-50 S10(V)

## Notes

- Dust map artefacts: the map's structures are elongated along lines of
  sight from the Sun (distance uncertainty). Invisible from the solar
  system; seen from far away they point at the Sun.
- Beyond the map (1.99 kpc from the Sun) the renderer uses the dust model
  in `galaxy.cfg`, blended in from 1.7 kpc.
- The ESA archive limits anonymous queries (60 s, 50k rows synchronous):
  the Hipparcos tables are fetched in parts; Gaia DR3 comes from GAVO.
- Stars: G < 12.5 from Gaia DR3 plus every Gaia star within 25 pc; the
  Hipparcos row stands in for stars brighter than V = 5.5 or without Gaia
  parallax or colour. Distances are drawn from each star's posterior
  (seeded by its identifier): a point estimate would line stars up in
  radial streaks seen from elsewhere. Temperatures from dereddened colour
  (dwarf sequence; giants' for luminous red stars); hot stars of known
  Hipparcos spectral type (O, B, Wolf-Rayet) from the type, with their
  extinction from their colour excess (the dust map misses much of the
  dust around them, which would make them look cool and faint). The
  client's nebulae are ionised by these stars.
- The client generates the stars the catalogue lacks (too faint from the
  Sun, brighter than its limit from elsewhere) from the luminosity
  functions in `galaxy.cfg`, fainter than M_V = 2.5 only: far beyond the
  catalogue's reach (~1 kpc) the brighter ones are missing.
