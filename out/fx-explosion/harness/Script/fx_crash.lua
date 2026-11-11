-- (VkTest/fx_crash: the explosion harness' crash (out/fx-explosion/run.py), from
-- VkTest/water_entry: V1 comes down ALT [m] up, SPEED [m/s] along a path DIVE
-- [deg] below the horizon, its nose PITCH [deg] above the horizon, wings level;
-- GROUND_REL=1: ALT above the terrain there (on land; else above the mean radius);
-- logs every LOGDT s; EXIT_T [s]: exits after)
proc.wait_frames(2)
local PLANET = os.getenv("PLANET") or "Earth" -- (the body crashed on: Earth, Moon, Mars - VACUUM_PLAN V0)
local earth = oapi.get_objhandle(PLANET)
local A = vessel.get_interface("V1")
local function L(s) oapi.write_log("entry: " .. s) end
local alt = tonumber(os.getenv("ALT") or "20")
local speed = tonumber(os.getenv("SPEED") or "70")
local dive = math.rad(tonumber(os.getenv("DIVE") or "3"))
local pitch = math.rad(tonumber(os.getenv("PITCH") or "6"))
local function gpos(v) return v:get_globalpos() end
local g = gpos(A)
local up = vec.unit(vec.sub(g, oapi.get_globalpos(earth)))
local x = vec.sub(A:local2global({x=1, y=0, z=0}), g)
local z = vec.sub(A:local2global({x=0, y=0, z=1}), g)
local yv = vec.crossp(z, x)
if vec.dotp(yv, up) < 0 then yv = vec.mul(yv, -1) end
local function tilt(a) return vec.add(vec.mul(z, math.cos(a)), vec.mul(yv, math.sin(a))), vec.sub(vec.mul(yv, math.cos(a)), vec.mul(z, math.sin(a))) end
local path = tilt(-dive)
local zA, yA = tilt(pitch)
local R = {m11=x.x, m12=yA.x, m13=zA.x, m21=x.y, m22=yA.y, m23=zA.y, m31=x.z, m32=yA.z, m33=zA.z}
local st = A:get_status(2)
local e0 = oapi.global_to_equ(earth, g)
st.status = 0; st.vrot = {x=tonumber(os.getenv("VROTX") or "0"), y=0, z=0}
local elev = 0
if os.getenv("GROUND_REL") then elev = oapi.surface_elevation(earth, e0.lng, e0.lat) end
L(string.format("terrain elevation %.1f m", elev))
st.rpos = vec.add(st.rpos, vec.mul(up, alt + elev - (e0.rad - oapi.get_size(earth))))
local ground = st.rvel
st.rvel = vec.add(st.rvel, vec.mul(path, speed))
-- (the orientation in the status too: a landed vessel's status has it in the planet's frame)
st.arot = {x=math.atan2(R.m23, R.m33), y=-math.asin(R.m13), z=math.atan2(R.m12, R.m11)}
A:defset_status(st)
-- (V2, if there is one: the same dive SEP m to the side - two fireballs)
local B = nil
pcall(function() B = vessel.get_interface("V2") end)
if B then
	local st2 = B:get_status(2)
	st2.rpos = vec.add(st.rpos, vec.mul(x, tonumber(os.getenv("SEP") or "90")))
	-- (SEP_SPEED: V2's own speed - the water entry's outcome varies with the wave it meets)
	st2.rvel = vec.add(ground, vec.mul(path, tonumber(os.getenv("SEP_SPEED") or tostring(speed)))); st2.arot = st.arot; st2.vrot = st.vrot; st2.status = 0
	B:defset_status(st2)
	L(string.format("V2 placed %.1f m to the side", tonumber(os.getenv("SEP") or "90")))
