-- benchmark: the crash explosion (Tests/Bench/Crash.scn, budget.py Crash). V1
-- comes down from 25 m at 40 m/s along a path 12 deg below the horizon, nose
-- 2 deg up, wings level, into the sea; a fixed observer 170 m aside (the
-- explosion harness' sea case, out/fx-explosion/run.py). The sim runs to
-- BENCH_AFTER [s] (default 12) after the break-up (the first frame with three
-- pieces of debris or more), logged as "bench: crash at <sim time>". The focus is
-- the ShuttlePB OBS (Crash.scn): V1's destruction brings no wreck view or pause.
-- Needs the damage model (ORBITER_DAMAGE=1 or the Launchpad option).
proc.wait_frames(2)
local earth = oapi.get_objhandle("Earth")
local A = vessel.get_interface("V1")
local alt, speed, dive, pitch = 25, 40, math.rad(12), math.rad(2)
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
-- the observer: 170 m to the side, 110 m ahead, 8 m up, a fixed view (phi from east towards north, tht up)
local g2 = gpos(A)
local up2 = vec.unit(vec.sub(g2, oapi.get_globalpos(earth)))
local e2 = oapi.global_to_equ(earth, g2)
local base = vec.sub(g2, vec.mul(up2, e2.rad - oapi.get_size(earth)))
local c = vec.add(vec.add(base, vec.mul(z, 110)), vec.add(vec.mul(x, -170), vec.mul(up2, 8)))
local ce = oapi.global_to_equ(earth, c)
local cam = {mode="ground", ref="Earth", lng=math.deg(ce.lng), lat=math.deg(ce.lat), alt=8, phi=math.rad(-97), tht=math.rad(4)}
oapi.set_cameramode(cam)
oapi.set_cameraaperture(math.rad(40) / 2)
local t0 = oapi.get_simtime()
local after = tonumber(os.getenv("BENCH_AFTER") or "12")
local crashT, wet = nil, false
while true do
	proc.wait_frames(1)
	local t = oapi.get_simtime() - t0
	oapi.set_cameramode(cam) -- (held: no wreck view when the vessel is destroyed; the focus is OBS, so no
	                         -- flight-over menu pauses the sim 4 s after it - wait_frames would not return)
	if not wet then
		local w = A:get_waterstate()
		if w.wet then wet = true else A:set_rotationmatrix(R); A:set_angvel({x=0, y=0, z=0}) end
	end
	if not crashT and oapi.get_debriscount() >= 3 then
		crashT = oapi.get_simtime()
		oapi.write_log(string.format("bench: crash at %.3f", crashT))
	end
	if crashT and oapi.get_simtime() - crashT > after then break end
	if t > 60 then oapi.write_log("bench: no crash within 60 s"); break end
end
oapi.write_log(string.format("bench: crash flight played to %.1f s after the break-up", crashT and oapi.get_simtime() - crashT or -1))
oapi.exit(0)
