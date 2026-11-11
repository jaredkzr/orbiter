# Explosions outside an atmosphere: review and plan

2026-11-11. Follows the atmospheric explosion rebuild (PLAN.md, phases 0-6, b48af968).
Goal (user): an explosion out of an atmosphere shows no smoke, no lingering burning and no
drag on its particles; the right effect is chosen by the atmosphere actually present.

## 1. What happens now

Everything hangs on one hard switch, `rho < 1e-4` kg/m^3 at the debris piece
(VObjects.cpp: VDebris::Effects/Fireball, ~20 sites):

| | in air (rho >= 1e-4) | in vacuum (rho < 1e-4) |
|---|---|---|
| fireball | volumetric ball, soot cap, site fire | `VacuumFlash`: 32 sprite jets + a grey "vapour" shell; or the marched `Ball::vacuum` shell (0.4 s) |
| pieces | flames, smoke trails, wreck smoke | nothing burns (`burning` needs rho) |
| fragments/sparks (`Spray`) | slowdown 0.4 | slowdown 0, faded at 4 s |
| light | two lights, shadowed | one flash light, 6 sun units, exp(-10 t) |
| jolt, shimmer, skirts | yes | none (they live in the air branch) |

Problems:
1. **It reads as smoke.** The vacuum shell is a grey, soft, billowy haze
   (phase4/vacspace_sheet.png, vacuum_sheet.png: it swallows the ISS for half a second and
   darkens the arrays). It has a smoke volume look (albedo 0.8 grey, isotropic), not a thin,
   radially streaming, sunlit cloud.
2. **It is not self-similar.** In free expansion every parcel keeps the velocity it got,
   so the cloud is a "Hubble flow", v = r / t: the density profile keeps its shape and scales
   with t, the column depth falls as 1/t^2, inhomogeneities stretch *radially* into
   streamers. The current shell is a fixed-width ring growing at one speed, with billow noise
   (an atmospheric feature).
3. **The switch is binary and in the wrong quantity.** Three different physical things are
   decided by one density threshold:
   - *confinement* (does a fireball form, or does the gas expand freely?) depends on ambient
     **pressure** against the burst's energy: the ball stops where its pressure meets the
     ambient one, R_eq ~ (E / p)^(1/3);
   - *drag, buoyancy, suspended smoke* depend on **density**;
   - *lingering burning* (crash-site fire, burning wreckage, its smoke) needs **oxygen** from
     the air: bipropellant tanks carry their own oxidiser, so the burst burns anywhere, but a
     fuel spill burns on only in an oxidising atmosphere.
   At 1e-4 kg/m^3 (Earth ~65 km) the effect flips from a full sea-level fireball to a vacuum
   flash. Mars (0.015 kg/m^3, 600 Pa, CO2, no oxygen) gets Earth's fireball, a burning slick
   or site fire and a smoke column, all of which are wrong there.
