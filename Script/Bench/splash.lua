-- benchmark: a Delta-glider ditching (Tests/Bench/Splashdown.scn). V1 comes down
-- from 20 m at 70 m/s along a path 3 deg below the horizon, nose 6 deg up,
-- wings held level until it touches the water; the camera ahead of it looking back (the
-- focus is the ShuttlePB OBS far away: no wreck view or flight-over menu when
-- V1 breaks up). The sim runs to BENCH_AFTER [s] (default 12) after the first
-- contact with the water ("bench: water at <sim t>"; "bench: crash at <sim t>"
-- once three pieces of debris are out), at most 40 s. Spray, the water's
-- surface, debris, the explosion: their first use. Needs the damage model
-- (ORBITER_DAMAGE=1) for the break-up.
proc.wait_frames(2)
local earth = oapi.get_objhandle("Earth")
local A = vessel.get_interface("V1")
local alt, speed, dive, pitch = 20, 70, math.rad(3), math.rad(6)
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
st.status = 0; st.vrot = {x=0, y=0, z=0}
st.rpos = vec.add(st.rpos, vec.mul(up, alt - (e0.rad - oapi.get_size(earth))))
st.rvel = vec.add(st.rvel, vec.mul(path, speed))
st.arot = {x=math.atan2(R.m23, R.m33), y=-math.asin(R.m13), z=math.atan2(R.m12, R.m11)}
A:defset_status(st)
proc.wait_frames(1)
A:set_rotationmatrix(R)
-- (ahead of it, looking back at it: phi, tht the view's yaw and pitch in the vessel's frame; reldist in vessel sizes)
oapi.set_cameramode({mode='track', trackmode='locked', reldist=3.5, phi=3.4, tht=0.25})
oapi.set_cameraaperture(math.rad(50) / 2)
local t0 = oapi.get_simtime()
oapi.write_log(string.format("bench: measure at %.3f", t0))
local after = tonumber(os.getenv("BENCH_AFTER") or "12")
local wetT, crashT = nil, nil
while true do
	proc.wait_frames(1)
	if not wetT then
		local w = A:get_waterstate()
		if w.wet then
			wetT = oapi.get_simtime()
			oapi.write_log(string.format("bench: water at %.3f", wetT))
		else A:set_rotationmatrix(R); A:set_angvel({x=0, y=0, z=0}) end
	end
	if not crashT and oapi.get_debriscount() >= 3 then
		crashT = oapi.get_simtime()
		oapi.write_log(string.format("bench: crash at %.3f", crashT))
	end
	if wetT and oapi.get_simtime() - wetT > after then break end
	if oapi.get_simtime() - t0 > 40 then break end
end
oapi.write_log(string.format("bench: splashdown played %.1f s, %s", oapi.get_simtime() - t0, crashT and "broke up" or "intact"))
oapi.exit(0)
