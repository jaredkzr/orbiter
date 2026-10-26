-- A view of Neptune's cloud decks from 60000 m above the 1-bar level (20
-- degrees from the subsolar latitude, 40 degrees of longitude from the
-- subsolar point), looking north, 20 degrees down; the focus vessel 2 km
-- ahead
local hP = oapi.get_objhandle("Neptune")
local R = oapi.get_size(hP)
local sub = oapi.global_to_equ(hP, oapi.get_globalpos(oapi.get_objhandle("Sun")))
local lng0 = sub.lng - math.rad(40.0)
local lat0 = math.max(math.min(sub.lat + math.rad(20.0), math.rad(70.0)), math.rad(-70.0))
local function place(lng, lat, alt)
	local v = vessel.get_focusinterface()
	local s = v:get_status(2)
	s.rbody = hP
	s.status = 0
	s.rpos = vec.sub(oapi.equ_to_global(hP, {lng = lng, lat = lat, rad = R + alt}), oapi.get_globalpos(hP))
	s.rvel = vec.sub(oapi.get_groundvector(hP, lng, lat, 3), oapi.get_globalvel(hP))
	s.vrot = {x = 0, y = 0, z = 0}
	v:defset_status(s)
end
local function strongest()
	local best = nil
	for _, c in ipairs(oapi.get_stormcells(hP, lng0, lat0, 5000e3)) do
		if not best or c.strength > best.strength then best = c end
	end
	return best
end
place(lng0 + 2e3 / (R * math.cos(lat0)), lat0, 60000)
oapi.set_cameramode({mode = 'ground', ref = 'Neptune', lng = math.deg(lng0), lat = math.deg(lat0), alt = 60000,
	alt_above_ground = 0, phi = math.rad(0), tht = math.rad(-20)})