4. **Particles are dragged in vacuum whenever the emitter asks.** particle_update.glsl
   applies `slowdown` (and the air's eddies, and the hot rise) whenever a body is near
   (`pc.bodyPos.w` is the body's radius, set on the Moon too). Explosion emitters avoid it
   by hand (`air ? 0.4 : 0.0`), not by construction; a new emitter that forgets drags its
   particles on the Moon.
5. **No data for oxygen.** `ATMCONST.O2pp` exists in the API (and Lua) but Planet.cpp never
   loads it: always 0.
6. Smaller ones:
   - fragments fade at a fixed 4 s, not by distance on screen;
   - hot pieces don't glow in vacuum (the glow is tied to the flame state);
   - on an airless surface (Moon) the burst is the free-space shell cut by the ground, with
     no surface spreading.

## 2. The physics to render

A tank rupture or collision in vacuum (bipropellant: hypergolic MMH/NTO, RP-1/LOX,
LH2/LOX):
- **The flash.** The fraction that mixes burns within milliseconds, then quenches as it
  expands (adiabatic cooling, T ~ t^(-2(gamma-1))). It is a bright, compact, white-yellow
  glow for a few frames: no fireball and no flame front. It lights nearby structures for
  about 0.1 s.
- **The cloud.** Burned products, unburned vapour and droplets expand freely. The fastest
  gas moves at about 2c/(gamma-1), around 1-3 km/s for hot products; the droplets and the
  bulk move tens to hundreds of m/s, so the mass moves slower than the fastest gas. The
  cloud's density keeps its shape as it grows. Its optical depth falls as 1/t^2, so it goes
  from opaque to invisible in a fraction of a second to a few seconds, depending on its
  mass. Expanding vapour freezes to ice and droplets: by sunlight it is a **bright white,
  strongly forward-scattering** haze (brightest seen against the sun), and dark in shadow.
  It has no billows: the clumps in it are drawn out into **radial streamers and filaments**,
  a chrysanthemum.
- **Motion.** Every particle moves in a straight line at constant velocity in the frame of
  the parent body's motion. Over seconds, gravity matters only near a surface. Nothing
  decelerates, nothing rises, nothing lingers. Things fade by dilution (cloud) or by leaving
  the view (fragments).
- **Debris.** Fragments and sparks tumble on straight lines. Hot metal cools by radiation
  only, so a heated piece glows dull red for longer than in air, with no smoke. Sparks
  (molten droplets) dim as they cool, in 1-2 s, without slowing.
- **No sound, no pressure wave beyond the cloud:** no camera jolt, no heat shimmer.
- **On an airless surface** (Moon, Mercury, small bodies):
  - the lower half of the cloud strikes the ground and spreads out flat along it as a
    radial, very thin sheet;
  - it throws regolith out on ballistic arcs (the existing vacuum dust sheet, Particles.cpp
    "dust thrown in vacuum");
  - it leaves a scorch and a disturbed bright halo on the surface (the Apollo blast zones);
  - no smoke column and no site fire.
- **In thin air** (Mars, Earth above ~30 km): a fireball still forms, but larger and
  faster than at sea level, with ambient pressure setting its size. It is more transparent
  and gone sooner. Soot lingers (thin air still suspends it, falling and drifting with the
  wind) but lighter. Nothing burns on without oxygen.

## 3. Design

### 3.1 One atmosphere sample, three continuous factors

A new `ExplosionAtmosphere` (VObjects.cpp, or its own small file), sampled per break-up at
the burst point and per piece each frame:

```
struct AtmSample { double rho, p, O2pp; };     // at the point (oapiGetPlanetAtmParams)
double Drag ()     = sqrt (rho / 1.225)         // = ParticleSystem::ExhaustDrag / 15: drag,
                                                //   eddies, buoyancy, smoke suspension
double Confine (R_vac) = smoothstep over R_eq / R_vis          // fireball vs free cloud
double Burn ()     = smoothstep (2 kPa, 12 kPa, O2pp)          // lingering fire allowed
```
- **Confinement.** R_eq = R_sea * (p0_earth / p)^(1/3) is where the burst's gas would
  stop at that pressure. R_vis is where the free cloud's optical depth falls to about 0.1.
  When R_eq << R_vis, the ball is held together and the atmospheric model runs, with
  R = min(R_eq, ...) scaled up. When R_eq >> R_vis, the gas thins out before the air
  can hold it: the free cloud. In between, both run with complementary weights. Rough
  values: Earth sea level about 1, Earth 40 km about 0.6, 60 km about 0.15, Mars about
  0.3, Moon 0.
- **Burn** gates the lingering parts only: CrashFires (site fire, slick), the burning
  pieces' flames and their smoke, and WreckSmoke. It does not gate the burst, which carries
  its own oxidiser.
- **Drag** scales everything that today uses `rho / 0.01` or `air ? x : 0`: the fireball
  brake and rise, the cap brake, the fragments' slowdown, the smoke columns' brake.
- The `rho < 1e-4` switches are replaced by these factors. A single place decides, logged
  under `ORBITER_VK_FIREBALL_LOG` (rho, p, O2pp and the three factors at formation).

### 3.2 Particles: no drag in vacuum, by construction

- Pass the local atmosphere to particle_update.glsl: the body's rho0 and scale height in
  the push constants (`ground.z`, `ground.w` are taken; use `trailNoise.w` or add a vec4).
  Scale `slowdown`, the eddies and the hot rise by `sqrt(rho(h) / rho_emit)`, where
  `rho_emit` is the density the emitter assumed. Simplest: emitters give their slowdown
  *at sea level* via a new `Emission::seaLevel` flag, and the shader scales it by the local
  density. Engine smoke keeps its CPU-side `ExhaustDrag` (flag off), so nothing existing
  changes unless it opts in.
- The explosion's emitters opt in. A particle born in vacuum, or flying into it, then
  coasts.
- Fragments in vacuum: fade by projected size, not by a fixed 4 s. They are visible until
  they are under about a pixel, which at 7 m/s and 50 m takes tens of seconds and looks
  right. The pool cost is bounded by the existing LOD thinning.

### 3.3 The free cloud (FireballRenderer, vacuum mode rebuilt)

The marched `Ball::vacuum` path gets a new density model, and the grid generator a new
branch:
- **Density.** A self-similar ball, rho(x, t) = f(|x| / (v t)) / (v t)^3. The profile f is
  a soft-edged sphere (a Gaussian core plus a power-law halo). v is the bulk expansion speed
  (from the burst: about sqrt(2 * 0.1 * Q * m_burned / m_total), clamped 50-600 m/s;
  the current `fbVacVs` is 3R, 20-300 m/s). Its total optical depth sets when it
  disappears: tau ~ (kappa M) / (pi (v t)^2), fading out at tau 0.05.
- **Structure: radial streamers, not billows.** Noise is sampled in (direction,
  log |x|) coordinates: angular frequency high, radial frequency low. This gives filaments
  pointing outward that keep their angular pattern as the cloud grows (self-similar, so the
  pattern doesn't crawl). Two octaves plus a few bright clumps (droplet jets) at seeded
  directions, slightly faster than the bulk.
- **Light.**
  - Scattering by ice and droplets: albedo about 0.9, a strong forward lobe (HG g 0.6).
    It lights white towards the sun and is dark in the planet's shadow (Earthshine via
    `amb`).
  - The sun self-shadow taps already exist (fireball_sun.comp) and are kept: a dense young
    cloud shades its own far side.
  - Emission: a few frames of hot core. A Planck curve on T(t) = T0 (1 + t / t_q)^(-1.2),
    with t_q about 0.02 s, cooling to invisible by about 0.15 s, plus the existing 2-frame
    white flash.
- **Asymmetry.** The cloud keeps the parent's momentum (it is centred on the
  momentum-weighted velocity of the pieces). A collision's burst is skewed along the
  impact direction: the expansion speed varies over directions as `1 + 0.4 dot(dir, impact)`.
- **Sprites fallback** (small on screen, or the Sprites setting): streak particles
  (`Emission::streak`) radially, at the same self-similar speeds, white, forward-scattering,
  no drag, alpha falling as 1/t^2.
- **Lifetime.** Until tau < 0.05: about 0.3 s for a DG's 2.5 t burned in a 300 m/s cloud,
  1-2 s for 100 t. The grid's box grows with v t. Its cost is like an atmospheric
  fireball's.

### 3.4 Flash light and pieces

- **Light.** One light at the burst, intensity rising over a frame and falling with the
  core's emission (about 0.15 s), radius growing with the core. Then a short, faint second
  "sunlit cloud" term is not a light (the cloud scatters the sun; nothing emits).
- **Hot pieces glow** in vacuum: the hull glow and a dull red emissive tint from
  `info.temp`, cooling over tens of seconds, with no smoke and no flames.
- **Sparks:** emissive, ramp-cooled over 1-2 s, no slowdown.
- **No jolt, no shimmer, no skirts in vacuum** (already true; kept by gating them on
  `Confine`).

### 3.5 Airless surfaces

When the burst's free cloud is cut by the ground (height of the centre < v t), the part
that would be underground is turned into a surface sheet:
- **The sheet:** a flat disc of the same self-similar profile spreading along the ground at
  1.2 v, about 0.1 v t thick, its gas fading as 1/t^2. Grid branch: an oblate profile under
  the hemisphere.
- **Regolith:** thrown on ballistic arcs by the existing vacuum dust path, rate by the
  burst. It falls back under the body's gravity (already in particle_update: `ballistic`).
- **Ground marks:** GroundTracks gets a scorch (dark centre) and a bright disturbed annulus
  (a blast zone halo, R_vis to 2 R_vis), permanent like the existing scorch.
- No crash-site fire, no smoke column: `Burn` = 0, `Drag` = 0.

### 3.6 Thin atmospheres (blend, no new model)

With 3.1 in place, the atmospheric model needs scaling only:
- **Size:** R scaled by the confinement (larger in thinner air), t_f shorter, density of the
  ball scaled down (more transparent).
- **Motion:** the soot cap's lifetime and its brake from `Drag` (Mars: the soot lingers
  and drifts but thinner).
