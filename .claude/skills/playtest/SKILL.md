---
name: playtest
description: Play Orbiter in the real game like a player - start a session, look at screenshots, press keys, click buttons by their labels, fly, read the flight state - to playtest a feature, reproduce a reported in-game bug, verify a change works in the actual game, or try a flight task (take off, land, reach orbit, dock) and judge how it plays. Use this whenever the user asks to playtest, try or check something "in the game", see how something feels or looks while flying, reproduce a problem they saw in Orbiter, or confirm a fix beyond the unit and input tests - even if they don't say "playtest".
---

# Playtesting Orbiter

You drive a real Orbiter session through `Utils/Playtest/orbctl.py`, run
from the repository root. Keys and mouse go into the game window's event
queue (the same path as a player's input), screenshots show what a player
sees, and `state` reads the flight as JSON. Details and every command:
`Utils/Playtest/README.md` and `orbctl.py help`. It needs the built tree
(`out/build/linux-x64-headless-release`); don't rebuild while a session
runs (its binary would change under it).

The point of a playtest is the player's view. Prefer doing things the way
a player would (keys, buttons, the mouse) and judging from what is on
screen; `state`, `eval` and `log` are your instruments for measuring and
explaining what you saw, not a substitute for looking.

## Who plays: a Sonnet agent, by default

Playing is long and screenshot-heavy, and a Sonnet player does it about
as well as Opus at roughly half the time and fewer tokens (compared on
the same three playtests: same pass rate, each model caught findings the
other missed). So the main session doesn't play itself; it hands the
playing to Sonnet agents and keeps the judging:

1. **Delegate.** Start one agent per independent playtest with the Agent
   tool and `model: "sonnet"`. Independent requests (different
   scenarios or features) can run in parallel; each gets its own session
   name. Tell the agent: the task as the user put it, that it is the
   player and should read this skill
   (`.claude/skills/playtest/SKILL.md`) and play directly without
   starting agents of its own, its session name, and that it should
   return the full report as its final message (screenshots and the
   contact sheet stay in `out/playtest/<session>/`). Subagents can be
   refused when they write report files, so the main session saves the
   report there itself.
2. **Read the results.** When an agent reports back, read its
   `report.md` and look at the screenshots behind the major findings
   yourself - don't just pass the agent's summary along. Check the
   severity of each finding (Sonnet can file a real bug as a side note,
   or overrate polish), merge duplicates when several agents ran, and
   rerun a step or ask the agent (SendMessage) when a finding is unclear
   or looks like the agent's own mistake (wrong key, its own Lua error
   banner).
3. **Report** to the user: the combined findings by severity, what
   worked, and a contact sheet of the key shots (see below).

