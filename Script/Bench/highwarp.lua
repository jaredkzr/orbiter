-- benchmark: a Delta-glider in low Earth orbit (Tests/Bench/HighWarp.scn), the
-- ground seen from its cockpit (nose held down), under time warp: BENCH_SCHED "s:w,..."
-- (from s seconds of frames - 60 frames a second, the same frames in real time
-- and with fixed steps - time acceleration w); ends at BENCH_END such seconds
-- (default 40). Terrain tiles and ray tracing churning with the ground moving
-- under the view, the core's step at warp. Logs "bench: warp <w> at <sim t>".
local sched = {}
for t, w in string.gmatch(os.getenv("BENCH_SCHED") or "0:1,10:100,20:1000,30:10000", "([%d%.]+):([%d%.]+)") do sched[#sched + 1] = {tonumber(t), tonumber(w)} end
local tend = tonumber(os.getenv("BENCH_END") or "40")
-- (from the cockpit, the nose held 50 deg below the horizon along the track: the ground ahead)
local v = vessel.get_focusinterface()
local earth = oapi.get_objhandle("Earth")
local down = math.rad(50)
local function hold()
	local gp = v:get_globalpos()
	local u = vec.unit(vec.sub(gp, oapi.get_globalpos(earth)))
	local vel = vec.sub(v:get_globalvel(), oapi.get_globalvel(earth))
	local zz = vec.unit(vec.sub(vel, vec.mul(u, vec.dotp(vel, u))))
	local xx = vec.unit(vec.crossp(u, zz))
	local zp = vec.sub(vec.mul(zz, math.cos(down)), vec.mul(u, math.sin(down)))
	local yp = vec.add(vec.mul(u, math.cos(down)), vec.mul(zz, math.sin(down)))
	v:set_rotationmatrix({m11=xx.x, m12=yp.x, m13=zp.x, m21=xx.y, m22=yp.y, m23=zp.y, m31=xx.z, m32=yp.z, m33=zp.z})
	v:set_angvel({x=0, y=0, z=0})
end
hold()
oapi.set_cameramode({mode='cockpit'})
oapi.set_cameraaperture(math.rad(50) / 2)
proc.wait_frames(30)
oapi.write_log(string.format("bench: measure at %.3f", oapi.get_simtime()))
local f, cur = 0, -1
while f < tend * 60 do
	local w = 1
	for _, s in ipairs(sched) do if f >= s[1] * 60 then w = s[2] end end
	if w ~= cur then
		oapi.set_tacc(w)
		cur = w
		oapi.write_log(string.format("bench: warp %g at %.3f", w, oapi.get_simtime()))
	end
	hold()
	proc.wait_frames(1)
	f = f + 1
end
oapi.set_tacc(1)
oapi.write_log(string.format("bench: highwarp to %.0f s sim, altitude %.0f km", oapi.get_simtime(), v:get_altitude() / 1000))
oapi.exit(0)