- **Lingering fire** from `Burn`. Mars has none, so there's no slick fire, site fire or
  burning wreckage, and the smoke column is only the cap.
- **Fade between models:** where `Confine` is in between, the ball and the free cloud are
  both drawn with complementary weights. The burst's light and the flash are shared.

## 4. Data: oxygen

`ATMCONST.O2pp` is in the API but never loaded. Proposal (core, small):
- Planet.cpp reads `AtmO2pp` [Pa] from the planet's cfg into `atm.O2pp`.
- **Default when the key is absent:** 0.21 * p0 if the planet's gas constant is air's
  (286.9, i.e. an Earth-like atmosphere declared without composition), else 0.
- Set it explicitly: Earth 21200; Mars 0.8 (0.13 % of 610 Pa, below Burn's floor);
  Venus 0; Titan 0 (if present).

The Vulkan client reads it through `oapiGetPlanetAtmConstants`, scaled to the local pressure
(O2pp * p / p0).

## 5. Phases (commit each; clips and sheets sent after each)

**V0. Harness and baselines (0.5 d)**
- run.py scenes:
  - `vacuum`/`vacspace` (ISS hit, exist);
  - `moon` (DG crash on the Moon, from crash_moon.scn);
  - `orbit` (two vessels colliding in LEO, side view with the sun behind, and one with the
    sun ahead);
  - `alt30`/`alt50`/`alt70`/`alt90` (a DG break-up at altitude over the sea);
  - `mars` (crash, from fx_hover_mars).
- Baseline sheets of all of them on the current build.
- A per-class live particle count at +5/+10/+30 s, from `ORBITER_VK_PARTICLE_STATS` per
  class: the "nothing lingers" check is a number.
- motion.py variant: in vacuum the cloud's projected radius must be linear in t (constant
  speed), its centroid velocity constant.