Use another model only when the user asks for one ("play this with
Opus") or for a single quick check (one screenshot, one key) where an
agent costs more than it saves. If you are yourself a playtest agent
(your prompt says you're the player), skip this section and play.

## Start a session

```bash
O="Utils/Playtest/orbctl.py --session <name>"   # a name of your own: other agents may be playing too
$O start "<scenario>"                            # a name under Scenarios/, a .scn path, or none (launchpad)
$O wait 90                                       # loading: terrain and textures need frames
```

- `--session <name>`: always give a unique, descriptive one (e.g.
  `pt-brighton-landing`). Sessions with the same name collide, and the
  default one may belong to someone else.
- Keep everything you write for the session - helper scripts, Lua probe
  files, notes - in its folder, `out/playtest/<name>/`, not in a shared
  scratch directory: other agents may be playing at the same time and
  using the same file names, and a shared wrapper script can send your
  commands into their game. The shell variable `O` above is enough of a
  wrapper.
- The window is 1920x1080 by default: the smallest screen the game
  supports, so play at that size (or larger, `--size 2560x1440`). Layout
  problems seen only in smaller windows are not findings.
- `--damage on` for crashes and structural damage; `--window` only if the
  user wants to watch live.
- Scenario names are paths under `Scenarios/` without `.scn`, e.g.
  `"Delta-glider/DG-S ready for takeoff"`, `"Delta-glider/Brighton Beach"`
  (`ls Scenarios/<folder>` to browse). No scenario: the launchpad, which you
  can operate by clicking (`widgets`, then `click "@ui:<scenario>"`,
  `click @ui:Launch`).

## The loop: look, decide, act, let time pass

Time only moves while a command runs (lockstep: a fixed 1/30 s per frame).
`wait 30` is one second of game time; a query (`state`, `eval`, `widgets`)
takes none; a key press takes two frames. So you can think as long as you
like between actions - but nothing happens until you `wait`.

1. **Look:** `$O shot <what>` and Read the PNG; `$O state` for numbers.
2. **Decide** what a player would do next, given what is on screen.
3. **Act:** `key`, `hold`, `click`, `drag`, `wheel`.
4. **Let it play:** `wait N`, then look again. While flying, look often
   (every 2-10 s of game time): things go wrong fast near the ground.

Finding what to click:
- `$O widgets [filter]` lists every visible button, tab, toggle, field and
  list row with its label, window and state (checked/unchecked). Click by
  label: `click "@ui:Camera/Free"`, `click "@ui:Launch"`,
  `click "@ui:Delete[2]"` (the second from the top). Quote labels with
  spaces.
- `$O anchors [prefix]` lists named points: glass-cockpit MFD buttons
  `mfd0.btn0`..`btn14` (left/right columns 0-11, bottom PWR/SEL/MNU 12-14),
  MFD display points, map handles, settings pages.
- Anything else (2D panel and virtual cockpit switches, things in the
  scene): pixel positions from the screenshot (`click 640,360`); they are
  the window's pixels.

Keys use SDL names: `F1`..`F12`, letters, `"Keypad 8"`, `"Keypad +"`,
`PageUp`, `Space`, `Escape`; combos `ctrl+F1`, `shift+F2`, `alt+F1`. Hold
a key with `hold <key> <frames>` (thrust, attitude, camera). Orbiter's
keys and the delta-glider's, and how to fly common tasks:
`references/controls.md` - read it before flying.

## What to do for each kind of request

- **Explore** ("playtest X", "how does Y feel"): play the feature the way
  a new player would meet it - find it, use it, use it wrongly, combine it
  with nearby features (camera modes while flying, dialogs while paused,
  time acceleration). Note anything confusing, broken, ugly or slow.
- **Reproduce a bug**: set up the reported situation as closely as
  possible, then vary one thing at a time until you can make it happen on
  demand. A repro is done when you have the shortest command sequence
  that shows it, a screenshot of it, and the state at that moment. If it
  won't reproduce, say what you tried; "not reproduced" is a result.
- **Verify a change**: exercise the changed behaviour directly, then the
  things around it that might have broken. Check the claim in the change
  (e.g. "the horizon stays level") with both a screenshot and numbers.
- **Fly a task** (take off, land, orbit, dock): fly it with player
  controls, and judge the experience as well as the outcome: could you see
  what you needed, did the instruments help, did the camera cooperate.

Instruments for measuring and setting up:
- `$O state`: altitude, speeds, vertical speed, pitch/bank/heading,
  propellant, thrust levels, autopilot (nav) modes, camera mode, sim time.
- `$O eval "<lua>"`: the whole Lua API, e.g.
  `vessel.get_focusinterface ():get_altitude ()`. Using Lua to *control*
  the vessel (set thrust, attitude) is fine for setting up a situation or
  rescuing a run, but say so in the report: it bypasses what a player does.
- `$O log`: Orbiter.log lines since the last `log` - flight events (damage,
  docking refused, destruction), errors, warnings. Check it after anything
  surprising.
- Time acceleration (`T` faster, `R` slower) or a long `wait` to skip
  uneventful stretches (a climb to orbit, a long coast).

## Evidence and the report

Take a screenshot at every finding and name it (`shot gear-clips-runway`).
Keep the `state` JSON that goes with it. When you finish, give the report
in this shape - as your final message if you are a playtest agent (the
main session saves it), otherwise written to the place the user asked
for, else `out/playtest/<session>/report.md`:

```markdown
# Playtest: <what was played> (<scenario>)

**Result:** one or two sentences: did it work, how did it play.

## Findings
1. **<Short title>** - <severity: blocker / major / minor / polish>
   - Seen: <what happened, with numbers from state>
   - Expected: <what a player would expect>
   - Steps: <the orbctl commands, shortest sequence that shows it>
   - Screenshot: <path>
2. ...

## What worked
- <things that behaved well, briefly>

## Session
<session name, scenario, size, damage on/off; anything done through Lua
rather than player input>
```

Order findings by severity. If there are many screenshots, combine them
with `python3 .claude/skills/playtest/scripts/contact_sheet.py out.png
shot1.png "caption" shot2.png "caption" ...` and list the sheet in the
report. The main session sends it to the user (with any key single
shots) - they can't see the screenshots otherwise.

## Pitfalls

- **Stop the session when done:** `$O stop`. A forgotten session keeps
  Orbiter running in the background.
- Right after loading, terrain may still be coarse: `wait` before judging
  visuals, and before blaming the renderer for blur.
- A dialog with a text field can take the keyboard; close it (`Escape` or
  its close button) before flying with keys. `Escape` with nothing to
  close opens the flight menu.
- Flight event banners and notifications fade in real (wall) time; take
  the screenshot right after the `wait` in which the event happened, and
  read `log` for their text.
- A failing command (label not on screen, Lua error) answers with an error
  and exit code 2; the session goes on. Take a screenshot to see why.
- A Lua error from `eval`/`lua` also pops up an error notification in the
  game, which shows in later screenshots and can hold the keyboard until it
  is dismissed. Test probe expressions carefully; mention any such banner
  in the report as your own, not the game's.
- A vessel landed on a base pad (`BASE <name>:<pad>` in the scenario) is
  refuelled automatically, so a pad start ignores the scenario's
  `PRPLEVEL`; to start with partial tanks, place it by `POS` instead.
- If Orbiter exits (crash, `quit`), `out/playtest/<session>/Orbiter.log`
  and `orbiter.out` say why; a crash is itself a finding.