end
proc.wait_frames(1)
A:set_rotationmatrix(R)
if B then B:set_rotationmatrix(R) end
if os.getenv("VROTX") then A:set_angvel({x=tonumber(os.getenv("VROTX")), y=0, z=0}) end
if os.getenv("CAM_X") then
	-- a fixed observer CAM_Z m ahead along the path and CAM_X m to the side, CAM_Y up
	-- (from where it is now: the global frame moves ~600 m a step with the Earth)
	local g2 = gpos(A)
	local up2 = vec.unit(vec.sub(g2, oapi.get_globalpos(earth)))
	local e2 = oapi.global_to_equ(earth, g2)
	local base = vec.sub(g2, vec.mul(up2, e2.rad - oapi.get_size(earth)))
	local c = vec.add(vec.add(base, vec.mul(z, tonumber(os.getenv("CAM_Z") or "60"))), vec.add(vec.mul(x, tonumber(os.getenv("CAM_X"))), vec.mul(up2, tonumber(os.getenv("CAM_Y") or "4"))))
	local ce = oapi.global_to_equ(earth, c)
	camGround = {mode="ground", ref=PLANET, lng=math.deg(ce.lng), lat=math.deg(ce.lat), alt=tonumber(os.getenv("CAM_Y") or "4")}
	-- (CAM_PHI, CAM_THT [deg]: a fixed view direction, from east towards north; up)
	if os.getenv("CAM_PHI") then camGround.phi = math.rad(tonumber(os.getenv("CAM_PHI"))); camGround.tht = math.rad(tonumber(os.getenv("CAM_THT") or "0")) end
	oapi.set_cameramode(camGround)
	if os.getenv("CAM_FOV") then oapi.set_cameraaperture(math.rad(tonumber(os.getenv("CAM_FOV"))) / 2) end
	-- (CUT_X/Z/Y, CUT_PHI/THT: a second observer the view cuts to CUT_T s after the break-up - run.py --cut)
	if os.getenv("CUT_X") then
		local cc = vec.add(vec.add(base, vec.mul(z, tonumber(os.getenv("CUT_Z") or "60"))), vec.add(vec.mul(x, tonumber(os.getenv("CUT_X"))), vec.mul(up2, tonumber(os.getenv("CUT_Y") or "4"))))
		local cce = oapi.global_to_equ(earth, cc)
		camCut = {mode="ground", ref=PLANET, lng=math.deg(cce.lng), lat=math.deg(cce.lat), alt=tonumber(os.getenv("CUT_Y") or "4"),
			phi=math.rad(tonumber(os.getenv("CUT_PHI") or "0")), tht=math.rad(tonumber(os.getenv("CUT_THT") or "0"))}
	end
	-- (camSetDist(d): the observer moved out along its bearing to d m from the crash point; PULL_T, PULL_D0,
	-- PULL_D1, PULL_DUR: from PULL_T s after the break-up, from D0 to D1 m geometrically over DUR s - run.py --pull)
	-- (in longitude and latitude from the start point: a global position goes stale at once - the frame
	-- moves 30 km/s with the Earth; the offset scaled, flat over these few km)
	local cx, cz = tonumber(os.getenv("CAM_X")), tonumber(os.getenv("CAM_Z") or "60")
	local d0 = math.sqrt(cx * cx + cz * cz)
	local eBase = oapi.global_to_equ(earth, base)
	local dLng, dLat = ce.lng - eBase.lng, ce.lat - eBase.lat
	function camSetDist(d)
		local k = d / d0
		camGround.lng, camGround.lat = math.deg(eBase.lng + dLng * k), math.deg(eBase.lat + dLat * k)
		oapi.set_cameramode(camGround)
	end