**V1. Atmosphere factors and particle drag by construction (1 d)**
- ExplosionAtmosphere (3.1); every `rho < 1e-4` / `rho / 0.01` site goes through it.
- Particle density scaling (3.2).
- O2pp loading (section 4).
- Burn gates the site fire, the burning pieces and WreckSmoke.
- Check: Mars and the altitude sweep log the factors. Moon fragments coast. Mars has no
  fire. Earth sea level is pixel-identical in A/B at the standard times. Engine smoke is
  unchanged (launch sheet A/B).

**V2. The free cloud (2-3 d)**
- Grid branch: self-similar profile, radial streamer noise, droplet jets.
- March: forward-scattering ice, self-shadow, cooling core.
- Box, lifetime, LOD; sprites fallback; light (3.4); grey vapour sprites removed.
- Check:
  - ISS hit sheets (sun behind and ahead);
  - LEO collision;
  - radius linear in t (motion.py);
  - tau fade timing;
  - nothing alive at +5 s (counts);
  - perf: the cloud at most about 1.5 ms close.

**V3. Pieces in vacuum and venting (1 d)**
- Hot glow and radiative cooling.
- Sparks cool without slowing.
- Fragments fade by screen size.
- Venting (decision 1): a piece with propellant left after the burst vents it for 2-5 s, by
  the amount left: a white jet off the piece's side (a seeded direction fixed to the piece,
  turning with its tumble), freezing at once to ice (forward-scattering white), no drag,
  each puff coasting straight at the jet speed plus the piece's velocity, fading by dilution
  within about a second of leaving the nozzle. In thin air its puffs are dragged by `Drag`;
  in oxygen-bearing air above Burn's floor, pieces burn rather than vent.
