-- The structure creaking (Tests/Audio/Creak.scn, rendered with
-- ORBITER_AUDIO_RENDER): a delta-glider set at 6 km, 250 m/s, level; its
-- cockpit. Quiet flight to 4 s; a hard pull-up 4-8 s (the load rising,
-- held); level again.
local earth = oapi.get_objhandle("Earth")
local v = vessel.get_interface("CO")
local function at(t, f) while oapi.get_simtime() < t do if f then f() end; proc.skip() end end
local function gpos() return oapi.get_globalpos(v:get_handle()) end
local fwd0 = vec.sub(v:local2global({x=0, y=0, z=1}), gpos())
local x0 = vec.sub(v:local2global({x=1, y=0, z=0}), gpos())
local y0 = vec.sub(v:local2global({x=0, y=1, z=0}), gpos())
local R0 = {m11=x0.x, m12=y0.x, m13=fwd0.x, m21=x0.y, m22=y0.y, m23=fwd0.y, m31=x0.z, m32=y0.z, m33=fwd0.z}
local ground = v:get_status(2).rvel
at(0.5)
local up = vec.unit(vec.sub(gpos(), oapi.get_globalpos(earth)))
local st = v:get_status(2)
st.status = 0
st.arot = {x=math.atan2(R0.m23, R0.m33), y=-math.asin(R0.m13), z=math.atan2(R0.m12, R0.m11)}
st.rpos = vec.add(st.rpos, vec.mul(up, 6000 - v:get_altitude()))
st.rvel = vec.add(ground, vec.mul(fwd0, 250))
st.vrot = {x = 0, y = 0, z = 0}
v:defset_status(st)
local t0 = oapi.get_simtime()
while oapi.get_simtime() == t0 do proc.skip() end
v:set_rotationmatrix(R0)
v:set_adcmode(7)
at(4.0)
at(8.0, function () v:set_adclevel(AIRCTRL.ELEVATOR, 1.0) end)
v:set_adclevel(AIRCTRL.ELEVATOR, 0.0)
at(12.0)
oapi.exit(0)
