-- A view of a convective storm in Jupiter's clouds: the strongest storm
-- cell within 3000 km of a point in daylight (40 degrees of longitude from
-- the subsolar point, 24 N), seen from 40 km above the 1-bar level, 200 km
-- west of its centre, looking at it (the focus vessel is at its centre)
local hJ = oapi.get_objhandle("Jupiter")
local R = oapi.get_size(hJ)
local sub = oapi.global_to_equ(hJ, oapi.get_globalpos(oapi.get_objhandle("Sun")))
local lng0, lat0 = sub.lng - math.rad(40.0), math.rad(24.0)
local best = nil
for _, c in ipairs(oapi.get_stormcells(hJ, lng0, lat0, 3000e3)) do
	if not best or c.strength > best.strength then best = c end
end
local lng, lat = lng0, lat0
if best then
	lat = best.lat
	lng = best.lng - 200e3 / (R * math.cos(lat))
	oapi.write_log(string.format("jupiter_storm: cell at %.2f %.2f deg, radius %.0f km, strength %.2f",
		math.deg(best.lng), math.deg(best.lat), best.radius / 1e3, best.strength))
end
local alt = 40e3
local v = vessel.get_focusinterface()
local s = v:get_status(2)
-- (the vessel at the storm's centre: the camera looks at it)
local vl = best and best.lng or lng
local valt = 30e3
s.rbody = hJ
s.status = 0
s.rpos = vec.sub(oapi.equ_to_global(hJ, {lng = vl, lat = lat, rad = R + valt}), oapi.get_globalpos(hJ))
s.rvel = vec.sub(oapi.get_groundvector(hJ, vl, lat, 3), oapi.get_globalvel(hJ))
s.vrot = {x = 0, y = 0, z = 0}
v:defset_status(s)
oapi.set_cameramode({mode = 'ground', ref = 'Jupiter', lng = math.deg(lng), lat = math.deg(lat), alt = alt,
	alt_above_ground = 0})
