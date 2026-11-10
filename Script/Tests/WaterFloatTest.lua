-- Floating (Scenarios/Tests/WaterFloatTest.scn): vessels let go just above
-- a calm sea come to float upright at rest. Run with ORBITER_SEA_WIND=0.

local failed = false
local function log(s) oapi.write_log("WaterFloatTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg) else log("FAIL " .. name .. " " .. msg); failed = true end
end

local earth
-- the CG's height above the sea's mean surface [m]
local function height(v)
	local e = oapi.global_to_equ(earth, v:get_globalpos())
	return e.rad - oapi.get_size(earth)
end

-- let a vessel go at height h above the water, level, still
local function release(v, h)
	local st = v:get_status(2)
	local g = v:get_globalpos()
	local up = vec.unit(vec.sub(g, oapi.get_globalpos(earth)))
	st.status = 0
	st.rpos = vec.add(st.rpos, vec.mul(up, h - height(v)))
	st.vrot = {x = 0, y = 0, z = 0}
	v:defset_status(st)
end

local function main()
	proc.wait_frames(2)
	earth = oapi.get_objhandle("Earth")
	local sp = oapi.get_seaparams(earth)
	check("sea", sp ~= nil and sp.hs < 0.01, sp and string.format("calm (Hs %.3f m)", sp.hs) or "no sea")
	local names = { "DG", "DGFULL", "PB" }
	local ves, m = {}, {}
	for _, n in ipairs(names) do
		ves[n] = vessel.get_interface(n)
		local e = ves[n]:get_equpos()
		check(n, oapi.is_water(earth, e.lng, e.lat), "is over the water")
		release(ves[n], 4.0)
		m[n] = { low = 1e9, late = 0, wlate = 0 }
	end
	proc.wait_frames(2)
	local t0 = oapi.get_simtime()
	local T = 90.0
	while oapi.get_simtime() - t0 < T do
		proc.wait_frames(1)
		local t = oapi.get_simtime() - t0
		for _, n in ipairs(names) do
			local v, r = ves[n], m[n]
			local hgt = height(v)
			r.low = math.min(r.low, hgt)
			if t > T - 15 then
				local gv = v:get_groundspeedvector(REFFRAME.HORIZON)
				r.late = math.max(r.late, math.sqrt(gv.x * gv.x + gv.z * gv.z))
				r.heave = math.max(r.heave or 0, math.abs(gv.y))
				r.wlate = math.max(r.wlate, vec.length(v:get_angvel()))
			end
		end
	end
	for _, n in ipairs(names) do
		local v, r = ves[n], m[n]
		local ws = nil
		log(string.format("%s: mass %.0f kg, height %.3f m (lowest %.3f), pitch %.2f bank %.2f deg, late drift %.4f m/s, heave %.4f m/s, rate %.5f rad/s%s",
			n, v:get_mass(), height(v), r.low, math.deg(v:get_pitch()), math.deg(v:get_bank()), r.late, r.heave, r.wlate,
			ws and string.format(", submerged %.2f of %.2f m3", ws.submerged, ws.volume) or ""))
		check(n, height(v) > -v:get_size() and height(v) < v:get_size(), "floats at the surface")
		check(n, r.low > -10.0, "does not sink")
		check(n, math.abs(v:get_bank()) < math.rad(5) and math.abs(v:get_pitch()) < math.rad(15), "floats upright")
		-- (it may still coast slowly from its fall: little drag at a few cm/s, as a boat)
		check(n, r.heave < 0.01 and r.wlate < 0.01, "has stopped bobbing in the last 15 s")
		check(n, r.late < 0.2, "drifts no faster than a few cm/s")
	end
end

local ok, err = xpcall(main, debug.traceback)
if not ok then log("ERROR " .. tostring(err)); failed = true end
oapi.exit(failed and 1 or 0)
