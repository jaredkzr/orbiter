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
