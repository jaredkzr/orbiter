-- The trailer shot of a black hole's tides (Tests/Audio/TrailerTides.scn):
-- a delta-glider lowered nose-first along the spin axis of a 600 solar-mass
-- hole (its shadow and ring ~40 deg across where the tides tear it), the
-- damage model on, the sound cinematic. Held by script - its state set each
-- step at rest, no thrust (that would load the structure) - so the tidal
-- load on its core rises steadily, as (R_CORE / r)^3:
--   A  outside, wide: the glider small against the hole, its ring and disc
--   B  the cockpit: the hole ahead through the windscreen, the creaks
--      quickening; the first part torn off (its tear through the hull), the
--      structure groaning
--   C  outside, behind it, the hole beyond: its parts torn off one by one
--   D  slow motion: the core's limit, broken up - the camera falling with
--      the wreck, the hole behind it (held at rest the pieces would drop away
--      at ~5e8 m/s^2: gone in milliseconds), the pieces drifting apart as the
--      shadow swells and swallows them; the end
local hole = oapi.get_objhandle("TrailerHole")
local v = vessel.get_interface("TIDE")
local function log(s) oapi.write_log("TrailerTides: " .. s) end
local function env(n, d) return tonumber(os.getenv(n) or "") or d end
-- the core torn apart at R_CORE: the 10 solar-mass calibration (3,275 km,
-- Scenario.BlackHoleTidesTest) scaled as M^1/3
local R_CORE = 3.275e6 * (oapi.get_mass(hole) / 1.98847e31) ^ (1 / 3)
local T_PRE = env("TT_PRE", 6.0)        -- [s] a pre-roll, held still (the music's quiet start; render.py trims it off)
local T_A = env("TT_TA", 6.0)            -- [s] the establishing shot
local T_B = env("TT_TB", 16.0)           -- [s] the cockpit, to LOAD_B
local LOAD_A, LOAD_B = 0.10, env("TT_LB", 0.6)
local HOLD_IN = env("TT_HOLD", 2.0)      -- [s] in the cockpit after the first tear
local T_C = env("TT_TC", 7.0)            -- [s] outside, to LOAD_C (parts torn off one by one)
local LOAD_C = env("TT_LC", 0.80)        -- (slowed from here: the core gives at about 0.9-0.95)
local SLOW = env("TT_SLOW", 0.015)       -- the time acceleration of the break-up (below 0.1: ORBITER_MIN_WARP)
local AFTER = env("TT_AFTER", 8.0)       -- [s of the session] after it broke up, at the most (the end: the wreck at the horizon)
local function rFor(load) return R_CORE * load ^ (-1.0 / 3.0) end
local function lost()
	local n = 0
	for i = 0, v:get_partcount() - 1 do
		if v:get_partinfo(i).flags % 2 == 1 then n = n + 1 end
	end
	return n
end

proc.skip(); proc.skip()
-- its approach: EL above the disc's plane (the spin axis +y, the disc in x-z;
-- the disc then seen nearly edge-on, its far side lensed over the shadow),
-- nose to the hole, its top towards the axis
local EL = math.rad(env("TT_EL", 15))
local ce, se = math.cos(EL), math.sin(EL)
local R = {m11 = 0, m12 = -se, m13 = -ce, m21 = 0, m22 = ce, m23 = -se, m31 = 1, m32 = 0, m33 = 0}
local st = v:get_status(2)
st.status = 0
st.rbody = hole
st.vrot = {x = 0, y = 0, z = 0}
st.arot = {x = math.atan2(R.m23, R.m33), y = -math.asin(R.m13), z = math.atan2(R.m12, R.m11)}
local function place(r)
	st.rpos = {x = r * ce, y = r * se, z = 0}
	st.rvel = {x = 0, y = 0, z = 0}   -- (at rest: hovering; broken up, its pieces fall from rest)
	v:defset_status(st)
	v:set_rotationmatrix(R)
end
local function cam(mode) local ok, e = pcall(oapi.set_cameramode, mode); if not ok then log("camera: " .. tostring(e)) end end
local function camDist()
	local h = oapi.get_objhandle("TIDE")
	if not h then return -1 end
	return vec.length(vec.sub(oapi.get_cameraglobalpos(), oapi.get_globalpos(h)))
end

