# Crash explosion: review and rebuild plan

Branch `linux-headless-core` at 75fdfaf6 (VK explosion pass). Review + plan only:
no tracked source was changed. Review material is in `out/fx-explosion/`:

| File | What |
|---|---|
| `review/v2_overview.png` | `out/water-video/dg_crash_explosion_v2.mp4` at 2 fps (one tile per 0.5 s) |
| `review/v2_closeups.png` | crops of frames 42-250 of the same video (f44 = first frame of fire; t label = video time) |
| `base/`, `base_sheet.png` | the harness re-run at HEAD with `ORBITER_VK_FIREBALL_LOG` + `PARTICLE_STATS`; log `base_orbiter.log` |
| `review/base_s04_debris.png` | crop of `base/s04.png` (t = 3.5 s): the pieces skipping ahead of the fireball |
| `land/`, `land_sheet.png`, `review/land_s09_wreck.png`, `review/land_s02_fire.png` | the same crash on land (scratch scenario `Scenarios/VkTest/fx_land_fuel.scn`, Texas; the crash point is at the frame's right edge because the ground is higher than the observer rig assumes) |
| `night/`, `night_sheet.png` | the same crash at night (scratch `fx_night_fuel.scn`, MJD - 0.5) |
| `perf1080/`, `review/base_1080p.ft` | 1080p run with `ORBITER_VK_PROFILE=1 ORBITER_VK_FRAMETIMES` (per-pass GPU ms) |

Harness: `out/water-video/stills.sh <ABS_DIR> [scenario]` (40 m/s, 12° dive, DG with
PRPLEVEL 0.5 = 7050 kg of propellant, observer 170 m to the side, 11 stills t = 1.9..12 s,
contact sheet `<dir>_sheet.png`). The crash log of the base run:

```
V1 was destroyed: broke up at 47 g (aft right section overloaded 6.5x)
fireball V1 aft right section fuel 597 kg (rho 1.22, water 1)
fireball ... aft left 597, upper 559, aft 1259, aft right 2 622, aft left 2 622, right 421, core 1952, left 421 kg: joins the first
```
Nine pieces, 7050 kg, one merged fireball: R = 2.9·7050^(1/3) = 55.6 m, fbDur = clamp(0.9·7050^0.26) = 9 s → clamped to 6 s.
Live particles peak 44 485 (`base_orbiter.log:318`), of which sprites 44 158; pool 262 144, no drops.

---

## 1. Review findings

### 1.1 What the frames show (evidence)

Times below are seconds after the first frame of fire (video frame 44, t_video 1.87 s).

| t | frame | what is seen (`review/v2_closeups.png`, `v2_overview.png`) | what a 7 t kerosene crash looks like |
|---|---|---|---|
| 0.00 | f44 | a cream/pale-orange cloud of round puffs already the size of the vessel; the intact DG mesh visible inside it; **no flash, nothing white** | a white-yellow flash < 0.1 s, blinding in daylight (the camera clips) |
| 0.1-0.5 | f46-f57 | the cloud grows as discrete puffs; still cream; the DG still intact inside it (the pieces have not separated) | rapid growth to ~0.7 R by 0.3-0.5 s; the core yellow-white, the rim orange; the wreck pieces inside are hidden |
| 0.9 | f70 | ~3 vessel lengths across, orange-tan, a few dark grey discs (soot rims of individual sprites) | near full size, brightest overall; soot begins on the upper surface |
| 1.5 | f90 | full size (u = 0.25: `VObjects.cpp:1324`), a pile of round balls | the ball lifts off; the top half blackens; the fire inside seen through gaps |
| 2.2 | f110 | still fully orange, dark polka-dot discs everywhere, lavender halos where sprites overlap sky | flames mostly gone except at the base; the black cap rising |
| 3.9 | f160 | still a large orange ball, bigger than at 1.5 s | black mushroom + stem; a pool fire at the site |
| 5.2 | f200 | orange-brown ball, little change | black smoke only |
| 6.9 | f250 | brown-orange ball, now with soot | black smoke, thinning |
| 8-12 | overview row 4 | a blurry black ball rising, nothing burning below it; the pieces have sunk | a smoke column from the burning fuel slick |

Night (`night_sheet.png`): a uniform orange glowing pile with no dark structure (soot is black
and unlit), the fire lights the sea nicely, but after burn-out nothing glows: only lit foam.
Land (`land_sheet.png`, `review/land_s09_wreck.png`): the same ball; the pieces that come to
rest DO burn with small flames and a column (`VObjects.cpp:1149-1211`), so the wreck-fire
path works on land; it is the sea case where the wreck fire is missing (1.4).

### 1.2 Timing: the fire lasts 3x too long and grows too slowly

- Fireball duration: `fbDur = clamp(0.9·M^0.26, 0.8, 6)` s (`VObjects.cpp:1098, 1108`). This is
  the Martinsen-Marx BLEVE correlation (t ≈ 0.9·M^0.25), which assumes the *whole* mass is
  airborne vapour (a flashing LPG tank). For a crash, liquid fuel is sprayed by the impact and
  only a fraction mixes and burns as a fireball; the rest pools (and on the sea, spreads under
  the surface). The BLEVE correlations give, for 7 t: D = 5.8·M^(1/3) = 111 m, t = 0.45·M^(1/3)
  = 8.6 s (Roberts / CCPS). Crash footage of aircraft with 3-10 t of fuel shows fireballs
  30-80 m across whose flames are gone in 2-4 s.
- Flames are emitted until u = 1 and live up to 1.2·fbDur (`:1346`), so orange sprites are
  visible for ~7 s after the impact (f250). The soot is emitted until u = 1.5 = 9 s (`:1383`).
- Growth: `r = R·(0.45 + 0.55·sqrt(min(4u,1)))` (`:1324`) reaches R at u = 0.25 = 1.5 s. Real:
  0.3-0.5 s for this mass (the Martinsen-Marx growth law D ∝ t^(1/3) reaches its maximum at
  t_d/3, but with t_d ≈ 2 s that is 0.7 s).
- No flash: the "flash" is 25 % extra sprites on the second step (`:1336-1337`), tinted
  orange like the rest; the light `I = 12·e^(-3t)` (`:1572`) is the only flash, and the sea
  shows it faintly.

### 1.3 Colour, brightness, structure

- Nothing is ever white. Fire sprites are tinted `(1, 0.58, 0.12)` (`:1349`) and the ramp
  starts at `K0 = (1, 0.93, 0.72)` (`particles.glsl:59`), product `(1, 0.54, 0.09)`: orange
  from birth. The core ring's tint `(1, 0.8, 0.45)` gives `(1, 0.74, 0.32)`. `glow = 4.5→3`
  × `alpha ≤ 0.6` × premultiplied cover (`gpuparticle.frag:126-130`) means a single sprite
  writes ≤ 2.7 sun-white and the "cover" alpha (≤ 0.92) stops the sum from building up. The
  tonemap whitens only above ~2-5 (`tonemap.frag:30-39`, shoulder 0.6): the fire sits at
  "bright orange", never "blinding". Day exposure is clamped to 0.6-1.3 so the fire cannot
  pull it down either (`VulkanClient.cpp:343-365`).
- Dynamic range within the ball is flat: every sprite is lit the same (emissive, no
  self-shadowing, no soot absorption of the flames behind). Real fireballs are opaque: you
  see a *surface* with bright hot regions, dark soot lanes and sunlit grey-black billows, not
  a translucent sum.
- Polka dots: each sprite's soot rim (the `cover` term rising with age, `frag:129`) draws a
  grey/black disc per sprite; with 160·D sprites the ball reads as a pile of balls (f110-f250).
  Sprite tuning cannot fix this: the look is a sum of round things.
- Lavender halos: additive orange over blue sky with partial cover (f110 top-left puffs).
- The soot: `albedo 0.06`, `lifetime 14 s`, `slowdown 0.3-0.7` (`:1399-1403`), one class of
  volumetric puffs: the "blurry black ball" is the froxel volume's look for a compact cloud
  (240×135×128 log-depth froxels: at 170 m a froxel is ~0.5 m wide but 14 m deep; the march
  erodes with one noise field). It has no stem, no cap/stem distinction, no lit grey top.

### 1.4 Scale and motion (inherited velocities)

- R = 55.6 m (D = 111 m, six DG lengths) is the full-BLEVE size; with a participating fraction
  it would be 35-40 m (see §5). Not wrong by an order of magnitude; a user decision (Q1).
- The fireball centre takes the first piece's velocity and is braked at
  `FIREBALL_SLOWDOWN = 3 /s` (`:1032, 1317-1318`): it stops within ~0.3 s and ~13 m of the
  impact point. The pieces skip on across the water at 20-30 m/s (`review/base_s04_debris.png`:
  at t = 3.5 s they are 40-60 m ahead of the ball, splashing, with no fire and no smoke). In a
  crash the fuel travels with the wreck: the fireball is elongated along the path and its
  front rides with the fuel-carrying pieces for the first second, then detaches and rises.
  The sprite spread is isotropic (`e.spread`, `:1343`).
- Rise: `0.5·sqrt(gR) = 11.7 m/s` for R 55 m once grown (`:1319`): right order (buoyant
  Froude number ~0.5 is what BLEVE lift-off data give), but applied from u = 0.5 on, i.e. the
  ball lifts at 3 s instead of ~0.7-1 s.
- Wind/air: sprites are braked toward the co-rotating air + wind (`particle_update.glsl:
  150-153`); the hot flag adds a 4 m/s rise decaying over 6 s (`:165-166`), fine for the soot.
- Debris: pieces inherit the vessel's velocity + spin and `KeepImpulse` (`Vessel.cpp:5531-
  5532`, `Debris.cpp:93-98`), with a 2 m/s separation push; the core gets none. OK for the
  effects: `e.p0/p1 = prev/gpos, e.vel = vel` (`:1162, 1187`) so trails inherit correctly.

### 1.5 Debris fire and smoke: why the trails and the burning fuselage are missing at sea

(Core-side facts from `Src/Orbiter/Vessel.cpp:5523-5535`, `Debris.cpp:119-135`,
`VesselWater.cpp:79`, `Debris.cpp:589-605`.)

- Each piece gets a fuel share by mass and `Ignite(f, clamp(4 + 0.02 f, 4, 60))`: here the
  core 1952 kg → burns 43 s, aft 1259 kg → 29 s, the wings 421 kg → 12 s. `burn` only counts
  down; water never puts it out, nothing extends it.
- All debris is "flooded" and displaces only `mass/2700` m³: a 2.7 t core displaces 1 m³ and
  sinks like a stone (W6 decision: "wrecks and debris sink by structural density"). By s07
  (6.5 s) no piece is at the surface. The client keeps emitting its flames, steam and the
  WreckSmoke column at the sunken position (`atRest` uses `alt` from the mean radius,
  `VObjects.cpp:1210`), under the sea surface, where the water hides it.
- On water the trail smoke is pale steam: albedo 0.85, alpha 0.3·f, 6 s (`:1195, 1199`), the
  column 0.45 (`:1469`). Burning fuel on a piece smokes black whether it is over water or
  not; steam belongs to the splash (and the quench when a burning piece goes under), not to
  the trail.
- Flames on a flying piece live `clamp(4·s/airSpd, 0.15, 0.7)` s (`:1165`) at 45 + airSpd/
  (0.35 s) per second at density ≤ 4: a 20 m/s piece of size 3 m gets ~60 small flames/s of
  0.6 s: a flicker, not a burning piece. The hull glow needs temp > 900 K (`:1616`); ignited
  pieces are at ≥ 1000-1200 K so they do glow slightly.
- Beads at speed were fixed by distance-based emission (`:1181-1183`, "f6 midair"), but the
  smoke's `e.alpha = 0.5·f` fades with burn time from the start on short-burn pieces (f < 1
  when burn < 4 s).

### 1.6 Lighting the world

- One point light at the ball centre: `I = min(1, R/10)·max(12·e^(-3t), 2.5(1-u))`, colour
  `(1, 0.55, 0.2)`, range 15 R, `att = {1, 0, 1/(0.01 range²)}` (`:1568-1591`). It lights
  meshes, terrain, water diffuse/foam, particles (per vertex, unshadowed) and the smoke volume
  froxels (occluded). Water has no local-light specular (`tile_main.glsl:845, 967`): the
  fire's glitter path on the sea is missing (the night sheet shows diffuse light on the waves
  only). RT shadows only for the first 4 lights after the stars (`RTLighting.h:22`).
- Radius 0: not an extended source; a 50 m fireball lights a wreck piece beside it as a point.

### 1.7 Cost (current)

All particle passes together cost 1.5-1.8 ms while the ball burns and 2.2-2.6 ms during the
soot phase (1080p; table in §7.3). The fire sprites themselves are 0.3-0.5 ms: the cheap part.
The frame goes from 8.6 to 17.6 ms at the crash, mostly RT (debris BLAS rebuilds, the
fireball light traced), SSR and the prepass, not the effect.

---

## 2. Volumetric fireball: design

### 2.1 Why not the froxel smoke volume

The smoke volume (`smokevol.glsl:8-37`; splat/resolve/march in `smoke_*.comp`) is the right
home for the *soot* and it already takes it (`Class::FireballSmoke`, volumetric). It cannot
draw the *fire*:

1. No emission or temperature channel: froxels hold extinction, coverage, sun+ambient light
   × albedo, sun share, core, albedo luminance (`VA_*`). Adding emissive channels means 3 more
   r32ui accumulators on 3 levels (+~36 MB), changes to splat, two resolves, the march, the
   integrator and `volInteg`, in a pipeline tuned over the FX audit.
2. Depth resolution: 128 log slices from 1 m to 20 km = 8 % depth per slice. A 70 m ball at
   170 m spans ln(205/135)/ln(20000) × 128 ≈ **5 slices**. The half-res march puts its 40
   jittered steps over the occupied slices, but the data has ~5 samples through the ball:
   the fire would be a blurred orange blob (exactly what we have, minus the dots).
3. The volume's noise erosion is one planet-anchored 64³ field for all smoke (`smokeNoise`);
   a fireball needs its own advected field (rolling, expanding) and a temperature-driven
   colour, which per-froxel data cannot carry.

What the volume should do for the fireball: carry its soot (as now, but shaped: cap and
stem), carry the fireball's *extinction* so sprites behind it are fogged (`volInteg`), and be
lit by the fireball's light (the local-light path in `smoke_resolve.comp:92-118` already does
this with occlusion).

### 2.2 Representation: a small per-fireball grid fed by a procedural model

One `FireballRenderer` (new files `OVP/VulkanClient/Fireball.h/.cpp`, shaders
`fireball_grid.comp`, `fireball_sun.comp`, `fireball.vert/.frag`), modelled on
`PlumeRenderer` (bounding proxy + fragment ray-march + half-res depth-aware composite +
reactive mask: `Plumes.cpp:1000-1021`, `plume.frag`, `plume_comp.frag`).

Per fireball (max 4 marched at once; CPU-side `FireballState`):

- Parameters from the break-up: position/velocity of the fuel (the mass-weighted velocity of
  the merged pieces, not the first piece's), total fuel M, participating mass M_p, R(t),
  t_f, the air density, up vector, water/land flag, the sun direction.
- A 3D grid, `48 × 48 × 64` (x, y across; z up), `RGBA16F` ×2 (ping-pong) +
  one `R8` sun-transmittance grid: ~2 MB per fireball. Channels: **density** ρ, **temperature**
  T (K/3000), **soot** s (0..1), **advection offset** packed as a second RGBA16F image (u, v, w,
  age). Grid extent 2.6 R × 2.6 R × 3.5 R (room for the rise), moving with the ball's centre.
- `fireball_grid.comp` each frame (sim dt, frozen while paused):
  1. The base field: an ellipsoid of radius R(t) (elongated along the initial velocity by
     1 + 0.5·|v|/20 for the first 0.5 t_f, relaxing to a sphere), with a toroidal density
     profile once lifted (denser ring, thinner centre: the vortex ring of a real fireball).
  2. Advected noise coordinates (Neyret-style flow noise; no pressure solve): the offset field
     is integrated by the analytic velocity v(x, t) = radial expansion R'(t)·x/R + buoyant
     rise w(t)(1 - (r_h/R)²) ẑ with inward flow below the ball + curl noise at two scales;
     two offset layers blended with a period of ~0.7 t_f so distortion never blows up.
  3. ρ = base × fbm(detailNoise, x + offset) (3 octaves, the existing 64³ `detailNoise`);
     T = T_core(t) × g(r/R) × (0.7 + 0.6·noise); s = soot(t, height in ball, r/R): the top and
     the rim soot first, the base last.
  Cost: 147 k voxels × 3 noise taps: < 0.1 ms.
- `fireball_sun.comp`: a sweep along the sun direction through the grid (48-64 steps per
  column) → sun transmittance per voxel for self-shadowing: < 0.05 ms.
- CPU model for everything the renderer needs without a readback: luminous centroid
  (light position), integrated emission (light intensity), R(t), the soot fraction.

Why not a real fluid simulation first: a 64³ Navier-Stokes step with a 30-iteration Jacobi
pressure solve is ~0.3-0.5 ms per fireball and a week of work before it looks like anything;
at 64³ a 70 m ball is blobby anyway. The advected-noise model gives the rolling, expanding,
lifting look that footage shows, deterministically (replay-safe, cut-safe), in a day. The grid
interface (ρ, T, s) is what a sim would produce, so a sim can replace the generator later
(phase 7) without touching the renderer.

### 2.3 Rendering: ray-march of the grid with detail noise

`fireball.frag`, on a proxy sphere (or ellipsoid) of radius 1.3 R(t) drawn after `DrawVolume`
and before the sprites (`Scene.cpp:2674-2795`: between `DrawVolume` and `DrawParticles`),
half resolution by default like plumes, with the depth-aware 4-tap composite:

- Ray-sphere entry/exit; clip the exit at the prepass depth with a soft fade over 0.3 R
  (as `gpuparticle.frag:113-114` does), so the ball sits on the sea or the ground instead
  of cutting through it.
- 48-96 steps (by screen size, §2.6), per-pixel hash jitter (as plumes). Per step:
  - ρ, T, s, τ_sun from the grid (trilinear) × detail: a 2-octave `detailNoise` at 3-4× the
    grid frequency, advected with the same offset field (sampled from the grid), eroding ρ
    (thin tongues, holes) and modulating T by ±15 %.
  - extinction σ = ρ·(σ_gas + σ_soot·s), with σ_soot ≫ σ_gas: the ball is opaque where
    sooty, translucent where clean and hot.
  - emission: Planck colour at T (a 1D LUT 800-2600 K → scene-linear RGB, normalised so
    that 2000 K ≈ 25 sun-white, 1500 K ≈ 3, 1100 K ≈ 0.3; the K0/K1/K2 of `ParticleRamp` are
    its hues) × ρ·(1 - 0.6 s). Values from the audit's exposure facts: core 10-30 sun-white
    reads blinding and blooms, body 2-5 keeps its orange, rim 0.3-1 (the tonemap shoulder
    at 0.6, whitening ≤ 60 %).
  - scattering of sunlight: albedo a(s) from 0.6 (hot gas, grey) to 0.06 (soot) ×
    τ_sun(grid) × a two-lobe phase (HG 0.35 forward + isotropic) × the same multiple-
    scattering floor the volume uses (`0.25 + 0.75·(0.7e^(-0.7τ) + 0.3e^(-0.1τ))`,
    `smoke_resolve.comp:160-171`); + ambient × `frame.ambient` (night factor `.w`).
  - the ball's own light on itself is implicit (emission); the local lights of other bodies
    are ignored (cheap and invisible next to the fire).
  - front-to-back accumulation, early out at T < 0.01.
- Smoke in front: the fireball is drawn after `DrawVolume`, so multiply its result by
  `volInteg.a` at the ball's front depth (per pixel) and add nothing (`gpuparticle.frag:
  118-129` pattern): soot in front of the fire darkens it. The fireball's own extinction gets
  into `volInteg` through a handful of volumetric "virtual particles" emitted into the froxel
  volume each frame (an Emission of 4-8 large black volumetric puffs tracking the ball,
  lifetime one frame + ε, class FireballSmoke) so the sprites (spray, sparks, trails) behind
  the ball are fogged by it. Sprites in front are drawn after and are unaffected (correct).
- Output: premultiplied colour + alpha = 1 - T, blend ONE / ONE_MINUS_SRC_ALPHA, aerial
  perspective via `Inscatter` as plumes do, and `outReactive = clamp(max(α, lum/(1+lum)), 0,
  0.9)` (`plume.frag:91` convention) so TAA/FSR do not smear it (`taa.comp:112-119`).
- Needs: the particle set's `volInteg` (binding 23) and the "volume active" flag exposed by
  `ParticleSystem` (a small accessor; today there is none).

### 2.4 How it lights the world

- Replace the single point light with two (`VDebris::CollectLights`, `VObjects.cpp:1559`):
  1. the ball: at the luminous centroid from the CPU model, `radius = 0.6 R(t)` (an extended
     source: `att.w` → `SphereLightCos` in `lights.glsl`), colour = Planck(T_mean(t)),
     intensity ∝ R² × Planck luminance, normalised so that at the surface of the ball the
     irradiance is ~10 sun at peak, falling with T; `sortRange` large so it takes one of the 4
     RT-shadowed slots (`RTLighting::SHADOWED_LIGHTS`, `rtlight_trace.comp:203-260`): the wreck
     pieces cast shadows in its light;
  2. the flash/ground light for the first 0.4 s at the impact point (12-20 sun at 50 m, decay
     10/s), colour white-yellow; this is what lights the sea and the ground under the ball
     before it has grown.
- The soot volume is lit by them per froxel with occlusion (existing).
- Water: add a local-light specular/glitter term to `tile_main.glsl` for point lights within
  range (a Blinn lobe on the wave normal, the same roughness as the sun's): the fireball and
  burning wreck reflect on the sea at night and dusk. Cheap (lights already looped there).
- SSR/RT reflections do not see particles or the fireball (drawn after); the water-specular
  term stands in for it. A mirror image of the ball in calm water is out of scope.
- Night metering: `meterFlames` excludes plume flames from the histogram
  (`VulkanClient.cpp:384`); add the fireball's screen coverage × its mean brightness, so a
  night crash does not black out the scene around it (Q5).

### 2.5 Soot and the froxel volume

Keep the soot in the volume but shape it:
- cap: emitted from the ball's upper hemisphere from 0.4 t_f on, with the ball's rise +
  2-3 m/s, long-lived (20-40 s), growth 0.3 R; the dark lit grey of real smoke (albedo
  0.08-0.12; sun-lit top, dark underside come from the volume's shading);
- stem: from the base (the pool/wreck fire) at 3-6 m/s for the pool-fire time, narrower
  (0.3 R), joining the cap: the mushroom silhouette of footage;
- the volume gets the ball's extinction (2.3) so the stem/cap are consistent in front/behind.

### 2.6 LOD, several fireballs, distance, warp

- Steps by screen radius: ≥ 400 px: 96 (half-res); 150-400: 64; 40-150: 40; < 40 px: no
  march, a single emissive sprite (FireballTexture 4) + the light; < 6 px: the light only.
- Grid update only for marched fireballs; the model is analytic so a fireball that comes
  into view late is correct at its age.
- At most 4 marched (sorted by screen radius); others sprites. Merging is as now (pieces of
  one break-up join the first piece's, `VObjects.cpp:1087-1111`), plus: two fireballs whose
  spheres overlap by more than half merge (sum M).
- Time warp: at > 10× the grid step is skipped and the fireball is drawn at its analytic
  age with frozen noise (no sim dt accumulation); at > 100× (nothing to see anyway) the
  sprite path.
- Memory: ≤ 4 × 2 MB, allocated on the first fireball, freed 120 s after the last (as the
  volume does, `Particles.cpp:1369-1398`).
- Budget (1440p, FSR 1.0, RT on, RX 9070 XT): one 70 m fireball at 170 m (~35 % of the
  frame width, half-res 0.2 Mpx × 64-96 steps × ~4 taps) ≈ 0.4-0.8 ms; + grid 0.1 ms; the
  whole explosion (fireball + soot volume + sprites) ≤ 2.5 ms at the peak frame and ≤ 1.0 ms
  from 1 km. Numbers to be measured in phase 0/2 with `ORBITER_VK_PROFILE` marks
  ("fb grid", "fb sun", "fb march", "fb comp").

### 2.7 Vacuum

No fireball: nothing holds burning propellant together. Keep `VacuumFlash` (`VObjects.cpp:
1410-1448`: an expanding shell of flame sprites + a faint vapour shell, 0.25-0.6 s) and its
10/s light decay, but render the flash itself through the marcher as a thin hot shell field
(T 2500 K, thickness 0.1 R, expanding at `vs = clamp(3R, 20, 300)` m/s, gone in 0.4 s): one
bright white frame, then the pale shell. Low priority (phase 4).

---

## 3. Shockwave: what is physically appropriate

A crash fireball is a **fuel-air deflagration** of a poorly mixed spray, not a detonation:
flame speeds of tens of m/s, overpressure well under 1 kPa beyond a few R. So:

| Effect | Verdict | Design |
|---|---|---|
| Visible pressure ring / Mach stem | **No** | none by default. Keep a `blast` parameter (0 = deflagration) so a future true detonation (none in Orbiter today) could add a ring. |
| Condensation (Wilson) cloud | **No** for a deflagration | not now; the same `blast` hook. |
| Bright flash | **Yes**, < 0.1 s | the ignition flash: light 2 (§2.4) + a 2-frame white-yellow sprite flash at the impact point (`FireballTexture 0`, alpha 1, 0.6 R); the marcher's first frames at T 2200 K do the rest. |
| Heat shimmer | **Yes**, strong | add the fireball to the plume shimmer pass (`shimmer.frag`, `PlumeRenderer::Shimmer`, `Plumes.cpp:535-567`): a heat term over the ball + a plume above it (the rising hot air column, 1.5 R tall, 60 s), strength scaled by T and R; the ring of displaced hot air at the front edge gives a brief (0.3 s) expanding refraction ring without inventing a shock. |
| Water surface | **Yes** | `WaterFx::Impulse(body, gpos, R0 = 0.6 R, strength ∝ M_p^(1/3))` for a ring at the fireball's foot + a spray/mist skirt (WaterSpray-style burst, white, low, outward at 10-20 m/s over 0.5 s) pushed by the expanding ball; the sea under the ball boils white (steam puffs for t_f). |
| Ground | **Yes** | a dust skirt: a ring of dust puffs thrown outward at ground level (as the blow puffs, `VObjects.cpp:1249-1262`, but in a ring at R(t) for 0.5 s, planar, surface colour), the scorch (exists, `:1135-1138`). |
| Camera shake | **Yes**, near only | a scene-level jolt API (`Scene::Jolt(pos, strength)`) feeding the existing cockpit shake (`Scene.cpp:1191-1211`) AND external cameras: amplitude ∝ M_p^(1/3)/d², zero beyond ~10 R, delayed by d/340 s (the thud arrives after the flash: a cheap and convincing cue), 0.4 s duration. Default on, scaled by the CameraShake setting. |
| Sound | out of scope | note: the same delay would drive an explosion sound if audio is added. |

---

## 4. Debris: trails, the burning fuselage, inherited velocities

### 4.1 Trails from flying pieces

- Burning pieces (`info.burn > 0`): black smoke (albedo 0.08) *over water too*; steam only
  from the splash and a 1 s white quench burst when a burning piece goes under (the water
  state's submersion from `oapiGetDebrisWaterState`). Fire: tongues 1.5-2× the piece's size
  streaming aft (the trail direction = -airVel, not -up: `e.dir`, `VObjects.cpp:1162`),
  lifetime by `FLAME_LENGTH` (as now) but 3× the rate and alpha, the ramp texture; a hot
  glow on the hull (exists).
- Hot non-burning pieces (`temp > 700 K`, burn = 0): a thin grey smoke/vapour trail at a
  third of the rate, 4 s, fading as the piece cools (temp cools with a 30 s constant,
  `Debris.cpp:128-135`).
- Beads: keep the distance-based emission (`:1181-1183`) and the band class; add a floor on
  puff overlap (spacing ≤ 0.4 × puff size at any speed, puffs at `D` per 0.5 s) and verify on
  the midair scene (`Script/VkTest/midair.lua`, 150 m/s) and a 300 m/s re-entry break-up
  (new). The audit's "thinned into fewer, larger" degradation must not apply to trails of
  < 2 pieces (priority).
- Inherited velocities are right (`p0/p1`, `vel`). Add the piece's spin to the emission
  point (the flame leaves the hot end) only if cheap: `info.hitpos`-style local offset of the
  fracture face transformed by `grot`.

### 4.2 The main fuselage burning: a crash-site fire

The piece-bound fire works on land (`land_s09_wreck.png`). At sea the core sinks within
seconds (§1.5), so the fire that remains must be the **burning fuel on the surface**, which
is also what happens in reality (fuel floats and burns). Design: a scene-level list of
crash-site fires, `Scene::CrashFire { pos (surface), up, M_p, t0, onWater, groundVel }`,
registered by the fireball owner at the break-up and advanced by the scene (not by a
VDebris, which may sink, be hidden by replay, or go away):

- flames: low, wide tongues (sprites, `FireballTexture 4`, ramp, 0.6-1 s, 2-5 m) over an
  area ~0.5 R, strongest for t_w then dying to embers over 0.5 t_w; the site drifts with the
  water (the current) or stays on land; on land it is the pieces' positions that burn
  (keep the piece path, but with t_w instead of `info.burn`).
- smoke: the stem of §2.5 (black, volumetric, rising at 3-6 m/s), for t_w + a 60 s smoulder
  tail (as the column's 25 s decay now, `:1209`).
- light: the burning-wreck light (`:1580-1584`) moved here, flickering.
- t_w = clamp(2 + 0.4·M^(1/3), 2, 15) s: 7 t → 9.7 s of flames, then smoulder. (Q3: the
  core's `Ignite` burn time is 4 + 0.02 f = 43 s for this core; the client should not show
  43 s of flames if "a few seconds" is wanted. Recommended: keep the core's timer (it drives
  temperature and the hull glow) and let the visible flames follow t_w; alternatively change
  `Vessel.cpp:5535` to the same formula.)
- Afloat pieces (light ones that do float, or if Q4 says wrecks keep some buoyancy): the
  piece path with black smoke, as on land.
- The sunken piece's effects stop: gate `burning`/`atRest` emission on the water state's
  depth (surface position from `WATERSTATE`): nothing emitted below the surface.

---

## 5. "Impressive but brief": timeline and scaling

Let M be the propellant released [kg], M_p = f_p·M the participating mass with
f_p = 0.35 on land/in air, 0.25 on water (fuel into the sea), 1.0 for a vapour burst (hook).

| Quantity | Formula | 1 kg | 100 kg | 1 t | 7 t (DG) | 100 t |
|---|---|---|---|---|---|---|
| R (land) | 2.9·M_p^(1/3) m | 2.0 | 9.5 | 20 | 39 | 95 |
| R (water) | f_p 0.25 | 1.8 | 8.5 | 18 | 35 | 85 |
| t_f, flames | clamp(0.45·M_p^0.2, 0.4, 6) s | 0.4 | 0.9 | 1.5 | 2.1 | 3.7 |
| peak brightness | 0.25 t_f | 0.1 | 0.2 | 0.4 | 0.5 | 0.9 |
| lift-off begins | 0.35 t_f | | | | 0.75 | 1.3 |
| rise speed | 0.5·sqrt(g R) m/s | 2.2 | 4.8 | 7 | 9.7 | 15 |
| soot visible | 8 + 3·M_p^(1/3) s (cap, rising) | 10 | 18 | 29 | 48 | 110 |
| t_w, site fire | clamp(2 + 0.4·M^(1/3), 2, 15) s | 2.4 | 3.9 | 6 | 9.7 | 15 |

Today: R = 2.9·M^(1/3) (55 m), t = clamp(0.9·M^0.26, 0.8, 6) (6 s), flames to 1.2 t, soot to
1.5 t. The 6 s clamp and the 0.26 exponent go; t_f replaces fbDur everywhere the stage
fractions are used, and the stage fractions change:

Timeline for the 7 t sea crash (t from ignition):
- **0-0.1 s** ignition flash: white-yellow, light 2 at 15 sun, the flash sprite; camera jolt
  queued for d/340 s later.
- **0.1-0.5 s** growth to 0.8 R, elongated along the path (the fuel travels with the wreck;
  ball centre velocity = the merged pieces' mass-weighted velocity, braked at 1 /s, not 3);
  T_core 2200 K: the centre clips to white in daylight, orange rim; the sea under it boils
  white; a spray skirt and the WaterFx ring go out.
- **0.5-0.9 s** full size; peak luminosity; soot starts on the top and the downwind rim;
  the ball detaches from the wreck; the shimmer column forms.
- **0.9-1.5 s** lift-off (the toroidal roll), T_core 1600 K: orange, dark soot lanes, sunlit
  grey-black billows on top; the base still bright; the pieces skip on ahead trailing black
  smoke and fire.
- **1.5-2.1 s** burn-out: the fire seen through gaps in the soot; T 1200 K dark red; the cap
  rises at 10 m/s.
- **2.1-2.8 s** (to 1.3 t_f) the last flames at the base become the site fire; the ball is a
  black mushroom cap with a stem from the burning slick.
- **3-10 s** the site fire burns (t_w 9.7 s): low flames, a black stem joining the cap; the
  cap at 60-100 m, spreading; the pieces sink (quench bursts).
- **10-60 s** smoulder: the stem thins to grey, the cap drifts with the wind.
Total "fire on screen": ~3 s of fireball + ~10 s of surface fire; smoke for a minute.

Why these laws: R keeps the hydrocarbon fireball constant 5.8 M^(1/3) (Roberts) applied to the
participating mass; t_f ∝ M^0.2 is between the BLEVE M^(1/3) and footage (a few seconds for
tonnes, half a second for kilograms) and is the user's "1-3 s burn-out" at this mass; soot
time grows faster than fire time (the cap rises for as long as it is buoyant, ~R/w × a few).

---

## 6. Phased implementation

Each phase is committable on its own and verified with the harness (§7). Order by value.
Effort is for one agent-day of focused work.

### Phase 0: harness, baseline, references (0.5 d)
- Scenes: sea (exists), land (`fx_land_fuel.scn`, fix the observer rig for the ground
  height: `CAM_Y` from the terrain elevation), night (`fx_night_fuel.scn`), midair break-up
  (`midair.scn/.lua`, exists), vacuum (an ISS/DG collision in orbit: `iss_hit.lua` exists),
  large vessel (`crash_Atlantis.scn` exists; ~100 t of propellant), small (a DG at PRPLEVEL
  0.02: ~300 kg). All with `stills.sh`-style timelines (t = 0.05, 0.1, 0.3, 0.5, 0.8, 1.2,
  2, 3, 5, 8, 12, 20 s after the break-up: read the break-up time from the log and
  re-base the stills on it so the sheets line up).
- Perf capture per scene (`ORBITER_VK_PROFILE=1 ORBITER_VK_FRAMETIMES`) at 1080p and 1440p
  with a `budget.py`-style checker ("Crash" flight; marks "pt *", "fb *").
- Files: `out/fx-explosion/run.sh`, `timeline.json`, scratch scenarios in the build tree.
- Verify: baseline sheets + a perf table in this folder.

### Phase 1: timing, scale, motion, site fire (on the existing sprites) (1.5 d)
The "brief" and the debris requests without waiting for the volume; also the A/B baseline
for the marcher.
- `VObjects.cpp` `Effects/Fireball`: M_p and f_p, R and t_f of §5, growth to R in 0.25 t_f,
  the flash on the first step (white: untinted K0, alpha 1, a dedicated flash sprite),
  flames gone by 1.3 t_f, soot cap from 0.4 t_f (cap/stem split), the ball's velocity from
  the merged pieces (mass-weighted; `formed` list gains velocity), brake 1 /s, elongation
  along v, lift from 0.35 t_f; the light curve of §2.4 (two lights, extended radius).
- `Scene` `CrashFire` list (§4.2) + VDebris registration; the sunken-piece gate; black trail
  smoke over water; quench burst; hot-piece thin trail; stronger piece flames.
- Keep `ORBITER_VK_FIREBALL_LOG` and add the model's R/t_f/stage to it.
- Verify: sea/land/night/midair sheets, timing table against §5 (the stage times read off
  the sheets within ±0.2 s), the pieces leave trails on the midair sheet, the site fire on
  the sea sheet at t = 5 and 8 s, light on the sea at night. Perf unchanged or lower.
- Risk: the sprite look does not improve beyond timing (expected: it is the stop-gap).

### Phase 2: FireballRenderer v1 (3-4 d)
- `Fireball.h/.cpp`: state per fireball, grid images, pipelines, the CPU model (centroid,
  intensity), registration from VDebris (replacing the fire sprite rings when the renderer is
  on: `ORBITER_VK_FIREBALL=sprites|volume` for A/B, default volume; the graphics settings get
  a "Fireball: sprites / volumetric" entry later).
- `fireball_grid.comp`, `fireball_sun.comp`, `fireball.vert/.frag`, `fireball_comp.frag`
  (half-res composite as `plume_comp.frag`), Planck LUT.
- Scene order: after `DrawVolume`, before `DrawParticles`; `volInteg` access; the virtual
  extinction particles; the reactive mask.
- Verify: A/B sheets sprites vs volume at the 12 times; close-up (observer at 60 m) and far
  (1 km) sheets; night; the ball sits on the sea (no cut-through); no TAA/FSR smear (a
  2-frame diff at the rim); perf table vs the budget (§2.6).
- Risks: the depth-aware half-res composite at the water line (the plumes' weight
  `exp(-rel²·2500)` is tuned for plume depths; may need a wider tolerance at 170 m);
  sprite/volume ordering artefacts (spray drawn over the ball) — mitigated by the virtual
  extinction particles, judged on the sea sheet; the noise look (balls again if the detail
  erosion is too weak; "cauliflower" if too strong) — budget a tuning day.

### Phase 3: lighting integration (2 d)
- Sun self-shadow sweep + multiple-scattering floor; soot albedo ramp; the two lights with
  `radius` and RT-shadow priority; water local-light specular (`tile_main.glsl`); night
  metering exclusion; the soot volume shaped (cap/stem) and lit by the ball.
- Verify: sun-side vs shade-side sheets (observer rotated 180° about the ball: `CAM_PHI`),
  night sheet (glitter path, the wreck lit and shadowed), the cap's sunlit top.
- Risk: 4 shadowed lights are shared with vessels' flames; a fireball next to a launching
  rocket loses its shadow (acceptable).

### Phase 4: flash, shimmer, water/ground skirts, jolt, vacuum shell (1.5 d)
- The flash sprite + light 2; shimmer term; `WaterFx::Impulse` + spray skirt + boiling
  steam; ground dust skirt; `Scene::Jolt` with sound delay (external cameras too);
  VacuumFlash through the marcher.
- Verify: 0.05/0.1 s stills (flash), 0.3-0.8 s (skirts/ring), a 200 m and a 1 km observer
  for the jolt (visible at 200 m, not at 1 km), vacuum sheet (ISS hit).
- Risk: shimmer disabled with MSAA (`Plumes.cpp:416`): same for the fireball; document.

### Phase 5: debris trail and wreck-fire polish (1.5 d)
- Flame tongues aft, trail spacing floor, hot-piece trails, quench, embers/smoulder, the
  hull glow tied to the flames; the land case's pieces burning for t_w.
- Verify: midair sheet at 150 m/s and a 300 m/s break-up (no beads at either), land sheet
  t = 5-20 s, sea sheet 3-10 s.

### Phase 6: LOD, several fireballs, warp, pause, cuts, settings, budget (1.5 d)
- Step/sprite LOD by screen size, 4-fireball cap + overlap merging, warp/pause behaviour
  (frozen, no accumulation: B9), camera cuts (history reset is handled by 6b4f4353; the grid
  is camera-independent), density setting, memory release, the graphics-settings entry,
  `budget.py` "Crash" flight with the budget of §2.6.
- Verify: Atlantis crash (100 t: R 95 m, 4 s), a DG at 300 kg, two DGs (midair: two
  fireballs), 10× warp sheet, a cut mid-explosion (the `cut_check.png` pattern), perf table.

### Phase 7 (optional, after user review): generator upgrades
- A mini fluid sim (64³, semi-Lagrangian + Jacobi) behind the same grid interface; propellant
  type per vessel class (hydrogen: pale, near-sootless, short; hypergolic: a red-brown NTO
  cloud) via a `Config/Damage/<class>.cfg` key; an audio hook.

Total for phases 0-6: ~12 agent-days; phases 1 and 2 give most of the value.

Files touched overall: `OVP/VulkanClient/VObjects.cpp/.h` (VDebris), new `Fireball.*` +
5 shaders, `Scene.cpp/.h` (order, CrashFire, Jolt, lights), `Particles.h/.cpp` (volInteg
accessor, virtual particles), `Plumes.cpp` + `shimmer.frag` (heat term), `WaterFx` (ring
call), `Shaders/tile_main.glsl` (local specular), `VulkanClient.cpp` (metering), `GfxSettings`
(entry), `Tests/Bench/budget.py` (Crash flight), optional `Src/Orbiter/Vessel.cpp:5535` (Q3).

---

## 7. Test and verification harness

### 7.1 Scenes (build tree `Scenarios/VkTest`, `Script/VkTest`; gitignored)
| Scene | Purpose | Exists |
|---|---|---|
| `water_entry_fuel.scn` (sea, day) | the reference case | yes |
| `fx_land_fuel.scn` | land: dust skirt, scorch, pieces burning at rest | scratch (this review) |
| `fx_night_fuel.scn` | night: exposure, lights, glitter | scratch (this review) |
| `midair.scn` | midair break-up: trails at 150 m/s, two fireballs | yes |
| fast break-up (new) | 300 m/s dive: beads test | to make (water_entry with SPEED=300, ALT=600) |
| `crash_Atlantis.scn` | ~100 t: the scaling laws' top end | yes |
| DG at PRPLEVEL 0.02 (new) | ~300 kg: the small end | to make |
| `iss_hit.lua` / orbit collision | vacuum shell | yes |
| close observer (CAM_X=-60) and far (CAM_X=-1000) variants of the sea case | LOD and the composite at the water line | env vars only |

### 7.2 Procedure
- One session at a time, offscreen, `ORBCTL_MEMMAX=6G`, `free -g` ≥ 6 GB; frames under
  `out/fx-explosion/<phase>/<scene>/`, sheets `<scene>_sheet.png`; A/B pairs side by side
  (`ffmpeg hstack`) for the review notes; one full 1080p video of the sea case per phase.
- Stage timing read from the sheets: ignition frame from `ORBITER_VK_FIREBALL_LOG` (the
  log gains "stage" lines: flash, full size, lift-off, burn-out, site fire end).
- Perf: `ORBITER_VK_PROFILE=1 ORBITER_VK_FRAMETIMES=<ft>` at 1080p and 1440p; `budget.py`-
  style summary of "pt *" + "fb *" over the 0-12 s window: median, p95, peak frame.
- Determinism: `--fixedstep` lockstep (orbctl) already; the fireball model uses sim time
  only, so repeats match to the pixel except the jittered march (compare against a
  re-seeded run as the testing memory says).
- Validation layer once per phase on the sea case (`ORBITER_VK_VALIDATION=1`).

### 7.3 Current cost (1080p, this review)

From `review/base_1080p.ft` (summary `review/perf_base_1080p.txt`; RX 9070 XT, radv, RT on,
1920×1080, the sea case; ignition at sim t ≈ 1.87 s). GPU ms, per 0.5 s window: frame median;
particle passes at their maximum in the window.

| t [s] | frame | particle passes (sum) | pt sprites | pt volume | pt march | rt build | ssr |
|---|---|---|---|---|---|---|---|
| 1.0 (before) | 8.6 | 0.02 | 0.00 | 0.00 | 0.00 | 0.57 | 1.99 |
| 2.0 (ignition) | 18.5 | 0.64 | 0.27 | 0.00 | 0.00 | 1.24 | 3.99 |
| 2.5 | 18.4 | 1.68 | 0.39 | 0.37 | 0.10 | 2.81 | 3.15 |
| 3.0-4.0 (full ball) | 16-17 | 1.5-1.8 | 0.4-0.56 | 0.5-0.56 | 0.14-0.24 | 1.0-1.2 | 2.9-3.2 |
| 5.0-7.0 (orange, soot) | 13-15 | 2.0-2.3 | 0.84-1.08 | 0.53-0.55 | 0.32-0.40 | 0.25-0.29 | 2.8-3.2 |
| 8.0-12.0 (soot only) | 10-11 | 2.2-2.6 | 0.75-1.25 | 0.55-0.58 | 0.49-0.62 | 0.18-0.21 | 1.9 |

Peak particle frame t = 8.87 s: 2.55 ms (sprites 1.20, volume 0.53, march half 0.48,
update 0.12, light 0.07, sort 0.06, shadow 0.05). The sprite cost grows *after* the fire is
over (the soot's sprite share + spray), so the volumetric fireball replaces the cheapest part.

The frame's +9 ms at the crash is mostly not particles: `rtl trace` 0.78 → 2.16 (the
fireball's local light traced against the debris), `ssr` 2.0 → 3.0, `rt build` 0.57 → 1.55
(nine debris BLAS rebuilt), `prepass` 0.68 → 1.45, `sky` 0.33 → 0.62. The fireball renderer's
budget (§2.6) sits on top of these; the RT-shadowed light is already paid for.

---

## 8. Decisions needed from the user

1. **Size.** Participating fraction f_p = 0.35 land / 0.25 water (7 t → R 39 / 35 m, D 70-80
   m, four DG lengths) instead of today's full-mass 55 m (D 111 m)? Or keep it big?
2. **Fire duration.** t_f = 0.45·M_p^0.2: ~2.1 s of flames for 7 t (bright 0.5 s, lift-off
   0.75 s), soot rising for ~50 s. Is "brief" this brief?
3. **Site/wreck fire length.** t_w ≈ 10 s of visible flames for 7 t, then smoulder. Keep the
   core's 43 s `burn` timer for physics (hull temperature/glow) and only shorten the visible
   flames (recommended), or change `Vessel.cpp:5535` to the same formula?
4. **Wrecks afloat.** At sea the core sinks within seconds (structural density). Should wrecks
   keep some buoyancy (trapped air, a 10-30 s flooding time) so a burning fuselage is seen
   afloat, or is the burning fuel slick enough? (Core physics change, W6 decision.)
5. **Night exposure.** Exclude the fireball from auto-exposure metering (the scene around it
   stays visible; the fire clips to white), or let it blind the camera as a real one would?
6. **Camera shake** for external observers, with the sound delay: on by default?
7. **Generator.** Analytic advected noise first (recommended, phase 2), a fluid sim later
   (phase 7) if the roll is not convincing? Or go straight to a sim?
8. **Budget.** ≤ 1 ms for the fireball close up at 1440p, ≤ 2.5 ms for the whole explosion's
   peak frame: acceptable?
9. **Propellant look.** Hydrocarbon (orange, sooty) for everything by default; a per-class
   propellant type (hydrogen pale/clean, hypergolic red-brown) later?

### 8.1 User decisions (2026-11-10)

1. Size: participating fraction (f_p 0.35 land / 0.25 water; 7 t → R 39 / 35 m).
2. Duration: ~2 s of flames for 7 t (t_f = 0.45·M_p^0.2), soot rising ~50 s.
3. Wreck fire: keep the core's `burn` timer for physics; shorten only the visible flames
   (t_w ≈ 10 s for 7 t, then smoulder).
4. At sea: the burning fuel slick is enough; wrecks keep flooding and sinking (no core change).
5. Night exposure: PARTLY - the fireball counts a little towards metering (a mild darkening,
   not blinding, not excluded).
6. Camera jolt: on by default (a setting to turn it off), delayed by d/340.
7. Generator: procedural advected noise first; fluid sim stays optional (phase 7).
8. Budget: LOOSER - up to ~2 ms for a close fireball and ~5 ms for the whole explosion's
   peak at 1440p (quality first; it is a brief event).
9. Propellant: hydrocarbon look for all now; propellant types later.
Proceed: all phases in order, a commit per phase, a clip/sheet sent to the user per phase.
