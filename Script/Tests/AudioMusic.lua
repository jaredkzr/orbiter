-- The music follows the vessel in focus (Tests/Audio/Music.scn, run with
-- ORBITER_DAMAGE=1 and ORBITER_AUDIO_MUSIC=1): on the ground; its breakup
-- (dropped from 50 m at 80 m/s: silence); in orbit; closing in on another
-- (1 km behind it, 100 m lower: catching up); far out (80 Earth radii);
-- falling into the air (50 km up at 3 km/s). Each mood holds 4 s before
-- the music follows.
local fails = 0
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
local function expect(t, mood)
	at(t)
	local m = audio.music()
	if m == mood then
		oapi.write_log(string.format("AudioMusic: PASS %s at %.1f s", m, oapi.get_simtime()))
	else
		oapi.write_log(string.format("AudioMusic: FAIL %s at %.1f s (expected %s)", m, oapi.get_simtime(), mood))
		fails = fails + 1
	end
end
local function focus(name) oapi.set_focusobject(oapi.get_objhandle(name)) end

expect(6, "ground")
local v = vessel.get_interface("GROUND")
local earth = oapi.get_objhandle("Earth")
local up = vec.unit(vec.sub(oapi.get_globalpos(v:get_handle()), oapi.get_globalpos(earth)))
local st = v:get_status(2)
st.status = 0
st.rpos = vec.add(st.rpos, vec.mul(up, 50))
st.rvel = vec.add(st.rvel, vec.mul(up, -80))
v:defset_status(st)
expect(9, "silent")
focus("ORBIT")   expect(15, "orbit")
focus("CHASER")  expect(21, "approach")
focus("DEEP")    expect(27, "deep")
focus("REENTRY") expect(33, "reentry")
oapi.write_log(fails == 0 and "AudioMusic: all PASS" or string.format("AudioMusic: %d FAIL", fails))
oapi.exit(0)
