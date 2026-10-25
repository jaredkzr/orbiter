-- Collisions between vessels (Scenarios/Tests/VesselCollisionTest.scn).
-- TAXI (delta-glider) taxis at about 5 m/s along the runway into PARKED
-- (Shuttle-A, landed) for 25 s, then brakes: it must be stopped by it, not
-- pass through, and PARKED is pushed along. In orbit PB1 is given 1 m/s
-- towards PB2, 12 m ahead of it: they must meet and not pass through each
-- other, and their relative speed must not grow.

local failed = false

local function log(s)
	oapi.write_log("VesselCollisionTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

local function gpos(v) return oapi.get_globalpos(v:get_handle()) end
local function gvel(v) return oapi.get_globalvel(v:get_handle()) end

local function main()
	local taxi, parked = vessel.get_interface("TAXI"), vessel.get_interface("PARKED")
	local pb1, pb2 = vessel.get_interface("PB1"), vessel.get_interface("PB2")
	proc.wait_frames(2)

	-- PB1 towards PB2 at 1 m/s
	do
		local s = pb1:get_status(2)
		local d = vec.unit(vec.sub(gpos(pb2), gpos(pb1)))
		s.rvel = vec.add(s.rvel, d)
		pb1:defset_status(s)
	end

	local d0 = vec.dist(gpos(taxi), gpos(parked))
	local o0 = vec.dist(gpos(pb1), gpos(pb2))
	log(string.format("start: TAXI-PARKED %.2f m, PB1-PB2 %.2f m", d0, o0))
	local dmin, omin = 1e9, 1e9
	local parked_woke = false
	local relv0 = nil -- (relative speed of the PBs before they meet)
	local t0 = oapi.get_simtime()
	local T = 40.0
	while oapi.get_simtime() - t0 < T do
		proc.wait_frames(1)
		local t = oapi.get_simtime() - t0
		if not relv0 and t > 0.5 then relv0 = vec.length(vec.sub(gvel(pb2), gvel(pb1))) end
		if t < 25.0 then -- (throttle to keep about 5 m/s)
			local gs = taxi:get_groundspeed()
			taxi:set_thrustergrouplevel(THGROUP.MAIN, math.max(0, math.min(0.4, 0.05 + (5.0 - gs) * 0.1)))
		else
			taxi:set_thrustergrouplevel(THGROUP.MAIN, 0)
			taxi:set_wheelbrakelevel(1.0, 0, true)
		end
		dmin = math.min(dmin, vec.dist(gpos(taxi), gpos(parked)))
		omin = math.min(omin, vec.dist(gpos(pb1), gpos(pb2)))
		if not parked:is_landed() then parked_woke = true end
	end
	local o1 = vec.dist(gpos(pb1), gpos(pb2))
	local rel = vec.sub(gvel(pb2), gvel(pb1))
	local sep = vec.dotp(rel, vec.unit(vec.sub(gpos(pb2), gpos(pb1))))
	log(string.format("TAXI-PARKED: min %.2f m, final %.2f m; TAXI speed %.3f m/s, PARKED speed %.3f m/s, PARKED woke %s",
		dmin, vec.dist(gpos(taxi), gpos(parked)), taxi:get_groundspeed(), parked:get_groundspeed(), tostring(parked_woke)))
	log(string.format("PB1-PB2: min %.2f m, final %.2f m, separating at %.3f m/s, relative speed %.3f -> %.3f m/s", omin, o1, sep, relv0, vec.length(rel)))

	check("TAXI", dmin > 15.0, "does not pass through PARKED")
	check("PARKED", parked_woke, "is pushed (no longer landed)")
	check("TAXI", dmin < d0 - 30.0, "reached PARKED")
	check("PB", omin > 1.5, "do not pass through each other")
	check("PB", omin < o0 - 5.0, "met")
	check("PB", sep > -0.05, "are not approaching after the collision")
	check("PB", vec.length(rel) < relv0 + 0.01, "gain no speed from the collision")
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