end
if os.getenv("ELEVATOR") then A:set_adclevel(AIRCTRL.ELEVATOR, tonumber(os.getenv("ELEVATOR"))) end
local e00 = oapi.global_to_equ(earth, gpos(A))
local function dist() local e = oapi.global_to_equ(earth, gpos(A)); local R = oapi.get_size(earth); return R * math.sqrt(((e.lat - e00.lat))^2 + ((e.lng - e00.lng) * math.cos(e.lat))^2) end
local t0 = oapi.get_simtime()
local logdt = tonumber(os.getenv("LOGDT") or "0.25")
local tl, peak, firstWet = -1, 0, nil
local exitT = tonumber(os.getenv("EXIT_T") or "0")
while true do
	proc.wait_frames(1)
	local t = oapi.get_simtime() - t0
	-- (CAM_RESET_T: the observer put back then - a vessel destroyed takes the camera to its core)
	if camGround and os.getenv("CAM_RESET_T") and not camReset and t > tonumber(os.getenv("CAM_RESET_T")) then camReset = true; oapi.set_cameramode(camGround) end
	-- (CAM_HOLD_T: the observer re-set every frame until then - no wreck view)
	if camGround and os.getenv("CAM_HOLD_T") and t < tonumber(os.getenv("CAM_HOLD_T")) then oapi.set_cameramode(camGround) end
	-- (the break-up: the first frame with three pieces of debris or more, as run.py; the cut and the pull-away from it)
	if not tbreak and oapi.get_debriscount() >= 3 then tbreak = t end
	if tbreak and camGround then
		local cutT, pullT = tonumber(os.getenv("CUT_T") or "-1"), tonumber(os.getenv("PULL_T") or "-1")
		if camCut and not cutDone and cutT >= 0 and t - tbreak >= cutT then camGround = camCut; cutDone = true; L(string.format("camera cut to the second observer at +%.2f s", t - tbreak)) end
		if pullT >= 0 and t - tbreak >= pullT then
			local d0, d1, dur = tonumber(os.getenv("PULL_D0") or "170"), tonumber(os.getenv("PULL_D1") or "8000"), tonumber(os.getenv("PULL_DUR") or "4")
			local f = math.min((t - tbreak - pullT) / dur, 1)
			camSetDist(d0 * (d1 / d0) ^ f)
		end
	end
	if B and math.floor(t / logdt) ~= math.floor((t - oapi.get_simstep()) / logdt) then
		local gb = gpos(B); local eb = oapi.global_to_equ(earth, gb)
		L(string.format("V2 t %.2f alt %.1f dist %.1f", t, eb.rad - oapi.get_size(earth), vec.length(vec.sub(gb, gpos(A)))))
	end
	local w = A:get_waterstate()
	-- (HOLD: the pilot holds the attitude, wings level, until the water is touched)
	-- (on land: until it is near the ground)
	if os.getenv("GROUND_REL") and not firstWet and A:get_altitude(ALTMODE.GROUND) < 3 then firstWet = t end
	if os.getenv("HOLD") and not w.wet and not firstWet then A:set_rotationmatrix(R); A:set_angvel({x=0, y=0, z=0}) end
	if B and os.getenv("HOLD") and not firstWet then pcall(function() B:set_rotationmatrix(R); B:set_angvel({x=0, y=0, z=0}) end) end
	peak = math.max(peak, w.accmax)
	if w.wet and not firstWet then firstWet = t end
	if os.getenv("TRACE") and t < tonumber(os.getenv("TRACE")) then
		local f = w.force
		local up = vec.unit(vec.sub(gpos(A), oapi.get_globalpos(earth)))
		local vel = vec.sub(A:get_globalvel(), oapi.get_globalvel(earth))
		local fu = vec.dotp(f, up) / A:get_mass() / 9.81
		local hv = vec.sub(vel, vec.mul(up, vec.dotp(vel, up)))
		local fd = -vec.dotp(f, vec.unit(hv)) / A:get_mass() / 9.81
		local av = A:get_angvel()
		local e = oapi.global_to_equ(earth, gpos(A))
		L(string.format("TR t %5.2f h %5.2f pitch %6.2f q %6.2f lift %6.2f g drag %6.2f g wet %5.1f m2 sub %5.2f n %d entry %4.1f", t, e.rad - oapi.get_size(earth), math.deg(A:get_pitch()), math.deg(av.x), fu, fd, w.wetted, w.submerged, w.substeps, w.entryspeed))
	end
	if t - tl >= logdt then
		tl = t
		local e = oapi.global_to_equ(earth, gpos(A))
		local gv = A:get_groundspeedvector(REFFRAME.HORIZON)
		L(string.format("t %6.2f h %7.2f vh %6.2f vv %6.2f pitch %6.1f bank %6.1f wet %s sub %6.2f/%6.1f acc %6.1f g entry %5.1f slam %6.0f kPa dist %7.1f n %d",
			t, e.rad - oapi.get_size(earth), math.sqrt(gv.x^2 + gv.z^2), gv.y, math.deg(A:get_pitch()), math.deg(A:get_bank()),
			tostring(w.wet), w.submerged, w.volume, w.accmax / 9.81, w.entryspeed, w.slammax / 1000, dist(), w.substeps))
	end
	if os.getenv("DEBRIS_WATCH") and math.abs(t * 2 - math.floor(t * 2 + 0.5)) < 0.011 then
		for i = 0, oapi.get_debriscount() - 1 do
			local d = oapi.get_debris(i)
			local v = vec.length(vec.sub(oapi.get_globalvel(d), oapi.get_globalvel(earth)))
			local e = oapi.global_to_equ(earth, oapi.get_globalpos(d))
			if v > 600 or e.rad - oapi.get_size(earth) > 200 then L(string.format("DEBRIS t %.1f #%d %s v %.0f h %.0f", t, i, oapi.get_debrisinfo(d).name, v, e.rad - oapi.get_size(earth))) end
		end
	end
	if exitT > 0 and t > exitT then
		L(string.format("peak water acc %.1f g, first wet at %.2f s, distance %.1f m", peak / 9.81, firstWet or -1, dist()))
		oapi.exit(0)
	end
end
