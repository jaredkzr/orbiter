Cloud decks of gas giants (Vulkan client, Clouds.cpp: ray-marched when the
camera is within RANGE of them). One file per planet, <Planet>.cfg.
Altitudes are above the 1-bar level (the planet's radius) [m].

LAYER base top extinction scale r g b coverage contrast towers [holes [stretch]]
    base, top    altitude of the deck's base and top [m]
    extinction   [1/m] at full density (optical depth: extinction x thickness)
    scale        size of its cloud features [m] (zonally stretched by STRETCH)
    r g b        single-scattering albedo of its particles (tinted by the
                 planet map's colour by TINT, all decks but the deepest).
                 Thick clouds multiply the
                 absorption: 0.9999 looks white, 0.998/0.989/0.959 brown-
                 orange (thin layers, extinction below 2e-5: their colour)
    coverage     cloud fraction 0..1
    contrast     how much belts (dark) and zones (bright) of the planet map
                 thin and thicken it, 0..1 (negative: thicker in the belts)
    towers       convective storm towers rise from it (1) or not (0)
    holes        clearings: large holes (hundreds of km) in the deck, 0 (none)
                 to about 0.6 (mostly open)
    stretch      its own zonal stretch, times STRETCH (default 1: e.g. Neptune's
                 cirrus drawn out into streaks along the latitudes)
GAS ar ag ab sr sg sb [dr dg db p]
    the gas: absorption (aerosols, chromophores, methane) and Rayleigh scattering (the
    blues) per bar of the column above, rgb (optical depth; the light
    fades and reddens with depth, the depths fog); optionally more
    absorption per bar below the pressure p [bar] (the depths' colour, the
    gas above the highest clouds clearer: their colours the planet map's)
VORTEX speed core spin twist
    the storms' vortices: peak swirl speed [m/s], core radius (share of the
    cell's radius), the towers' turning speed at their edge [m/s], the
    spiral the vortex has wound the decks into (turn at the core) [rad]
HAZE r g b extinction height
    the gas and haze between and below the decks: albedo, extinction at the
    1-bar level [1/m], scale height [m] (it thickens with depth)
TOWERTOP alt    altitude the storm towers reach (their anvils) [m]; they
                rise from the lowest deck through all above, and the decks
                clear in a moat about each storm
STRETCH s       zonal stretch of the cloud features (bands)
TINT t          how strongly the upper decks take the planet map's colour
                (their absorption: the map's, times t, varied along the folds
                and between the cloud's hills and valleys)
CHROMO base top strength
                the chromophores: a layer [m] over the highest cloud deck
                absorbing as the planet map's colour says (none under its
                white zones: the belts' brown), the clouds and the gas below
                it seen and lit through it ("creme brulee" models of
                Jupiter's colours); strength 1: the map's colour over a white
                cloud seen from above. Default none
FOLDS folds lanes
                the flow's folds within the bands, 0..1: masses of the zones'
                white cloud carried into the belts and lanes of the belts
                into the zones (some 300 km across, sheared along the jets),
                and the clear lanes between them (the highest deck thinned to
                wisps: dark blue-grey, as Jupiter's hot spots). Default 0 0
REACH h         the decks are drawn up to h [m] above their top (their folds
                fading out above 30 percent of it, the decks themselves
                above 70 percent: the planet's cloud layer takes over).
                Default 1500e3 (planets with rings: the rings are drawn
                behind the decks)
POPUPS density  pop-up clouds: small convective domes (8-25 km across, rising
                5-12 km above the highest deck's tops), dense about the
                storms, scattered in the belts, rare in the zones, each
                growing and fading over some hours; density 0..1. Default 0
