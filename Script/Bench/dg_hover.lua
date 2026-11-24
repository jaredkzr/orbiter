-- benchmark: a Delta-glider at night lifting off on its hover engines
-- (Tests/Bench/DGHover.scn), holding about 60 m, then climbing away on its
-- main engines; ends at BENCH_END [s] sim time after the lift-off starts
-- (default 70). From the showcase video's controller:
-- Showcase video: Delta-glider at night with some 5 % fuel, lifting off on its
-- hover engines, holding about 60 m, then the main engines lit and a steep
-- climb away as the hover engines are throttled back.
-- (vertical speed held by the hover thrust, attitude by RCS; after the main
-- engines light the pitch target rises to 60 deg, elevator and RCS blended
-- by the dynamic pressure as in dg_climb.lua)
local v = vessel.get_focusinterface()
local tend = tonumber(os.getenv("BENCH_END") or "70")
local start = tonumber(os.getenv("DG_START") or "4")
-- (close, as the lift-off clips: the splash, its light and the dust in view)
local function cam () oapi.set_cameramode({mode='track', trackmode='locked', reldist=3.5, phi=3.0, tht=-0.35}); oapi.set_cameraaperture(math.rad(25)) end
cam()
proc.wait_simdt(start)
oapi.write_log("DGHOVER: hover up at " .. oapi.get_simtime())
oapi.write_log(string.format("bench: measure at %.3f", oapi.get_simtime()))
local function clamp(x, a, b) return math.max(a, math.min(b, x)) end
local HOLD = 60          -- hover altitude [m]
local hov, main = 0, 0   -- thrust levels
local gearup = false
local phase, tPhase = 1, oapi.get_simtime()
local pitchT = 0
local logT = 0
local held = 0
local tStart = oapi.get_simtime()
while true do
	local t = oapi.get_simtime()
	if t - tStart >= tend then break end
	local dt = math.max(oapi.get_simstep(), 1e-3)
	local alt, spd = v:get_altitude(), v:get_airspeed()
	local hv = v:get_horizonairspeedvector()
	local vs = hv.y
	local w = v:get_angvel()
	local pitch, bank = math.deg(v:get_pitch()), math.deg(v:get_bank())
	local q, p = math.deg(w.x), -math.deg(w.z)
	-- vertical speed target: up at 5 m/s, easing into the hold
	local vsT = clamp((HOLD - alt) * 0.35, -2, 5)
	if phase == 1 then
		-- (spool up gently from rest)
		vsT = math.min(vsT, (t - tPhase) * 1.5)
		if alt > HOLD - 6 then held = held + dt end
		if held > 3 then phase, tPhase = 2, t; oapi.write_log("DGHOVER: main engines at " .. t) end
	end
	if phase >= 2 then
		local u = t - tPhase
		main = clamp(u / 4, 0, 1)
		-- nose up as the main engines push: to 15 deg first, then steeper as
		-- the airspeed builds and the wings carry it
		pitchT = math.min(15, u * 4)
		if spd > 60 then pitchT = math.min(60, 15 + (alt - HOLD) * 0.04) end
		-- the hover engines hold the height until the main engines and the
		-- wings take over, then fade out
		local fade = clamp((spd - 40) / 60, 0, 1)
		vsT = (1 - fade) * math.max(vsT, 0) + fade * 50
		if fade >= 1 and phase == 2 then phase = 3; oapi.write_log("DGHOVER: hover off at " .. t) end
	end
	-- hover thrust: vertical speed loop
	if phase < 3 then
		local fadeH = phase == 2 and clamp(1 - (spd - 40) / 60, 0, 1) or 1
		-- (PI: the integral carries the weight, the proportional term damps)
		local err = vsT - vs
		hov = clamp(hov + 0.12 * err * dt, 0, 1)
		v:set_thrustergrouplevel(THGROUP.HOVER, clamp(hov + 0.12 * err, 0, 1) * fadeH)
	else
		v:set_thrustergrouplevel(THGROUP.HOVER, 0)
	end
	v:set_thrustergrouplevel(THGROUP.MAIN, main)
	if not gearup and alt > 15 then oapi.simulatebufferedkey(OAPI_KEY.G); gearup = true end
	-- attitude: pitch to its target, wings level
	local up = clamp(0.12 * (pitchT - pitch) - 0.45 * q, -1, 1)
	local ub = clamp(0.05 * (0 - bank) - 0.3 * p, -1, 1)
	local dyn = v:get_dynpressure()
	local aero = clamp(dyn / 8000, 0, 1)
	local rcs = clamp(1.2 - aero, 0.15, 1)
	v:set_adclevel(AIRCTRL.ELEVATOR, up * aero)
	v:set_adclevel(AIRCTRL.AILERON, -ub * aero)
	v:set_thrustergrouplevel(THGROUP.ATT_PITCHUP, up > 0 and up * rcs or 0)
	v:set_thrustergrouplevel(THGROUP.ATT_PITCHDOWN, up < 0 and -up * rcs or 0)
	v:set_thrustergrouplevel(THGROUP.ATT_BANKLEFT, ub > 0 and ub * rcs or 0)
	v:set_thrustergrouplevel(THGROUP.ATT_BANKRIGHT, ub < 0 and -ub * rcs or 0)
	if t - logT >= 2 then
		logT = t
		oapi.write_log(string.format("DGHOVER t %.1f ph %d alt %.0f vs %.1f spd %.0f pitch %.1f (tgt %.1f) bank %.1f hov %.2f main %.2f fuel %.0f",
			t, phase, alt, vs, spd, pitch, pitchT, bank, hov, main, v:get_propellantmass(v:get_propellanthandle(0))))
	end
	proc.wait_frames(1)
end
oapi.write_log(string.format("bench: dg hover from %.1f to %.1f s", tStart, oapi.get_simtime()))
oapi.exit(0)
