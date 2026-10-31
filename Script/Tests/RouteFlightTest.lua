-- A planned gravity-assist route flown (Scenarios/Tests/RouteFlightTest.scn):
-- from a 300 km Earth orbit by Venus to a 500 km Mars orbit, the nodes armed
-- for the burn autopilot, time warp between the burns.

local failed = false
local function log(s) oapi.write_log("RouteFlightTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local function wait_done(hv, serial, timeout)
	local t0 = oapi.get_systime()
	repeat
		proc.wait_frames(1)
		local ti = oapi.transfer_info(hv)
		if ti and ti.job == 0 and ti.serial > serial then return ti end
	until oapi.get_systime() - t0 > timeout
	return oapi.transfer_info(hv)
end

local function main()
	local v = vessel.get_interface("GL-F")
	local hv = v:get_handle()
	local mars, venus = oapi.get_objhandle("Mars"), oapi.get_objhandle("Venus")
	local mjd0 = oapi.get_simmjd ()

	oapi.transfer_route(hv, {"Venus"})
	oapi.transfer_search(hv, mars, 0, 500e3, 0)
	local ti = wait_done(hv, 0, 120)
	check("search", ti.valid and ti.nflyby == 1, ti.message)
	local fb = oapi.transfer_flybys(hv)[1]
	oapi.transfer_nodes(hv, true)
	ti = wait_done(hv, ti.serial, 120)
	check("plan", ti.plan_valid, ti.message)
	fb = oapi.transfer_flybys(hv)[1]
	log(string.format("ejection in %.1f h, Venus in %.1f days (planned %.0f km, predicted %.0f km), arrival in %.1f days",
		(ti.burn_mjd - mjd0) * 24, fb.mjd - mjd0, fb.alt / 1000, fb.pred_alt / 1000, ti.arr_mjd - mjd0))

	-- fly: time warp between the burns, real time near them and while a
	-- correction is computed; the lowest altitude at Venus
	local t0 = oapi.get_systime()
	local lastlog = 0
	local done = nil
	local venusAlt = 1e30
	while oapi.get_systime () - t0 < 900 do
		if not done and ti.plan_valid and ti.next_mjd == 0 and v:get_maneuvernodecount () == 0 and ti.job == 0 then done = oapi.get_simmjd () end
		if done and oapi.get_simmjd () > done + 0.1 then break end
		ti = oapi.transfer_info(hv)
		local now = oapi.get_simmjd ()
		local ref = v:get_gravityref ()
		if ref == venus then venusAlt = math.min (venusAlt, v:get_altitude ()) end
		local nxt = ti.next_mjd > 0 and (ti.next_mjd - now) * 86400 or 1e9
		local w
		if ti.job ~= 0 then w = 1
		elseif v:get_maneuvernodecount () > 0 and (v:get_maneuvernode (0).mjd - now) * 86400 < 1200 then w = 1
		elseif nxt < 6 * 3600 then w = 10
		elseif nxt < 2 * 86400 then w = 100
		elseif v:get_altitude () < 2e6 then w = 1000
		elseif ref == venus and v:get_altitude () < 1e8 then w = 1000 -- (the flyby)
		else w = 100000 end
		if oapi.get_tacc () ~= w then oapi.set_tacc (w) end
		if now - lastlog > 2 then
			lastlog = now
			log(string.format("T+%.1f d: %s, %d nodes, %d corrections, next %s in %.0f s", now - mjd0, oapi.get_objname (ref),
				v:get_maneuvernodecount (), ti.ncorrections, ({"ejection", "correction", "capture", "flyby burn", "manoeuvre"})[ti.next_role + 1] or "-", nxt))
		end
		proc.wait_frames(1)
	end
	oapi.set_tacc (1)
	local el = v:get_elements ()
	local ref = v:get_gravityref ()
	local R = oapi.get_size (mars)
	local pe = el.a * (1 - el.e) - R
	local ap = el.a * (1 + el.e) - R
	log(string.format("Venus flyby at %.0f km (planned %.0f km); final orbit about %s: periapsis %.1f km, apoapsis %.1f km, %d corrections, %.0f s",
		venusAlt / 1000, fb.alt / 1000, oapi.get_objname (ref), pe / 1000, ap / 1000, ti.ncorrections, oapi.get_systime () - t0))
	check("venus", math.abs (venusAlt - fb.alt) < math.max (0.1 * fb.alt, 100e3), string.format("flyby at %.0f km (planned %.0f km)", venusAlt / 1000, fb.alt / 1000))
	check("mars", ref == mars, "orbiting Mars")
	-- (the capture is a burn of some 4.5 km/s centred on the periapsis: the
	-- autopilot's finite burn under time warp lands within about 250 km)
	check("orbit", pe > 400e3 and pe < 750e3 and ap > 400e3 and ap < 900e3, string.format("periapsis %.0f km, apoapsis %.0f km", pe / 1000, ap / 1000))
	oapi.exit(failed and 1 or 0)
end

main()
