-- Transfer planning (Scenarios/Tests/TransferPlanTest.scn): searches from a
-- 300 km Earth orbit to the Moon and to Mars, and their manoeuvre nodes
-- (ejection refined with the numerical prediction to the target periapsis).

local failed = false
local function log(s) oapi.write_log("TransferPlanTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

-- wait for the planner's computation to finish
local function wait_done(hv, serial, timeout)
	local t0 = oapi.get_systime()
	repeat
		proc.wait_frames(1)
		local ti = oapi.transfer_info(hv)
		if ti and ti.job == 0 and ti.serial > serial then return ti end
	until oapi.get_systime() - t0 > timeout
	return oapi.transfer_info(hv)
end

local function date(mjd)
	local d = oapi.formatdate and oapi.formatdate(mjd) or string.format("MJD %.2f", mjd)
	return d
end

local function main()
	local v = vessel.get_interface("GL-P")
	local hv = v:get_handle()
	local moon, mars = oapi.get_objhandle("Moon"), oapi.get_objhandle("Mars")
	local now = oapi.get_simmjd()

	-- the Moon: 100 km circular orbit
	local t0 = oapi.get_systime()
	check("moon_search_start", oapi.transfer_search(hv, moon, 0, 100e3, 0), "search started")
	local ti = wait_done(hv, 0, 40)
	log(string.format("Moon: departure in %.2f h, %.2f days, ejection %.0f m/s, capture %.0f m/s (%.1f s)",
		(ti.dep - now) * 24, ti.tof, ti.dv_eject, ti.dv_capture, oapi.get_systime() - t0))
	check("moon_found", ti.valid and ti.dep > now and ti.dep < now + 30, ti.message)
	check("moon_dv", ti.dv_eject > 3000 and ti.dv_eject < 3300 and ti.dv_capture > 600 and ti.dv_capture < 1100,
		string.format("ejection %.0f, capture %.0f m/s", ti.dv_eject, ti.dv_capture))
	check("moon_tof", ti.tof > 2 and ti.tof < 7, string.format("%.2f days", ti.tof))

	t0 = oapi.get_systime()
	check("moon_nodes_start", oapi.transfer_nodes(hv, false), "targeting started")
	local s = ti.serial
	ti = wait_done(hv, s, 60)
	log(string.format("Moon targeted: %s (%.1f s)", ti.message, oapi.get_systime() - t0))
	check("moon_plan", ti.plan_valid and math.abs(ti.peri_alt - 100e3) < 5e3,
		string.format("periapsis %.1f km (target 100 km)", ti.peri_alt / 1000))
	check("moon_nodes", v:get_maneuvernodecount() == 2, string.format("%d nodes (ejection, capture)", v:get_maneuvernodecount()))
	check("moon_incl", math.abs(ti.incl - ti.incl_min) < 0.5 * math.pi / 180,
		string.format("arrival inclination %.2f deg (least possible %.2f deg)", ti.incl * 180 / math.pi, ti.incl_min * 180 / math.pi))

	-- Mars: the 2001 window, 300 km circular orbit
	t0 = oapi.get_systime()
	s = ti.serial
	check("mars_search_start", oapi.transfer_search(hv, mars, 0, 300e3, 0), "search started")
	ti = wait_done(hv, s, 60)
	log(string.format("Mars: departure MJD %.1f, %.1f days, C3 %.2f km2/s2, ejection %.0f m/s, capture %.0f m/s, vinf %.0f m/s (%.1f s)",
		ti.dep, ti.tof, ti.c3 * 1e-6, ti.dv_eject, ti.dv_capture, ti.vinf_arr, oapi.get_systime() - t0))
	-- (Mars Odyssey: launch 7 April 2001 = MJD 52006, C3 10.8 km2/s2, arrival 24 October 2001 after 200 days)
	check("mars_window", ti.valid and ti.dep > 51990 and ti.dep < 52040, string.format("departure MJD %.1f", ti.dep))
	check("mars_c3", ti.c3 > 7e6 and ti.c3 < 14e6, string.format("C3 %.2f km2/s2", ti.c3 * 1e-6))
	check("mars_tof", ti.tof > 150 and ti.tof < 320, string.format("%.0f days", ti.tof))

	t0 = oapi.get_systime()
	s = ti.serial
	check("mars_nodes_start", oapi.transfer_nodes(hv, false), "targeting started")
	ti = wait_done(hv, s, 120)
	log(string.format("Mars targeted: %s (%.1f s)", ti.message, oapi.get_systime() - t0))
	check("mars_plan", ti.plan_valid and math.abs(ti.peri_alt - 300e3) < 15e3,
		string.format("periapsis %.1f km (target 300 km)", ti.peri_alt / 1000))
	check("mars_incl", math.abs(ti.incl - ti.incl_min) < 0.5 * math.pi / 180,
		string.format("arrival inclination %.2f deg (least possible %.2f deg)", ti.incl * 180 / math.pi, ti.incl_min * 180 / math.pi))
	check("mars_nodes", v:get_maneuvernodecount() == 2, string.format("%d nodes (ejection, capture)", v:get_maneuvernodecount()))
	oapi.exit(failed and 1 or 0)
end

main()
