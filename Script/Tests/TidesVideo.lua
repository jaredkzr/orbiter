-- A black hole's tides heard and seen (Tests/Audio/TidesVideo.scn): a
-- delta-glider lowered by script towards a 10 solar-mass hole, the damage
-- model on. In free fall the tides go from felt to tearing in well under a
-- second; here it is held and lowered step by step (its state set each
-- frame: no thrust - that would load the structure), so the tidal load on
-- its core rises steadily, as (R_CORE / r)^3:
--   from the cockpit: the load rising through KNOTS - the creaks quicker,
--   the first parts torn off (the tears through the hull), the groans and
--   the structure's groaning bed as the core nears its limit; at CUT_LOAD
--   from outside (a track camera close behind): on to the core's limit -
--   broken up - the pieces flying apart (they fall into the hole within
--   some 0.2 s of simulation, its steps then tiny), the end.
local hole = oapi.get_objhandle("SmallHole")
local v = vessel.get_interface("TIDE")
local function log(s) oapi.write_log("TidesVideo: " .. s) end
local function env(n, d) return tonumber(os.getenv(n) or "") or d end
local R_CORE = env("TV_RCORE", 3.275e6)   -- [m] the core torn apart (Scenario.BlackHoleTidesTest: free fall)
-- the core's tidal load (of its limit, by R_CORE) at times [s]: the first
-- parts go at about 0.2-0.5 held (earlier than falling free); creaks
-- quicken, groans from about 0.6
local KNOTS = {{0, 0.15}, {8, 0.25}, {env("TV_T2", 24), env("TV_L2", 0.85)}}
local LOAD0 = KNOTS[1][2]
local CUT_LOAD = KNOTS[3][2]             -- the cut to outside
local FINAL = 3.0                        -- [s] from the cut past the core's limit (to 1.1)
local function loadAt(t)
	for i = 2, #KNOTS do
		local a, b = KNOTS[i - 1], KNOTS[i]
		if t <= b[1] then return a[2] * (b[2] / a[2]) ^ ((t - a[1]) / (b[1] - a[1])) end
	end
	return KNOTS[#KNOTS][2]
end
local function rFor(load) return R_CORE * load ^ (-1.0 / 3.0) end
local function lost()
	local n = 0
	for i = 0, v:get_partcount() - 1 do
		if v:get_partinfo(i).flags % 2 == 1 then n = n + 1 end
	end
	return n
end

proc.skip(); proc.skip()
oapi.set_cameramode({mode = 'cockpit', cockpitmode = 'vc'})   -- (the virtual cockpit: the cabin seen, not stars only)
local st = v:get_status(2)
st.status = 0
st.rbody = hole
st.vrot = {x = 0, y = 0, z = 0}
local function place(r, vr)
	st.rpos = {x = 0, y = r, z = 0}
	st.rvel = {x = 0, y = -vr, z = 0}
	v:defset_status(st)
end
local t0 = oapi.get_simtime()
local r = rFor(LOAD0)
place(r, 0)
log(string.format("lowered from %.0f km (tidal load %.2f of the core's limit)", r / 1e3, LOAD0))

local tFirst, tCut, cutLoad, tGone
local gone = false
local nextLog = 0
-- (destroyed: a ghost kept as the focus - its handle stays)
oapi.write_log("TidesVideo: watching")
local ok, err = pcall(function ()
	while true do
		local t = oapi.get_simtime() - t0
		-- (destroyed: the camera turns to its wreck - a ghost of it stays the focus)
		if tCut and oapi.get_cameratarget() ~= oapi.get_objhandle("TIDE") then gone = true end
		if not oapi.get_objhandle("TIDE") or v:get_partinfo(0).flags % 2 == 1 or gone then break end
		local load
		if not tCut then
			load = loadAt(t)
		else
			load = cutLoad * (1.1 / cutLoad) ^ math.min((t - tCut) / FINAL, 1.0)
		end
		local rn = rFor(load)
		local dt = math.max(oapi.get_simstep(), 1e-6)
		place(rn, math.max(0.0, (r - rn) / dt))
		r = rn
		if not tFirst and lost() > 0 then
			tFirst = t
			log(string.format("first part torn off at %.2f s, %.0f km (load %.2f)", t, r / 1e3, load))
		end
		if not tCut and load >= CUT_LOAD - 1e-6 then
			tCut, cutLoad = t, load
			oapi.set_cameramode({mode = 'track', trackmode = 'targetrelative', reldist = 0.0025, phi = math.rad(160), tht = math.rad(12)})
			log(string.format("cut to outside at %.2f s (load %.2f, %d parts lost)", t, load, lost()))
		end
		if t >= nextLog then
			log(string.format("t %.2f r %.0f km load %.2f lost %d", t, r / 1e3, load, lost()))
			nextLog = t + 1.0
		end
		proc.skip()
	end
end)
if not ok then log("the vessel is gone (" .. tostring(err) .. ")") end
tGone = oapi.get_simtime() - t0
log(string.format("broken up at %.3f s", tGone))
-- the pieces flying apart (near the hole the simulation's steps get tiny:
-- slowed by itself, about a fifth), for AFTER s of the session's clock,
-- then the end (render.py cuts the clip where the wreck has fallen into the
-- hole's shadow and the picture goes black)
local AFTER = env("TV_AFTER", 2.5)
local SLOW = env("TV_SLOW", 0.2)   -- (time at the break: at 1 the pieces are gone in some 6 frames, at 0.1 they hang still for seconds)
oapi.set_tacc(SLOW)
-- (the camera on the wreck, near: destroyed, the vessel's view backs far off
-- to frame its wreck - the pieces then specks)
pcall(oapi.set_cameramode, {mode = 'track', trackmode = 'targetrelative', reldist = env("TV_REDIST", 0.012), phi = math.rad(160), tht = math.rad(12)})
local w0, s0, k = oapi.get_systime(), oapi.get_simtime(), 0
while oapi.get_systime() - w0 < AFTER do
	k = k + 1
	if k % 30 == 0 then log(string.format("after: %.2f s of the session, %.4f s simulated, tacc %.2f", oapi.get_systime() - w0, oapi.get_simtime() - s0, oapi.get_tacc())) end
	proc.skip()
end
log(string.format("the end: %d cycles over %.2f s of the session, %.4f s simulated", k, oapi.get_systime() - w0, oapi.get_simtime() - s0))
oapi.exit(0)
