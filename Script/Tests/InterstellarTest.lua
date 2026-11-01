-- Interstellar flight (Scenarios/Tests/InterstellarTest.scn): deep space,
-- the ship-time warp, the world on conics at extreme time acceleration.

local failed = false
local function log(s) oapi.write_log("InterstellarTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local C = 299792458.0
local G0 = 9.80665
local YEAR = 365.25 * 86400
local AU = 1.495978707e11
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
local function unit(a) local l = len(a); return {x = a.x / l, y = a.y / l, z = a.z / l} end
local function cross(a, b) return {x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x} end
local function sinh(x) return (math.exp(x) - math.exp(-x)) / 2 end
local function cosh(x) return (math.exp(x) + math.exp(-x)) / 2 end
local function tanh(x) local e = math.exp(2 * x); return (e - 1) / (e + 1) end

local function point(v, d)
	local z = unit(d)
	local ref = math.abs(z.y) < 0.9 and {x = 0, y = 1, z = 0} or {x = 1, y = 0, z = 0}
	local x = unit(cross(ref, z))
	local y = cross(z, x)
	v:set_rotationmatrix({m11 = x.x, m12 = y.x, m13 = z.x, m21 = x.y, m22 = y.y, m23 = z.y, m31 = x.z, m32 = y.z, m33 = z.z})
	v:set_angvel({x = 0, y = 0, z = 0})
end

local function until_(cond, sec)
	local tw = oapi.get_systime()
	repeat proc.wait_frames(1) until cond() or oapi.get_systime() - tw > (sec or 10)
end

local function main()
	proc.wait_frames(2)
	local v = vessel.get_interface("DS")
	local hv = v:get_handle()
	local leo = vessel.get_interface("LEO")
	local earth = oapi.get_objhandle("Earth")
	local sun = oapi.get_objhandle("Sun")
	local a = G0

	-- 1. a year at 1 g on the ship's clock, ship-time warp 10^6 (2 10^4 s
	-- of ship time a frame), outwards
	local out = unit(sub(v:get_globalpos(), oapi.get_globalpos(sun)))
	point(v, out)
	until_(function() local m = v:get_rotationmatrix(); return m.m13 * out.x + m.m23 * out.y + m.m33 * out.z > 0.999999 end)
	-- (positions relative to the Sun: the global frame's origin follows the
	-- vessel in deep space)
	local function helio(o) return sub(o:get_globalpos(), oapi.get_globalpos(sun)) end
	local r0, v0 = helio(v), v:get_globalvel()
	local t0, c0 = oapi.get_simtime(), oapi.get_relstate(hv).clockofs
	local leo0 = len(sub(leo:get_globalpos(), oapi.get_globalpos(earth)))
	oapi.set_reldrive(hv, a, true)
	proc.wait_frames(1)
	oapi.set_shipwarp(1e6)
	local wmax, leoMin, leoMax = 0, leo0, leo0
	local tau = 0
	until_(function()
		wmax = math.max(wmax, oapi.get_tacc())
		local d = len(sub(leo:get_globalpos(), oapi.get_globalpos(earth)))
		leoMin, leoMax = math.min(leoMin, d), math.max(leoMax, d)
		tau = oapi.get_simtime() - t0 + oapi.get_relstate(hv).clockofs - c0
		return tau >= YEAR
	end, 120)
	-- (time, clock and state of the same frame)
	local t = oapi.get_simtime() - t0
	local rs = oapi.get_relstate(hv)
	tau = t + rs.clockofs - c0
	local r1, v1 = helio(v), v:get_globalvel()
	oapi.set_reldrive(hv, a, false)
	oapi.set_shipwarp(0)
	-- hyperbolic motion from rest (the initial 0.67 km/s changes these by ~2e-6)
	local eta = a * tau / C
	local dx = len(sub(sub(r1, r0), {x = v0.x * t, y = v0.y * t, z = v0.z * t}))
	local dv = len(sub(v1, v0))
	log(string.format("after %.4f years on board: %.4f years in the Solar System (expected %.4f), speed %.6f c (%.6f), distance %.6e ly (%.6e), largest time acceleration %.3g",
		tau / YEAR, t / YEAR, C / a * sinh(eta) / YEAR, dv / C, tanh(eta), dx / (C * YEAR), C * C / a * (cosh(eta) - 1) / (C * YEAR), wmax))
	check("time", math.abs(t / (C / a * sinh(eta)) - 1) < 1e-5, "Solar System time")
	check("speed", math.abs(dv / (C * tanh(eta)) - 1) < 1e-5, "speed")
	check("dist", math.abs(dx / (C * C / a * (cosh(eta) - 1)) - 1) < 1e-4, "distance")
	check("warp", wmax > 1e6, "time acceleration beyond Orbiter's limit in deep space")
	log(string.format("low Earth orbit over the flight: %.3f-%.3f km (from %.3f)", leoMin / 1e3, leoMax / 1e3, leo0 / 1e3))
	check("parked", math.abs(leoMin / leo0 - 1) < 1e-3 and math.abs(leoMax / leo0 - 1) < 1e-3, "the world on conics")
	local de = len(sub(oapi.get_globalpos(earth), oapi.get_globalpos(sun)))
	check("earth", math.abs(de / AU - 1) < 0.02, string.format("Earth %.4f AU from the Sun", de / AU))
	check("shipwarpoff", oapi.get_shipwarp() == 0 and oapi.get_tacc() <= 1e5, "ship-time warp off")

	-- 2. 1000 pc from the Sun, moving at 1 m/s: the global frame's origin
	-- near the vessel, its motion resolved to micrometres (doubles about the
	-- Sun resolve 4 km there)
	local PC = 3.0856775814913673e16
	oapi.set_tacc(1)
	local st = v:get_status(2)
	st.status = 0
	st.rbody = sun
	st.rpos = {x = 1000 * PC, y = 0, z = 0}
	st.rvel = {x = 1, y = 0, z = 0}
	v:defset_status(st)
	until_(function() return math.abs(len(helio(v)) / (1000 * PC) - 1) < 1e-9 end)
	proc.wait_frames(5)
	local sunAt = len(oapi.get_globalpos(sun))
	local pa, ta, va = v:get_globalpos(), oapi.get_simtime(), len(v:get_globalvel())
	check("origin", math.abs(sunAt / (1000 * PC) - 1) < 1e-6 and len(pa) < 1e12,
		string.format("the global frame's origin moved: the Sun %.6g pc from it, the vessel %.3g m", sunAt / PC, len(pa)))
	proc.wait_frames(100)
	local pb, tb = v:get_globalpos(), oapi.get_simtime()
	local moved = len(sub(pb, pa))
	log(string.format("1000 pc out: moved %.9f m in %.4f s at %.9f m/s (%.9f m)", moved, tb - ta, va, va * (tb - ta)))
	check("precision", math.abs(moved - va * (tb - ta)) < 1e-6, "motion resolved far from the Sun")

	-- 3. back in the Solar System, 1 AU from the Sun: the origin returns,
	-- the vessels about the planets recovered from their conics; the ship-time warp
	-- holds Orbiter's time acceleration within its limit
	st = v:get_status(2)
	st.status = 0
	st.rbody = sun
	st.rpos = {x = AU, y = 0, z = 0}
	st.rvel = {x = 0, y = 0, z = 30e3}
	v:defset_status(st)
	until_(function() return math.abs(len(sub(v:get_globalpos(), oapi.get_globalpos(sun))) / AU - 1) < 1e-3 end)
	-- (steps taken since: a script's frames are not the simulation's)
	local tr = oapi.get_simtime()
	until_(function() return oapi.get_simtime() - tr > 0.2 end)
	log(string.format("back: vessel %.6f AU from the Sun, the Sun %.4g m from the origin, time acceleration %g",
		len(helio(v)) / AU, len(oapi.get_globalpos(sun)), oapi.get_tacc()))
	check("originback", len(oapi.get_globalpos(sun)) < 1e10, "the origin back at the barycentre")
	-- (low Earth orbit's energy at successive steps: exact, not 4 km coarse)
	local GM = 3.986004418e14
	local function sma()
		local r = len(sub(leo:get_globalpos(), oapi.get_globalpos(earth)))
		local w = len(sub(leo:get_globalvel(), oapi.get_globalvel(earth)))
		return 1 / (2 / r - w * w / GM), r
	end
	local a1, rr = sma()
	local t1 = oapi.get_simtime()
	until_(function() return oapi.get_simtime() > t1 end)
	local a2 = sma()
	log(string.format("low Earth orbit after the return: r %.3f km, a %.4f / %.4f km", rr / 1e3, a1 / 1e3, a2 / 1e3))
	check("recovered", math.abs(rr / 7e6 - 1) < 2e-3 and math.abs(a2 / a1 - 1) < 1e-7, "the orbit recovered exactly")
	oapi.set_shipwarp(1e6)
	proc.wait_frames(3)
	local w = oapi.get_tacc()
	check("cap", w <= 1e5 * (1 + 1e-9) and oapi.get_shipwarp() == 1e6, string.format("time acceleration %.4g in the Solar System", w))
	oapi.set_tacc(1)
	proc.wait_frames(2)
	check("settacc", oapi.get_shipwarp() == 0 and oapi.get_tacc() == 1, "setting the time acceleration ends the ship-time warp")

	-- 4. to a star: Barnard's Star (1.83 pc) at 1 g from deep space under a
	-- ship-time warp, arriving at rest where its light is the Sun's at 1 AU;
	-- on board, flip-and-burn's 2 (c/a) acosh (1 + a d / (2 c^2))
	st = v:get_status(2)
	st.status = 0
	st.rbody = sun
	st.rpos = {x = 2000 * AU, y = 0, z = 0}
	st.rvel = {x = 0, y = 0, z = 0}
	v:defset_status(st)
	until_(function() return math.abs(len(helio(v)) / (2000 * AU) - 1) < 1e-6 end)
	check("starset", oapi.reltrip_set(hv, {star = "Barnard's Star", accel = G0}), "Barnard's Star the target")
	local ri = oapi.reltrip_info(hv)
	local d0 = ri.distance
	log(string.format("to %s: %.5f ly, arriving %.4f AU from it", tostring(ri.star), d0 / (C * YEAR), ri.alt / AU))
	check("engage", oapi.reltrip_engage(hv, true), "engaged")
	local ct0, cc0 = oapi.get_simtime(), oapi.get_relstate(hv).clockofs
	oapi.set_shipwarp(1e7)
	local tw = oapi.get_systime()
	-- (Barnard's Star a body of the world, Config/Systems: held at rest
	-- against its gravity there)
	until_(function() ri = oapi.reltrip_info(hv); return ri.phase == "Arrived" or ri.phase == "Holding" or ri.phase == "No trip" end, 240)
	local trip = oapi.get_simtime() - ct0
	local ship = trip + oapi.get_relstate(hv).clockofs - cc0
	-- (as it arrived: then drifting at what speed was left, under the time warp)
	oapi.set_shipwarp(0)
	oapi.set_tacc(1)
	local expect = 2 * C / G0 * math.log(1 + G0 * (d0 - ri.alt) / (2 * C * C) + math.sqrt((1 + G0 * (d0 - ri.alt) / (2 * C * C)) ^ 2 - 1))
	log(string.format("%s after %.4f years on board (flip-and-burn %.4f), %.4f years in the Solar System, %.1f s: %.6f AU from the star (%.6f), %.3f m/s",
		ri.phase, ship / YEAR, expect / YEAR, trip / YEAR, oapi.get_systime() - tw, ri.distance / AU, ri.alt / AU, ri.speed))
	check("arrived", ri.phase == "Arrived" or ri.phase == "Holding", "arrived")
	-- (held: settled on the arrival point within minutes; the distance
	-- from the star's centre, the altitude above its surface)
	local hb = oapi.get_objhandle("Barnard")
	local R = hb and oapi.get_size(hb) or 0
	if ri.phase == "Holding" then
		local t0 = oapi.get_simtime()
		until_(function() ri = oapi.reltrip_info(hv)
			return oapi.get_simtime() - t0 > 240 end, 120)
	end
	check("there", math.abs(ri.distance - R - ri.alt) < 1e-5 * ri.alt and ri.speed < 1,
		string.format("at rest at the distance: %.1f m off, %.4f m/s", ri.distance - R - ri.alt, ri.speed))
	check("shiptime", math.abs(ship / expect - 1) < 0.01, "time on board")

	-- 5. the Solar System beyond its theories (VSOP87 and the moons'
	-- theories hold some millennia about J2000): carried on along conics
	-- from the edge of their range, without a jump there
	local J2000 = 51544.5
	local function at(mjd)
		oapi.set_simmjd(mjd)
		until_(function() return math.abs(oapi.get_simmjd() - mjd) < 0.01 end)
		-- (the bodies' states after steps taken from there)
		local t = oapi.get_simtime()
		until_(function() return oapi.get_simtime() - t > 0.1 end)
	end
	local function dist(a, b) return len(sub(oapi.get_globalpos(oapi.get_objhandle(a)), oapi.get_globalpos(oapi.get_objhandle(b)))) end
	local edge = J2000 + 3000 * 365.25
	at(edge - 0.5)
	local e0 = sub(oapi.get_globalpos(earth), oapi.get_globalpos(sun))
	local m0 = oapi.get_simmjd()
	at(edge + 0.5)
	local e1 = sub(oapi.get_globalpos(earth), oapi.get_globalpos(sun))
	local m1 = oapi.get_simmjd()
	local step = len(sub(e1, e0)) / ((m1 - m0) * 86400)
	log(string.format("Earth across the edge of its theory (MJD %.0f): %.3f km/s", edge, step / 1e3))
	check("edge", step > 28.5e3 and step < 31e3, "no jump at the edge")
	for _, yr in ipairs({30000, 1e6}) do
		at(J2000 + yr * 365.25)
		local de, dm, dj, dt = dist("Earth", "Sun") / AU, dist("Moon", "Earth"), dist("Jupiter", "Sun") / AU, dist("Titan", "Saturn")
		log(string.format("%g years from J2000: Earth %.4f AU from the Sun, the Moon %.0f km from Earth, Jupiter %.3f AU, Titan %.0f km from Saturn",
			yr, de, dm / 1e3, dj, dt / 1e3))
		check("far" .. yr, de > 0.97 and de < 1.03 and dm > 3.4e8 and dm < 4.2e8 and dj > 4.9 and dj < 5.5 and dt > 1.15e9 and dt < 1.27e9,
			"the Solar System in shape")
	end

	oapi.exit(failed and 1 or 0)
end

main()
