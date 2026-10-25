-- Ground contact tests (Scenarios/Tests/ContactTest.scn): delta-gliders on
-- a runway. REST stays put; DROP hovers up 3 m and cuts its engines; HARD
-- climbs higher and comes down at about 3.5 m/s; BRAKE accelerates to
-- 30 m/s on the runway and brakes. Each is measured over the run and checked
-- against limits; the measurements are written to the log. Run with a fixed
-- time step (--fixedstep) for repeatable results.

local failed = false

local function log(s)
	oapi.write_log("ContactTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

-- position on the surface (planet frame) [m]
local function pos_of(ves)
	local e = ves:get_equpos()
	return {x = e.rad * math.cos(e.lat) * math.cos(e.lng), y = e.rad * math.sin(e.lat), z = e.rad * math.cos(e.lat) * math.sin(e.lng)}
end

local function main()
	local names = {"REST", "DROP", "HARD", "BRAKE"}
	local ves = {}
	for _, n in ipairs(names) do ves[n] = vessel.get_interface(n) end
	proc.wait_frames(2)

	-- ground altitude of a vessel at rest on its gear
	local alt0 = ves.REST:get_altitude(ALTMODE.GROUND)
	log(string.format("resting altitude %.4f m", alt0))

	-- control: climb on the hover engines to a height, then cut them
	-- (DROP: 3 m drop; HARD: hits the ground at about 3.5 m/s);
	-- accelerate on the main engines to 30 m/s, then cut and brake
	local ctl = {
		DROP = { phase = "climb", top = alt0 + 3.0 },
		HARD = { phase = "climb", top = alt0 + 0.65 },
		BRAKE = { phase = "accel" },
	}
	local released = {} -- (time when the vessel was left to itself)
	local brake_p0

	local m = {}
	for _, n in ipairs(names) do
		m[n] = { amin = 1e9, abounce = -1e9, touched = false, vmax_late = 0, wmax_late = 0, vimpact = 0, p0 = pos_of(ves[n]) }
	end

	local t0 = oapi.get_simtime()
	local T = 60.0
	while oapi.get_simtime() - t0 < T do
		proc.wait_frames(1)
		local t = oapi.get_simtime() - t0
		for n, c in pairs(ctl) do
			local v = ves[n]
			local a = v:get_altitude(ALTMODE.GROUND)
			if c.phase == "climb" then
				-- (full hover thrust, until close to the top)
				v:set_thrustergrouplevel(THGROUP.HOVER, 1.0)
				if a >= c.top - 0.15 then
					v:set_thrustergrouplevel(THGROUP.HOVER, 0)
					c.phase = "fall"
					released[n] = t
				end
			elseif c.phase == "accel" then
				v:set_thrustergrouplevel(THGROUP.MAIN, 0.3)
				if v:get_groundspeed() >= 30.0 then
					v:set_thrustergrouplevel(THGROUP.MAIN, 0)
					v:set_wheelbrakelevel(1.0, 0, true)
					c.phase = "brake"
					released[n] = t
					brake_p0 = pos_of(v)
				end
			end
		end
		for _, n in ipairs(names) do
			local r, v = m[n], ves[n]
			local a = v:get_altitude(ALTMODE.GROUND)
			local free = n == "REST" or released[n] ~= nil
			if free then
				r.amin = math.min(r.amin, a)
				local vs = v:get_groundspeedvector(REFFRAME.HORIZON).y
				if not r.touched then r.vimpact = math.min(r.vimpact, vs) end
				if a <= alt0 + 0.05 then r.touched = true
				elseif r.touched then r.abounce = math.max(r.abounce, a) end
			end
			if t > T - 10.0 then
				r.vmax_late = math.max(r.vmax_late, v:get_groundspeed())
				local w = v:get_angvel()
				r.wmax_late = math.max(r.wmax_late, vec.length(w))
			end
		end
	end

	for _, n in ipairs(names) do
		local r, v = m[n], ves[n]
		log(string.format("%s: impact %.3f m/s, alt min %.4f, final %.4f, bounce %.4f, late speed %.5f m/s, late rate %.6f rad/s, pitch %.3f, bank %.3f, landed %s",
			n, r.vimpact, r.amin, v:get_altitude(ALTMODE.GROUND), r.abounce, r.vmax_late, r.wmax_late,
			math.deg(v:get_pitch()), math.deg(v:get_bank()), tostring(v:is_landed() and true or false)))
	end
	local creep = vec.dist(pos_of(ves.REST), m.REST.p0)
	local stop = brake_p0 and vec.dist(pos_of(ves.BRAKE), brake_p0) or -1
	log(string.format("REST creep %.5f m, BRAKE stopping distance %.2f m", creep, stop))

	-- limits
	for _, n in ipairs(names) do
		local r, v = m[n], ves[n]
		check(n, math.abs(v:get_altitude(ALTMODE.GROUND) - alt0) < 0.05, "comes to rest on its gear")
		check(n, r.amin > alt0 - 0.5, "does not sink into the ground")
		check(n, r.vmax_late < 0.05 and r.wmax_late < 0.01, "is at rest in the last 10 s")
	end
	check("REST", creep < 0.05, "does not creep")
	check("DROP", m.DROP.abounce < alt0 + 0.5, "does not bounce high")
	check("HARD", m.HARD.vimpact < -2.5, "hits the ground hard enough to test")
	check("HARD", m.HARD.abounce < alt0 + 0.5, "does not bounce high")
	check("BRAKE", stop > 20 and stop < 400, "stops within a plausible distance")
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
