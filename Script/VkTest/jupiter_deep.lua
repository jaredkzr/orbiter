-- A view of Jupiter's cloud decks from -50e3 m above the 1-bar level (24 N, in
-- the eastward jet, 40 degrees of longitude from the subsolar point: the
-- sun about 45 degrees up), looking east, -4 degrees from the horizon; the
-- focus vessel 2 km ahead, drifting with the jet
local hJ = oapi.get_objhandle("Jupiter")
local R = oapi.get_size(hJ)
local sub = oapi.global_to_equ(hJ, oapi.get_globalpos(oapi.get_objhandle("Sun")))
local lng, lat, alt = sub.lng - math.rad(40.0), math.rad(24.0), -50e3
local v = vessel.get_focusinterface()
local s = v:get_status(2)
local vl = lng + 2e3 / (R * math.cos(lat))
s.rbody = hJ
s.status = 0
s.rpos = vec.sub(oapi.equ_to_global(hJ, {lng = vl, lat = lat, rad = R + alt}), oapi.get_globalpos(hJ))
s.rvel = vec.sub(oapi.get_groundvector(hJ, vl, lat, 3), oapi.get_globalvel(hJ))
s.vrot = {x = 0, y = 0, z = 0}
v:defset_status(s)
oapi.set_cameramode({mode = 'ground', ref = 'Jupiter', lng = math.deg(lng), lat = math.deg(lat), alt = -50000,
	alt_above_ground = 0, phi = math.rad(90), tht = math.rad(2)})