-- (the HUD folded away - Ctrl+H: its glass in the middle of the windscreen)
v:send_bufferedkey(OAPI_KEY.H, {ctrl = true})
local t0 = oapi.get_simtime()
place(rFor(LOAD_A))
log(string.format("R_CORE %.0f km; held from %.0f km (load %.2f); horizon %.0f km", R_CORE / 1e3, rFor(LOAD_A) / 1e3, LOAD_A, oapi.get_size(hole) / 1e3))
-- A: wide, pushing in
local A_D0, A_D1 = env("TT_AD0", 40), env("TT_AD1", 10)   -- (camera distance, in the glider's sizes)
-- (absdir: behind it along its nose, turned PH, TH off: the hole beside it in view)
local A_PH, A_TH = math.rad(env("TT_APH", 9)), math.rad(env("TT_ATH", 1))
cam({mode = 'track', trackmode = 'absdir', reldist = A_D0, phi = A_PH, tht = A_TH})

local phase, tB, tFirst, tCut, tSlow, loadCut = "P"
local nextLog = 0
local ok, err = pcall(function ()
	while true do
		local t = oapi.get_simtime() - t0
		if not oapi.get_objhandle("TIDE") or v:get_partinfo(0).flags % 2 == 1 then break end
		local load
		if phase == "P" then
			load = LOAD_A
			if t >= T_PRE then phase = "A"; log(string.format("A: the establishing shot at %.2f s", t)) end
		elseif phase == "A" then
			local ta = t - T_PRE
			load = LOAD_A * (1.0 + 0.2 * ta / T_A)
			cam({mode = 'track', trackmode = 'absdir', reldist = A_D0 + (A_D1 - A_D0) * (ta / T_A), phi = A_PH - math.rad(4) * ta / T_A, tht = A_TH})
			if ta >= T_A then
				phase, tB = "B", t
				cam({mode = 'cockpit', cockpitmode = 'vc'})
				log(string.format("B: the cockpit at %.2f s", t))
			end
		elseif phase == "B" then
			local l0 = LOAD_A * 1.2
			load = l0 * (LOAD_B / l0) ^ math.min((t - tB) / T_B, 1.0)
			if not tFirst and lost() > 0 then tFirst = t; log(string.format("first part torn off at %.2f s, %.0f km (load %.2f)", t, rFor(load) / 1e3, load)) end
			if (tFirst and t >= tFirst + HOLD_IN) or (t - tB >= T_B + 4.0) then
				phase, tCut, loadCut = "C", t, load
				cam({mode = 'track', trackmode = 'absdir', reldist = env("TT_CD", 4.5), phi = math.rad(env("TT_CPH", 14)), tht = math.rad(env("TT_CTH", 2))})
				log(string.format("C: outside at %.2f s (load %.2f, %d parts lost)", t, load, lost()))
			end
		elseif phase == "C" then
			load = loadCut * (LOAD_C / loadCut) ^ math.min((t - tCut) / T_C, 1.0)
			if t - tCut >= T_C then
				phase, tSlow = "D", t
				oapi.set_tacc(SLOW)
				log(string.format("D: slow motion (%.2f) at %.2f s (load %.2f, %d parts lost)", SLOW, t, load, lost()))
			end
		else
			-- (slowed: on to the core's limit over a little of the simulation)
			load = LOAD_C * (1.2 / LOAD_C) ^ math.min((t - tSlow) / env("TT_TD", 0.03), 1.0)
		end
		place(rFor(load))
		if t >= nextLog then
			log(string.format("t %.3f %s r %.0f km load %.2f lost %d cam %.0f m", t, phase, rFor(load) / 1e3, load, lost(), camDist()))
			nextLog = t + (phase == "D" and 0.01 or 1.0)
		end
		proc.skip()
	end
end)
if not ok then log("the vessel is gone (" .. tostring(err) .. ")") end
local tGone = oapi.get_simtime() - t0
if not tSlow then oapi.set_tacc(SLOW) end
log(string.format("broken up at %.3f s (%d parts lost)", tGone, lost()))
-- E: falling with the wreck, the hole behind it (its view would back far
-- off to frame the wreck: the camera put close again, on the line from the
-- hole through the wreck)
local w0, s0, k = oapi.get_systime(), oapi.get_simtime(), 0
local rh, tIn = oapi.get_size(hole), nil
while oapi.get_systime() - w0 < AFTER and not (tIn and oapi.get_systime() - tIn > env("TT_TAIL", 0.8)) do
	local tg = oapi.get_cameratarget()
	if not tIn and tg and vec.length(vec.sub(oapi.get_globalpos(tg), oapi.get_globalpos(hole))) < 1.3 * rh then
		tIn = oapi.get_systime(); log(string.format("E: the wreck at the horizon at %.2f s of the session", tIn - w0))
	end
	if k < 3 then cam({mode = 'track', trackmode = 'absdir', reldist = env("TT_ED", 7.0), phi = math.rad(env("TT_EPH", 12)), tht = math.rad(env("TT_ETH", 3))}) end
	k = k + 1
	if k % 15 == 0 then
		log(string.format("E: %.2f s of the session, %.4f s simulated, target %s, cam %.0f m from it, %.0f km from the hole",
			oapi.get_systime() - w0, oapi.get_simtime() - s0, tg and oapi.get_objname(tg) or "-",
			tg and vec.length(vec.sub(oapi.get_cameraglobalpos(), oapi.get_globalpos(tg))) or -1,
			vec.length(vec.sub(oapi.get_cameraglobalpos(), oapi.get_globalpos(hole))) / 1e3))
	end
	proc.skip()
end
log(string.format("the end: %d frames over %.2f s of the session, %.4f s simulated", k, oapi.get_systime() - w0, oapi.get_simtime() - s0))
oapi.exit(0)
