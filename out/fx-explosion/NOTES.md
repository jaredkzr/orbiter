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
