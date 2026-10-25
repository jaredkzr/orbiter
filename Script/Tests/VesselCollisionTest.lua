-- Collisions between vessels (Scenarios/Tests/VesselCollisionTest.scn).
-- TAXI (delta-glider) taxis at about 5 m/s along the runway into PARKED
-- (Shuttle-A, landed) for 25 s, then brakes: it must be stopped by it, not
-- pass through, and PARKED is pushed along. In orbit PB1 is given 1 m/s
-- towards PB2, 12 m ahead of it: they must meet and not pass through each
-- other, and their relative speed must not grow. PB4's collision hulls
-- are removed (PB3 must pass through it); PB6 is given a 16 m box instead
-- of its hulls (PB5 must meet it further out).

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

	local pb3, pb4 = vessel.get_interface("PB3"), vessel.get_interface("PB4")
	local pb5, pb6 = vessel.get_interface("PB5"), vessel.get_interface("PB6")
	pb4:clear_collisionhulls()
	local box = {}
	for _, x in ipairs({-8, 8}) do for _, y in ipairs({-8, 8}) do for _, z in ipairs({-8, 8}) do
		box[#box+1] = {x = x, y = y, z = z}
	end end end
	pb6:add_collisionhull(box)
	log(string.format("hulls: PB4 %d, PB6 %d", pb4:get_collisionhullcount(), pb6:get_collisionhullcount()))

	-- PB1 towards PB2 (and PB3 to PB4, PB5 to PB6) at 1 m/s
	for _, pr in ipairs({{pb1, pb2}, {pb3, pb4}, {pb5, pb6}}) do
		local s = pr[1]:get_status(2)
		local d = vec.unit(vec.sub(gpos(pr[2]), gpos(pr[1])))
		s.rvel = vec.add(s.rvel, d)
		pr[1]:defset_status(s)
	end
	local min34, min56 = 1e9, 1e9

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
		min34 = math.min(min34, vec.dist(gpos(pb3), gpos(pb4)))
		min56 = math.min(min56, vec.dist(gpos(pb5), gpos(pb6)))
		if not parked:is_landed() then parked_woke = true end
	end
	local o1 = vec.dist(gpos(pb1), gpos(pb2))
	local rel = vec.sub(gvel(pb2), gvel(pb1))
	local sep = vec.dotp(rel, vec.unit(vec.sub(gpos(pb2), gpos(pb1))))
	log(string.format("TAXI-PARKED: min %.2f m, final %.2f m; TAXI speed %.3f m/s, PARKED speed %.3f m/s, PARKED woke %s",
		dmin, vec.dist(gpos(taxi), gpos(parked)), taxi:get_groundspeed(), parked:get_groundspeed(), tostring(parked_woke)))
	log(string.format("PB1-PB2: min %.2f m, final %.2f m, separating at %.3f m/s, relative speed %.3f -> %.3f m/s", omin, o1, sep, relv0, vec.length(rel)))

	log(string.format("PB3-PB4: min %.2f m; PB5-PB6: min %.2f m", min34, min56))
	check("PB4", pb4:get_collisionhullcount() == 0 and min34 < 0.5, "without hulls is passed through")
	check("PB6", pb6:get_collisionhullcount() == 1 and min56 > 8.0, "with a 16 m box is met further out")
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
