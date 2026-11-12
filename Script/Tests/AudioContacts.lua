-- Contacts (Tests/Audio/Contacts*.scn, rendered with ORBITER_AUDIO_RENDER):
-- a delta-glider set into the air over KSC's runway at 1 s, heard from the
-- ground 40 m to the side of where it comes down.
--   LAND:  gear down, 4 m up, 70 m/s, sinking 2 m/s: touchdown, rolling,
--          brakes from 6 s
--   BELLY: gear up, 1.5 m up, 30 m/s, sinking 1 m/s: the hull's thud, scraping
local earth = oapi.get_objhandle("Earth")
local belly = vessel.get_interface("BELLY") ~= nil
local v = vessel.get_interface(belly and "BELLY" or "LAND")
local function at(t, f) while oapi.get_simtime() < t do if f then f() end; proc.skip() end end
local function gpos() return oapi.get_globalpos(v:get_handle()) end
local up = vec.unit(vec.sub(gpos(), oapi.get_globalpos(earth)))
-- the listener: 40 m to the vessel's right, 60 m ahead of it (where it comes down)
local fwd = vec.sub(v:local2global({x=0, y=0, z=1}), gpos())
local right = vec.sub(v:local2global({x=1, y=0, z=0}), gpos())
local L = vec.add(gpos(), vec.add(vec.mul(fwd, belly and 30 or 90), vec.mul(right, 40)))
local eq = oapi.global_to_equ(earth, L)
oapi.set_cameramode({mode = 'ground', ref = 'Earth', lng = math.deg(eq.lng), lat = math.deg(eq.lat), alt = 2})
at(1.0)
-- (as DestructionTest's airborne(): level as on the ground; the attitude
-- set again a frame later, as the status's angles don't hold it)
local x = vec.sub(v:local2global({x=1, y=0, z=0}), gpos())
local y = vec.sub(v:local2global({x=0, y=1, z=0}), gpos())
local z = fwd
local R = {m11=x.x, m12=y.x, m13=z.x, m21=x.y, m22=y.y, m23=z.y, m31=x.z, m32=y.z, m33=z.z}
local st = v:get_status(2)
local h, spd, vs = belly and 0.5 or 0.8, belly and 30 or 70, -1.0
st.status = 0
st.arot = {x=math.atan2(R.m23, R.m33), y=-math.asin(R.m13), z=math.atan2(R.m12, R.m11)}
st.rpos = vec.add(st.rpos, vec.mul(up, h))
st.rvel = vec.add(st.rvel, vec.add(vec.mul(fwd, spd), vec.mul(up, vs)))
st.vrot = {x = 0, y = 0, z = 0}
v:defset_status(st)
local t0 = oapi.get_simtime()
while oapi.get_simtime() == t0 do proc.skip() end
v:set_rotationmatrix(R)
oapi.write_log("AudioContacts: released at " .. oapi.get_simtime())
local function report() oapi.write_log(string.format("AudioContacts: t %.2f alt %.2f groundspeed %.1f%s", oapi.get_simtime(), v:get_altitude(), v:get_groundspeed(), "")) end
local nextReport = 0
local function tick () if oapi.get_simtime() >= nextReport then report(); nextReport = oapi.get_simtime() + 0.5 end end
at(6.0, tick)
if not belly then at(12.0, function () v:set_wheelbrakelevel(1.0, 0, false); tick () end) end
at(12.0, tick)
oapi.exit(0)
