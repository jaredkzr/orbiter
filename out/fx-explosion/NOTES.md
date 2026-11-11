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
