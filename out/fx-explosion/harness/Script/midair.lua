-- (VkTest/midair: two delta-gliders collide in flight. GL-01 flies level at
-- ALT [m], SPEED [m/s]; GL-02 crosses its path at ANGLE [deg], its nose
-- meeting GL-01's left wing T [s] later. Camera: CAM_* (track, absolute
-- direction), or a ground observer CAM_X [m] beside the meeting point,
-- CAM_Y below it (CAM=ground), following GL-01 and then its heaviest piece)
proc.wait_frames(3)
local A, B = vessel.get_interface("GL-01"), vessel.get_interface("GL-02")
local earth = oapi.get_objhandle(os.getenv("PLANET") or "Earth")
local alt = tonumber(os.getenv("ALT") or "300")
local speed = tonumber(os.getenv("SPEED") or "150")
local ang = math.rad(tonumber(os.getenv("ANGLE") or "60"))
local T = tonumber(os.getenv("T") or "2.5")
local function gpos(v) return oapi.get_globalpos(v:get_handle()) end
local g = gpos(A)
local up = vec.unit(vec.sub(g, oapi.get_globalpos(earth)))
local x = vec.sub(A:local2global({x=1, y=0, z=0}), g)
local y = vec.sub(A:local2global({x=0, y=1, z=0}), g)
local z = vec.sub(A:local2global({x=0, y=0, z=1}), g)
-- GL-02's axes: GL-01's turned about the vertical (from its left)
local c, s = math.cos(ang), math.sin(ang)
local zB = vec.add(vec.mul(z, c), vec.mul(x, s))
local xB = vec.sub(vec.mul(x, c), vec.mul(z, s))
local function rotm(x, y, z) return {m11=x.x, m12=y.x, m13=z.x, m21=x.y, m22=y.y, m23=z.y, m31=x.z, m32=y.z, m33=z.z} end
local RA, RB = rotm(x, y, z), rotm(xB, y, zB)
local function arot(R) return {x=math.atan2(R.m23, R.m33), y=-math.asin(R.m13), z=math.atan2(R.m12, R.m11)} end
-- (a ground observer beside the meeting point: where, now)
local camX, camY = tonumber(os.getenv("CAM_X") or "250"), tonumber(os.getenv("CAM_Y") or "-60")
local camP = vec.add(vec.add(g, vec.mul(z, speed * T)), vec.add(vec.mul(x, camX), vec.mul(up, alt + camY)))
local camE = oapi.global_to_equ(earth, camP)
local stA = A:get_status(2)
local ground = stA.rvel
stA.arot = arot(RA); stA.vrot = {x=0, y=0, z=0}; stA.status = 0
stA.rpos = vec.add(stA.rpos, vec.mul(up, alt))
stA.rvel = vec.add(ground, vec.mul(z, speed))
-- where GL-01's left wing will be after T, and GL-02's nose then
local wing = vec.add(vec.add(vec.mul(x, tonumber(os.getenv("HIT_X") or "-6.5")), vec.mul(z, -2.0)), vec.mul(z, speed * T))
local noseB = vec.mul(zB, 8.0)
local stB = B:get_status(2)
stB.arot = arot(RB); stB.vrot = {x=0, y=0, z=0}; stB.status = 0
stB.rpos = vec.add(stA.rpos, vec.sub(vec.sub(wing, noseB), vec.mul(zB, speed * T)))
stB.rvel = vec.add(ground, vec.mul(zB, speed))
A:defset_status(stA)
B:defset_status(stB)
proc.wait_frames(1)
A:set_rotationmatrix(RA)
B:set_rotationmatrix(RB)
-- (level flight: some lift needed; trimmed by elevator)
for _, v in ipairs({A, B}) do v:set_adclevel(AIRCTRL.ELEVATORTRIM, tonumber(os.getenv("TRIM") or "0")) end
local ground = os.getenv("CAM") == "ground"
if ground then
	-- (the focus elsewhere: the camera does not chase the wreck)
	oapi.set_focusobject(oapi.get_objhandle("GL-03"))
	proc.wait_frames(1)
	oapi.set_cameratarget(A:get_handle(), 0)
	oapi.set_cameramode({mode="ground", ref=os.getenv("PLANET") or "Earth", lng=math.deg(camE.lng), lat=math.deg(camE.lat), alt=alt + camY})
	if os.getenv("CAM_FOV") then oapi.set_cameraaperture(math.rad(tonumber(os.getenv("CAM_FOV"))) / 2) end
else
	oapi.set_cameramode({mode="track", trackmode="absdirection", reldist=tonumber(os.getenv("CAM_R") or "6"),
		phi=tonumber(os.getenv("CAM_PHI") or "0.6"), tht=tonumber(os.getenv("CAM_THT") or "-0.35")})
end
local t0 = oapi.get_simtime()
oapi.write_log(string.format("midair: started t=%.2f", t0))
local logged = 0
while true do
	proc.wait_frames(1)
	local n = oapi.get_debriscount()
	if n ~= logged then
		oapi.write_log(string.format("midair: t=%.2f %d pieces", oapi.get_simtime() - t0, n))
		logged = n
		-- (the observer follows the heaviest piece of GL-01 once it broke up)
		if ground and n > 0 and A:get_partinfo(0).flags % 2 == 1 then
			local best, bm = nil, 0
			for i = 0, n - 1 do
				local h = oapi.get_debris(i)
				local d = oapi.get_debrisinfo(h)
				if d.mass > bm and string.find(d.name, "GL-01", 1, true) then best, bm = h, d.mass end
			end
			if best then oapi.set_cameratarget(best, 0) end
		end
	end
end
