# Playtest: play Orbiter from the command line

`orbctl.py` starts Orbiter with its remote control
(`OVP/VulkanClient/RemoteControl.h`) and sends it a player's input: keys
and mouse go into the window's event queue, the same path as real input.
Screenshots show what a player sees; the flight's state can be read as
JSON. Made for playtesting, by a person or by an agent.

```bash
U=Utils/Playtest/orbctl.py
$U start "Delta-glider/Brighton Beach"     # or a .scn file; no scenario: the launchpad
$U wait 60                                 # let it load (frames)
$U shot                                    # -> out/playtest/default/shots/0001.png
$U state                                   # altitude, speeds, attitude, fuel, camera...
$U key F2                                  # a key; ctrl+F1, shift+F2, alt+F1 too
$U hold "Keypad 0" 90                      # hover engines up for 90 frames
$U widgets                                 # the buttons, tabs, fields on screen
$U click "@ui:Camera/Free"                 # a widget, by its window and label
$U click 640,360 right                     # or a pixel (the screenshot's)
$U eval "vessel.get_focusinterface ():get_altitude ()"
$U log                                     # Orbiter.log since the last "log"
$U stop
```

`orbctl.py help` lists all commands; `--session NAME` runs several at once.

## Time

**Lockstep** (default): the simulation runs only while a command runs: a
key press takes two frames, `wait 90` ninety, a query (`state`, `eval`,
`widgets`) none. Each frame is a fixed step (`--step`, default 1/30 s), so
however long the player takes to decide, nothing happens meanwhile, and
a run is repeatable. **Realtime** (`start --realtime`, or `pace realtime`
later): Orbiter runs on as usual and commands act as they come.

## Finding things to click

- `widgets [filter]`: every visible ImGui widget with a label (menu bar,
  dialogs, launchpad, MFD windows), its centre, size and state
  (checked/unchecked, input, open/closed). Click with `@ui:LABEL`,
  `@ui:WINDOW/LABEL` or `@ui:LABEL[2]` (the second from the top).
- `anchors [prefix]`: named points published by instruments and views
  (`mfd0.btn13` = the left MFD's SEL button in the glass cockpit,
  `map.node0.prograde`, `settings.graphics`...).
- Pixels: positions in the screenshot are window pixels.

## Sessions

Each session runs in `out/playtest/NAME`: links to the build tree, but its
own `Orbiter.cfg` (the build's, in a window of `--size`, default
1920x1080; `--damage on|off`), `Orbiter.log`, saved scenarios and
`shots/`. Your own configuration and "Current state" are left alone.
`--window` shows the window (to watch); otherwise it renders offscreen.

Tested by `Playtest.Remote` (`Tests/Playtest/remote_test.py`).
