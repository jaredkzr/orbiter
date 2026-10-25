-- Instant replay (Scenarios/Tests/ReplayTest.scn): a delta-glider fires its
-- main engine; the replay must show the recorded state of an earlier moment,
-- and leaving it must restore the live state exactly.

local failed = false
local function log(s) oapi.write_log("ReplayTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end
local function dist(a, b) return math.sqrt((a.x-b.x)^2 + (a.y-b.y)^2 + (a.z-b.z)^2) end

local function main()
	local v = vessel.get_interface("GL-R")
	proc.wait_frames(2)
	v:set_thrustergrouplevel(THGROUP.MAIN, 1)
	proc.wait_simdt(3)
	-- (in a script step the vessel states are those at the end of the step)
	local p3, mjd3 = v:get_globalpos(), oapi.get_simmjd() + oapi.get_simstep() / 86400
	proc.wait_simdt(3)
	v:set_thrustergrouplevel(THGROUP.MAIN, 0)
	proc.wait_simdt(2)
	local plive, vlive, mjdlive = v:get_globalpos(), v:get_globalvel(), oapi.get_simmjd()

	-- (scripts do not run while the simulation is held: the checks are made
	-- at once, in the same step)
	check("enter", oapi.set_replay(true), "replay started")
	local r = oapi.get_replay()
	check("span", r and (r.mjd1 - r.mjd0) * 86400 > 7.5, r and string.format("%.2f s recorded", (r.mjd1 - r.mjd0) * 86400) or "none")

	oapi.replay_seek(mjd3)
	local p = v:get_globalpos()
	local d = dist(p, p3)
	check("position", d < 0.01, string.format("%.6f m from the recorded position", d))
	check("time", math.abs(oapi.get_simmjd() - mjd3) * 86400 < 1e-3, string.format("shown at %+.4f s", (oapi.get_simmjd() - mjd3) * 86400))
	check("thrust", v:get_thrustergrouplevel(THGROUP.MAIN) > 0.99, string.format("main engine at %.2f", v:get_thrustergrouplevel(THGROUP.MAIN)))

	oapi.set_replay(false)
	local pl, vl = v:get_globalpos(), v:get_globalvel()
	check("restored", dist(pl, plive) < 1e-6 and dist(vl, vlive) < 1e-9 and math.abs(oapi.get_simmjd() - mjdlive) * 86400 < 1e-6,
		string.format("live state: %.3g m, %.3g m/s, %.3g s", dist(pl, plive), dist(vl, vlive), (oapi.get_simmjd() - mjdlive) * 86400))
	check("thrust_live", v:get_thrustergrouplevel(THGROUP.MAIN) < 0.01, "main engine off again")
	proc.wait_simdt(1)
	check("running", (oapi.get_simmjd() - mjdlive) * 86400 > 0.9, "the simulation continues")
	oapi.exit(failed and 1 or 0)
end

main()
