-- XRSound's API on Orbiter's own sound (Tests/Audio/XRSound.scn, rendered
-- with ORBITER_AUDIO_RENDER): Lua's xrsound on a delta-glider on KSC's
-- runway, heard from its cockpit.
--   1.0 s  a vessel's sound (BothViewClose): on the vessel, through the hull
--   2-5 s  a looped sound in the headset (Radio): paused 3-3.5 s, panned,
--          faster, stopped at 5 s
--   6.0 s  a module's sound (Global)
--   7-9 s  an InternalOnly sound in an outside view: not heard
-- Checks (PASS/FAIL in the log; the exit code) and the render's check.
local failed = false
local function check(name, cond)
	oapi.write_log("AudioXRSound: " .. (cond and "PASS " or "FAIL ") .. name)
	if not cond then failed = true end
end
local v = vessel.get_interface("GL-01")
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
local BEEP, FLOW = "XRSound\\Default\\BeepHigh.wav", "XRSound/Default/Fuel Flow.wav"
oapi.set_cameramode({mode = 'cockpit'})

local xr = xrsound.create_instance(v)
check("an instance for the vessel", xr ~= nil and xr:is_present())
check("version 3.0", math.abs(xr:get_version() - 3.0) < 1e-6)
check("a file loaded (backslashes, any case)", xr:load_wav(20000, BEEP, PlaybackType.BothViewClose))
check("no such file: not loaded", not xr:load_wav(20001, "XRSound/no such file.wav", PlaybackType.Radio))
check("not loaded: not played", not xr:play_wav(20001))

at(1.0)
check("a vessel's sound plays", xr:play_wav(20000, false, 0.6) and xr:is_wavplaying(20000))
check("not panned: on the vessel", not xr:set_pan(20000, 0.5))

at(2.0)
check("a headset sound loaded", xr:load_wav(20002, FLOW, PlaybackType.Radio))
check("looped", xr:play_wav(20002, true, 0.7) and xr:is_wavplaying(20002))
at(3.0)
check("paused", xr:set_paused(20002, true) and xr:is_paused(20002) and not xr:is_wavplaying(20002))
at(3.5)
check("going again", xr:set_paused(20002, false) and xr:is_wavplaying(20002))
check("panned", xr:set_pan(20002, 0.5) and math.abs(xr:get_pan(20002) - 0.5) < 1e-6)
check("faster", xr:set_playbackspeed(20002, 1.5) and math.abs(xr:get_playbackspeed(20002) - 1.5) < 1e-6)
check("its position", xr:set_playposition(20002, 100) and xr:get_playposition(20002) >= 0)
at(5.0)
check("stopped", xr:stop_wav(20002) and not xr:is_wavplaying(20002))

-- default sounds: switched, their groups' folders
check("a default sound off", xr:set_defaultsoundenabled(DefaultSoundID.MainEngines, false) and not xr:get_defaultsoundenabled(DefaultSoundID.MainEngines))
check("and on", xr:set_defaultsoundenabled(DefaultSoundID.MainEngines, true) and xr:get_defaultsoundenabled(DefaultSoundID.MainEngines))
check("a group's folder", xr:set_defaultsoundgroupfolder(DefaultSoundID.AltitudeCalloutsGroup, "XRSound/Default")
	and xr:get_defaultsoundgroupfolder(DefaultSoundID.AltitudeCalloutsGroup) == "XRSound/Default")
check("a folder for a sound, not a group: refused", not xr:set_defaultsoundgroupfolder(DefaultSoundID.MainEngines, "XRSound"))

at(6.0)
local m = xrsound.create_instance("AudioXRSoundTest")
check("a module's instance", m ~= nil and m:is_present())
check("a module's sound plays", m:load_wav(1, BEEP, PlaybackType.Global) and m:play_wav(1, false, 0.6))

at(7.0)
oapi.set_cameramode({mode = 'track', trackmode = 'locked', reldist = 2.2, phi = 0.4, tht = 0.15})
check("an InternalOnly sound plays (unheard outside)", xr:load_wav(20003, FLOW, PlaybackType.InternalOnly) and xr:play_wav(20003, true))
at(9.0)
xr:stop_wav(20003)
at(10.0)
oapi.exit(failed and 1 or 0)
