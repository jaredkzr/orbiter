# Crash explosion rebuild: notes, deviations, status

Plan: `PLAN.md` (decisions in 8.1 override it). One section per phase.

## Harness (phase 0)

- `run.py SCENE OUTDIR [--video PRE POST] [--ft] [--size WxH] [--times ...] [--tag NAME]`:
  one offscreen orbctl session (lockstep 30 fps, `ORBCTL_MEMMAX` 6G default), installs
  `harness/Scenarios/*.scn` and `harness/Script/*.lua` into the build tree (gitignored
  there), finds the **break-up** as the first frame with >= 3 pieces of debris
  (`oapi.get_debriscount()`; the land crash loses one section 0.25 s before the break-up,
  so "first debris" was wrong), takes stills at t = 0.05 0.1 0.3 0.5 0.8 1.2 2 3 5 8 12 20 s
  after it, unpauses every frame after it (the flight-over pause), writes a labelled 4x3
  sheet `<OUTDIR>_sheet.png` and copies the Orbiter log (`ORBITER_VK_FIREBALL_LOG`,
  `ORBITER_VK_PARTICLE_STATS`, `ORBITER_VK_CUT_LOG` on). `--video` keeps every frame from
  PRE s before the break-up (a ring) to POST s after; `--ft` turns on
  `ORBITER_VK_PROFILE` + `ORBITER_VK_FRAMETIMES`.
- `perf.py DIR [DIR_B]`: GPU ms per window after the break-up (A/B side by side).
- `stages.py FRAMEDIR FIRST_FIRE_FRAME`: stage times off a 30 fps video (fire pixels,
  white-hot pixels, the fire's top above the horizon, dark smoke pixels).
- Scenes (`run.py SCENES`): `sea` (DG, 7 t, 40 m/s, 12 deg into the Atlantic, observer 170 m
  aside, as `out/water-video/stills.sh`), `land` (the same in Texas, `GROUND_REL=1`),
  `night` (sea, MJD - 0.5), `small` (DG with ~260 kg, 70 m/s, 15 deg; at 40 m/s it lost a
  few sections but did not break up), `large` (Atlantis into the sea), `midair` (two DGs,
  150 m/s, ground observer), `vacuum` (DG into the ISS).
- `harness/Script/fx_crash.lua` = the reviewer's `water_entry.lua` + `GROUND_REL`: ALT is
  above the terrain (oapi.surface_elevation); the reviewer's land run placed the DG 665 m
  under the Texas terrain (ALT from the mean radius), so it "crashed" at t = 0 where it
  stood. The hold of the attitude stops near the ground on land (no water contact there).

### Deviations (phase 0)

- **Commit**: `out/**` is gitignored and the instructions put the harness under
  `out/fx-explosion/`. The phase-0 commit force-adds the harness sources only (`run.py`,
  `perf.py`, `stages.py`, `harness/`, `PLAN.md`, this file); frames, sheets and logs stay
  untracked.
- `large`: Atlantis carries only ~8 t of propellant here (OMS/RCS; the ET is not part of
  the vessel), not the ~100 t the plan assumed, and its pieces formed **two** fireballs
  (pieces farther apart than the merge radius, created 0.1 s apart). The 100 t top end needs
  a different vessel (phase 6). Its sheet is dominated by the water spray of 60 m/s pieces.
- 1440p perf runs are in the table; a `budget.py` "Crash" flight is left for phase 6.

### Baseline (75fdfaf6): `phase0/`

Sheets: `phase0/{sea,land,night,midair,small,large,vacuum}_sheet.png`. Perf:
`phase0/perf1080`, `phase0/perf1440` (`perf.py`). Baseline video stage measure:
`phase0/stages_v2.txt` (from `out/water-video/dg_crash_explosion_v2.mp4`).

Status: done.

## Phase 1 (sprites: timing, scale, motion, site fire, debris)

What changed (OVP/VulkanClient):
- `FireModel` (VObjects.cpp): f_p 0.35 / 0.25 (water), R = 2.9 M_p^(1/3), t_f =
  clamp(0.45 M_p^0.2, 0.4, 6), t_w = clamp(2 + 0.4 M^(1/3), 2, 15), soot 8 + 3 M_p^(1/3).
  7 t at sea: R 35.0 m, t_f 2.01 s, t_w 9.7 s, soot 44 s (logged).
- Fireball: one per break-up (pieces of one source and creation time join, whatever the
  distance; others by distance as before); centre velocity = the pieces' propellant-weighted
  mean (fuel shares are by mass: the same as mass-weighted), braked at 1/s (flames still
  3/s: they spread about the centre), drawn out along its path (line source, up to 1 + 0.5
  v/20, gone by 0.5 t_f), size 0.3 R at once and R by 0.25 t_f, lift-off from 0.35 t_f
  (smoothstep to 0.6 t_f, 0.5 sqrt(gR)), flames to t_f, none past 1.3 t_f, late flames
  born darker (dull red by t_f), a white flash (5 large untinted-white sprites, 0.2 s),
  the soot cap from 0.4 t_f (from the top, little spread, lifetime 0.8 soot, albedo 0.1).
- Light: one light: the ball (extended, radius 0.1 r) brightest at 0.25 t_f (~10 sun at the
  surface), reddening, out by 1.3 t_f, plus the flash (20 sun-units, decay 10/s, 0.4 s)
  blended into it.
- `CrashFires` (new CrashFire.h/.cpp, owned by Scene): registered when the fireball ignites
  within max(0.5 R, 15 m) of the ground/sea; planet-fixed (lng/lat), a spill 0.5 R wide and
  0.5 R (1 + v/40) long from the impact on along the path; low flame tongues from 0.5 t_f
  for t_w, embers to 1.5 t_w; a black volumetric stem (smoulder 25 s decay); a flickering
  light; removed when the smoke is gone (or the time goes back: replay).
- Debris: visible flames for min(core burn, t_w - age) (the core's timer untouched); flames
  3x rate, alpha 0.8, thrown aft (-airVel); black trail smoke over water too; no trail
  sprites at rest (the column takes it); hot non-burning flying pieces (> 700 K) a faint
  grey trail; under water (>= 95 % submerged or the centre a size below sea level) nothing
  burns or smokes; a 2 s white quench burst as a burning or hot piece goes under;
  WreckSmoke black afloat too.
- Log (`ORBITER_VK_FIREBALL_LOG`): the model line (M, f_p, M_p, R, t_f, t_w, soot, mean v),
  "stage" lines (flash, full size, lift-off, soot cap, burn-out, flames gone; crash fire
  flames / out / gone; each piece under + quench).

### Deviations (phase 1)

- **One light, not two** (plan 2.4): a second RT-shadowed light for the flash cost +3 ms of
  `rtl trace` at 1080p over 0-0.5 s; the flash is blended into the ball's light (position and
  colour weighted by the flash's share).
- **Light source radius 0.1 r, not 0.6 r**; crash fire radius <= 2 m: `rtlight_trace.comp`
  jitters shadow rays over the source within 30 radii, so a 20 m radius made every sea pixel
  in range trace incoherent rays (+3-4 ms `rtl trace` at 1080p for 2 s). Soft shadows from
  the full ball wait for phase 3 (a cheaper soft-shadow path or a cap on the jitter distance).
- **Hot-piece trail**: as many puffs as a burning trail but faint (alpha 0.12), not "a third
  of the rate": at a third the spacing was beads.
- **Spill placement**: the plan had the slick at the fireball's foot; it is shifted ahead
  along the path (the fuel thrown on with the wreck), elongated with the crash speed.
- **Trail smoke at rest** suppressed (the volumetric column carries it): the black sprite
  puffs of afloat pieces read as discs.
- The crash fire is registered for land too (a pool fire at the impact); the pieces still
  burn where they lie on land (piece path, with t_w).
- "Sunken": the core's flooded debris takes 4-14 s to go fully under in this crash (not by
  6.5 s as the review thought); pieces burn afloat until then (the decision allows it: they
  sink, no core change).

### Timing (7 t sea crash; t from the break-up)

Model = `ORBITER_VK_FIREBALL_LOG` stage lines (`phase1/sea/orbiter.log`); measured =
`stages.py` on the 30 fps video frames (`phase1/stages_sea.txt`, baseline
`phase0/stages_v2.txt` from `dg_crash_explosion_v2.mp4`) and the sheets.

| Stage | plan 5 | phase 1 model | phase 1 measured | baseline measured |
|---|---|---|---|---|
| flash (white) | 0-0.1 s | +0.03 s | white-hot px 0.03-0.13 s, gone by 0.17 s | none (cream/orange from the first frame) |
| full size | 0.5 s (0.25 t_f) | +0.53 s | fire area plateau from ~0.2 s; sheet: 0.5 = 0.8 s size | still growing at 6 s |
| peak brightness | 0.5-0.9 s | (light) 0.5 s | fire px maximum 0.77-0.83 s (2nd max 1.7 s: the ball seen past the cap) | 6.0 s |
| lift-off | 0.75 s | +0.73 s | fire top rises from ~1.0 s (+0.25 s late) | top rises steadily from 0 s |
| soot cap | 0.8 s (0.4 t_f) | +0.83 s | dark px from 0.95 s; sheet: on top at 1.2 s | soot from 0.5 s, a black ball 6-12 s |
| burn-out | 2.1 s | +2.03 s | fire px < 50 % of peak after 1.9 s | 6.5 s |
| ball flames gone | 2.8 s (1.3 t_f) | +2.63 s | ball fire gone by 2.2-2.5 s (~500 px left = spill + pieces) | 6.9 s |
| site fire | 3-10 s, then smoulder | flames +1.03 s, out +14.5 s, t_w 9.7 s | spill fire px ~500 from 3 to 10 s, 320 at 11 s; sheets: burning at 3/5/8/12 s, none at 20 s | none at sea (pieces sank burning) |
| soot | rising ~50 s | 44 s (cap lifetime 35 s + volume fade) | cap + stem at 20 s (sheet) | black ball to ~20 s |

Within +-0.2 s except the visible lift-off (+0.25 s) and the flash's length (0.15 s).

### Perf (GPU ms; sea case; `phase1/perf1080.txt`, `phase1/perf1440.txt`)

| | 1080p before | 1080p after | 1440p before | 1440p after |
|---|---|---|---|---|
| crash 0-3 s median frame | 17.12 | 17.29 | 21.98 | 21.02 |
| slowest frame 0-3 s | 28.04 | 28.71 | 37.81 | 36.59 |
| 0-0.5 s median frame (rtl trace) | 22.20 (7.29) | 19.52 (4.94) | 34.28 (12.52) | 32.12 (10.38) |
| 0.5-1 s median frame | 21.94 | 18.79 | 26.08 | 22.66 |
| 5-12 s median frame | 11.0-12.4 | 10.7-11.8 | 14.9-15.4 | 16.2-16.3 |
| peak particle-pass frame | 2.51 | 2.21 | 2.85 | 2.71 |

The particle passes are cheaper throughout (shorter fire, smaller ball); the crash peak is
lower; the site fire's light costs +0.5 ms (1080p) / +0.8 ms (1440p) of `rtl trace` while it
burns (5-12 s), so the 1440p frame is ~1 ms slower then.

### Status (phase 1)

Done: sheets `phase1/{sea,land,night,midair,small,vacuum}_sheet.png`, A/B
`phase1/ab_*.png`, videos `phase1/sea_crash.mp4`, `phase1/land_crash.mp4` (1080p, the
camera-cut frame dropped), perf `phase1/perf1080.txt`, `phase1/perf1440.txt`, stage measure
`phase1/stages_sea.txt`. Validation layer clean on the sea case (`phase1/validation`).
ctest -R "Particles|Water|Damage|Debris": 12/13, the known DamageEffectsTest AIR failure.

Open for phase 2+ (what still does not look right):
- The ball is still a pile of round sprites with grey translucent rims; no surface, no
  self-shadowing, flat brightness: the marcher's job.
- The soot cap is a soft grey cloud; no cap/stem roll (vortex ring), no sunlit top.
- The slick flames are a row of round orange puffs, not tongues; small black sprite discs
  remain in burning-piece trails on the water (the small scene shows black bead-like puffs).
- Visible lift-off lags the model by ~0.25 s (the ball's top starts rising at ~1.0 s vs
  0.73 s): the sprites live ~0.5 s and keep their birth velocity.
- The flash lasts ~0.15 s (plan < 0.1 s).
- The crash fire's light adds +0.5 ms `rtl trace` (1080p) for its t_w; the 1440p frame is
  +1 ms over 5-12 s.

## Phase 2 (FireballRenderer: the volumetric fireball)

What changed (OVP/VulkanClient):
- `Fireball.h/.cpp` (new) `FireballRenderer`, owned by the Scene; shaders `fireball.glsl`,
  `fireball_grid.comp`, `fireball_sun.comp`, `fireball.vert/.frag`, `fireball_comp.frag`.
  Per fireball (at most 4 marched, the largest on screen) a slab of three 3D grids:
  A 96x96x112 RGBA16F (density, temperature, soot), C 96x96x112 RGBA16F (the noise's
  coordinates carried by the flow), B 48x48x56 RG16F (optical depth towards the sun and
  the sky). Box in ball space (x along the path, z up), 1.7 r (x elongation) a side,
  clipped at the ground; ~17 MB per slab, allocated on the first fireball, freed 2 min
  after the last.
- Model (`fireball_grid.comp`, analytic in the age: frame-rate independent, frozen when
  paused, right at any warp): ellipsoid of 0.9 r drawn out along the path (VDebris's
  elongation), flattened as it lifts (z / (1 - 0.2 lift)), base drawn in (inflow under the
  ring); billows = displacement by normalized noise at three scales (shape noise 0.42, 1.05,
  detail 0.9 per r) with a varying raggedness, the coordinates domain-warped by a slowly
  drifting (churn) field and turned about the vortex ring's core by the roll (back-traced,
  differential, saturating at ~1.1 rad); temperature = mix (surface T_s, core T_c) over a
  thin layer under the local surface (0.04-0.3 of the radius by noise) - each billow
  limb-darkened, hot cores glimpsed where the layer is thin; burn progress c (outer layer and
  top first, noise lanes) cools to 700 K soot; flash 0.08 s at 2450 K.
  T_c(u) 2350/2250/2150/2000/1750/1500/1200 K and T_s(u) 1690/1660/1630/1580/1500/1380/1150 K
  at u = 0/0.1/0.25/0.6/0.9/1.2/1.8.
