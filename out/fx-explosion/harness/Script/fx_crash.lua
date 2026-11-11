-- (VkTest/fx_crash: the explosion harness' crash (out/fx-explosion/run.py), from
-- VkTest/water_entry: V1 comes down ALT [m] up, SPEED [m/s] along a path DIVE
-- [deg] below the horizon, its nose PITCH [deg] above the horizon, wings level;
-- GROUND_REL=1: ALT above the terrain there (on land; else above the mean radius);
-- logs every LOGDT s; EXIT_T [s]: exits after)
proc.wait_frames(2)
local earth = oapi.get_objhandle("Earth")
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
st.rvel = vec.add(st.rvel, vec.mul(path, speed))
-- (the orientation in the status too: a landed vessel's status has it in the planet's frame)
st.arot = {x=math.atan2(R.m23, R.m33), y=-math.asin(R.m13), z=math.atan2(R.m12, R.m11)}
A:defset_status(st)
proc.wait_frames(1)
A:set_rotationmatrix(R)
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
	camGround = {mode="ground", ref="Earth", lng=math.deg(ce.lng), lat=math.deg(ce.lat), alt=tonumber(os.getenv("CAM_Y") or "4")}
	-- (CAM_PHI, CAM_THT [deg]: a fixed view direction, from east towards north; up)
	if os.getenv("CAM_PHI") then camGround.phi = math.rad(tonumber(os.getenv("CAM_PHI"))); camGround.tht = math.rad(tonumber(os.getenv("CAM_THT") or "0")) end
	oapi.set_cameramode(camGround)
	if os.getenv("CAM_FOV") then oapi.set_cameraaperture(math.rad(tonumber(os.getenv("CAM_FOV"))) / 2) end
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
	local w = A:get_waterstate()
	-- (HOLD: the pilot holds the attitude, wings level, until the water is touched)
	-- (on land: until it is near the ground)
	if os.getenv("GROUND_REL") and not firstWet and A:get_altitude(ALTMODE.GROUND) < 3 then firstWet = t end
	if os.getenv("HOLD") and not w.wet and not firstWet then A:set_rotationmatrix(R); A:set_angvel({x=0, y=0, z=0}) end
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
