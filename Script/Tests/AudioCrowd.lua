-- A crowd (Tests/Audio/Crowd.scn): 40 delta-gliders' main engines at a
-- quarter throttle from 1 s to 6 s (held on the runway by their brakes);
-- the audio's load and voices in the log (ORBITER_AUDIO_LOG).
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
at(1)
for i = 0, 39 do
	local v = vessel.get_interface(string.format("DG%02d", i))
	v:set_wheelbrakelevel(1, 0, true)
	v:set_thrustergrouplevel(THGROUP.MAIN, 0.25)
end
at(6)
for i = 0, 39 do vessel.get_interface(string.format("DG%02d", i)):set_thrustergrouplevel(THGROUP.MAIN, 0) end
at(12)
oapi.write_log("AudioCrowd: done")
oapi.exit(0)
