-- A planned transfer flown (Scenarios/Tests/TransferMarsFlightTest.scn): from a
-- 300 km Earth orbit to a 500 km Mars orbit, the nodes armed for the burn
-- autopilot, time warp between the burns.

local failed = false
local function log(s) oapi.write_log("TransferMarsFlightTest: " .. s) end
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
	local v = vessel.get_interface("GL-M")
	local hv = v:get_handle()
	local moon = oapi.get_objhandle("Mars")

	oapi.transfer_search(hv, moon, 0, 500e3, 0)
	local ti = wait_done(hv, 0, 40)
	check("search", ti.valid, ti.message)
	oapi.transfer_nodes(hv, true)
	ti = wait_done(hv, ti.serial, 60)
	check("plan", ti.plan_valid, ti.message)
	local arr = ti.arr_mjd
	ti = oapi.transfer_info(hv)
	log(string.format("ejection in %.1f h, arrival in %.2f days", (ti.burn_mjd - oapi.get_simmjd ()) * 24, arr - oapi.get_simmjd ()))

	-- fly: time warp between the burns, real time near them and while a
	-- correction is computed
	local t0 = oapi.get_systime()
	local lastlog = 0
	local done = nil -- (when the last burn was flown)
	while oapi.get_systime () - t0 < 400 do
		if not done and ti.plan_valid and ti.next_mjd == 0 and v:get_maneuvernodecount () == 0 and ti.job == 0 then done = oapi.get_simmjd () end
		if done and oapi.get_simmjd () > done + 0.1 then break end
		ti = oapi.transfer_info(hv)
		local now = oapi.get_simmjd ()
		local nxt = ti.next_mjd > 0 and (ti.next_mjd - now) * 86400 or 1e9
		local w
		if ti.job ~= 0 then w = 1
		elseif v:get_maneuvernodecount () > 0 and (v:get_maneuvernode (0).mjd - now) * 86400 < 1200 then w = 1
		elseif nxt < 6 * 3600 then w = 10
		elseif nxt < 2 * 86400 then w = 100
		elseif v:get_altitude () < 2e6 then w = 1000
		else w = 100000 end
		if oapi.get_tacc () ~= w then oapi.set_tacc (w) end
		if now - lastlog > 0.5 then
			lastlog = now
			log(string.format("T+%.2f d: %d nodes, %d corrections, next %s in %.0f s", now - 51982.6685767396,
				v:get_maneuvernodecount (), ti.ncorrections, ({"ejection", "correction", "capture"})[ti.next_role + 1] or "-", nxt))
		end
		proc.wait_frames(1)
	end
	oapi.set_tacc (1)
	-- a lunar orbit?
	local el = v:get_elements ()
	local ref = v:get_gravityref ()
	local R = oapi.get_size (moon)
	local pe = el.a * (1 - el.e) - R
	local ap = el.a * (1 + el.e) - R
	log(string.format("final orbit about %s: periapsis %.1f km, apoapsis %.1f km, %d corrections, %.0f s",
		oapi.get_objname (ref), pe / 1000, ap / 1000, ti.ncorrections, oapi.get_systime () - t0))
	check("mars", ref == moon, "orbiting Mars")
	check("orbit", pe > 400e3 and pe < 600e3 and ap > 400e3 and ap < 800e3, string.format("periapsis %.0f km, apoapsis %.0f km", pe / 1000, ap / 1000))
	oapi.exit(failed and 1 or 0)
end

main()
