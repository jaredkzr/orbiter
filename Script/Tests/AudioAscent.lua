-- The voice's callouts for a rocket's ascent (Tests/Audio/Ascent.scn,
-- ORBITER_AUDIO_CALLOUTS=1): Space Shuttle Atlantis on its pad, the main
-- engines (and the boosters) lit at 2 s, flown straight up.
--   ignition (lit on the ground, pointing up), Mach 1, Max Q
-- Checks the callouts said (audio.callouts), PASS/FAIL in the log; the exit
-- code.
local failed = false
local function check(name, cond)
	oapi.write_log("AudioAscent: " .. (cond and "PASS " or "FAIL ") .. name)
	if not cond then failed = true end
end
local v = vessel.get_focusinterface()
local function said(part)
	for _, f in ipairs(audio.callouts()) do if string.find(f, part, 1, true) then return true end end
	return false
end
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
at(2.0)
v:set_thrustergrouplevel(THGROUP.MAIN, 1.0)
at(4.0)
check("ignition called", said("Ignition"))
local nextLog = 0
while oapi.get_simtime() < 120 and not said("Max Q") do
	if oapi.get_simtime() >= nextLog then
		oapi.write_log(string.format("AudioAscent: t %.0f alt %.0f Mach %.2f q %.0f", oapi.get_simtime(), v:get_altitude(), v:get_machnumber(), v:get_dynpressure()))
		nextLog = oapi.get_simtime() + 10
	end
	proc.skip()
end
check("Mach 1 called", said("Mach 1."))
check("Max Q called", said("Max Q"))
for i, f in ipairs(audio.callouts()) do oapi.write_log("AudioAscent: said " .. f) end
oapi.exit(failed and 1 or 0)
