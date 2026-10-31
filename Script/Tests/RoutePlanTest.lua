-- Transfer planning with gravity assists (Scenarios/Tests/RoutePlanTest.scn):
-- a route to Mars by Venus from a 300 km Earth orbit (patched-conic optimum,
-- then each encounter targeted in full gravity), and suggested routes to
-- Jupiter.

local failed = false
local function log(s) oapi.write_log("RoutePlanTest: " .. s) end
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
	local v = vessel.get_interface("GL-R")
	local hv = v:get_handle()
	local mars, jupiter = oapi.get_objhandle("Mars"), oapi.get_objhandle("Jupiter")
	local now = oapi.get_simmjd()

	-- the direct transfer for comparison
	local t0 = oapi.get_systime()
	check("direct_start", oapi.transfer_search(hv, mars, 0, 300e3, 0), "search started")
	local ti = wait_done(hv, 0, 60)
	local direct = ti.dv_total
	log(string.format("Mars direct: %.0f m/s, departure MJD %.1f, %.0f days (%.1f s)", ti.dv_total, ti.dep, ti.tof, oapi.get_systime() - t0))

	-- Earth - Venus - Mars
	t0 = oapi.get_systime()
	check("route_set", oapi.transfer_route(hv, {"Venus"}, {revs = 0}), "route set")
	local s = ti.serial
	check("route_start", oapi.transfer_search(hv, mars, 0, 300e3, 0), "search started")
	ti = wait_done(hv, s, 120)
	log(string.format("Mars by Venus: %s (%.1f s)", ti.message, oapi.get_systime() - t0))
	local fb = oapi.transfer_flybys(hv)
	check("route_found", ti.valid and ti.nflyby == 1 and #fb == 1, ti.message)
	if #fb ~= 1 then oapi.exit(1) return end
	log(string.format("  Venus flyby MJD %.1f at %.0f km, turn %.1f deg, vinf %.0f -> %.0f m/s, powered %.0f m/s",
		fb[1].mjd, fb[1].alt / 1000, fb[1].turn * 180 / math.pi, fb[1].vinf_in, fb[1].vinf_out, fb[1].dv))
	check("route_flyby_alt", fb[1].alt >= fb[1].min_alt - 1, string.format("%.0f km (least %.0f km)", fb[1].alt / 1000, fb[1].min_alt / 1000))
	check("route_dates", ti.dep > now and fb[1].mjd > ti.dep and fb[1].mjd < ti.dep + ti.tof, "departure, flyby, arrival in order")

	-- each encounter in full gravity
	t0 = oapi.get_systime()
	s = ti.serial
	check("route_nodes_start", oapi.transfer_nodes(hv, false), "targeting started")
	ti = wait_done(hv, s, 400)
	log(string.format("Route targeted: %s (%.1f s)", ti.message, oapi.get_systime() - t0))
	fb = oapi.transfer_flybys(hv)
	check("route_plan", ti.plan_valid, ti.message)
	if fb[1] then
		check("route_flyby_pred", fb[1].pred_mjd > 0 and math.abs(fb[1].pred_alt - fb[1].alt) < math.max(0.05 * fb[1].alt, 20e3),
			string.format("Venus periapsis %.0f km predicted (planned %.0f km)", fb[1].pred_alt / 1000, fb[1].alt / 1000))
	end
	check("route_arrival", ti.plan_valid and math.abs(ti.peri_alt - 300e3) < 15e3,
		string.format("Mars periapsis %.1f km (target 300 km)", ti.peri_alt / 1000))
	log(string.format("  %d nodes", v:get_maneuvernodecount()))

	-- suggestions to Jupiter
	t0 = oapi.get_systime()
	s = ti.serial
	oapi.transfer_route(hv, {}, {revs = 1}) -- (suggestions with legs of full revolutions: Earth-Earth resonances)
	-- (least ejection: a capture into a low orbit about Jupiter would outweigh the rest)
	check("jupiter_search", oapi.transfer_search(hv, jupiter, 1, 0, 0), "search started")
	ti = wait_done(hv, s, 120)
	log(string.format("Jupiter direct: %.0f m/s, %.0f days", ti.dv_total, ti.tof))
	local direct_j = ti.dv_total
	s = ti.serial
	check("suggest_start", oapi.transfer_suggest(hv), "suggestions started")
	ti = wait_done(hv, s, 300)
	log(string.format("Suggestions: %s (%.1f s)", ti.message, oapi.get_systime() - t0))
	local sg = oapi.transfer_suggestions(hv)
	for i, r in ipairs(sg) do
		local names = #r.flybys > 0 and table.concat(r.flybys, "-") or "direct"
		log(string.format("  %d: %s, %.0f m/s, departure MJD %.1f, %.0f days", i, names, r.dv, r.dep, r.tof))
	end
	check("suggestions", #sg >= 2, string.format("%d routes", #sg))
	-- (gravity assists pay to Jupiter: Galileo went by Venus and twice by Earth)
	check("suggest_better", sg[1] and #sg[1].flybys > 0 and sg[1].dv < 0.9 * direct_j,
		string.format("best %.0f m/s against %.0f m/s direct", sg[1] and sg[1].dv or 0, direct_j))
	oapi.exit(failed and 1 or 0)
end

main()