- March (`fireball.frag`, half resolution, proxy box, depth-aware composite as the plumes):
  detail erosion (two octaves + a fine one at ~r/25, more on soot), a crisp density rise,
  extinction 14/r (flame) + 16/r x soot, albedo 0.05 (hot gas) to 0.17 (soot), emission
  (1 - albedo) x FbPlanck (T), sun through B (multiple-scatter floor 0.08 + ...), HG 0.35 +
  isotropic, ambient darkened by the sky depth; soft fade 0.12 r before the prepass depth;
  aerial perspective; reactive mask as the plumes; a third target: 1/z where it begins and
  turns opaque + its transmittance (`OcclusionView`).
- Composite (`fireball_comp.frag`): after `DrawVolume`, before the sprites; dimmed by the
  smoke volume's integral (`volInteg`) at its front depth, the volume's light added where it
  hides the scene (as the sprites do); reactive mask.
- Sprites behind a fireball are hidden by its opacity at their depth (`gpuparticle.frag`
  binding 48, flag 64): replaces the plan's "virtual extinction particles".
- `VDebris`: `CollectFireballs` (each frame, before the prepass); `WantsVolume` decides
  volume vs sprites per step (screen radius >= 12 px and among the 4, the last frame's
  first; grants within a frame counted); in volume mode the flame/core/flash sprites are
  not emitted; the soot cap (froxel, FireballSmoke) is emitted inside the ball from
  0.55 t_f to 1.8 t_f at its drawn centre (spread 0.7 r, size 1.15 r, albedo 0.07, 1.5x
  the particles) so that it fills the soot ball as the march fades it (extinction x
  (1.8 - u)/0.5 from 1.3 t_f): the hand-off is in place and size.
- `ORBITER_VK_FIREBALL=sprites|volume` (default volume); `ORBITER_VK_FIREBALL_FULLRES=1`
  (march at full resolution); `ORBITER_VK_FIREBALL_TUNE=rag,warp,erosion,emission,extinction`
  (debug factors); `ORBITER_VK_FIREBALL_NOJOIN=1` (a fireball per piece: several at once);
  `ORBITER_VK_FIREBALL_LOG` gains "volume/sprites" stage lines and the ball's height/rise
  every 0.5 s. GPU marks "fb grid", "fb march", "fb comp".
- Exposure: at night the brightest 0.6 x the fireballs' screen share is left out of the
  meter (`meterFlames`): it counts partly (decision 5).
- Harness: `run.py` scene `close` (sea crash, observer ~72 m, FOV 70) and `--pausecheck T`;
  `phase2/ab.py` (A/B sheets), `phase2/fbperf.py` (fireball pass costs).

### Deviations (phase 2)

