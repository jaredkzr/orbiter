-- benchmark: reentry (Tests/Bench/Reentry.scn): a Delta-glider put at 70 km,
-- 6.8 km/s along its heading 3 deg below the horizon, nose 40 deg up, the
-- attitude held; a close view from behind and above, the ground beyond it,
-- until BENCH_SWITCH [s] (default 30), then a far chase looking down; ends at
-- BENCH_END [s] (default 60). The plasma volume and its flecks, terrain under
-- a hypersonic descent. Logs "bench: far at <sim t>".
proc.wait_frames(3)
local tend = tonumber(os.getenv("BENCH_END") or "60")
local tsw = tonumber(os.getenv("BENCH_SWITCH") or "30")
local A = vessel.get_interface("GL-01")
local alt, speed, pitch, path = 70000, 6800, math.rad(40), math.rad(3)
local earth = oapi.get_objhandle("Earth")
local g = oapi.get_globalpos(A:get_handle())
local up = vec.unit(vec.sub(g, oapi.get_globalpos(earth)))
local x = vec.sub(A:local2global({x=1, y=0, z=0}), g)
local z = vec.sub(A:local2global({x=0, y=0, z=1}), g)
z = vec.unit(vec.sub(z, vec.mul(up, vec.dotp(z, up)))) -- (level)
x = vec.unit(vec.crossp(up, z)); if vec.dotp(x, vec.sub(A:local2global({x=1, y=0, z=0}), g)) < 0 then x = vec.mul(x, -1) end
local st = A:get_status(2)
st.status = 0; st.vrot = {x=0, y=0, z=0}
st.rpos = vec.add(st.rpos, vec.mul(up, alt))
st.rvel = vec.add(st.rvel, vec.sub(vec.mul(z, speed * math.cos(path)), vec.mul(up, speed * math.sin(path))))
A:defset_status(st)
proc.wait_frames(1)
local function hold()
	local gp = oapi.get_globalpos(A:get_handle())
	local u = vec.unit(vec.sub(gp, oapi.get_globalpos(earth)))
	local v = vec.sub(A:get_globalvel(), oapi.get_globalvel(earth))
	local zz = vec.unit(vec.sub(v, vec.mul(u, vec.dotp(v, u))))
	local xx = vec.unit(vec.crossp(u, zz))
	local zp = vec.add(vec.mul(zz, math.cos(pitch)), vec.mul(u, math.sin(pitch)))
	local yp = vec.sub(vec.mul(u, math.cos(pitch)), vec.mul(zz, math.sin(pitch)))
	A:set_rotationmatrix({m11=xx.x, m12=yp.x, m13=zp.x, m21=xx.y, m22=yp.y, m23=zp.y, m31=xx.z, m32=yp.z, m33=zp.z})
	A:set_angvel({x=0, y=0, z=0})
end
hold()
-- (reldist in vessel sizes)
oapi.set_cameramode({mode='track', trackmode='locked', reldist=3.5, phi=0.4, tht=-1.2}) -- (phi, tht: the view's yaw and pitch in the vessel's frame)
oapi.set_cameraaperture(math.rad(50) / 2)
local t0 = oapi.get_simtime()
oapi.write_log(string.format("bench: measure at %.3f", t0))
local switched = false
while true do
	local T = oapi.get_simtime() - t0
	if T >= tend then break end
	if not switched and T >= tsw then
		oapi.set_cameramode({mode='track', trackmode='chase', reldist=25, phi=0, tht=-0.7})
		oapi.set_cameraaperture(math.rad(50) / 2)
		oapi.write_log(string.format("bench: far at %.3f", oapi.get_simtime()))
		switched = true
	end
	hold()
	proc.wait_frames(1)
end
oapi.write_log(string.format("bench: reentry for %.1f s, altitude %.1f km, speed %.0f m/s", oapi.get_simtime() - t0, A:get_altitude() / 1000, A:get_airspeed()))
oapi.exit(0)
