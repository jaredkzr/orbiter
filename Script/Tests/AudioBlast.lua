-- An explosion's blast (Tests/Audio/Blast.scn, ORBITER_DAMAGE=1, rendered
-- with ORBITER_AUDIO_RENDER): a fuelled delta-glider 1 km north of the
-- listener (on the ground, 2 m up), set 50 m up at 1 s, falling at 80 m/s:
-- it breaks up on the ground; its blast arrives about 1 km / 347 m/s later.
local earth = oapi.get_objhandle("Earth")
local v = vessel.get_interface("BOOM")
local obs = vessel.get_interface("OBS")
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
local function gpos(x) return oapi.get_globalpos(x:get_handle()) end
local g = oapi.global_to_equ(earth, gpos(obs))
oapi.set_cameramode({mode = 'ground', ref = 'Earth', lng = math.deg(g.lng), lat = math.deg(g.lat), alt = 2})
at(1.0)
local up = vec.unit(vec.sub(gpos(v), oapi.get_globalpos(earth)))
local st = v:get_status(2)
st.status = 0
st.rpos = vec.add(st.rpos, vec.mul(up, 50))
st.rvel = vec.add(st.rvel, vec.mul(up, -80))
v:defset_status(st)
oapi.write_log(string.format("AudioBlast: dropped at %.2f, %.0f m from the listener", oapi.get_simtime(), vec.length(vec.sub(gpos(v), gpos(obs)))))
at(8.0)
oapi.write_log("AudioBlast: done")
oapi.exit(0)
