-- (VkTest/iss_hit: GL-01 flies nose first into the ISS at SPEED [m/s]
-- relative, along DIR (ISS frame, default from the front: 0 0 -1), its nose
-- meeting AIM (ISS frame) T [s] later. Camera: track, absolute direction,
-- on the ISS (CAM_R sizes, CAM_PHI, CAM_THT); OBS, far away, has the focus)
proc.wait_frames(3)
local S, A = vessel.get_interface("ISS"), vessel.get_interface("GL-01")
local function L(s) oapi.write_log("iss_hit: " .. s) end
local function vecenv(n, d)
	local s = os.getenv(n) or d
	local x, y, z = s:match("([%-%d.]+)[ ,]+([%-%d.]+)[ ,]+([%-%d.]+)")
	return {x=tonumber(x), y=tonumber(y), z=tonumber(z)}
end
local speed = tonumber(os.getenv("SPEED") or "30")
local T = tonumber(os.getenv("T") or "3")
local aim = vecenv("AIM", "20 0 0")
local dir = vec.unit(vecenv("DIR", "0 0 -1"))
local function gpos(v) return oapi.get_globalpos(v:get_handle()) end
local gs = gpos(S)
local function toG(p) return vec.sub(S:local2global(p), oapi.get_globalpos(S:get_handle())) end -- (ISS frame direction to global)
local d = toG(dir)
-- GL-01 nose (z = +8 m) along d: its axes, z = d, y = ISS y made normal to it
local yS = toG({x=0, y=1, z=0})
if math.abs(vec.dotp(yS, d)) > 0.9 then yS = toG({x=1, y=0, z=0}) end
local z = vec.unit(d)
local x = vec.unit(vec.crossp(yS, z))
local y = vec.crossp(z, x)
local function rotm(x, y, z) return {m11=x.x, m12=y.x, m13=z.x, m21=x.y, m22=y.y, m23=z.y, m31=x.z, m32=y.z, m33=z.z} end
local R = rotm(x, y, z)
local function arot(R) return {x=math.atan2(R.m23, R.m33), y=-math.asin(R.m13), z=math.atan2(R.m12, R.m11)} end
local stS = S:get_status(2)
stS.vrot = {x=0, y=0, z=0}
S:defset_status(stS)
local st = A:get_status(2)
local aimRel = toG(aim)
st.rpos = vec.add(stS.rpos, vec.sub(vec.sub(aimRel, vec.mul(z, 8.0)), vec.mul(z, speed * T)))
st.rvel = vec.add(stS.rvel, vec.mul(z, speed))
st.arot = arot(R); st.vrot = {x=0, y=0, z=0}; st.status = 0
A:defset_status(st)
proc.wait_frames(1)
A:set_rotationmatrix(R)
S:set_angvel({x=0, y=0, z=0})
oapi.set_cameratarget(S:get_handle(), 0)
-- (default: from the sun's side, turned by CAM_OFS (ISS frame) off it: the
-- arrays seen lit; target-relative, phi = atan2 (x, -z), tht = asin (-y))
local sunL = vec.unit(S:global2local(oapi.get_globalpos(oapi.get_objhandle("Sun"))))
local u = vec.unit(vec.add(sunL, vecenv("CAM_OFS", "0.5 0.4 0.6")))
local phi, tht = math.atan2(u.x, -u.z), math.asin(-u.y)
L(string.format("sun in ISS frame (%.2f %.2f %.2f), camera phi %.2f tht %.2f", sunL.x, sunL.y, sunL.z, phi, tht))
if os.getenv("CAM") == "fixed" then
	-- (fixed in the orbiting frame: OBS moved CAM_D [m] beyond the ISS on the
	-- line of sight, the camera CAM_DIST [m] from the ISS on the near side,
	-- tracking OBS in an absolute direction: the ISS in the middle)
	local ug = toG(u)
	local D2, R1 = tonumber(os.getenv("CAM_D") or "3000"), tonumber(os.getenv("CAM_DIST") or "700")
	local so = O_status or vessel.get_interface("OBS"):get_status(2)
	local sn = S:get_status(2) -- (now: the ISS has moved on since)
	so.rpos = vec.sub(sn.rpos, vec.mul(ug, D2)); so.rvel = sn.rvel; so.status = 0; so.vrot = {x=0, y=0, z=0}
	vessel.get_interface("OBS"):defset_status(so)
	proc.wait_frames(1)
	local obs = vessel.get_interface("OBS")
	obs:set_rotationmatrix(S:get_rotationmatrix()) -- (turned as the ISS: the same camera angles)
	oapi.set_cameratarget(obs:get_handle(), 0)
	oapi.set_cameramode({mode="track", trackmode="targetrelative", reldist=(R1 + D2) / obs:get_size(), phi=phi, tht=tht})
	proc.wait_frames(2)
	local c = oapi.get_cameraglobalpos()
	local gi, go = gpos(S), gpos(obs)
	L(string.format("fixed camera: from ISS %.0f m along u %.2f; OBS from ISS %.0f m along u %.2f; obs rot m11 %.3f iss %.3f",
		vec.length(vec.sub(c, gi)), vec.dotp(vec.unit(vec.sub(c, gi)), ug), vec.length(vec.sub(go, gi)), vec.dotp(vec.unit(vec.sub(go, gi)), ug),
		obs:get_rotationmatrix().m11, S:get_rotationmatrix().m11))
