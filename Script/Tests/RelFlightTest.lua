-- Relativistic flight (Scenarios/Tests/RelFlightTest.scn): the ideal drive
-- against hyperbolic motion, the clock carried along, the drive saved in a
-- scenario, an impact found within one time-warped frame.

local failed = false
local function log(s) oapi.write_log("RelFlightTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local C = 299792458.0
local G0 = 9.80665
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
local function unit(a) local l = len(a); return {x = a.x / l, y = a.y / l, z = a.z / l} end
local function cross(a, b) return {x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x} end
local function asinh(x) return math.log(x + math.sqrt(x * x + 1)) end
local function cosh(x) return (math.exp(x) + math.exp(-x)) / 2 end
local function tanh(x) local e = math.exp(2 * x); return (e - 1) / (e + 1) end

-- turn a vessel's +z axis to direction d (global frame)
local function point(v, d)
	local z = unit(d)
	local ref = math.abs(z.y) < 0.9 and {x = 0, y = 1, z = 0} or {x = 1, y = 0, z = 0}
	local x = unit(cross(ref, z))
	local y = cross(z, x)
	-- (columns: the vessel's axes in the global frame)
	v:set_rotationmatrix({m11 = x.x, m12 = y.x, m13 = z.x, m21 = x.y, m22 = y.y, m23 = z.y, m31 = x.z, m32 = y.z, m33 = z.z})
	v:set_angvel({x = 0, y = 0, z = 0})
end

-- (the state a script reads lags the simulation by a frame: wait on its time)
local function wait_sim(dt)
	local t = oapi.get_simtime()
	repeat proc.wait_frames(1) until oapi.get_simtime() - t >= dt
end

local function main()
	proc.wait_frames(2)

	-- 1. Into the Moon at 10^6 g, time warp 100 (2 s a frame): the fall of
	-- ~8,300 km takes ~1.3 s, within the first frame
	local imp = vessel.get_interface("IMP")
	local hi = imp:get_handle()
	local moon = oapi.get_objhandle("Moon")
	local R = oapi.get_size(moon)
	point(imp, sub(oapi.get_globalpos(moon), imp:get_globalpos()))
	oapi.set_tacc(100)
	check("impdrive", oapi.set_reldrive(hi, 1e6 * G0, true), "drive on")
	wait_sim(6)
	local rs = oapi.get_relstate(hi)
	local d = len(sub(imp:get_globalpos(), oapi.get_globalpos(moon)))
	local vr = len(sub(imp:get_globalvel(), oapi.get_globalvel(moon)))
	log(string.format("after the impact: %.1f km from the Moon's centre (radius %.1f km), %.1f m/s, drive %s",
		d / 1000, R / 1000, vr, tostring(rs.drive)))
	check("impact", not rs.drive and d < R + 30e3 and d > R - 30e3 and vr < 100, "stopped at the surface")
	-- (back to real time: a script's settings take effect a frame late, a
	-- frame of 0.02 s, not 2 s, of the drive's start missed below)
	oapi.set_tacc(1)
	wait_sim(0.5)

	-- 2. 10,000 g from solar orbit, half an hour of simulation time
	local v = vessel.get_interface("REL")
	local hv = v:get_handle()
	local a = 1e4 * G0
	point(v, {x = 0, y = 0, z = 1})
	local r0, v0 = v:get_globalpos(), v:get_globalvel()
	local t0 = oapi.get_simtime()
	local c0 = oapi.get_relstate(hv).clockofs
	oapi.set_reldrive(hv, a, true)
	oapi.set_tacc(100)
	local T = 1800
	while oapi.get_simtime() - t0 < T do proc.wait_frames(1) end
	-- (time, clock and state of the same frame, the drive still on)
	local t = oapi.get_simtime() - t0
	rs = oapi.get_relstate(hv)
	local r1, v1 = v:get_globalpos(), v:get_globalvel()
	oapi.set_reldrive(hv, a, false)
	oapi.set_tacc(1)
	local tau = t + rs.clockofs - c0
	-- hyperbolic motion from rest (the initial 30 km/s, 1e-4 c, changes these by ~1e-4)
	local eta = asinh(a * t / C)
	local dx = len(sub(sub(r1, r0), {x = v0.x * t, y = v0.y * t, z = v0.z * t}))
	local dv = len(sub(v1, v0))
	log(string.format("after %.1f s: proper time %.2f s (expected %.2f), speed %.5f c (%.5f), distance %.6e m (%.6e), relativistic %s",
		t, tau, C / a * eta, dv / C, tanh(eta), dx, C * C / a * (cosh(eta) - 1), tostring(rs.relativistic)))
	check("tau", math.abs(tau / (C / a * eta) - 1) < 1e-5, "proper time")
	check("speed", math.abs(dv / (C * tanh(eta)) - 1) < 1e-5, "speed")
	check("dist", math.abs(dx / (C * C / a * (cosh(eta) - 1)) - 1) < 1e-4, "distance")
	wait_sim(1)
	rs = oapi.get_relstate(hv)
	check("rel", rs.relativistic and not rs.drive, "coasting relativistically")

	-- 3. coasting: the clock runs at 1/gamma
	local ta, ca = oapi.get_simtime(), rs.clockofs
	local g = rs.gamma
	while oapi.get_simtime() - ta < 100 do proc.wait_frames(1) end
	local tb = oapi.get_simtime()
	local rate = 1 + (oapi.get_relstate(hv).clockofs - ca) / (tb - ta)
	log(string.format("coasting: clock rate %.8f, 1/gamma %.8f", rate, 1 / g))
	check("coast", math.abs(rate - 1 / g) < 1e-6, "clock rate 1/gamma")

	-- 4. the drive and the clock in a saved scenario
	oapi.set_reldrive(hv, a, true)
	check("save", oapi.savescenario("Tests/RelFlightSaved", "Saved by RelFlightTest"), "scenario saved")
	local f = io.open("Scenarios/Tests/RelFlightSaved.scn")
	local txt = f and f:read("*a") or ""
	if f then f:close() end
	local drive = txt:match("RELDRIVE ([%d%.e%+]+) ON")
	local clock = txt:match("CLOCK (%-?[%d%.e%+]+)")
	log(string.format("saved: RELDRIVE %s, CLOCK %s", tostring(drive), tostring(clock)))
	check("saved", drive and math.abs(tonumber(drive) / a - 1) < 1e-9 and clock and tonumber(clock) < -50, "drive and clock saved")
	oapi.set_reldrive(hv, a, false)

	-- 5. past Earth at 0.3 c, 10,000 km out: the velocity change across the
	-- path (relative to Earth), 2GM/(b v) (1 + v^2/c^2) = 0.966 m/s
	-- post-Newtonian (0.886 Newtonian); Earth the reference body at the
	-- closest approach (spheres of influence crossed within seconds)
	local fly = vessel.get_interface("FLY")
	local earth = oapi.get_objhandle("Earth")
	local b, vp = 1e7, 0.3 * C
	local st = fly:get_status(2)
	st.status = 0
	st.rbody = earth
	st.rpos = {x = -1e9, y = b, z = 0}
	st.rvel = {x = vp, y = 0, z = 0}
	fly:defset_status(st)
	wait_sim(0.1)
	local function vrel() return sub(fly:get_globalvel(), oapi.get_globalvel(earth)) end
	local function prel() return sub(fly:get_globalpos(), oapi.get_globalpos(earth)) end
	local p0, w0 = prel(), vrel()
	local refAtClosest
	while prel().x < 1e9 do
		proc.wait_frames(1)
		if not refAtClosest and prel().x > 0 then refAtClosest = fly:get_gravityref() end
	end
	local w1 = vrel()
	local dvy = w1.y - w0.y
	local GM = 3.986004418e14
	local x0 = -p0.x -- (the path covered: from x0 before to beyond 1e9 m after)
	local x1 = prel().x
	local frac = 0.5 * (x0 / math.sqrt(x0 * x0 + b * b) + x1 / math.sqrt(x1 * x1 + b * b))
	local expect = -2 * GM / (b * vp) * (1 + vp * vp / (C * C)) * frac
	log(string.format("past Earth at 0.3 c: across %.4f m/s (post-Newtonian %.4f, Newtonian %.4f), reference at closest %s",
		dvy, expect, expect / (1 + vp * vp / (C * C)), refAtClosest and oapi.get_objname(refAtClosest) or "-"))
	check("bend", math.abs(dvy / expect - 1) < 0.01, "post-Newtonian deflection")
	check("soi", refAtClosest == earth, "Earth the reference body")

	-- 6. the prediction (map, Orbit MFD) of a driven vessel: from rest 10^11 m
	-- before Earth at 10,000 g, closest approach after 1,465 s (1,428 s
	-- Newtonian); predicted against flown
	st = fly:get_status(2)
	st.status = 0
	st.rbody = earth
	st.rpos = {x = -1e11, y = b, z = 0}
	st.rvel = {x = 0, y = 0, z = 0}
	fly:defset_status(st)
	-- (a status is set some frames late: the attitude after it, the drive
	-- once the attitude holds)
	local function until_(cond) local tw = oapi.get_systime(); repeat proc.wait_frames(1) until cond() or oapi.get_systime() - tw > 10 end
	until_(function() return math.abs(prel().x + 1e11) < 1e6 and len(vrel()) < 1 end)
	point(fly, {x = 1, y = 0, z = 0})
	until_(function() local m = fly:get_rotationmatrix(); return m.m13 > 0.999999 end)
	local hf = fly:get_handle()
	oapi.set_focusobject(hf)
	oapi.set_reldrive(hf, a, true)
	wait_sim(0.1)
	-- fly at time warp 10 (0.2 s frames) until abeam of Earth (x = 0: the
	-- closest approach, 10,000 km out), taking the predictions made meanwhile
	local t0p = oapi.get_simmjd()
	oapi.set_tacc(10)
	local pred, predAt
	local px, pm = prel().x, oapi.get_simmjd()
	local tw = oapi.get_systime()
	while prel().x < 0 and oapi.get_systime() - tw < 60 do
		px, pm = prel().x, oapi.get_simmjd()
		local ev = oapi.get_trajectoryevents(hf)
		for _, e in ipairs(ev or {}) do
			if e.type == "periapsis" and e.ref == earth and e.mjd > pm then pred, predAt = e.mjd, pm end
		end
		proc.wait_frames(1)
	end
	local x1, m1 = prel().x, oapi.get_simmjd()
	local flown = pm + (m1 - pm) * (-px) / (x1 - px)
	oapi.set_reldrive(hf, a, false)
	oapi.set_tacc(1)
	log(string.format("driven past Earth: closest approach flown after %.2f s, predicted at %.0f s into the flight for %.2f s",
		(flown - t0p) * 86400, predAt and (predAt - t0p) * 86400 or -1, pred and (pred - t0p) * 86400 or -1))
	check("predict", pred and math.abs(pred - flown) * 86400 < 1.0, "prediction of a driven vessel")

	oapi.exit(failed and 1 or 0)
end

main()
