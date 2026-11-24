-- benchmark: a Delta-glider climbing through the clouds (Tests/Bench/CloudDeck.scn,
-- East Africa, morning): put in flight BENCH_ALT [m] (default 800) above the
-- ground at 160 m/s, main engines full, nose held BENCH_PITCH [deg] (default
-- 22) above the horizon; a chase view, then from BENCH_SWITCH [s] (default 30)
-- the cockpit, from 2 BENCH_SWITCH a far view from behind and above (the deck
-- below); ends at BENCH_END [s] (default 90). The cloud passes, entering and
-- leaving the layer. Logs "bench: view <v> at <sim t>".
proc.wait_frames(3)
local tend = tonumber(os.getenv("BENCH_END") or "90")
local tsw = tonumber(os.getenv("BENCH_SWITCH") or "30")
local A = vessel.get_interface("V")
local alt, speed, pitch = tonumber(os.getenv("BENCH_ALT") or "800"), 160, math.rad(tonumber(os.getenv("BENCH_PITCH") or "22"))
local earth = oapi.get_objhandle("Earth")
local g = oapi.get_globalpos(A:get_handle())
local up = vec.unit(vec.sub(g, oapi.get_globalpos(earth)))
local z = vec.sub(A:local2global({x=0, y=0, z=1}), g)
z = vec.unit(vec.sub(z, vec.mul(up, vec.dotp(z, up))))
local st = A:get_status(2)
local ground = st.rvel
st.status = 0; st.vrot = {x=0, y=0, z=0}
st.rpos = vec.add(st.rpos, vec.mul(up, alt))
st.rvel = vec.add(ground, vec.mul(z, speed))
A:defset_status(st)
proc.wait_frames(1)
local hdg = z -- (the heading kept)
local function hold()
	local gp = oapi.get_globalpos(A:get_handle())
	local u = vec.unit(vec.sub(gp, oapi.get_globalpos(earth)))
	local zz = vec.unit(vec.sub(hdg, vec.mul(u, vec.dotp(hdg, u))))
	local xx = vec.unit(vec.crossp(u, zz))
	local zp = vec.add(vec.mul(zz, math.cos(pitch)), vec.mul(u, math.sin(pitch)))
	local yp = vec.sub(vec.mul(u, math.cos(pitch)), vec.mul(zz, math.sin(pitch)))
	A:set_rotationmatrix({m11=xx.x, m12=yp.x, m13=zp.x, m21=xx.y, m22=yp.y, m23=zp.y, m31=xx.z, m32=yp.z, m33=zp.z})
	A:set_angvel({x=0, y=0, z=0})
end
hold()
A:set_thrustergrouplevel(THGROUP.MAIN, 1)
oapi.simulatebufferedkey(OAPI_KEY.G) -- (gear up)
local views = {
	function () oapi.set_cameramode({mode='track', trackmode='locked', reldist=3, phi=3.3, tht=0.2}); oapi.set_cameraaperture(math.rad(50) / 2) end,
	function () oapi.set_cameramode({mode='cockpit'}); oapi.set_cameraaperture(math.rad(60) / 2) end,
	function () oapi.set_cameramode({mode='track', trackmode='locked', reldist=40, phi=3.0, tht=0.35}); oapi.set_cameraaperture(math.rad(50) / 2) end,
}
views[1]()
local t0 = oapi.get_simtime()
oapi.write_log(string.format("bench: measure at %.3f", t0))
local cur = 1
while true do
	local T = oapi.get_simtime() - t0
	if T >= tend then break end
	local v = math.min(#views, math.floor(T / tsw) + 1)
	if v ~= cur then
		cur = v
		views[v]()
		oapi.write_log(string.format("bench: view %d at %.3f", v, oapi.get_simtime()))
	end
	hold()
	A:set_thrustergrouplevel(THGROUP.MAIN, 1)
	proc.wait_frames(1)
end
oapi.write_log(string.format("bench: cloud climb for %.1f s to %.0f m, %.0f m/s", oapi.get_simtime() - t0, A:get_altitude(), A:get_airspeed()))
oapi.exit(0)