else
oapi.set_cameramode({mode="track", trackmode="targetrelative", reldist=tonumber(os.getenv("CAM_R") or "2.2"),
	phi=tonumber(os.getenv("CAM_PHI") or phi), tht=tonumber(os.getenv("CAM_THT") or tht)})
end
if os.getenv("CAM_FOV") then oapi.set_cameraaperture(math.rad(tonumber(os.getenv("CAM_FOV"))) / 2) end
local t0 = oapi.get_simtime()
L(string.format("started, %.0f m/s, aim (%.1f %.1f %.1f)", speed, aim.x, aim.y, aim.z))
local logged = 0
-- (MOMENTUM=t: the pieces' velocities relative to OBS (the ISS's at the
-- start) and the total momentum, t [s] after the start)
local tm = tonumber(os.getenv("MOMENTUM") or "-1")
local O = vessel.get_interface("OBS")
local function rel(h) return vec.sub(oapi.get_globalvel(h), oapi.get_globalvel(O:get_handle())) end
L(string.format("before: ISS %.0f kg, GL-01 %.0f kg at %.1f m/s", S:get_mass(), A:get_mass(), vec.length(rel(A:get_handle()))))
local zdir = d
local watchT = -1
while true do
	if os.getenv("WATCH") then
		for i = 0, oapi.get_debriscount() - 1 do
			local h = oapi.get_debris(i)
			local di = oapi.get_debrisinfo(h)
			if di.name == os.getenv("WATCH") then
				local rv = rel(h)
				L(string.format("watch t=%.2f %s: along %.1f across %.1f", oapi.get_simtime() - t0, di.name, vec.dotp(rv, zdir), vec.length(vec.sub(rv, vec.mul(zdir, vec.dotp(rv, zdir))))))
			end
		end
	end
	if os.getenv("SCREEN") and oapi.get_simtime() - t0 >= tonumber(os.getenv("SCREEN")) and os.getenv("CAM") == "fixed" then
		-- (each piece on the screen: the camera's axes in OBS's frame (as the ISS's), rrot = Ry(phi) Rx(tht))
		local obs = vessel.get_interface("OBS")
		local sp, cp, st, ct = math.sin(phi), math.cos(phi), math.sin(tht), math.cos(tht)
		local cx, cy = {x=cp, y=0, z=sp}, {x=sp*st, y=ct, z=-cp*st}
		local cam = obs:global2local(oapi.get_cameraglobalpos())
		local fwd = vec.unit(vec.sub({x=0,y=0,z=0}, cam))
		for i = 0, oapi.get_debriscount() - 1 do
			local h = oapi.get_debris(i)
			local di = oapi.get_debrisinfo(h)
			local r = vec.sub(obs:global2local(oapi.get_globalpos(h)), cam)
			local zf = vec.dotp(r, fwd)
			L(string.format("screen %-34s %6.0f kg  x %6.3f  y %6.3f  (dist %.0f m, speed %.0f m/s)", di.name, di.mass,
				vec.dotp(r, cx) / zf, vec.dotp(r, cy) / zf, zf, vec.length(rel(h))))
		end
		oapi.exit(0)
	end
	if tm > 0 and oapi.get_simtime() - t0 >= tm then
		tm = -1
		local P = {x=0, y=0, z=0}
		local M = 0
		for _, v in ipairs({S, A}) do
			if v:get_partinfo(0).flags % 2 == 0 then
				local pv = vec.mul(rel(v:get_handle()), v:get_mass()); P = vec.add(P, pv); M = M + v:get_mass()
				L(string.format("vessel %s %.0f kg: %.2f m/s (along %.2f)", v:get_name(), v:get_mass(), vec.length(rel(v:get_handle())), vec.dotp(rel(v:get_handle()), zdir)))
			end
		end
		for i = 0, oapi.get_debriscount() - 1 do
			local h = oapi.get_debris(i)
			local di = oapi.get_debrisinfo(h)
			local rv = rel(h)
			P = vec.add(P, vec.mul(rv, di.mass)); M = M + di.mass
			L(string.format("piece %-36s %7.0f kg: %7.2f m/s (along %7.2f)", di.name, di.mass, vec.length(rv), vec.dotp(rv, zdir)))
		end
		L(string.format("total %.0f kg, momentum along the path %.0f t m/s (|P| %.0f)", M, vec.dotp(P, zdir) / 1000, vec.length(P) / 1000))
		if os.getenv("MOMENTUM_EXIT") then oapi.exit(0) end
	end

	proc.wait_frames(1)
	local n = oapi.get_debriscount()
	if n ~= logged then
		L(string.format("t=%.2f %d pieces", oapi.get_simtime() - t0, n))
		logged = n
	end
end
