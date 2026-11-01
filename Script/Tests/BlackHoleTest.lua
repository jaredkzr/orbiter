-- Black holes (Scenarios/Tests/BlackHoleTest.scn): flight in a Kerr
-- spacetime. A circular orbit, a hover on the drive, a fall through the
-- horizon, against the spacetime's closed forms (Bardeen, Press &
-- Teukolsky 1972). The hole's spin axis is the global y axis; Kerr
-- coordinates (x, y, z) are global (x, z, y).

local failed = false
local function log(s) oapi.write_log("BlackHoleTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local C = 299792458.0
local G = 6.67430e-11
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end

local function wait_sim(dt)
	local t = oapi.get_simtime()
	repeat proc.wait_frames(1) until oapi.get_simtime() - t >= dt
end

-- set a state relative to the hole; wait until it holds
local function place(v, hole, pos, vel)
	local st = v:get_status(2)
	st.status = 0
	st.rbody = hole
	st.rpos = pos
	st.rvel = vel
	st.vrot = {x = 0, y = 0, z = 0}
	v:defset_status(st)
	for _ = 1, 50 do
		proc.wait_frames(1)
		local p = sub(v:get_globalpos(), oapi.get_globalpos(hole))
		if len(sub(p, pos)) < 1e-3 * len(pos) then return end
	end
end

local function main()
	proc.wait_frames(2)
	local hole = oapi.get_objhandle("TestHole")
	check("type", oapi.get_objecttype(hole) == OBJTP.BLACKHOLE, "a black hole")
	local M = G * oapi.get_mass(hole) / (C * C) -- [m]
	local a = 0.9 * M
	local rh = M + math.sqrt(M * M - a * a)
	check("horizon", math.abs(oapi.get_size(hole) / rh - 1) < 1e-9, string.format("size = horizon, %.1f km", rh / 1e3))

	-- 1. Hover on the spin axis at r = 10 M on the drive: on the axis the
	-- static observer's acceleration is sqrt(D/S) (r - M)/D - r/S) (r in
	-- M), D = r^2 - 2 r + a^2, S = r^2 + a^2; its clock's rate sqrt(D/S)
	local hov = vessel.get_interface("HOV")
	local hh = hov:get_handle()
	do
		local r, s = 10.0, 0.9
		local D, S = r * r - 2 * r + s * s, r * r + s * s
		local acc = math.sqrt(D / S) * ((r - 1) / D - r / S) * C * C / M
		local rate = math.sqrt(D / S)
		-- (pointing +z away from the hole, the global +y; the drive on
		-- before the state is set: a frame undriven would grow)
		hov:set_rotationmatrix({m11 = 1, m12 = 0, m13 = 0, m21 = 0, m22 = 0, m23 = 1, m31 = 0, m32 = -1, m33 = 0})
		oapi.set_reldrive(hh, acc, true)
		wait_sim(0.1)
		local p0 = {x = 0, y = r * M, z = 0}
		place(hov, hole, p0, {x = 0, y = 0, z = 0})
		local rs = oapi.get_relstate(hh)
		check("hovstrong", rs.strongfield and rs.relativistic, "in the strong field")
		-- (on the axis: tides 2M (r^3 - 3 r a^2) / (r^2 + a^2)^3 along it)
		local tz = 2 * (r ^ 3 - 3 * r * s * s) / (r * r + s * s) ^ 3 * C * C / (M * M) * oapi.get_size(hh)
		log(string.format("at 10 M: tides %.6g m/s^2 across the vessel (expected %.6g), hover %.6g m/s^2, clock rate %.9f",
			rs.tidal, tz, rs.hover, rs.clockrate))
		check("hovinfo", math.abs(rs.tidal / tz - 1) < 1e-4 and math.abs(rs.hover / acc - 1) < 1e-6 and math.abs(rs.clockrate - rate) < 1e-6,
			"tides, hover, clock reported")
		local t0, c0 = oapi.get_simtime(), rs.clockofs
		local y0 = hov:get_globalpos().y - oapi.get_globalpos(hole).y
		local dmax = 0
		local t1 = t0
		while oapi.get_simtime() - t0 < 20 do
			proc.wait_frames(1)
			local p = sub(hov:get_globalpos(), oapi.get_globalpos(hole))
			dmax = math.max(dmax, len(sub(p, {x = 0, y = y0, z = 0})))
		end
		local t = oapi.get_simtime() - t0
		local got = 1 + (oapi.get_relstate(hh).clockofs - c0) / t
		log(string.format("hover at 10 M (%.0f km) at %.0f g: moved %.1f km in %.1f s; clock rate %.9f (expected %.9f)",
			r * M / 1e3, acc / 9.80665, dmax / 1e3, t, got, rate))
		check("hover", dmax / (r * M) < 1e-4, "held in place")
		check("hovclock", math.abs(got - rate) < 1e-6, "gravitational time dilation")
		oapi.set_reldrive(hh, 0, false)
		-- (out of the way: far out, falling slowly)
		place(hov, hole, {x = 0, y = 0, z = 1.2e12}, {x = 0, y = 0, z = 0})
	end

	-- 2. A circular orbit at r = 10 M, prograde: in Kerr-Schild coordinates
	-- at sqrt(r^2 + a^2) from the axis, turning at Omega = 1/(r^1.5 + a);
	-- the clock's rate r^0.75 sqrt(r^1.5 - 3 r^0.5 + 2a) / (r^1.5 + a)
	local orb = vessel.get_interface("ORB")
	local ho = orb:get_handle()
	do
		local r, s = 10.0, 0.9
		local W = 1 / (r ^ 1.5 + s) * C / M
		local rate = r ^ 0.75 * math.sqrt(r ^ 1.5 - 3 * r ^ 0.5 + 2 * s) / (r ^ 1.5 + s)
		local rho = math.sqrt(r * r + s * s) * M
		-- Kerr (r, a, 0), velocity Omega (-a, r, 0) [M]
		local p0 = {x = r * M, y = 0, z = s * M}
		place(orb, hole, p0, {x = -W * s * M, y = 0, z = W * r * M})
		local T = 2 * math.pi / W
		local t0, c0 = oapi.get_simtime(), oapi.get_relstate(ho).clockofs
		local rmin, rmax, ymax = 1e30, 0, 0
		local q0 = sub(orb:get_globalpos(), oapi.get_globalpos(hole))
		local ang, last = 0, math.atan2(q0.z, q0.x)
		oapi.set_tacc(10)
		while oapi.get_simtime() - t0 < 3 * T do
			proc.wait_frames(1)
			local p = sub(orb:get_globalpos(), oapi.get_globalpos(hole))
			local d = math.sqrt(p.x * p.x + p.z * p.z)
			rmin, rmax = math.min(rmin, d), math.max(rmax, d)
			ymax = math.max(ymax, math.abs(p.y))
			local th = math.atan2(p.z, p.x)
			local dth = th - last
			if dth > math.pi then dth = dth - 2 * math.pi elseif dth < -math.pi then dth = dth + 2 * math.pi end
			ang, last = ang + dth, th
		end
		oapi.set_tacc(1)
		local t = oapi.get_simtime() - t0
		local got = 1 + (oapi.get_relstate(ho).clockofs - c0) / t
		local wgot = ang / t
		log(string.format("orbit at 10 M, %.4f c: radius %.6f-%.6f of %.0f km, %.4f km off the plane; turn rate %.9e (expected %.9e); clock rate %.9f (expected %.9f)",
			W * rho / C, rmin / rho, rmax / rho, rho / 1e3, ymax / 1e3, wgot, W, got, rate))
		check("orbit", math.abs(rmin / rho - 1) < 1e-6 and math.abs(rmax / rho - 1) < 1e-6 and ymax < 1e-6 * rho, "circular")
		check("period", math.abs(wgot / W - 1) < 1e-5, "Kerr's orbital frequency")
		check("orbclock", math.abs(got - rate) < 1e-6, "time dilation in orbit")
		-- (out of the way)
		place(orb, hole, {x = 1.3e12, y = 0, z = 0}, {x = 0, y = 0, z = 0})
	end

	-- 3. A fall from rest on the axis at r = 20 M (below the hole): through
	-- the horizon, lost at the inner horizon
	local fall = vessel.get_interface("FALL")
	local hf = fall:get_handle()
	do
		local rm = M - math.sqrt(M * M - a * a)
		place(fall, hole, {x = 0, y = -20 * M, z = 0}, {x = 0, y = 0, z = 0})
		local t0 = oapi.get_simtime()
		local inside, lost, tin
		oapi.set_tacc(10)
		while oapi.get_simtime() - t0 < 400 do
			proc.wait_frames(1)
			local d = len(sub(fall:get_globalpos(), oapi.get_globalpos(hole)))
			if d < rh and not inside then inside, tin = d, oapi.get_simtime() - t0 end
			-- (the flag is read ahead of the state)
			if oapi.get_relstate(hf).swallowed then
				wait_sim(1)
				lost = len(sub(fall:get_globalpos(), oapi.get_globalpos(hole)))
				break
			end
		end
		oapi.set_tacc(1)
		log(string.format("fall from 20 M: inside the horizon after %.1f s, lost at %.4f M (inner horizon %.4f M)",
			tin or -1, (lost or -1) / M, rm / M))
		check("horizon", inside ~= nil, "crossed the horizon")
		check("lost", lost and math.abs(lost - rm) < 1e-4 * M, "lost at the inner horizon")
		local p1 = fall:get_globalpos()
		wait_sim(1)
		check("held", len(sub(fall:get_globalpos(), p1)) == 0, "its state held")
	end

	-- 4. Trips to the hole on the autopilot at 10^5 g: at rest (where the
	-- drive holds the vessel with a quarter of its acceleration: ~50 M), held
	-- there; in orbit at Kerr's orbital speed (in the equatorial plane)
	local G0 = 9.80665
	local function trip(name, arrival)
		local v = vessel.get_interface(name)
		local hv = v:get_handle()
		oapi.set_focusobject(hv)
		oapi.reltrip_set(hv, {target = hole, arrival = arrival, accel = 1e5 * G0})
		oapi.reltrip_engage(hv, true)
		local t0 = oapi.get_systime()
		local ri
		repeat
			proc.wait_frames(1)
			ri = oapi.reltrip_info(hv)
			if oapi.get_tacc() < 100 and ri.time and ri.time > 120 and math.deg(ri.atterr) < 5 then oapi.set_tacc(100) end
		until ri.phase == "Arrived" or ri.phase == "Holding" or ri.phase == "No trip" or oapi.get_systime() - t0 > 120
		oapi.set_tacc(1)
		wait_sim(1)
		return v, hv, ri
	end
	do
		local v, hv, ri = trip("ORB", "rest")
		wait_sim(60) -- (the hold settles: the arrival's last step leaves ~0.5 km/s, taken out over ~45 s)
		local p0 = sub(v:get_globalpos(), oapi.get_globalpos(hole))
		local r0 = len(p0)
		wait_sim(30)
		local p1 = sub(v:get_globalpos(), oapi.get_globalpos(hole))
		local vr = len(sub(v:get_globalvel(), oapi.get_globalvel(hole)))
		ri = oapi.reltrip_info(hv)
		local rs = oapi.get_relstate(hv)
		log(string.format("trip at rest: %s at %.2f M (%.0f km), moved %.3f km in 30 s, %.3f m/s; drive %.4g g (hover %.4g g); %s",
			ri.phase, r0 / M, r0 / 1e3, len(sub(p1, p0)) / 1e3, vr, rs.accel / G0, rs.hover / G0, ri.message))
		check("triprest", ri.phase == "Holding" and r0 > 40 * M and r0 < 60 * M, "arrived at rest, held")
		check("hold", len(sub(p1, p0)) < 100 and vr < 1, "held in place on the drive")
		oapi.reltrip_engage(hv, false)
	end
	do
		local v, hv, ri = trip("HOV", "orbit")
		local p0 = sub(v:get_globalpos(), oapi.get_globalpos(hole))
		local r0 = len(p0)
		local rmin, rmax = r0, r0
		local t0 = oapi.get_simtime()
		oapi.set_tacc(10)
		while oapi.get_simtime() - t0 < 600 do
			proc.wait_frames(1)
			local d = len(sub(v:get_globalpos(), oapi.get_globalpos(hole)))
			rmin, rmax = math.min(rmin, d), math.max(rmax, d)
		end
		oapi.set_tacc(1)
		local rs = oapi.get_relstate(hv)
		log(string.format("trip to orbit: %s at %.2f M, %.4f M off the plane; over 600 s %.4f-%.4f M; drive %s",
			ri.phase, r0 / M, p0.y / M, rmin / M, rmax / M, tostring(rs.drive)))
		check("triporbit", ri.phase == "Arrived" and r0 > 40 * M and r0 < 60 * M and math.abs(p0.y) < 1e-3 * r0, "arrived in the equatorial plane")
		check("orbit2", (rmax - rmin) / r0 < 0.01 and not rs.drive, "a free circular orbit")
	end

	-- 5. Beyond the strong field, Orbiter's own flight (Newton): a root body
	-- at rest at the origin without an ephemeris (its state carried over, its
	-- rotation turned; positions interpolated without a direction to bisect)
	do
		local v = vessel.get_interface("FALL")
		place(v, hole, {x = 3e13, y = 0, z = 0}, {x = 0, y = 0, z = 0})
		local t0 = oapi.get_simtime()
		local w0 = sub(v:get_globalvel(), oapi.get_globalvel(hole))
		wait_sim(2)
		local w = sub(sub(v:get_globalvel(), oapi.get_globalvel(hole)), w0)
		local t = oapi.get_simtime() - t0
		local g = G * oapi.get_mass(hole) / (3e13 * 3e13)
		log(string.format("far out (%.0f M): falling at %.6g m/s after %.2f s (Newton %.6g)", 3e13 / M, -w.x, t, g * t))
		check("far", w.x == w.x and math.abs(-w.x / (g * t) - 1) < 0.02, "Orbiter's flight about the hole beyond the strong field")
	end

	oapi.exit(failed and 1 or 0)
end

main()
