-- benchmark: a trip on the Relativistic Travel autopilot from low Earth
-- orbit: TRIP_TARGET (a body), TRIP_ARR (orbit/rest), TRIP_ALT [m or "default"],
-- TRIP_G [g], time acceleration TRIP_WARP once aligned, ship-time warp
-- TRIP_SHIPWARP (interstellar); TRIP_LINGER [s wall] at the end;
-- TRIP_PROBE [m]: each frame logged within this distance of the target;
-- TRIP_EVERY [s wall]: the state logged this often
local function log(s) oapi.write_log("bench trip: " .. s) end
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
local tgt = os.getenv("TRIP_TARGET")
local acc = tonumber(os.getenv("TRIP_G") or "1000") * 9.80665
local warp = tonumber(os.getenv("TRIP_WARP") or "100")
local sw = tonumber(os.getenv("TRIP_SHIPWARP") or "0")
local linger = tonumber(os.getenv("TRIP_LINGER") or "40")
local probe = tonumber(os.getenv("TRIP_PROBE") or "0")
local every = tonumber(os.getenv("TRIP_EVERY") or "0")
local t0, tw
proc.wait_frames(2)
local v = vessel.get_interface("GL")
local hv = v:get_handle()
local ht = oapi.get_objhandle(tgt)
local function state(ri)
	local r = sub(v:get_globalpos(), oapi.get_globalpos(ht))
	local d = len(r)
	local u = sub(v:get_globalvel(), oapi.get_globalvel(ht))
	local vr = (r.x * u.x + r.y * u.y + r.z * u.z) / d
	return string.format("t %.6g %s r %.6g km vrad %.6g vtan %.4g km/s arrive in %.4g s step %.3g s warp %.4g atterr %.2f deg",
		oapi.get_simtime() - t0, ri.phase, d / 1000, vr / 1000, math.sqrt(math.max(0, len(u) ^ 2 - vr * vr)) / 1000,
		ri.time, oapi.get_simstep(), oapi.get_tacc(), math.deg(ri.atterr))
end
-- (TRIP_ALT=default: the autopilot's own (a black hole: where holding takes a quarter of the drive))
local altenv = os.getenv("TRIP_ALT") or "500e3"
oapi.reltrip_set(hv, {target = ht, arrival = os.getenv("TRIP_ARR") or "orbit", alt = altenv ~= "default" and tonumber(altenv) or nil, accel = acc})
local ri = oapi.reltrip_info(hv)
log(string.format("%s: plan ok %s, %.4g h (ship %.4g h), vmax %.4f c", tgt, tostring(ri.ok), ri.time / 3600, ri.shiptime / 3600, ri.vmax / 299792458))
t0, tw = oapi.get_simtime(), oapi.get_systime()
oapi.reltrip_engage(hv, true)
local lastph, warped, tlast = "", false, tw
local wallmax = tonumber(os.getenv("TRIP_WALLMAX") or "1500")
while oapi.get_systime() - tw < wallmax do
	ri = oapi.reltrip_info(hv)
	if not warped and ri.time > 120 and math.deg(ri.atterr) < 5 then
		if sw > 0 then oapi.set_shipwarp(sw) else oapi.set_tacc(warp) end
		warped = true
	end
	if ri.phase ~= lastph then
		local rs = oapi.get_relstate(hv)
		log(string.format("T+%.0f s (wall %.0f s) %s: arrive in %.4g s, speed %.4f c, warp %g", oapi.get_simtime() - t0, oapi.get_systime() - tw, ri.phase, ri.time, rs.speed / 299792458, oapi.get_tacc()))
		lastph = ri.phase
	end
	if probe > 0 and len(sub(v:get_globalpos(), oapi.get_globalpos(ht))) < probe then log("probe " .. state(ri)) end
	if every > 0 and oapi.get_systime() - tlast >= every then log("state " .. state(ri)); tlast = oapi.get_systime() end
	if ri.phase == "Arrived" or ri.phase == "No trip" then break end
	proc.wait_frames(1)
end
oapi.set_shipwarp(0)
oapi.set_tacc(1)
local d = len(sub(v:get_globalpos(), oapi.get_globalpos(ht)))
log(string.format("end: %s, %.0f km from %s centre, sim %.4g s, wall %.0f s", ri.phase, d / 1000, tgt, oapi.get_simtime() - t0, oapi.get_systime() - tw))
local tl = oapi.get_systime()
while oapi.get_systime() - tl < linger do proc.wait_frames(1) end
oapi.exit(0)
