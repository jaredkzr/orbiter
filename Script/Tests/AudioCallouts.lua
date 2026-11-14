-- The voice's callouts (Tests/Audio/Callouts.scn, rendered with
-- ORBITER_AUDIO_RENDER, ORBITER_AUDIO_CALLOUTS=1): a delta-glider on KSC's
-- runway, from its cockpit.
--   1-9 s    modes: RCS rotation, translation, off; autopilot (hold level)
--            on and off; aerodynamic controls on and off
--   9 s      main engines full: "100 knots", rotate at 100 m/s, liftoff
--            ("wheels up", the DG's cfg); the gear up at 40 m (its cfg's
--            voice: "gear up", "gear up and locked")
--   then     set at 700 m, sinking 35 m/s, the gear up: the heights from
--            600 m down, "warning, terrain" (closing fast), "warning, gear is up"
--            below 275 m
--   then     set at 8 km at Mach 1.1, engines off: "Mach 1", slowing through
--            it: "subsonic"
--   then     at 60 km, climbing, the main engines on for a second: "main
--            engine cutoff"; at 300 km, faster than a circular orbit: "orbit
--            achieved"
-- Checks the callouts said (audio.callouts), PASS/FAIL in the log; the exit
-- code.
local failed = false
local function check(name, cond)
	oapi.write_log("AudioCallouts: " .. (cond and "PASS " or "FAIL ") .. name)
	if not cond then failed = true end
end
local earth = oapi.get_objhandle("Earth")
local v = vessel.get_interface("CO")
local function at(t, f) while oapi.get_simtime() < t do if f then f() end; proc.skip() end end
local function gpos() return oapi.get_globalpos(v:get_handle()) end
local function said(part)
	for _, f in ipairs(audio.callouts()) do if string.find(string.lower(f), string.lower(part), 1, true) then return true end end
	return false
end

-- the runway's frame at the start (level; north-north-west)
local fwd0 = vec.sub(v:local2global({x=0, y=0, z=1}), gpos())
local x0 = vec.sub(v:local2global({x=1, y=0, z=0}), gpos())
local y0 = vec.sub(v:local2global({x=0, y=1, z=0}), gpos())
local R0 = {m11=x0.x, m12=y0.x, m13=fwd0.x, m21=x0.y, m22=y0.y, m23=fwd0.y, m31=x0.z, m32=y0.z, m33=fwd0.z}
local ground = v:get_status(2).rvel -- (the ground's velocity: the Earth turning)

-- set the vessel at a height, level, at a speed forward, sinking
local function release(h, spd, vs)
	local up = vec.unit(vec.sub(gpos(), oapi.get_globalpos(earth)))
	local st = v:get_status(2)
	st.status = 0
	st.arot = {x=math.atan2(R0.m23, R0.m33), y=-math.asin(R0.m13), z=math.atan2(R0.m12, R0.m11)}
	st.rpos = vec.add(st.rpos, vec.mul(up, h - v:get_altitude()))
	st.rvel = vec.add(ground, vec.add(vec.mul(fwd0, spd), vec.mul(up, vs)))
	st.vrot = {x = 0, y = 0, z = 0}
	v:defset_status(st)
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() == t0 do proc.skip() end
	v:set_rotationmatrix(R0)
end

-- modes
-- (1.5 s apart: a line is about a second)
at(1.0); v:set_rcsmode(2)
at(2.5); v:set_rcsmode(1)
at(4.0); v:set_navmode(NAVMODE.HLEVEL, true)
at(5.5); v:set_navmode(NAVMODE.HLEVEL, false)
at(7.0); v:set_adcmode(7)
at(8.5); v:set_adcmode(0)

-- takeoff
at(9.0); v:set_thrustergrouplevel(THGROUP.MAIN, 1.0)
local gearUp = false
local function fly()
	local spd, alt = v:get_airspeed(), v:get_altitude()
	local pitch = math.deg(v:get_pitch())
	local q = math.deg(v:get_angvel().x)
	local tp = (spd > 100 or alt > 8) and 12 or 0
	v:set_adclevel(AIRCTRL.ELEVATOR, math.max(-1, math.min(1, 0.12 * (tp - pitch) - 0.45 * q)))
	if not gearUp and alt > 40 then oapi.simulatebufferedkey(OAPI_KEY.G); gearUp = true end
end
while oapi.get_simtime() < 60 and (v:get_altitude() < 250 or v:get_animation(7) > 0) do fly(); proc.skip() end
oapi.write_log(string.format("AudioCallouts: climbing at %.0f m, %.0f m/s, t %.1f", v:get_altitude(), v:get_airspeed(), oapi.get_simtime()))
check("RCS modes called", said("Rotation") and said("Translation"))
check("autopilot mode called by name, and off", said("Hold Level") and said("Autopilot Off"))
check("aerodynamic controls off and on called", said("/Off.wav") and said("/On.wav"))
check("100 knots called", said("100 Knots"))
check("liftoff called (the DG's: wheels up)", said("Wheels Up"))
check("positive rate called (a runway takeoff)", said("Positive Rate"))
check("gear up called (the DG's cfg: its animation)", said("Gear Up"))
check("no ignition for a plane", not said("Ignition"))

-- a descent with the gear up
v:set_thrustergrouplevel(THGROUP.MAIN, 0.0)
release(700, 90, -35)
local t1 = oapi.get_simtime()
while v:get_altitude() > 110 and oapi.get_simtime() < t1 + 30 do proc.skip() end
oapi.write_log(string.format("AudioCallouts: down to %.0f m, t %.1f", v:get_altitude(), oapi.get_simtime()))
check("heights called (500 m)", said("/500.wav"))
check("terrain warning called (sinking 35 m/s near the ground)", said("Warning Terrain"))
check("gear warning called", said("Warning Gear is Up"))

-- through Mach 1, slowing
release(8000, 340, 0)
local t2 = oapi.get_simtime()
oapi.write_log(string.format("AudioCallouts: Mach %.2f at %.0f m", v:get_machnumber(), v:get_altitude()))
while oapi.get_simtime() < t2 + 40 and v:get_machnumber() > 0.97 do proc.skip() end
at(oapi.get_simtime() + 2.0)
oapi.write_log(string.format("AudioCallouts: Mach %.2f, t %.1f", v:get_machnumber(), oapi.get_simtime()))
check("Mach 1 called", said("Mach 1."))
check("subsonic called", said("subsonic"))

-- the ascent: the main engines cut high up (MECO); in orbit
release(60000, 3000, 100)
v:set_thrustergrouplevel(THGROUP.MAIN, 1.0)
at(oapi.get_simtime() + 1.0)
v:set_thrustergrouplevel(THGROUP.MAIN, 0.0)
at(oapi.get_simtime() + 2.5)
check("MECO called", said("MECO"))
release(300000, 8000, 0)
at(oapi.get_simtime() + 3.0)
check("orbit called", said("Orbit"))

for i, f in ipairs(audio.callouts()) do oapi.write_log("AudioCallouts: said " .. f) end
oapi.exit(failed and 1 or 0)
