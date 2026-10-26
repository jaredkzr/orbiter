-- Collisions with base structures (Scenarios/Tests/BaseCollisionTest.scn).
-- TAXI (delta-glider) taxis north at about 5 m/s for 30 s towards a tank
-- of Edwards (centre 302 m north and 691 m east of the base's origin, 25 m
-- wide, 40 m high), then brakes. Without collisions it would pass through
-- it; it must stop at the tank.

local failed = false

local function log(s)
	oapi.write_log("BaseCollisionTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

local R = 6371010.0
local blat = 34.919998 + math.deg(302.0 / R)
local blng = -117.889999 + math.deg(691.0 / (R * math.cos(math.rad(34.919998))))

-- position of TAXI north of the block's centre [m]
local function north(v)
	local e = v:get_equpos()
	return (math.deg(e.lat) - blat) * math.pi / 180.0 * R
end

local function main()
	local taxi = vessel.get_interface("TAXI")
	proc.wait_frames(2)
	local n0 = north(taxi)
	log(string.format("start: %.2f m south of the tank's centre", -n0))
	local nmax = -1e9
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 40.0 do
		proc.wait_frames(1)
		local t = oapi.get_simtime() - t0
		if t < 30.0 then -- (throttle to keep about 5 m/s)
			local gs = taxi:get_groundspeed()
			taxi:set_thrustergrouplevel(THGROUP.MAIN, math.max(0, math.min(0.4, 0.05 + (5.0 - gs) * 0.1)))
		else
			taxi:set_thrustergrouplevel(THGROUP.MAIN, 0)
			taxi:set_wheelbrakelevel(1.0, 0, true)
		end
		nmax = math.max(nmax, north(taxi))
	end
	local speed = taxi:get_groundspeed()
	log(string.format("closest: %.2f m south of the centre; final speed %.3f m/s", -nmax, speed))
	-- (the tank's wall is 12.5 m from its centre, the glider's nose about
	-- 9 m ahead of its centre of gravity)
	check("stopped", nmax < -19.0, string.format("(stays south of the tank: %.2f m < -19 m)", nmax))
	check("reached", nmax > -30.0, string.format("(drove up to it: %.2f m > -30 m)", nmax))
	check("at rest", speed < 0.5, string.format("(speed %.3f m/s)", speed))
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
