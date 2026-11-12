-- The sound API (Tests/Audio/Api.scn, rendered with ORBITER_AUDIO_RENDER):
-- a delta-glider on KSC's runway, heard from its cockpit.
--   0.5 s  a sound file on the vessel (Sound/Tests/beep.wav), once
--   2.0 s  the same on the interface
--   3-5 s  looped, at twice the pitch from 4 s, stopped at 5 s
--   6 s    the nose cone opens (20 s): its actuator (AUDIO_ANIM_DOOR, set by
--          the DG's module), its latch at the end (26 s)
-- Checks (in the log, PASS/FAIL; the exit code) and the render's check.
local failed = false
local function check(name, cond)
	oapi.write_log("AudioApi: " .. (cond and "PASS " or "FAIL ") .. name)
	if not cond then failed = true end
end
local v = vessel.get_interface("GL-01")
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
oapi.set_cameramode({mode = 'cockpit'})

at(0.5)
local h1 = audio.play(v, "Tests/beep.wav", {pos = {x = 0, y = 0, z = 6}, level = 0.3})
check("a sound on the vessel plays", h1 > 0 and audio.playing(h1))
check("no such file: not played", audio.play(v, "Tests/no such file.wav") == 0)
at(1.5)
check("a sound not looped is gone when played", not audio.playing(h1))
at(2.0)
local h2 = audio.play(nil, "Tests/beep.wav", {level = 0.3})
check("a sound on the interface plays", h2 > 0)
at(3.0)
local h3 = audio.play(v, "Tests/beep.wav", {loop = true, level = 0.3})
at(3.9)
check("a looped sound plays on", audio.playing(h3))
check("its pitch set", audio.set(h3, {pitch = 2.0}))
at(5.0)
check("stopped", audio.stop(h3) and not audio.playing(h3))
check("tune: a key", audio.tune(v, "engine_level", 1.0))
check("tune: no such key", not audio.tune(v, "no_such_key", 1.0))
at(6.0)
oapi.simulatebufferedkey(OAPI_KEY.K) -- (the nose cone)
at(29.0)
oapi.exit(failed and 1 or 0)
