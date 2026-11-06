# Orbiter controls for playtesting

Key names are SDL's, as `orbctl.py key/hold` take them. Default keymap:
`Src/Orbiter/Keymap.cpp`; the in-game sheet: `alt+F1`.

## Contents
- Views and interface
- Engines and attitude
- Autopilots (nav modes)
- Delta-glider specific
- Flying common tasks
- Reading the instruments

## Views and interface

| Key | Does |
| --- | --- |
| `F1` | cockpit / external view |
| `F2`, `shift+F2` | external camera mode: Auto, Free, Orbital, Chase, Locked, Flyby (and back) |
| `F8` | cockpit mode: glass cockpit, 2D panel, virtual cockpit |
| `Home` | cockpit: look forward; external: camera behind the vessel (Flyby: ahead again) |
| `ctrl+Left/Right/Up/Down` | external: orbit the camera (cockpit 2D panel: switch panels) |
| `PageDown` / `PageUp` | external: camera closer / farther |
| `alt+Left/Right/Up/Down` | cockpit: look around |
| `Z` / `X` | zoom in / out (field of view) |
| `ctrl+F1` | Camera dialog |
| `F3` / `ctrl+F3` | switch vessel / previous vessel |
| `F4` | main menu; `F6` Settings; `F5` replay; `M` map view; `ctrl+M` map dialog |
| `Escape` | close the focused dialog, else the flight menu (pause, settings, quit) |
| `ctrl+P` | pause; `T` / `R` time acceleration up / down (x10 steps) |
| `ctrl+S` | quicksave |
| `ctrl+H` | HUD on/off; `H` HUD mode (orbit, surface, docking); `ctrl+R` HUD reference |
| `alt+F1` | key help sheet |

Mouse: right-drag turns the camera (external: about the target);
wheel: camera distance (external) or zoom (cockpit); `ctrl`+wheel: field
of view in external views. In `orbctl`: `drag A B` is a left-button drag;
for a right-drag use `down right`, `move`..., `up right` via `run`.

## Engines and attitude

| Key | Does |
| --- | --- |
| `ctrl+Keypad +` / `ctrl+Keypad -` | main throttle up / down, about 20 %/s while held; it stays where you leave it |
| `Keypad +` (held) | full main thrust only while held: back to the throttle setting on release |
| `Keypad -` (held) | full retro thrust while held |
| `Keypad *` | kill main and retro thrust |
| `Keypad 0` / `Keypad .` | hover thrust up / down (hold) |
| `Keypad /` | RCS mode: rotation / translation; `ctrl+Keypad /` RCS on/off |
| translation mode: `Keypad 2` / `8` | move up / down |
| translation mode: `Keypad 1` / `3` | move left / right |
| translation mode: `Keypad 6` / `9` | move forward / back |
| `Keypad 2` / `Keypad 8` | pitch up / down (rotation mode); also the elevators in the air |
| `Keypad 1` / `Keypad 3` | yaw left / right |
| `Keypad 4` / `Keypad 6` | bank left / right (also the ailerons) |
| `Keypad 5` | kill rotation (autopilot) |
| `Delete` / `Insert` | elevator trim up / down |
| `,` / `.` | left / right wheel brake |

Hold attitude keys for a number of frames (`hold "Keypad 2" 15` = half a
second of pitch-up input). In the atmosphere the control surfaces act
only once there is airspeed.

## Autopilots (nav modes)

| Key | Mode |
| --- | --- |
| `L` | level with the horizon (wings level, nose on the horizon) |
| `A` | hold altitude (hover engines) |
| `[` / `]` | prograde / retrograde |
| `;` / `'` | orbit normal / antinormal |
| `Keypad 5` | kill rotation |

`state` lists the active ones under `vessel.navmodes`. Press the key again
to turn a mode off.

## Delta-glider specific

| Key | Does |
| --- | --- |
| `G` | landing gear up / down |
| `ctrl+B` | airbrake |
| `K` | nose cone (docking port cover) open / close |
| `O` / `ctrl+O` | outer / inner airlock |
| `U` | top hatch |
| `D` | radiator |
| `alt+Keypad +` / `alt+Keypad -` | scramjet thrust up / down |

The glass cockpit's MFDs: bottom buttons `@mfd0.btn12` PWR, `btn13` SEL
(mode list), `btn14` MNU; side buttons `btn0`-`btn11` (left column 0-5,
right 6-11). Right MFD: `mfd1.*`. `eval "oapi.get_mfdmode (0)"` reads the
mode (1 Orbit, 2 Surface, 3 Map, ...).

## Flying common tasks

**Take off (delta-glider on a runway):** full main thrust
(`ctrl+Keypad +` held until `vessel.main` is 1, or hold `Keypad +`),
wait until airspeed passes about 100 m/s (`state`), pitch up gently
(`hold "Keypad 2" 10`, look, repeat) to about 10-15 degrees, gear up
(`G`) once climbing. Check pitch and vertical speed every second or two.

**Climb and level off:** pitch for a steady positive `vspeed`; near the
target altitude, ease the pitch down and reduce thrust; `L` levels the
wings and the nose.

**Hover and land vertically (glider or a lander, airless bodies):**
hover thrust with `Keypad 0` / `Keypad .` held a few frames at a time,
watching `vspeed`: descend at a few m/s, slower close to the ground; `A`
holds the altitude. Gear down (`G`) before touchdown. Touchdown is
`vessel.landed == true`.

**Runway landing:** line up, descend at 3-5 degrees, speed around
100-150 m/s near the ground, flare a little before touchdown, kill thrust,
wheel brakes (`,` and `.` held).

**To orbit (delta-glider from a runway):** climb steeply (30+ degrees)
through the atmosphere on full main thrust, ease towards the horizon
above about 30 km, keep thrust until the apoapsis (Orbit MFD) reaches the
target, coast to it (time acceleration `T`), burn prograde (`[` then full
thrust) to raise the periapsis. Takes many minutes of game time: use time
acceleration while coasting, and expect to need patience; say how far you
got.

**Docking:** the Docking MFD and HUD docking mode (`H`) guide the
approach; translation RCS (`Keypad /` to switch; keys above) moves the
vessel without turning it. Close in under 0.5 m/s at the port (faster, or
misaligned, is refused with a notification: "Docking refused: closing at
... (limit 0.50)"). `ctrl+D` undocks.

## Reading the instruments

- `state` gives the numbers a player reads off the HUD and MFDs; compare
  them with what the screenshot shows (a disagreement is a finding).
- HUD modes (`H`): surface (altitude, airspeed, pitch ladder), orbit
  (prograde marker), docking.
- Units: altitude m, speeds m/s, angles degrees, propellant kg; `simt`
  seconds since the scenario started.
