-- Puts the focus vessel into Jupiter's atmosphere at -40e3 m above the 1-bar
-- level (24 N, in the eastward jet, in daylight), moving with the planet's rotation
-- (test views of the gas giant plunge)
local hJ = oapi.get_objhandle("Jupiter")
local R = oapi.get_size(hJ)
-- (40 degrees of longitude from the subsolar point: the sun about 45 degrees up)
local sub = oapi.global_to_equ(hJ, oapi.get_globalpos(oapi.get_objhandle("Sun")))
local lng, lat, alt = sub.lng - math.rad(40.0), math.rad(24.0), -40e3
local v = vessel.get_focusinterface()
local s = v:get_status(2)
s.rbody = hJ
s.status = 0
s.rpos = vec.sub(oapi.equ_to_global(hJ, {lng = lng, lat = lat, rad = R + alt}), oapi.get_globalpos(hJ))
s.rvel = vec.sub(oapi.get_groundvector(hJ, lng, lat, 3), oapi.get_globalvel(hJ))
s.vrot = {x = 0, y = 0, z = 0}
v:defset_status(s)
