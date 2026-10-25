-- Manoeuvre node tests (Scenarios/Tests/ManeuverTest.scn): a prograde node
-- of 100 m/s in a 300 km orbit. The predicted apoapsis after the node must
-- agree with the vis-viva equation (to the effect of Earth's oblateness and
-- the Moon); the burn autopilot then flies the node, and the trajectory
-- predicted afterwards must have the same apoapsis.

local failed = false

local function log(s)
	oapi.write_log("ManeuverTest: " .. s)
end

local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

-- first event of a type after node idx (in the latest prediction)
local function find_event(hv, typ, node)
	local ev = oapi.get_trajectoryevents(hv)
	if not ev then return nil end
	for _, e in ipairs(ev) do
		if e.type == typ and e.node == node then return e end
	end
	return nil
end

local function wait_event(hv, typ, node, timeout)
	local t0 = oapi.get_systime()
	repeat
		local e = find_event(hv, typ, node)
		if e then return e end
		proc.wait_frames(1)
	until oapi.get_systime() - t0 > timeout
	return nil
end

local function main()
	local v = vessel.get_interface("GL-M")
	local hv = v:get_handle()
	local earth = oapi.get_objhandle("Earth")
	local R = oapi.get_size(earth)
	local mu = 6.67259e-11 * oapi.get_mass(earth)
	proc.wait_frames(2)

	-- node 200 s ahead
	local tnode = oapi.get_simmjd() + 200 / 86400
	v:add_maneuvernode({mjd = tnode, dv = {x = 100, y = 0, z = 0}})
	check("node", v:get_maneuvernodecount() == 1, "added")

	local ap = wait_event(hv, "apoapsis", 0, 20)
	check("prediction", ap ~= nil, "apoapsis after the node predicted")
	if not ap then oapi.exit(1) return end

	-- vis-viva: from a circular orbit of radius r, prograde by dv
	local r = 6671000
	local vc = math.sqrt(mu / r)
	local a = 1 / (2 / r - (vc + 100)^2 / mu)
	local ap_vv = 2 * a - r - R
	log(string.format("predicted apoapsis %.1f km, vis-viva %.1f km", ap.value / 1000, ap_vv / 1000))
	check("visviva", math.abs(ap.value - ap_vv) < 15000, string.format("difference %.2f km", (ap.value - ap_vv) / 1000))
	local ap_before = ap

	-- fly it with the burn autopilot
	local n = v:get_maneuvernode(0)
	n.autoburn = true
	v:set_maneuvernode(0, n)
	local t0 = oapi.get_simtime()
	while v:get_maneuvernodecount() > 0 and oapi.get_simtime() - t0 < 400 do proc.wait_simdt(1) end
	check("burn", v:get_maneuvernodecount() == 0, string.format("completed after %.0f s", oapi.get_simtime() - t0))

	-- the prediction after the burn (no nodes left)
	proc.wait_simdt(2)
	local ap_after = wait_event(hv, "apoapsis", -1, 20)
	check("prediction2", ap_after ~= nil, "apoapsis after the burn predicted")
	if ap_after then
		local dalt = ap_after.value - ap_before.value
		local dt = (ap_after.mjd - ap_before.mjd) * 86400
		log(string.format("apoapsis after the burn %.2f km (%.2f km, %.0f s from the prediction)", ap_after.value / 1000, dalt / 1000, dt))
		check("execution", math.abs(dalt) < 3000 and math.abs(dt) < 60, string.format("apoapsis %+.2f km, %+.0f s", dalt / 1000, dt))

		-- fly to the predicted apoapsis: the simulation must get there
		oapi.set_tacc(10)
		while oapi.get_simmjd() < ap_after.mjd - 60 / 86400 do proc.wait_simdt(10) end
		oapi.set_tacc(1)
		local amax, tmax = 0, 0
		while oapi.get_simmjd() < ap_after.mjd + 60 / 86400 do
			local alt = v:get_altitude()
			if alt > amax then amax, tmax = alt, oapi.get_simmjd() end
			proc.wait_simdt(0.5)
		end
		log(string.format("flown apoapsis %.2f km at %+.0f s (predicted %.2f km)", amax / 1000, (tmax - ap_after.mjd) * 86400, ap_after.value / 1000))
		check("flown", math.abs(amax - ap_after.value) < 1000 and math.abs(tmax - ap_after.mjd) * 86400 < 20,
			string.format("apoapsis %+.3f km, %+.0f s from the prediction", (amax - ap_after.value) / 1000, (tmax - ap_after.mjd) * 86400))
	end
	oapi.exit(failed and 1 or 0)
end

main()
