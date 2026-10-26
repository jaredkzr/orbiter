Cloud decks of gas giants (Vulkan client, Clouds.cpp: ray-marched when the
camera is within RANGE of them). One file per planet, <Planet>.cfg.
Altitudes are above the 1-bar level (the planet's radius) [m].

LAYER base top extinction scale r g b coverage contrast towers [holes]
    base, top    altitude of the deck's base and top [m]
    extinction   [1/m] at full density (optical depth: extinction x thickness)
    scale        size of its cloud features [m] (zonally stretched by STRETCH)
    r g b        albedo of its particles (tinted by the planet map's colour
                 by TINT)
    coverage     cloud fraction 0..1
    contrast     how much belts (dark) and zones (bright) of the planet map
                 thin and thicken it, 0..1
    towers       convective storm towers rise from it (1) or not (0)
    holes        clearings: large holes (hundreds of km) in the deck, 0 (none)
                 to about 0.6 (mostly open)
HAZE r g b extinction height
    the gas and haze between and below the decks: albedo, extinction at the
    1-bar level [1/m], scale height [m] (it thickens with depth)
TOWERTOP alt    altitude the storm towers reach (their anvils) [m]; they
                rise from the lowest deck through all above, and the decks
                clear in a moat about each storm
STRETCH s       zonal stretch of the cloud features (bands)
TINT t          share of the planet map's colour in the upper decks, 0..1