- **Brightness scale**: the plan's 2000 K = 25, 1500 K = 3 whitened the whole ball to a
  pale salmon (the tone curve mixes towards white above ~0.6 and a saturated orange can only
  be displayed up to luminance ~0.4). FbPlanck is now about as steep as a real blackbody
  (exp(-20000 (1/T - 1/2100))): 1 at 2100 K, 0.3 at 1850 K (a billow's middle: orange),
  0.1 at 1700 K (rims), 2-4 at 2300-2500 K (cores, blown out). The colour is Planck at a
  colour temperature of 0.82 T rising to 1.3 T for the hottest (photographs' fire orange is
  ~(1, 0.3, 0.02) linear; at its own temperature it displayed tan; the hot cores whiten to
  cream, not salmon). A brightness surface + core temperature pair replaces one T(u).
- **Sigma**: 14/r for the burning gas (plan: translucent): at 3-8/r the ball was a soft,
  foggy glow; the billows' shapes come from their limb darkening, which needs a thin
  visible layer.
- **Soot ball marched to 1.8 t_f** (plan: the march ends with the flames at 1.3 t_f): the
  rolling black cap with fire glimpsed is the volume's best part and the froxel volume
  blurs it; the froxel cap is built inside it and takes over by 1.8 t_f.
- **Rise 0.75 sqrt(g R)** (was 0.5, phase 1/plan): the volume's ball had hardly left the
  sea when it burned out (centre +9 m by 2 s); now ~10.5 m/s with the brake (+19 m by 3 s).
  Shared with the sprites.
- **Ball raised 0.7 r** above where it formed when that is on the ground or the sea (a ball
  resting on it, rising from there), not a hemisphere cut by the surface.
- **Grid 96x96x112** (plan 48x48x64): 64x64x80 was blurry close up; B at half resolution
  (the sun/sky sweep at full resolution cost 0.6 ms per fireball).
- **No advection-offset ping-pong**: the noise coordinates are analytic (roll back-traced,
  churn, growth in units of r) and written each frame into C; no accumulation, so pause,
  warp and cuts need nothing.
- **Light unchanged** (phase 3): one light at fbPos, source radius 0.1 r. The water
  specular, the soot cap shaped as cap/stem, the fireball lighting the smoke: phase 3.
- **LOD**: below 12 px screen radius sprites (the plan's 40 px: the march is cheap there);
  steps by size (0.5 / 0.7 / 1 cell). The "light only < 6 px" level is not separate (the
  sprites are cheap at that size).
- **Overlap merging** of fireballs (plan 2.6): not done; overlapping balls are marched
  one after another into the same half images (far to near) and read as one.

### Verification

- A/B sheets `phase2/ab_{sea,land,night}.png` (sprites left, volume right, 12 times);
  sheets `phase2/{sea,land,night,midair,small,close}_sheet.png`; close-ups `phase2/close/`
  (1080p, observer ~72 m); videos `phase2/sea_crash.mp4`, `phase2/land_crash.mp4` (1080p,
  1 s before to 10 s after the break-up).
- Several at once (`ORBITER_VK_FIREBALL_NOJOIN=1`, 9 balls): 4 marched, 5 sprites, no
  artefacts; grid 1.0-1.3 ms, march 0.4-0.6 ms (`phase2/dev/multi2`).
- Pause (`--pausecheck 0.8`): three frames 4 apart while paused differ in the ball only by
  sub-pixel TAA jitter at its edges (`phase2/dev/pause1`).
- Validation layer (sea): no messages (`phase2/validation`).

### Perf (GPU ms, RX 9070 XT, RT on; `phase2/fbperf.py`, `perf.py`)

Fireball passes (median / max per window after the break-up; one fireball, 7 t):

| window | sea 1080p (170 m) | sea 1440p | close 1080p (~90 m) | close 1440p |
|---|---|---|---|---|
| 0-0.5 s | 0.49 / 0.86 | 0.63 / 0.82 | 0.84 / 1.39 | 0.76 / 0.91 |
| 0.5-1 s | 0.49 / 0.54 | 0.77 / 0.87 | 0.88 / 1.22 | 1.04 / 1.20 |
| 1-2 s | 0.49 / 0.83 | 0.57 / 0.74 | 0.88 / 1.25 | 0.69 / 0.90 |
| 2-3 s | 0.51 / 0.74 | 0.52 / 0.55 | 0.82 / 0.88 | 0.65 / 1.11 |
| 3-3.6 s (soot ball) | 0.58 / 0.94 | 0.54 / 0.93 | 0.86 / 1.12 | 0.71 / 1.18 |

(grid 0.13-0.30, march 0.30-1.04, composite 0.03-0.09.) Four at once (NOJOIN, 1080p):
grid 1.0-1.3, march 0.4-0.6. Budget (decision 8): <= 2 ms close, <= 5 ms the whole
explosion at 1440p: met (peak particle + fireball frame 2.9 ms at 1440p).

Frame totals, sea, same build (`ORBITER_VK_FIREBALL=sprites` for B):

| | 1080p volume | 1080p sprites | phase 1 (1080p) | 1440p volume | 1440p sprites | phase 1 (1440p) |
|---|---|---|---|---|---|---|
| crash 0-3 s median | 17.39 | 17.19 | 17.29 | 23.42 | 21.64 | 21.02 |
| slowest 0-3 s | 28.75 | 28.10 | 28.71 | 44.49 | 37.87 | 36.59 |
| 3-5 s median | 15.59 | 15.27 | | 16.32 | 16.16 | |
| peak particle(+fb) frame | 2.62 | 2.25 | 2.21 | 2.91 | 2.75 | 2.71 |

The 1440p 0-1 s difference is `rtl trace` (17.4 vs 7.9 ms median at 0.5-1 s): the debris'
light shadows, untouched by the fireball passes; the crash differs from run to run (pieces,
where they lie) and the 1080p pair shows the opposite (7.1 volume vs 9.2 sprites). The
fireball itself adds 0.5-0.9 ms; the sprites it replaces saved 0.2-0.5.

### Status (phase 2)

Done; commits "VK explosion phase 2: ...". ctest -R "Particles|Water|Damage|Debris": 12/13
(the known DamageEffectsTest AIR failure). Validation clean.

What convinces: the ball is a ball of fire with a surface - limb-darkened billows of several
sizes, yellow-white cores where the cool layer is thin, orange rims, dark lanes; soot
appears on the top and the outer layer from ~0.5 t_f, the fire glimpsed through gaps late;
it lifts off into a dark rolling cap with a stem from the spill fire, and the froxel cap
takes over in place; temporally smooth (no crawl), frozen when paused; at night a glowing
ball with dark soot; on land the most convincing (saturated orange over the desert).

What does not (open for phase 3+):
- 0.05-0.5 s at sea reads pale peach-cream (the peak phase is blown out by the tone curve's
  whitening); a warmer, more saturated peak would need the tone curve's whitening to go
  towards yellow (global change) or a lower peak.
- Close up (~70-90 m) the surface is still soft (half resolution, 96^3 grid): fine
  cauliflower detail, curled flame tongues and sharp soot edges are missing; a fluid sim or
  a finer procedural octave on the march (phase 7) would help.
- The sunlit soot is a fairly uniform grey: the sun/shade contrast on the cap is weak
  (phase 3: lighting integration, also the fireball's own light on its soot).
- The ball is not lit by and does not light anything new: the existing point light only
  (phase 3: the two lights, water specular, the soot volume lit by the ball).
- A streaky seam at the waterline (the half-resolution composite's depth weights where the
  ball meets the sea).
- The early ball is drawn out into a wide loaf at 40 m/s (VDebris's elongation, up to 2.5x);
  footage shows a ragged wedge.
- The sprite fallback below 12 px and beyond 4 fireballs pops (sprites build up over their
  lifetimes when the switch happens mid-burn).

## Phase 3 (lighting integration)

What changed (OVP/VulkanClient):
- **Two lights of one source** (`VDebris::CollectLights`): the ball (an extended source of
  half its radius) at its centre *as drawn* (raised 0.7 r above where it formed, rising;
  the sprite fallback: unraised), 14 sun-units at 0.25 t_f, out by 1.3 t_f; and its foot at
  the surface below it (the flash, 20 units decaying at 10/s for 0.4 s, and the fire at the
  surface it formed on, 8 units fading by 0.6 t_f; radius 0.35 r). Both of the fireball's
  owner: `rtlight_trace.comp` shares one shadow ray per pixel between the lights of one
  owner (towards one of them at random per pixel and frame), so the second light traces
  nothing more. Both take RT-shadowed slots (sortRange = range).
- **Cheap soft shadows of a large source** (`rtlight_trace.comp`): the shadow ray's point
  on the source is jittered per pixel only within `LIGHT_JITTER_PIXEL` = 3 m of its
  centre (the rays stay coherent); the rest of the source by one point per frame, the same
  for all pixels, which the RT filter's history (w 0.1, ~10 frames) averages into the soft
  shadow. Phase 1's finding (a 20 m source: +3-4 ms `rtl trace`) is gone: with two
  extended lights the 0.5-1 s `rtl trace` is *lower* than phase 2's (table below).
- **Sun self-shadowing at full resolution** (`fireball.frag`): at each contributing
  sample three taps along the sun through grid A (0.8, 1.8, 3.2 cells) and the half-res
  optical-depth grid B beyond (4.2 cells on); attenuation `0.05 + 0.95 (0.85 e^-tau + 0.15
  e^-0.2 tau)` (was a floor of 0.08 with `0.7 e^-0.7 tau + 0.3 e^-0.12 tau`, so soft that
  the whole cap sat at 25-50 %: flat grey).
- **Soot scatters evenly** (`fireball.frag`): the forward HG lobe (g 0.35) is blended to
  isotropic by the soot fraction; with the lobe alone the cap's *sunlit* side was the darker
  (the shade-side camera looks towards the sun: 1.26 x, the sun side 0.68 x).
- **Soot albedo ramp**: 0.13 (fresh, near black) to 0.30 as the ball burns out and the
  cap ages (u 0.9 to 1.8); the hot gas 0.05.
- **The ball's own light on its soot** (`fireball_sun.comp`, B now RGBA16F): a third
  sweep per B cell, looking in towards the ball's centre through grid A and integrating the
  burning gas's blackbody emission (`Inward`): the luminance of the fire as seen from
  there. The march adds `albedo x 0.7 x Inward x Planck colour (0.9 T_core)`: the soot
  wrapping the fire glows orange from inside, the cap's underside at night.
- **Froxel soot lit by the ball**: the existing `LocalLightAt` path, now with the light at
  the drawn centre (it was 0.7 r = 25 m below the ball, at the unraised fbPos).
- **Water glitter from local lights** (`tile_main.glsl`): a GGX lobe per local light on the
  wave normal, widened in alpha (roughness squared) by the source's angular size, shadowed
  by the RT visibility of the first four; the source's radiance (its strength over its
  solid angle, at least half a metre across) held to one sunlit white, since the lights'
  strengths (tuned as light thrown on surfaces, 14-20 sun-units) are far above a flame's
  drawn brightness and the unclamped lobe was a white blob on the sea.
- **Peak colour** (`fireball.glsl`, `fireball.frag`): the pale peach of 0.05-0.5 s is
  gone. Cause: the global tone curve mixes towards white above 0.6, and the ball's area was
  mostly its hot cores (2250 K: luminance 1.9, colour temperature 1.15 T: yellow-white) ->
  salmon/cream. Fix, local to the fireball: (1) FbPlanck's luminance is held to <= 1.6 above
  2100 K (`1 + 0.6 (1 - e^-(L-1)/0.6)`), its colour temperature 0.82 T rising only to 1.1 T
  at 2500 K (was 1.3 T from 1900 K); (2) the surface (mixing layer) temperature keys raised
  by 200 K (1890/1860/1830/1780/1700/1560/1230 K at u 0/0.1/0.25/0.6/0.9/1.2/1.8: the body
  at luminance ~0.35, a saturated orange) so that the body, not the cores, is what is seen;
  (3) a per-channel knee on the marched radiance (linear to 0.6, exp to 1.0): red clips
  first, green stays - an orange body, yellow-white cores, as a camera's sensor clips.
  No change to the global tone curve.
- **Soot cap shaped** (`VDebris::Fireball`): in volume mode a third of the cap's puffs from
  the top (rising), two thirds from a ring of 0.65 r at the ball's centre going out and over
  (0.25 sqrt(g R) x lift, direction 0.8 radial + 0.35 up): a wider, flatter cap over the
  spill's stem. Albedo 0.08.
- Harness: `run.py` scene `sunside` (the sea crash from the south, looking north, the sun
  behind the observer; `sea` is the shade side); `phase3/ab.py` (A/B of any two runs),
  `phase3/sunshade.py` (sun/shade crops), `phase3/batch2.sh` (the deliverables; the video
  encode drops the wreck-view frame the camera cuts to as the vessel is destroyed).

### Deviations (phase 3)

- **Foot light instead of a plan-2.4 "flash light at the impact point"**: the second light
  is at the surface below the ball's centre (where the fire sits before lift-off), carrying
  the flash and the ground fire; before the ground is known, at the ignition point.
- **Soft shadows by per-frame source points**, not a wider per-pixel jitter: the shadow of
  a wreck piece in the ball's light swims a little over the ~10-frame history while the
  ball itself flickers far more; judged acceptable in the videos.
- **Glitter radiance clamp** (one sunlit white per source) is a visual cap, not physics: the
  light strengths are not radiometric (a 2000 K ball would be ~900 sun at its surface).
- **Night metering unchanged** (phase 2's 60 % exclusion): the night sheets show the sea
  lit by the flash at 0.05 s, the ball blown out at 0.05-0.1 s and orange with structure
  from 0.3 s, the glitter path and the lit wreck pieces; "partly" as decided.
- The stem's small WreckSmoke puffs still draw as round discs under the cap (sprites until
  they grow): phase 5.

### Perf (GPU ms, RX 9070 XT, RT on; sea case; `perf.py phase2/perfNNNN phase3/perfNNNN`)

Measured on the phase-3 build before the last two cosmetic constants (soot phase blend,
surface keys -50 K); `batch2.sh` re-measures into the same directories.

| window | 1080p ph2 frame / rtl trace | 1080p ph3 frame / rtl trace | 1440p ph2 | 1440p ph3 |
|---|---|---|---|---|
| 0-0.5 s | 21.61 / 6.63 | 21.93 / 6.46 | 32.43 / 10.67 | 31.66 / 12.10 |
| 0.5-1 s | 21.34 / 7.12 | 19.23 / 3.18 | 33.63 / 17.42 | 21.85 / 5.89 |
| 1-2 s | 17.52 / 3.03 | 17.60 / 2.42 | 20.86 / 5.26 | 20.34 / 3.87 |
| 2-3 s | 16.15 / 2.25 | 16.98 / 2.44 | 17.78 / 2.47 | 17.98 / 2.56 |
| crash 0-3 s median | 17.39 | 17.50 | 23.42 | 20.97 |
| peak particle+fb frame | 2.62 | 3.01 | 2.91 | 2.98 |

Fireball passes (`fbperf.py`, median / max): sea 1080p march 0.32-0.39 / 0.73, grid
0.16-0.20 / 0.34; sea 1440p march 0.36-0.52 / 0.70; close 1080p march 0.60-0.92 / 1.23,
sum max 1.69; close 1440p march 0.64-0.95 / 1.45, sum max 1.86. Budget (<= 2 ms close,
<= 5 ms the explosion's peak at 1440p): met. The full-resolution sun taps and the inward
sweep cost ~+0.1 ms march, +0.05-0.1 ms grid.

### Status (phase 3)

Evidence: `phase3/{sea,sunside,land,night,close,peak}_sheet.png`, `phase3/sunshade.png`
(sun side vs shade side crops), `phase3/ab_{sea,land,night}.png` (phase 2 vs phase 3),
`phase3/peak_sheet.png` (0.05-0.65 s close up), videos `phase3/{sea,land,night}_crash.mp4`
(1080p, 1 s before to 10 s after), perf `phase3/perf{1080,1440}`, `perfclose{1080,1440}`.
Validation layer (sea): clean (`phase3/validation`). ctest -R "Particles|Water|Damage|
Debris": 12/13 (the known DamageEffectsTest AIR failure).

What convinces: the ball is a saturated orange-yellow with yellow-white cores and orange
lanes from 0.1 s on, at 170 m and close up; the soot ball is lit orange from inside while
the fire burns; the cap has a sunlit top and a dark underside; at night a glitter path on
the waves towards the fire and the burning slick, the wreck pieces lit.

What does not (open): the sun/shade difference of the cap is still modest with a 55-degree
sun (a low-sun scene would show it better); the daytime glitter path under the ball is a
little strong at 0.3-1.2 s; the 0.05 s flash is a flat pale yellow (phase 4: the flash
sprite/light); the cap's cauliflower is soft close up (phase 7); the stem discs (phase 5).

## Phase 4 (flash, shimmer, water and ground skirts, jolt, vacuum shell)

What changed (OVP/VulkanClient):
- **Ignition flash** (`fireball.frag`, `Fireball.cpp`, `VDebris::Fireball`): the march mixes
  its light to white (4, 3.7, 3: all three channels over its knee) by the flash factor, now
  1 for the first 0.05 s and 0 by 0.1 s (was a linear 0.08 s fade of the grid temperature
  alone: a 2450 K blackbody under the per-channel knee displayed lemon yellow - red clipped,
  green nearly, blue 0.6). The 0.033/0.067 s frames are white, 0.1 s is the orange ball
  with yellow-white cores (sea, land, night: `phase4/{sea,land,night}flash_sheet.png`). A
  white foot flash sprite (`FireballTexture (5)`, 0.6 R, two frames, alpha 3) at the impact
  point in both modes; the sprite mode's own white sprites 0.1 s (were 0.2 s, cream).
- **Heat shimmer** (`PlumeRenderer::AddHeat`, `HeatSource`; `shimmer.frag`; `VDebris::
  CollectHeat`, `CrashFires::CollectHeat`; `FireballRenderer::ModelTemps`): up to 8 heat
  sources a frame besides the plumes, in a second buffer of the shimmer set (binding 5):
  a ball of hot air of radius 1.1 r at the drawn centre - a layer a quarter of r thick at its
  surface, cut at 1.5 r (with Gaussian tails it reached over the whole sea) - with heat
  from the *model's* core temperature ((T_h - 1100) / 1100: the drawn brightness is held
  by the knee), a column of hot air 1.5 R tall rising from it (fading in over 0.3 s), and
  for 0.3 s the thin shell of air its front pushed out (radius 1.15 r + 60 t, width 0.08
  R: the background drawn in across its inside and out across its outside - a brief
  refraction ring, no shock). Eddies 0.15 r, carried up at 10 m/s. On to 2.5 t_f (the soot
  ball's heat); the site fire adds a dome of 0.5 R and a column of 1.5 R for its t_w. The
  displacement is 4x the plumes' per unit heat (up to 9 px). Cost 0.1-0.25 ms (the
  full-screen pass, 1080p/1440p).
  **MSAA**: the shimmer pass is off with a multisampled scene (`Plumes.cpp:416`, as the
  plumes'): the fireball's shimmer too. Documented, not changed.
- **Water** (`VDebris::Effects`, `Fireball`): `WaterFx::Impulse` at the foot (0.6 R,
  strength M_p^(1/3) / 12) as it ignites; a spray skirt - a ring at 1.25 r + 15 t at the
  surface (outside its billows: at 0.8 r it was hidden in the ball), 8 segments thrown
  outward at 18 m/s, 900 D/s falling to 0 over 0.5 s, white droplets of 0.06 R; the sea
  boiling: steam (albedo 0.85) from chords across its foot (+-r) at 40 D/s for t_f.
- **Ground**: the same ring as a dust skirt in the ground's colour (`DustColourGlobal`),
  12 m/s, 500 D/s, 2.5 s; the scorch unchanged.
- **Camera jolt** (`Scene::Jolt (body, gpos, strength, R)`, `UpdateCamera`): kept in the
  body's frame (the global frame moves a km a step); it *arrives* when the sound has come as
  far as the camera is **then** (at the flash the view has cut away to the wreck for a
  frame: 879 m), amplitude min (2 deg, 2333 M_p^(1/3) / d^2) x clamp ((10 R - d) / 3 R):
  0.68 deg at 204 m, 0 at 1 km; a 7 Hz pitch + 5 Hz roll shake, decay 0.15 s, 0.4 s;
  applied to `camRot` for any view (external cameras too), scaled by CameraShake (0: off;
  its description updated). `ORBITER_VK_JOLT_LOG` logs it. Default on (decision 6).
  **No camera cut**: `ORBITER_VK_CUT_LOG` shows cuts only at the vessel's destruction
  (frames 56/57, the wreck view, pre-existing); the jolt is far under the 35 deg rule and
  does not move the camera. The histories (TAA, SSR, RTL) carry through it.
- **Vacuum shell** (`FireballRenderer::Ball::vacuum`, `fireball_grid.comp`, `fireball.
  frag`, `VDebris::CollectFireballs`): the pieces of one break-up join in vacuum too (one
  shell, one light; seven flashes of 12 sun-units lit the station out white: the light is
  6 units, range 10 R); the shell is marched when 12 px or larger on screen (else
  `VacuumFlash`'s sprites): a Gaussian shell of radius 0.15 R + v_s t and width 0.1 R +
  0.1 v_s t (v_s = clamp (3 R, 20, 300) m/s), ragged by noise, in a cube box; white for
  its first frame or two (the flash's light, 0.08 s), then a pale vapour lit by the sun
  (albedo 0.85, a scattering floor of 0.7, no inward glow, no residual emission - its
  opacity carries the look: a third face on, more at its limb), thinning out, gone by 0.4 s.
  `phase4/vacspace_sheet.png` (the camera below the station, space behind it),
  `phase4/vacuum_sheet.png` (the plan's view: against the clouds the shell hardly shows).
- `FireballModel::blast = 0` (the deflagration): the hook for a future detonation's ring
  (plan section 3); nothing uses it.
- Harness: `run.py` scenes `jolt200`, `jolt1k`, `shimmer` (the sea crash from 50 m up,
  the sea behind the ball), `vacspace`; `ORBITER_VK_JOLT_LOG` on; `phase4/jolt.py` (the
  logged angles and the frame-to-frame shift of the horizon strip from the video frames),
  `phase4/shimmer_pair.py`, `phase4/batch.sh`.

### Deviations (phase 4)

- **Shimmer evidence** from a raised camera (`shimmer` scene): from the standard observer the
  halo and column stand against a uniform sky, where a displacement shows nothing (right,
  physically); the sea behind the rim and the column shows it (`phase4/shimmer_pair.png`).
- **The jolt plot's runs have the shimmer off** (`jolt200ns`, `jolt1kns`): at 200 m the
  halo (1.5 r = 50 m) reaches the frame's edge strip and wobbles the horizon there; the
  video clip (`jolt_200m.mp4`) has it on. The frame measure is the shift of each frame
  against the one before (the waves move under a pixel a frame): steps of up to 13 px over
  0.63-0.85 s at 200 m, none at 1 km (`phase4/jolt.png`, `jolt.txt`). The harness's
  screenshot index and the log's simulation time differ by about two frames (the same in
  all phases' stills).
- **Vacuum "one bright white frame"**: a light-grey disc at frame 1 (the shell's alpha
  0.4-0.6 over black space), not a blown-out white; the pale shell after it reads as a
  grey cloud, fairly smooth. Low priority (plan 2.7).
- **The spray and dust skirts** are sprites: a ring of white puffs at close range
  (`phase4/close_sheet.png` 0.3-0.8 s); at 170 m a white band at the waterline. The dust
  skirt on sand is subtle (the ground's own colour).
- **Lean from wind**: none (no wind model in the particle system); the lean of phase 4b is
  from the ball's momentum only.
- The foot and the vacuum shell were first registered a step old in the global frame
  (`fbPos` = where the piece was a step ago): 870 m off (the planet moves that far in a
  step). Both are taken after the step now.

### Perf (GPU ms, RX 9070 XT, RT on; sea case; `perf.py phase3/perfNNNN phase4/perfNNNN`)

| window | 1080p ph3 frame / rtl | 1080p ph4 frame / rtl | 1440p ph3 | 1440p ph4 |
|---|---|---|---|---|
| 0-0.5 s | 21.99 / 6.64 | 21.61 / 6.51 | 32.12 / 11.90 | 31.84 / 11.60 |
| 0.5-1 s | 19.19 / 3.20 | 19.34 / 3.14 | 20.45 / 5.14 | 21.77 / 5.67 |
| 1-2 s | 17.60 / 2.64 | 17.39 / 2.43 | 19.80 / 4.18 | 20.23 / 3.98 |
| 2-3 s | 16.86 / 2.17 | 16.84 / 2.24 | 18.59 / 2.89 | 18.46 / 2.78 |
| crash 0-3 s median | 17.62 | 17.60 | 20.49 | 20.16 |
| peak particle+fb frame | 3.04 | 2.98 | 2.74 | 2.92 |
| shimmer pass (median / max, 0-3 s) | - | 0.08-0.11 / 0.17 | - | 0.12-0.16 / 0.33 |

(The phase-3 columns are `phase3/perf*` re-measured by phase 3's batch2; both runs of each
pair on the same day.) Close (`fbperf.py`): 1080p march 0.56-0.90 / 1.16, sum max 1.60;
1440p march 0.50-0.93 / 1.40, sum max 1.80; shimmer 0.15-0.23 / 0.46. Budget (<= 2 ms
close, <= 5 ms the explosion's peak at 1440p): met.

### Status (phase 4)

Evidence: `phase4/{sea,land,night}_sheet.png`, `phase4/{sea,land,night}flash_sheet.png`
(0.033-0.3 s), `phase4/close_sheet.png`, `phase4/shimmer_pair.png`, `phase4/vacspace_sheet.png`,
`phase4/vacuum_sheet.png`, A/B `phase4/ab_{sea,land,night}.png` (phase 3 vs 4), videos
`phase4/{sea,land,night}_crash.mp4` (1080p, 1 s before to 10 s after, the wreck-view frame
dropped), `phase4/jolt_200m.mp4`, `phase4/jolt.png` + `jolt.txt`, perf `phase4/perf{1080,
1440}.txt`, `perfclose{1080,1440}.txt`, `fbperf.txt`. Validation layer (sea): clean
(`phase4/validation`). ctest -R "Particles|Water|Damage|Debris": 12/13 (the known
DamageEffectsTest AIR failure).

What convinces: the flash is white for two frames and orange after, by day and at night;
the sea behind the ball's rim bends and the waves wobble under the hot air; the thud
arrives 0.6 s after the flash at 200 m as a short shake, none at 1 km; the white spray
skirt and the steam at the waterline, the foam ring; one shell in vacuum.

What does not (open): the skirts' sprite look close up; the vacuum shell's first frame is
grey, not white; the dust skirt on sand barely reads; the half-resolution composite's seam
at the waterline (phase 2); the stem discs (phase 5).

Hand-off for phase 5 (debris trail and wreck-fire polish): flame tongues aft, a trail
spacing floor (no beads), hot-piece trails, the quench, embers/smoulder, the hull glow tied
to the flames, land pieces burning for t_w; the stem's WreckSmoke puffs draw as round dark
discs below the cap (the sprites until they grow: a volumetric stem or larger, fewer puffs
with a soft edge). The skirt's particles could use the same (a low mist sheet, not puffs).

## Phase 4b (fireball animation: no contraction, asymmetric)

The user's review of the phase 3 video (`phase3/anim/sea_anim.png`): from 0.4 to 1.2 s
the silhouette shrank towards its centre with its features pinned to it (a balloon
deflating), and the ball was a near-symmetric ellipsoid. Measured (`phase4b/silhouette.py`:
the ball's share of the frame's upper part against time, `phase4b/silhouette.png`,
`silhouette.txt`): phase 4 sea 16.5 % at 0.55 s down to 9.8 % at 1.2 s (-40 %); land 17 %
to 11.6 %.

Causes found and what changed (`VObjects.cpp`, `Fireball.cpp`, `fireball_grid.comp`,
`fireball.frag`):
- **Elongation contracting**: `el` fell to 0 by 0.5 t_f with r capped at R - the long axis
  halved. Now the half length along the path never shrinks (`fbAlong`, the most so far);
  the ball grows across to it. The elongation itself is 0.3 v / 20 m/s at most 1.0 (was 0.5
  v / 20, at most 1.5).
- **No growth after 0.25 t_f**: r grows on by 12 % per t_f after 0.25 t_f (entrainment):
  1.13 R at burn-out, 1.19 R at 1.8 t_f. The grid box is 1.85 r (was 1.7).
- **Burn-out eating the silhouette**: the march eroded sooty parts 1.5 x as much as the
  flames (the cap's crisp cauliflower); as the outer layer turned to soot the envelope was
  eaten from outside. Now 1.2 x.
- **Lift-off squashing the height**: the cap's flattening divided z by (1 - 0.2 L): the
  height fell 20 % at full lift. It is a widening of the horizontal axes now (1 / 0.88 at
  full lift): the same oblateness by growth.
- **Features pinned**: the noise coordinates were in units of r (scaled about the centre
  with it) with a slow churn (0.08 t). Now the billows are carried outward (the noise
  sampled nearer the centre as it ages: 0.5 u in units of 0.9 r, so they emerge from inside
  and push out) and the warp field drifts at 0.2 t (boiling).
- **Symmetry**: three to five sub-lobes (smooth union, k 0.3) off the centre, most of them
  ahead along the path (0.1-0.6 r), of differing sizes (0.35-0.65 r), each igniting up to
  0.2 t_f after the first and lifting unevenly, seeded from the fireball's seed; a lean of
  the top ahead of the foot by the momentum it keeps (0.012 per m/s, a third at most: at
  60 m/s the ball tipped over), growing in with the lift; the vortex ring's turn 40 %
  stronger on one side (by azimuth, seeded).
- Timings kept (full size 0.25 t_f, lift-off 0.35 t_f, t_f, R).

### Verification (phase 4b)

Strips at 0.1 s intervals from -0.1 s (`phase4b/strip.py`, 1080p video frames, the shake
off for the measure): `phase4b/strip_before_{sea,land}.png` (phase 4's videos) and
`phase4b/strip_after_{sea,land,seafast}.png` (`seafast`: the sea crash at 60 m/s, a longer
loaf; no "before" of it: the phase 4 build is gone). The silhouette area never decreases
before the soot fade: sea 10.3 % (0.3 s) rising to 20 % (1.9 s), a wobble of -2 % of the
frame at 2.0-2.2 s (the lit grey top of the soot against the sky at the measure's
threshold; the strips show no shrink), 20 % at 2.7 s; land 10.4 to 23 % monotonic (largest
frame-to-frame drop -0.07 %); 60 m/s 11.6 to 24 % (-0.27 % at most). A/B at the standard
times: `phase4b/ab_{sea,land}.png`. Perf (sea 1440p, `phase4b/perf1440`): march 0.61-0.68
ms median (phase 4: 0.47-0.64; the ball is larger on screen and the box 9 % wider), grid
unchanged (0.17), fb sum max 1.13 ms; crash median frame 20.34 vs 20.16 ms. Budget met.

### Deviations and open items (phase 4b)

- The soot ball at 2-3 s is larger than phase 4's (1.2-1.3 x across: `ab_sea.png` 3 s):
  the growth and the cap's widening are on the envelope that the smoke volume's cap takes
  over; the fire's own radius is the decision's R. If it reads too big, the 12 % growth and
  the widening (0.12 L) are the two constants.
- The silhouette measure is on video frames with the camera shake off (the jolt of phase 4
  shifts the frame at 0.5-0.6 s and the difference mask reads it as area); the "before"
  curves (phase 4's videos) have the jolt's blip at 0.63 s.
- Wind: no lean from wind (no wind model in the particle system).
- The lobes are in the shape only; the noise coordinates and the roll are the one ball's.

## Phase 4c (fireball animation fixes: brushed streaks, box clipping)

The review of the 4b renders (`phase4b/full_2p6.png`, `land_2p0.png`, the strips): (a) the
texture smeared into brushed, mostly vertical streaks on the flanks and the front from
0.3 s on; (b) a straight vertical edge on the lower left of the land ball at 1.0 s, flat
sides elsewhere.

Causes found and what changed (`fireball_grid.comp`, `fireball.frag`, new
`fireball_flow.glsl`, `Fireball.cpp/.h`, `VObjects.cpp`):
- **The streaks were the warp**, not the drift: with `ORBITER_VK_FIREBALL_TUNE` the
  warp off (`1,0,1,1`) the texture was an isotropic cauliflower, with it on (any
  churn, with or without the 4b drift) brushed streaks (`phase4c/dev/n_sea*`,
  `dev/sea_nowarp`). The warp field (0.22 of 0.9 r over features of 0.8 r, sampled at
  0.3 per unit) had a gradient of up to ~1.7: the noise through it was drawn out ~3x
  where it folded; 4b's 2.5x faster churn and the drift's tangential stretch made it
  worse than phase 4's. Now 0.12 over features of 1.4 r (sampled at 0.18): the stretch
  stays under ~1.5. The paused frame (`--pausecheck`) showed the same streaks: not TAA.
- **The 4b drift (a radial remap)** is gone. The noise is carried outward by a flow map:
  two phases, each the coordinates scaled down about the centre, evenly (1 / (1 + 0.5
  tau / t_f): features emerge from inside and push out keeping their shape), each
  living 0.7 t_f, half a period apart, cross-faded with triangle weights normalized to
  keep the variance; each phase has its own seed and the ring's turn *since it began*
  (`FbRolled`, the saturation 1.1 -> 1.5 rad), so no stretch ever accumulates. The
  grid's warp is stored in C (was: the warped coordinates) and the march rebuilds both
  phases' coordinates per sample (the lean, the roll, the warp, the scale): 5 noise taps
  a step (was 3; the finest octave at 6.1 is not carried: too fine to be seen moving).
  `ORBITER_VK_FIREBALL_TUNE` gains `,churn,flow` (the 6th and 7th factors).
- **The lean was never drawn in 4b**: `Ball::lean` was declared but `CollectFireballs`
  did not set it. It is passed now (a quarter of its height ahead at 20 m/s, a third at
  most): the land and 60 m/s balls lean ahead visibly (`land_2p0.png`, `strip_after_seafast`).
- **The box**: from 1.85 r a side to the shape's reach - 0.9 r x 1.9 (the ball with its
  displacement and its lobes) x e along the path / (1 - 0.12 L) across, 0.1 r more
  above, and 2.6 x the lean on the side it leans to (asymmetric); the early-out tests the
  lobes too; the grid fades the shape out over the last 3 cells to the x, y and top
  faces, so nothing can be cut flat whatever reaches a face. Cells are ~1.1-1.4x
  larger (the march's step with them: it is cheaper per pixel, the ball larger).
- The straight edge itself: soft (~10 px, the march's first jittered step at a face),
  fixed in place as the ball moved. Not reproduced frame-for-frame (the crash differs
  run to run); with the new box and fade there are no straight edges in the sea, land
  and 60 m/s strips (`strip_after_*`), the full frames or the videos.

### Verification (phase 4c)

Strips (`strip_before_*` = 4b, `strip_after_*` = 4c, the same crops, 0.1 s steps from
-0.1 s, shake off), full frames `full_2p6.png` (sea 1.6 s), `land_2p0.png` (land 1.0 s),
`sea_3p4.png`, `land_3p0.png`; A/B `ab_{sea,land}.png`; videos `sea_crash.mp4`,
`land_crash.mp4` (1080p, shake on, the wreck-view frame dropped). Silhouette
(`silhouette.py`, `silhouette.png/.txt`): monotonic before the soot fade - the largest
frame-to-frame drop before 2.6 s is -0.17 % of the frame (sea, 1.2 s), -0.11 % (land),
-0.00 % (60 m/s); sea 12.0 % (0.4 s) -> 17.7 % (2.0 s) -> 21.0 % (2.9 s), land 11.9 ->
22.8 -> 23.5, 60 m/s 12.2 -> 22.8 -> 26.1. The sea ball is ~1.5 % of the frame smaller
than 4b's from 1.2 s (the smaller warp pushes less out; the box fade), the land and
60 m/s balls larger. No popping: the cross-fade is continuous, the box changes
continuously.

Perf (sea 1440p, `perf1440.txt`, `fbperf.txt`; 4b -> 4c): march 0.68 -> 0.84 ms median
over 1-2 s (max 0.86 -> 1.01), 0.61 -> 0.89 over 3-3.8 s (max 1.58: the soot ball in a
wider box), grid 0.17 -> 0.19, fb sum max 1.13 -> 1.89 (3-3.8 s); crash 0-3 s median
frame 20.34 -> 19.92 ms; peak particle + fb frame 2.99 -> 3.41 ms. Close (1440p,
`fbperfclose.txt`, against phase 4's close run): march 0.74 -> 0.91 ms median over
1-2 s (max 1.40 -> 1.82), fb sum max 1.80 -> 2.33 ms (one frame; the median sum
~1.2). Budget (about 2 ms close, about 5 ms the explosion's peak at 1440p): the close
peak frame is 0.3 ms over "about 2"; the explosion's peak frame 3.4 ms. If it matters,
phase B's two detail taps could be skipped where its weight is under 0.15 (a third of
the time) for ~0.15 ms.

### Status (phase 4c)

Done. What convinces: the texture is an isotropic cauliflower at all times, billows
boil outward without smearing, the ball leans ahead and is lobed; no flat sides. What
does not: the sea ball reads slightly smaller than 4b's from 1.2 s on; the 60 m/s
ball at 0.3 s still shows faint vertical fibres (the elongated shape's limb
darkening, not the noise); the march is ~25 % dearer.

## Phase 5 (debris trail and wreck-fire polish)

What changed (`VObjects.cpp/.h`, `CrashFire.cpp/.h`, `gpuparticle.vert`):
- **Flame tongues**: a burning piece's flames are drawn as streaks along their motion
  through the air (`Emission::streak`, 1-2 x its size over its airspeed, 0.03-0.4 s;
  the length varying flame by flame): aft of a flying piece, up from one at rest. The
  vertex shader's streak cap (1 m over the particle, for dust and sparks) is lifted for
  `PC_FIRE` to 2.5 x its half size (tongues of up to ~3.5 sizes), and a flame is not
  dimmed below half for its length. The spill's flames (CrashFires) streak along their
  rise (0.12-0.42 s), rising a little faster, smaller (0.8 of 0.16 R) and a little
  brighter: tongues over the slick instead of a row of round puffs.
- **Trail spacing floor**: the puff count is by the distance moved only (D puffs per
  0.4 of the piece's size, 12 D a second at least): phase 1 scaled it by the flames
  left (f), so a piece dying down left beads. The puffs are larger (2.5 x its size,
  was 2) and fainter (0.35 x max (f, 0.3), was 0.5 f): the band reads as a band.
- **Burning pieces' trails into the smoke volume**: as sprites a trail was a band of
  round discs, polka dots, however many overlapped (the sum of round things, as the
  plan's 1.3 said of the fire sprites; the midair sheets at 3-8 s, the sea at 3-10 s).
  Tried first for slow pieces only (under 12 m/s: the water-skipping ones, whose smoke
  joins the wreck's column and the spill's stem), then for all: with D puffs per 0.4
  of the piece's size the volume draws an unbroken soft band at 150 and at 300 m/s
  (`dev/m150vol`, `dev/m300vol`; the beads of phase 1's "f6 midair" were from a rate
  capped per second, not from the volume). A hot piece's faint grey trail stays sprites.
- **Hot-piece trails**: as phase 1 (faint grey, the same spacing); kept.
- **Quench**: as phase 1 (a 2 s steam burst as a burning or hot piece goes under);
  kept - the sea log's "under ... quench" lines.
- **Embers**: glowing specks (Class::Spark, ramp-coloured, streaked, at least a pixel)
  carried up from a burning piece as its flames die down (the last two seconds) and
  for half of t_w after; and over the spill from t_w - 2 s until its flames are out
  (1.5 t_w), 4 R D a second.
- **Smoulder**: the wreck's column and the spill's stem lighten from soot black (0.08-
  0.09) towards grey (0.3) as their flames go out, over the 25 s smoulder.
- **Hull glow tied to the flames**: a burning piece's mesh glows (`SetObjectHeat`) at
  0.6 x its flames' strength, flickering, on the side its flames stream off (aft when
  flying, the top at rest), whatever its temperature; the temperature glow as before.
- **Land pieces burning for t_w**: as phase 1 (the visible flames `min (burn, t_w -
  age)`); kept - the land sheet at 5-12 s burns, 15-25 s smoulders.
- **The stem a column**: 14 D puffs a second (was 10) from a third of the spill (was a
  seventh), the same 30 s life; the pieces' slow trails join it (above).
- Harness: `run.py` scene `midair300` (the midair collision at 300 m/s);
  `phase5/pre.sh` (the "before" sheets on the phase 4c build), `phase5/batch.sh`.

### Hand-off for phase 6 (LOD, several fireballs, warp, pause, cuts, settings, budget)

- **LOD**: the march's step is by screen size (0.5 / 0.7 / 1 cell at > 400 / > 150 px
  of radius); below 12 px of radius sprites. Not done: a "light only" level, and the
  march's phase-B detail taps could be skipped where phase B's weight is small (~0.15
  ms close). The sprite fallback pops when the switch happens mid-burn (phase 2's note).
- **Several fireballs and overlap merging**: at most 4 marched (the largest on screen),
  the rest sprites; pieces of one break-up join; balls within R of each other join;
  two balls whose spheres overlap by more than half do NOT merge yet (plan 2.6) -
  overlapping balls are marched far to near into the same half images and read as one.
  `ORBITER_VK_FIREBALL_NOJOIN=1` tests several at once. `large` (Atlantis into the sea)
  formed two fireballs; it carries only ~8 t here (OMS/RCS): the 100 t case needs a
  vessel with the tank (phase 0's deviation).
- **Warp**: the model is analytic in the age (grid, lean, lift, roll, the flow map's
  phases all from t): right at any warp; nothing accumulates. The froxel cap and the
  CrashFires' particles are emitted per step: at high warp they thin out (fewer steps a
  second). Not verified at 10x (plan 2.6 wants a 10x sheet).
- **Pause**: the march's jitter sequence advances with the simulation time only; the
  grid is frozen; `--pausecheck` showed sub-pixel TAA differences only (phase 2). The
  flow map and the lean are in the age too.
- **Cuts**: the grid is camera-independent; the TAA/SSR/RTL histories are reset by the
  cut logic (6b4f4353); the jolt does not cut. Not re-verified since phase 4.
- **Settings**: the graphics settings have no "Fireball: sprites / volumetric" entry yet
  (`ORBITER_VK_FIREBALL=sprites|volume` only); the density setting applies to the
  particles, not the march.
- **Budget**: sea 1440p fb sum max 1.65-1.89 ms (3-3.8 s, the wide soot ball), close
  1440p 2.33 ms peak frame (about 2 asked); the explosion's peak particle + fb frame
  3.2-3.4 ms at 1440p (5 asked). The trails in the volume add to `pt volume` on the
  midair scenes (not measured at 1440p). A `budget.py` "Crash" flight is still to do.
- **The Atlantis 100 t case**: not possible with the stock Atlantis vessel (the ET is not
  part of it); either a scratch vessel class with 100 t of propellant or the ET as a
  vessel. R 95 m, t_f 3.7 s, the grid's cells 2-3 m: the march's detail may need the
  finest octave scaled by R.
- Open looks: the spill's tongues are a row of upright strokes (uniform heights would
  need the spill's flames varying more in size and rate across the slick); the stem's
  froxel puffs still show as soft blobs where the column is thin (3-5 s); the skirts
  are sprites; the vacuum shell's first frame is grey.

### Verification (phase 5)

Sheets (1080p): `phase5/{sea,land,midair,midair300}_sheet.png` (sea 2-12 s, land 3-25 s,
the midair break-ups at 150 and 300 m/s at the standard times); "before" on the phase
4c build (`pre.sh`): `before_*_sheet.png`; A/B `ab_{sea,land,midair,midair300}.png`
(4c left, 5 right). Videos `sea_crash.mp4` (12 s), `land_crash.mp4` (14 s),
`midair_crash.mp4` (8 s), 1080p, the wreck-view frame dropped. Dev: `dev/m150vol`,
`dev/m300vol` (the volumetric trails tried), `dev/{sea,land,midair}` (the first build).
- Midair 150 and 300 m/s: the trails are soft unbroken bands at 2-8 s where phase 4c
  had bands of round discs (polka dots) at both speeds; no beads at the far ends down
  to 12 s; flame tongues stream aft of the pieces; the quench line in the sea log for
  every piece going under (`sea/orbiter.log`: "under ... quench").
- Sea 3-10 s: the spill's flames are rising tongues (a row of round puffs before); the
  black trail discs of the skipping pieces are gone (their smoke is in the volume, a
  dark haze at the surface joining the stem); the stem a column.
- Land 5-25 s: the pieces and the spill burn for t_w (9.7 s) with tongues, the column
  black; at 15-25 s the smoke lightens to grey as it smoulders; embers over the spill
  and the pieces at 8-15 s (specks; small at 350 m).
- Validation layer (sea, 3 and 8 s): clean (`phase5/validation`). ctest -R
  "Particles|Water|Damage|Debris": the known DamageEffectsTest (AIR roll) failure only.

Perf (sea 1440p, `perf1440.txt`, `fbperf.txt`; 4c -> 5): crash 0-3 s median frame
19.92 -> 20.20 ms; 3-5 s 16.59 -> 16.27; 5-12 s 17.1-17.3 -> 15.5-15.6 ms (the
trails' sprites 1.25-1.45 ms -> 0.1: into the volume, `pt volume` +0.1); peak particle
+ fb frame 3.41 -> 2.91 ms; fb passes unchanged (march 0.86 median over 1-2 s, sum max
1.81 at 3-3.8 s). Budget: met as in 4c.

### Status (phase 5)

Done. What convinces: the trails as bands at any speed, the tongues off the pieces and
over the slick, the smoulder's grey, the land wreck burning out over t_w. What does
not: the spill's tongues are a row of upright strokes of much the same height; the
embers are hardly seen at 350 m (a pixel each); a piece's dark trail puff sits on the
piece itself as it flies (a black blob behind it at 2-3 s midair); the hull glow is not
judged at this distance.

## Phase 6 (LOD, several fireballs, warp, pause, cuts, settings, budget)

What changed (`Fireball.h/.cpp`, `fireball.frag`, `VObjects.cpp/.h`, `Particles.cpp/.h`,
`GfxSettings.cpp/.h`, `Scene.cpp`, `Tests/Bench`):
- **LOD by screen size**: the march's step by the ball's screen radius: 0.5 cells over
  700 px, 0.6 over 400, 0.75 over 150, 1.0 over 60, 1.4 below (was 0.5 / 0.7 / 1 at 400 /
  150); the finest noise octave (the flame's texture at r/25) weighted out below ~150 px
  of radius (gone under 50: `Params.asym.z`); a phase of the flow map whose cross-fade
  weight is under 0.07 skips its two noise taps (a sixth of the march's taps, on average).
  Below 12 px of radius the sprites, as before.
- **No pop at the switch**: hysteresis (one marched stays marched down to 8 px; one not
  needs 12) and a cross-fade: `VDebris::fbVolFade` moves between 0 and 1 over 0.35 s as
  the choice switches (at its birth no fade); the volume's extinction is scaled by it
  (`Ball::fade` -> `heat.w`) and the flame and core sprites are emitted at `1 - fade` of
  their alpha meanwhile (they build up over their lifetimes, about as long). A ball fading
  out is still drawn down to 5 px (`DRAW_PIXELS`). A ball beyond the 4-fireball cap still
  pops to sprites (rare: five or more at once).
- **Overlap merging** (plan 2.6): a ball forming within a live ball under 0.35 t_f old,
  with the centres closer than half the sum of the radii, joins it: its propellant is
  added, R grown is eased to the new value over 0.2 s (`fbRGoal`; the same-step join sets
  it at once as before), t_f follows. A registry of the live fireballs (`LiveFireballs`,
  owners removed in `~VDebris`). The centres compared *now* (`gpos`): the other ball's
  `fbPos` has been moved on this step - comparing against `p0` (a step ago) put them a
  kilometre apart in the global frame (the Earth's orbital motion). Logged with
  `ORBITER_VK_FIREBALL_LOG` ("forms N m from X's ball ...: joins it / apart / too old").
- **Warp**: beyond 100x `WantsVolume` says sprites (the ball's whole life is a frame or
  two); at 10x the model is drawn at its analytic age (nothing accumulates; the grid is
  cheap to recompute - the plan's "grid step skipped at > 10x" is moot, there is no step).
- **Pause**: unchanged (frozen; the jitter's sequence advances with the simulation time).
- **Cuts**: unchanged (camera-independent grid; the history reset of 6b4f4353).
- **Settings**: `Fireballs` on the Graphics page (Effects group, requires Particles):
  Sprites / Volumetric / Volumetric, full resolution; presets 0 / 1 / 1 / 1; live;
  `ORBITER_VK_FIREBALL=sprites|volume|full` overrides (and locks) it;
  `ORBITER_VK_FIREBALL_FULLRES=1` still means full resolution. `FireballRenderer::
  ApplySettings` runs each frame from `SetPrepass`; the half-size images are remade at the
  new resolution when the choice changes.
- **Memory release**: the half-size images (color, mask, occlusion, depth: ~20 MB at
  1440p) are now made when the first fireball asks to be marched (`WantsVolume` in the
  visuals' update sets `wanted`; `SetPrepass`, early in the render frame, allocates them
  in-frame with no idle wait) and released with the grids (~70 MB: 4 slabs of A, B, C)
  two minutes after the last fireball (`DeferDestroy`). The sprites' occlusion image is a
  1x1 "nothing" meanwhile (`dummyOcc`; `onOcclusion` tells the particles of the change),
  and `ParticleSystem::SetFireballOcclusion` no longer waits for the device: each frame
  slot's set is written at its next `Simulate` (`fbOccDirty`), as the volume's sets are.
- **Budget**: `Tests/Bench/Crash.scn` + `Script/Bench/crash.lua` (the sea case, observer
  170 m aside, run to 12 s after the break-up, "bench: crash at T" logged), `run.sh Crash`
  (fixed step 1/30, damage on), `budget.py` "Crash": the explosion's peak frame (particle
  + fireball passes) <= 5.0 ms and the fireball passes' peak <= 2.0 ms (decision 8) over
  0-12 s after the break-up; median and p95 reported.
- Harness: `run.py --warp K` (time acceleration from the break-up, stills by simulation
  time), `--cut T` (the camera cut to the scene's second observer, `fx_crash.lua CUT_*` ->
  `camCut`, six frames `cut_N.png`), `--pull T D0 D1 DUR` (the observer pulled back
  geometrically, `camSetDist`); scenes `large100`/`large100l` (`VkTest_Atlantis`: the
  Atlantis with `PropellantResource1 = 136000` in `harness/Config/Vessels`, installed into
  the build tree's Config/Vessels - 95 t of propellant aboard after the OMS/RCS levels),
  `small300` (DG at PRPLEVEL 0.028: 308 kg), `twin` / `twinmerge` (`fx_twin.scn`: a second
  DG V2 flown SEP m to the side at SEP_SPEED by `fx_crash.lua`), `seacut`, `lod200/1k/3k/8k`.
- **Fixes found on the way**: (1) `WantsVolume` measured the ball's distance from `fbPos`
  a step behind (the global frame moves 30 km/s with the Earth: a kilometre off at 1x,
  ten at 10x) - at 10x warp the ball went to sprites in its second step, and at 1x every
  LOD decision was made as if the ball were a kilometre farther; `FireballPosNow` (fbPos
  moved on by fbVel to now) is used, and `gpos` for the vacuum shell. (2) The volume's
  last two frames (1.8 t_f, extinction fading to 0) flashed a pale sunlit grey over the
  dark cap taking over (thin soot loses its self-shadowing; seen in phase 5's sea video
  at 3.57-3.60 s too): the soot albedo now goes down with the extinction to the cap's
  (0.075 at the end). (3) The march's step was 0.5 cells at > 400 px: 0.6 there now (the
  budget), 0.5 kept over 700 px.
- **Perf** (1440p, RX 9070 XT, RT on, FSR 1.0; `phase6/fbperf.txt`, `perf1440.txt`,
  `perfothers1440.txt`; the final build). Fireball passes (grid + march + composite), GPU ms:

  | case | march median 1-2 s | fb sum max 1-2 s | fb sum max 3-3.8 s (soot ball) | peak particle + fb frame |
  |---|---|---|---|---|
  | sea 170 m, phase 5 | 0.86 | 1.26 | 1.81 | 2.91 |
  | sea 170 m, phase 6 | 0.69 | 1.11 | 1.20 | 2.69 |
  | close 72 m, phase 4c/5 | 0.91 | 2.33 | 1.46 | 3.4 (phase 5) |
  | close 72 m, phase 6 | 0.85 | 1.66 | 1.67 | 4.21 (3.1 s: pt volume 0.9, march 1.2) |
  | twin (two balls, 220 m) | 0.35 (grid 0.35: two slabs) | 0.81 | 1.03 | 3.69 (spray sprites 1.5) |
  | Atlantis 93 m at 450 m | 0.42 | 0.89 | 1.17 | 2.80 |

  Budget (decision 8: ~2 ms close, ~5 ms the explosion's peak): met - the close fireball's
  passes peak at 1.67 ms (was 2.33), the explosion's particle + fireball peak 4.2 ms close,
  2.7 ms at 170 m. Bench "Crash" flight (`bench/budget.txt`): peak 2.54 / fb 1.10 ms. The
  savings: the 0.6-cell step over 400 px (was 0.5), the flow phases' taps skipped at low
  weight, the fine octave only near, the fading soot ball's step up to 1.5x.

### Deviations (phase 6)

- **The settings entry** is Sprites / Volumetric / Volumetric, full resolution (the plan's
  "off/sprites/volume or quality"): "off" would leave a crash with no fire at all, and the
  full-resolution march is the one quality step that shows (about 4x the march's cost:
  for screenshots). No separate "density" slider: the particle density applies to the
  sprites around the ball as before; the march's step is by screen size.
- **"Light only" below 6 px** (plan 2.6): not a separate level; below 12 px the sprites are
  a few pixels and cheap.
- **Grid step at > 10x warp** (plan 2.6): nothing to skip - the grids are recomputed from
  the analytic model each frame (0.2 ms) and carry no state.
- **The Atlantis 100 t case**: the stock Atlantis carries ~8 t (phase 0); `VkTest_Atlantis`
  (a harness class: Atlantis + `PropellantResource1 = 136000`, which with the OMS/RCS
  levels gives 95 t aboard) is installed into the build tree's Config/Vessels by `run.py`.
  On land R 93 m, t_f 3.6 s (plan: 95 m, 4 s); on the sea R 83 m. The sea sheet is
  dominated by the pieces' spray (60 m/s into the water) until ~2 s, as phase 0 noted.
- **Two DGs mid-air**: in `midair` both DGs break up at the same step in one place and
  their pieces join one ball (the join rule: within R; 16.7 t, R 52 m) - physically one
  explosion. Two fireballs at once are shown by `twin`: two DGs in formation 90 m apart into
  the sea (V2 at 50 m/s: at the same speed the water entry's outcome varied with the wave
  met - one of the two did not break up); `twinmerge` (20 m apart) shows the overlap merge.
- **The cross-fade beyond the 4-ball cap** is not done (the fifth ball pops to sprites).
- **Memory**: the plan's "<= 4 x 2 MB" became ~70 MB of grids in phase 2 (96x96x112);
  phase 6 adds their release and the half-size images' (~20 MB at 1440p): nothing stays
  allocated while no fireball has burned for two minutes.
- **The harness' cut and pull-away** are done by the scenario script (`CUT_T`, `PULL_*`
  from `run.py`): the remote's `lua` command runs in its own interpreter (TestInput.cpp:
  `oapiCreateInterpreter`), so the first pass's "cut" never cut (the cuts logged were the
  wreck-view cut at the destruction) and the pull-away never moved; found by the frames.
- **The sprite switch at 8 px** is exercised by `pullfast` (170 m to 8 km in 1.5 s), not by
  the 4 s pull-away (the ball is gone before 6.5 km, where 8 px is at 1080p, FOV 40).

### Verification (phase 6)

All 1080p unless noted; `phase6/batch.sh` (first pass), `batch2.sh` (the cut and pull-away
moved into the script, the warp and merge fixes), `batch3.sh` (the pull-away videos, the
bench flight), `batch4.sh` (perf, validation, ctest on the final build).
- **Atlantis 100 t**: `large100l_sheet.png` (land, R 93 m, t_f 3.6 s: a desert ball
  lifting into a mushroom by 4-6 s), `atlantis100_land_crash.mp4` (1 s before to 10 s
  after), `large100_sheet.png` (sea, R 83 m: the pieces' spray hides it until ~2 s).
- **300 kg DG**: `small300_sheet.png` (M 361 kg aboard, M_p 90 kg: R 13 m, t_f 1.1 s - a
  puff the pieces fly on out of).
- **Two DGs**: `twin_sheet.png` + `twin_crash.mp4` (two balls 90 m apart, 0.1 s apart, each
  marched, two soot balls, two slicks); `twinmerge_sheet.png` (20 m apart: V1's pieces join
  V2's ball 0.1 s old - the log: "joins it (overlap): R 37.0 -> 40.2 m" and on to 45);
  `midair_sheet.png` + `midair_crash.mp4` (the collision at 300 m: one ball of both, the
  trails).
- **10x warp**: `warp10_sheet.png` (`--warp 10`: stills by simulation time): the ball at its
  analytic age at every still (full size 0.3 s, lifting 0.7, soot ball 1-2, cap 3 s), marched
  throughout (0 switches to sprites after the distance fix; before it the second step went
  to sprites). The spill's tongues and the cap's puffs are sparser (per-step emissions at
  0.33 s steps), as the hand-off expected.
- **Pause**: `pause_check.png`, `pausediff.py`: three frames 4 apart while paused at 0.8 s
  differ by a mean of 0.4-0.7 (of 255), all pixels over 8 in the sea's rows (648-952: the
  water moves), none in the fireball's.
- **Cut**: `cut_check.png` (eight frames from one before the cut at +0.83 s to the observer
  on the far side): the first frame after the cut is clean (the ball from its sunlit side,
  no ghost of the shade side, no smear); the log's "camera cut at frame 83" resets the
  histories.
- **LOD**: `lod_row_1s.png` (200 m, 1 km, 3 km, 8 km at 1 s: marched at 260, 52 and 17 px
  of radius, sprites at 6.5 px), `lod{200,1k,3k,8k}_sheet.png`; `lod_pullaway.mp4` (170 m
  to 8 km over 4 s from 0.3 s: `pull_tiers.png` crops at the step changes 0.75 -> 1.0 cell
  (1.0 s, 350 m) and 1.0 -> 1.4 (2.0 s, 865 m) show no pop; `pullcheck.py`: the largest
  frame-to-frame changes after the flash are at 3.3-3.5 s, the ball fading out at 4 km);
  `lod_pullaway_fast.mp4` (to 8 km in 1.5 s: the switch to sprites mid-burn at 1.77 s, 5.4
  km, 8 px: `pullfast_switch.png` - the cross-fade leaves no pop at that size).
- **Settings**: `settings_row_1s.png` (`ORBITER_VK_FIREBALL=sprites` / the default / `full`
  / a `Fireballs = Volumetric, full resolution` line in a file via `ORBITER_VK_GRAPHICS_CFG`):
  sprites a pile of discs, the volume the ball, full resolution the same ball with crisper
  edges; `set_*_sheet.png`.
- **Validation layer** (sync validation on): sea and the Atlantis on land: 0 lines
  (`validation/`, `validation100/`), the twin too (`validationtwin/`).
- **ctest** `-R "Particles|Water|Damage|Debris"` serial: 12/13, the known
  DamageEffectsTest (AIR roll) failure only (`ctest.txt`).
- **Bench Crash** (`Tests/Bench/run.sh Crash`, 1440p, fixed step 1/30; `bench/budget.txt`):
  356 frames over 2-14 s: explosion peak frame 2.54 ms (<= 5.0), fireball passes' peak
  1.10 ms (<= 2.0), median 1.07, p95 2.19; allocFails 0, dropped 0: met. (The first run
  hit the 600 s timeout: the flight-over menu's pause stops `proc.wait_frames` from
  returning, so the script could not unpause; the focus is a ShuttlePB `OBS` now.)
- **The volume's end**: `seaend_strip.png` (3.40-3.73 s): the dark cap passes to the froxel
  cap without the pale two-frame flash (`phase5_end_strip.png` for the before).

### Status (phase 6)

Done. What convinces: the same ball at 200 m, 1 km and 3 km with nothing seen at the
step changes; the fade to sprites at 8 px invisible; the 10x warp sheet reads as the 1x
sheet sampled; a cut mid-explosion lands on a clean frame; two balls side by side, each a
fireball with its own cap; the overlap merge is seen only in the log and the ball's size
(as it should be); the Atlantis ball on land is the plan's top end (93 m, lifting into a
mushroom); the 300 kg puff is over in a second; the volume's end no longer flashes pale.
What does not: the sprites at warp beyond ~30x are visibly sparse (per-step emission);
the froxel cap that takes over from the ball is faint beyond ~3 km (the volume's far
slices are coarse); the settings' "full resolution" sharpens the edges more than the
billows; a fifth ball pops; the sea Atlantis is hidden by its pieces' spray.

## State of the explosion (after phase 6)

**Done** (phases 0-6, the plan's sections 2-6 and decisions 8.1):
- Timing and scale by the propellant: f_p 0.35 land / 0.25 sea, R = 2.9 M_p^(1/3) (7 t DG:
  35-39 m), t_f = 0.45 M_p^0.2 (2 s), the wreck's visible flames ~10 s then smouldering,
  the soot rising ~50 s; a 300 kg DG a 13 m, 1.1 s puff, a 95 t Atlantis a 93 m, 3.6 s ball.
- The fireball a marched volume: a procedural model (ellipsoid drawn out along the path,
  lobes, billows of three sizes carried outward by a two-phase flow map and rolled by the
  vortex ring, a lean, burn progress to soot) in 96x96x112 grids, blackbody emission with a
  camera-like knee, self-shadowed sunlight, the fire's light on its own soot; the ignition
  flash; the sea/ground skirts, boiling, scorch; heat shimmer; the camera jolt with the
  sound delay; the vacuum shell; two lights (ball + foot) RT-shadowed; the exposure counts
  it partly at night; the soot ball handed to the froxel smoke volume in place.
- Debris: trails as soft bands in the volume at any speed, flame tongues, embers, quench
  steam, hull glow, the spill's fire with tongues and a column, smoulder to grey.
- Phase 6: LOD by screen size with hysteresis and a cross-fade (no pop at the switch), the
  4-ball cap and overlap merging, warp (10x right, > 100x sprites), pause and cuts verified,
  the Fireballs setting (sprites / volumetric / full), memory released when idle, the
  bench "Crash" flight with the budget.
- Cost (RX 9070 XT, 1440p, FSR 1.0, RT on): the fireball's passes 0.75 ms median, <= 1.9
  ms peak close up; the whole explosion's particle + fireball peak 2.8 ms (sea, 170 m),
  4.4 ms close (72 m); nothing allocated two minutes after the last fireball.

**Open issues** (none blocking; by weight):
1. Close up (< 100 m) the surface is soft: the half-resolution march and the 96^3 grid
   give billows, not the curled flame tongues and crisp soot edges of footage; the full-
   resolution setting sharpens the edges but not the structure (the model's).
2. The soot ball's hand-over to the froxel cap is a swap of looks (volume's cauliflower ->
   the froxel's soft puffs) over 1.3-1.8 t_f; it reads, but the cap afterwards is blurrier
   than the ball was.
3. A fifth fireball on screen pops to sprites (no cross-fade beyond the cap); rare.
4. The spill's tongues are a row of upright strokes of similar height; the embers are a
   pixel each at 350 m; a flying piece's trail puff sits on the piece for a frame or two.
5. The streaky seam where the half-resolution composite meets the sea at the waterline
   (phase 2's note) is faint but there in close views.
6. The sea case's spray from fast heavy pieces (Atlantis at 60 m/s) hides the ball for its
   first two seconds - the spray's scale for large pieces is the water effects', not this
   work's.
7. The vacuum shell's first frame is grey (phase 5's note); the flash light carries it.

**Phase 7 recommendation** (for the user to decide; optional, after review):
- *Fluid sim*: a 64^3 (or the grid's 96x96x112) semi-Lagrangian advection + a few Jacobi
  pressure iterations behind the same grid interface would replace the flow map's "noise
  carried outward" with real rolling: curls at the ring's edge, tongues pulled into the
  cap, the stem's inflow. Cost: ~0.5-1 ms of compute per ball per frame (4-6 passes over 1
  M cells), 2-3 agent-days, and it gives up frame-rate independence (a fixed sim step with
  sub-stepping at warp; pause and cuts are fine since the state is in the grid). Worth it
  only if the close-up look (open issue 1) matters to the user: at 170 m the present
  procedural ball already reads as a fireball.
- *Propellant types*: per vessel class via `Config/Damage/<class>.cfg` (e.g. `Propellant =
  hydrogen | hydrocarbon | hypergolic`): hydrogen pale yellow-white, near-sootless, 0.6 t_f;
  hypergolic a red-brown NTO cloud over an orange ball, less soot. About a day: a type in
  FireballModel (emission colour, soot share, t_f factor) and the sprites' albedo. Cheap and
  visible (the Atlantis/ET case would look different from the DG's).
- *Audio hook*: the flash's thud at d / 340 s (the jolt already knows when): a one-line
  callback for the sound module when there is one.
- Recommended order if any: propellant types (cheap, visible) first; the fluid sim only
  after a look at the close-up review frames (`phase6/perfclose1440`, `phase4/close`).