- Check: LEO collision clip, fragments coasting straight (track two in frames), the venting
  jets ending within 5 s, nothing alive at +10 s.

**V4. Airless surfaces (1 d)**
- Ground sheet, regolith throw, scorch and blast-zone halo (3.5).
- Check: Moon crash sheet and clip, with no smoke and no fire at +10 s (counts); the halo
  persists.

**V5. Thin atmospheres and the transition (1 d)**
- Confinement-scaled ball, Drag-scaled cap and soot, blended band (3.6).
- Check:
  - an altitude sweep strip at 0, 30, 50, 60, 70, 90 km, the same burst: a continuous
    progression, no flip;
  - Mars sheet;
  - motion plots at 50 km show partial drag.

**V6. Polish, perf, settings, tests (0.5 d)**
- The Fireballs setting covers the cloud: Sprites / Volumetric / Full.
- Validation layer, `ctest -R "Particles|Water|Damage|Debris"` serial, perf table.
- NOTES.md.

## 6. Risks

- **Global particle drag change.** Opt-in per emitter keeps existing effects unchanged; a
  later pass can opt engine smoke in and drop its CPU-side `ExhaustDrag`.
- **The burst energy is unknown** (the core gives propellant mass, not type or mixing). The
  expansion speed and burned fraction are guesses tuned by eye against footage of upper
  stage break-ups and tank tests. Hypergolic colour (NO2 orange-brown) is deferred, per the
  earlier "hydrocarbon look for all" decision (section 7).
- **A dense, opaque cloud lit by the sun at 7 km/s closing speed** is on screen for a
  frame or two only. Motion blur is not modelled; TAA may smear it. The flash frames need
  the cut and reactive-mask handling the atmospheric flash already has.
- **Pressure-based confinement thresholds** need calibration against the atmospheric
  model's sea-level look, so that it stays as it is (Confine = 1 at p >= about 30 kPa).

## 7. Decisions needed

1. **Venting.** Pieces with propellant left could vent it for a few seconds as a white,
   freezing jet (physically right; not burning, but it lingers). Include it, or nothing at
   all after the burst?
2. **Thin atmospheres.** Blend, as in 3.6 (Mars gets a larger, briefer fireball, thin
   drifting soot, no lingering fire), or treat anything under some density as vacuum?
3. **Oxygen data.** Load `AtmO2pp` from the planet configs (a core change; the API field
   exists), or keep a client-side table?
4. **Cloud colour.** Neutral white ice for all propellants, or tint by propellant later?
5. **Airless surfaces.** Include the blast-zone halo and scorch on the regolith (permanent
   ground marks), or only the burst and ejecta?
6. **Hypervelocity collisions** (km/s closing speeds in orbit: a vaporisation flash, a
   debris cone along the impact): in scope here, or later?

### 7.1 User decisions (2026-11-11)

1. Venting: **include brief venting** (2-5 s white freezing jets from pieces with propellant
   left; V3).
2. Thin atmospheres: **continuous blend** (3.1, 3.6, V5).
3. Oxygen data: (not asked) load `AtmO2pp` in Planet.cpp, the default as in section 4.
4. Cloud colour: (not asked) neutral white ice, per the earlier "hydrocarbon look for all".
5. Airless surfaces: **scorch and blast-zone halo** (3.5, V4).
6. Hypervelocity collisions: **later, in a separate plan**.
