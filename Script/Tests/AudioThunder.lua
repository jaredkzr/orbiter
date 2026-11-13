-- Thunder (Tests/Audio/Thunder.scn, rendered with ORBITER_AUDIO_RENDER): at
-- 1 s a lightning channel 4 km long, 2 km up, its middle 2 km north of the
-- listener (on the ground, 2 m up), going east: its nearest point 2.8 km
-- off - the first thunder about 8.3 s later, rolling on for seconds.
local earth = oapi.get_objhandle("Earth")
local obs = vessel.get_interface("OBS")
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
local g0 = oapi.get_globalpos(obs:get_handle())
local eq = oapi.global_to_equ(earth, g0)
oapi.set_cameramode({mode = 'ground', ref = 'Earth', lng = math.deg(eq.lng), lat = math.deg(eq.lat), alt = 2})
at(1.0)
local g = oapi.get_globalpos(obs:get_handle())
local up = vec.unit(vec.sub(g, oapi.get_globalpos(earth)))
local north = vec.unit(vec.sub(oapi.equ_to_global(earth, {lng = eq.lng, lat = eq.lat + 1e-4, rad = vec.length(vec.sub(g, oapi.get_globalpos(earth)))}), g))
local east = vec.crossp(up, north)
if vec.dotp(east, vec.sub(oapi.equ_to_global(earth, {lng = eq.lng + 1e-4, lat = eq.lat, rad = vec.length(vec.sub(g, oapi.get_globalpos(earth)))}), g)) < 0 then east = vec.mul(east, -1) end
local start = vec.add(g, vec.add(vec.mul(up, 2000), vec.add(vec.mul(north, 2000), vec.mul(east, -2000))))
audio.lightning(earth, start, east, 4000, 2e5, 3, 0.3, 11)
oapi.write_log(string.format("AudioThunder: lightning at %.2f", oapi.get_simtime()))
at(20.0)
oapi.exit(0)
