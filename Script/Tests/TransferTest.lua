-- Trajectory prediction of an Earth-Moon transfer (Scenarios/Tests/TransferTest.scn):
-- a 3.13 km/s prograde node at the best of 24 times around the orbit (closest
-- approach to the Moon); its trajectory must enter the Moon's sphere of
-- influence and pass a periapsis there.

local failed = false
local function log(s) oapi.write_log("TransferTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

-- events of the prediction once it reflects the node state (has events after
-- node 0 if want_node, none otherwise)
local function wait_prediction(hv, want_node, timeout)
	local t0 = oapi.get_systime()
	repeat
		local ev = oapi.get_trajectoryevents(hv)
		if ev then
			local has = false
			for _, e in ipairs(ev) do if e.node == 0 then has = true end end
			if has == want_node then return ev end
		end
		proc.wait_frames(1)
	until oapi.get_systime() - t0 > timeout
	return nil
end

local function main()
	local v = vessel.get_interface("GL-T")
	local hv = v:get_handle()
	local moon = oapi.get_objhandle("Moon")
	oapi.set_trajectorytarget(moon)
	proc.wait_frames(2)

	local period = 2 * math.pi * math.sqrt(6571000^3 / 3.986004e14)
	local mjd0 = oapi.get_simmjd() + 600 / 86400
	local best, bestk, bestev = 1e20, -1, nil
	for k = 0, 23 do
		v:clear_maneuvernodes()
		wait_prediction(hv, false, 20)
		v:add_maneuvernode({mjd = mjd0 + k * period / 24 / 86400, dv = {x = 3130, y = 0, z = 0}})
		local ev = wait_prediction(hv, true, 30)
		if ev then
			for _, e in ipairs(ev) do
				if e.type == "closest" and e.node == 0 and e.value < best then best, bestk, bestev = e.value, k, ev end
			end
		end
	end
	log(string.format("closest approach to the Moon %.0f km (node %d/24)", best / 1000, bestk))
	check("closest", best < 60000e3, string.format("%.0f km", best / 1000))
	if not bestev then oapi.exit(1) return end

	local soi, peri = nil, nil
	for _, e in ipairs(bestev) do
		if e.type == "soi_enter" and e.ref == moon and not soi then soi = e end
		if e.type == "periapsis" and e.ref == moon and not peri then peri = e end
	end
	check("soi", soi ~= nil, soi and string.format("enters the Moon's sphere of influence %.2f days after the start", (soi.mjd - mjd0)) or "no")
	check("periapsis", peri ~= nil, peri and string.format("lunar periapsis altitude %.0f km", peri.value / 1000) or "no")
	oapi.exit(failed and 1 or 0)
end

main()
