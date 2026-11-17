-- Contacts (Tests/Audio/Contacts*.scn, rendered with ORBITER_AUDIO_RENDER):
-- a delta-glider set into the air over KSC's runway at 1 s, heard from the
-- ground 40 m to the side of where it comes down.
--   LAND:  gear down, 4 m up, 70 m/s, sinking 2 m/s: touchdown, rolling,
--          brakes from 6 s
--   BELLY: gear up, 1.5 m up, 30 m/s, sinking 1 m/s: the hull's thud, scraping
--   MOON:  gear down on the Moon's bare ground, 0.8 m up, sinking 1 m/s
-- For listening (not the tests'): CONTACTS_SINK=<m/s> (how fast it sinks),
-- CONTACTS_PITCH=<deg> (nose up: the mains first, the nose some time after),
-- CONTACTS_VIEW=cockpit (heard from the cockpit, not from the ground) |
-- outside (a camera 30 m beside it, 6 m up: on the Moon, vacuum - its contacts unheard),
-- CONTACTS_SPEED=<m/s> (along at touchdown: the Moon's regolith rolled on)
local belly = vessel.get_interface("BELLY") ~= nil
local moon = vessel.get_interface("MOON") ~= nil
local v = vessel.get_interface(belly and "BELLY" or moon and "MOON" or "LAND")
local earth = v:get_surfaceref()
local earthName = oapi.get_objname(earth)
local function at(t, f) while oapi.get_simtime() < t do if f then f() end; proc.skip() end end
local function gpos() return oapi.get_globalpos(v:get_handle()) end
local up = vec.unit(vec.sub(gpos(), oapi.get_globalpos(earth)))
-- the listener: 40 m to the vessel's right, 60 m ahead of it (where it comes down)
local fwd = vec.sub(v:local2global({x=0, y=0, z=1}), gpos())
local right = vec.sub(v:local2global({x=1, y=0, z=0}), gpos())
local L = vec.add(gpos(), vec.add(vec.mul(fwd, belly and 30 or 90), vec.mul(right, 40)))
local eq = oapi.global_to_equ(earth, L)
if os.getenv("CONTACTS_VIEW") == "cockpit" then
	oapi.set_cameramode({mode = 'cockpit'})
elseif os.getenv("CONTACTS_VIEW") == "outside" then
	local e = oapi.global_to_equ(earth, vec.add(gpos(), vec.add(vec.mul(fwd, 15), vec.mul(right, 30))))
	oapi.set_cameratarget(v:get_handle(), 1)   -- (on the vessel: a scenario's "TARGET MOON" may mean the Moon)
	oapi.set_cameramode({mode = 'ground', ref = earthName, lng = math.deg(e.lng), lat = math.deg(e.lat), alt = 6})
else
	oapi.set_cameramode({mode = 'ground', ref = earthName, lng = math.deg(eq.lng), lat = math.deg(eq.lat), alt = 2})
end
at(1.0)
-- (as DestructionTest's airborne(): level as on the ground; the attitude
-- set again a frame later, as the status's angles don't hold it)
local x = vec.sub(v:local2global({x=1, y=0, z=0}), gpos())
local y = vec.sub(v:local2global({x=0, y=1, z=0}), gpos())
local z = fwd
local pitch = math.rad(tonumber(os.getenv("CONTACTS_PITCH") or "0"))
if pitch ~= 0 then   -- (nose up: about the vessel's right axis)
	local c, sn = math.cos(pitch), math.sin(pitch)
	y, z = vec.sub(vec.mul(y, c), vec.mul(z, sn)), vec.add(vec.mul(z, c), vec.mul(y, sn))
end
local R = {m11=x.x, m12=y.x, m13=z.x, m21=x.y, m22=y.y, m23=z.y, m31=x.z, m32=y.z, m33=z.z}
local st = v:get_status(2)
local h, spd, vs = belly and 0.5 or 0.8, tonumber(os.getenv("CONTACTS_SPEED") or "") or (belly and 30 or moon and 0 or 70), -tonumber(os.getenv("CONTACTS_SINK") or "1.0")
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
if not belly and not moon then at(12.0, function () v:set_wheelbrakelevel(1.0, 0, false); tick () end) end
at(12.0, tick)
oapi.exit(0)
