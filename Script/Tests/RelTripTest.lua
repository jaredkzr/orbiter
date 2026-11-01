-- The trip autopilot of the ideal drive (Scenarios/Tests/RelTripTest.scn):
-- each vessel flies a trip from low Earth orbit; arrival state, top speed,
-- closest approach and arrival time checked.

local failed = false
local function log(s) oapi.write_log("RelTripTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local C = 299792458.0
local G0 = 9.80665
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end

-- fly a trip; returns the final state (after the arrival) and what was seen on the way
local function fly(name, spec, limit)
	local v = vessel.get_interface(name)
	local hv = v:get_handle()
	local ht = oapi.get_objhandle(spec.target)
	spec.target = ht
	oapi.set_focusobject(hv)
	oapi.reltrip_set(hv, spec)
	local plan = oapi.reltrip_info(hv)
	oapi.reltrip_engage(hv, true)
	local t0 = oapi.get_systime()
	local seen = {vmax = 0, dmin = 1e300, departed = nil}
	local ri
	repeat
		proc.wait_frames(1)
		ri = oapi.reltrip_info(hv)
		-- time warp: up while far from events (the autopilot lowers it ahead of them)
		if oapi.get_tacc() < 100 and ri.time > 120 and math.deg(ri.atterr) < 5 then oapi.set_tacc(100) end
		local rs = oapi.get_relstate(hv)
		seen.vmax = math.max(seen.vmax, rs.speed)
		seen.dmin = math.min(seen.dmin, len(sub(v:get_globalpos(), oapi.get_globalpos(ht))))
		if not seen.departed and ri.phase ~= "Aligning" then seen.departed = oapi.get_simmjd() end
	until ri.phase == "Arrived" or ri.phase == "No trip" or oapi.get_systime() - t0 > limit
	local arrived = oapi.get_simmjd()
	oapi.set_tacc(1)
	proc.wait_frames(5)
	local R = oapi.get_size(ht)
	local el = v:get_elements()
	local st = {
		phase = ri.phase, message = ri.message, ref = v:get_gravityref(), R = R, plan = plan,
		alt = len(sub(v:get_globalpos(), oapi.get_globalpos(ht))) - R,
		vrel = len(sub(v:get_globalvel(), oapi.get_globalvel(ht))),
		pe = el.a * (1 - el.e) - R, ap = el.a * (1 + el.e) - R,
		arrived = arrived, departed = seen.departed, vmax = seen.vmax, dmin = seen.dmin}
	-- (done with: removed, so it doesn't fall or orbit into the later trips' time warp)
	local nextFocus
	for _, n in ipairs({"T1", "T2", "T3", "T4", "T5"}) do
		local h = oapi.get_objhandle(n)
		if h and n ~= name and not nextFocus then nextFocus = h end
	end
	if nextFocus then oapi.set_focusobject(nextFocus) end
	oapi.del_vessel(hv)
	proc.wait_frames(2)
	log(string.format("%s to %s (%s, %.4g g): %s; %.1f km above, %.1f m/s, orbit %.1f x %.1f km, top %.4f c, closest %.1f km, %.0f s wall",
		name, oapi.get_objname(ht), spec.arrival, spec.accel / G0, st.phase, st.alt / 1000, st.vrel, st.pe / 1000, st.ap / 1000,
		st.vmax / C, (st.dmin - R) / 1000, oapi.get_systime() - t0))
	return st
end

local function main()
	proc.wait_frames(2)
	local mars, moon = oapi.get_objhandle("Mars"), oapi.get_objhandle("Moon")

	local s = fly("T1", {target = "Mars", arrival = "orbit", alt = 500e3, accel = 1000 * G0}, 60)
	check("mars1000", s.phase == "Arrived" and s.ref == mars and s.pe > 450e3 and s.ap < 550e3,
		string.format("Mars orbit %.1f x %.1f km at 1000 g", s.pe / 1000, s.ap / 1000))

	s = fly("T2", {target = "Moon", arrival = "rest", alt = 1000e3, accel = 10 * G0}, 60)
	check("moonrest", s.phase == "Arrived" and math.abs(s.alt - 1000e3) < 5e3 and s.vrel < 5,
		string.format("at rest above the Moon: %.1f km, %.1f m/s", s.alt / 1000, s.vrel))

	-- (another altitude: not into the first vessel's orbit)
	s = fly("T3", {target = "Mars", arrival = "orbit", alt = 700e3, accel = 1e5 * G0}, 60)
	check("mars1e5", s.phase == "Arrived" and s.ref == mars and s.pe > 600e3 and s.ap < 800e3 and s.vmax > 0.8 * C,
		string.format("Mars orbit %.1f x %.1f km at 100,000 g, top %.3f c", s.pe / 1000, s.ap / 1000, s.vmax / C))

	s = fly("T4", {target = "Mars", arrival = "flyby", alt = 1000e3, accel = 1000 * G0}, 60)
	check("flyby", s.phase == "Arrived" and math.abs(s.dmin - s.R - 1000e3) < 50e3,
		string.format("past Mars %.1f km out", (s.dmin - s.R) / 1000))

	s = fly("T5", {target = "Moon", arrival = "rest", alt = 1000e3, accel = 10 * G0, time = 3 * 3600}, 60)
	local took = (s.arrived - s.departed) * 86400
	-- (the set time counts from the engagement; the departure waits for a clear path)
	check("settime", s.phase == "Arrived" and math.abs(s.alt - 1000e3) < 5e3 and s.vrel < 5 and s.plan.time > 0,
		string.format("Moon in a set time: %.1f km, %.1f m/s, flown in %.0f s", s.alt / 1000, s.vrel, took))
	check("arrivaltime", math.abs((s.arrived - s.plan.arr_mjd) * 86400) < 60,
		string.format("arrived %.1f s from the set time", (s.arrived - s.plan.arr_mjd) * 86400))

	oapi.exit(failed and 1 or 0)
end

main()
