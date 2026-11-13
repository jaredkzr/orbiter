-- A cockpit's clicks (Tests/Audio/Clicks.scn, rendered with
-- ORBITER_AUDIO_RENDER): in orbit (nothing heard outside), in the cockpit -
-- a switch at 1 s, a button at 2 s (in the cabin, at a switch's loudness);
-- the interface's at 3 s; paused, a switch at 3.5 s of real time (on the
-- interface's bus). (A script's clicks: audio.click; MFD buttons need a
-- graphics client's pane.)
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
at(1) audio.click(1)
at(2) audio.click(2)
at(3) audio.click(0)
oapi.write_log("AudioClicks: done")
at(4)
oapi.exit(0)
