-- A plunge through Jupiter's cloud decks (a camera flight for test videos):
-- the strongest storm cell within 3000 km of a point in the late
-- afternoon (75 degrees of longitude from the subsolar point, 24 N: the
-- sun low in the east, the decks' and towers' shadows long); the focus
-- vessel at its centre; the camera, 150 km south of it (the sun at the
-- side), descends from 50 km above the 1-bar level to 150 km below it
-- (into the dark under the water clouds) in 14 s, looking at the storm
local hJ = oapi.get_objhandle("Jupiter")
local R = oapi.get_size(hJ)
local sub = oapi.global_to_equ(hJ, oapi.get_globalpos(oapi.get_objhandle("Sun")))
local lng0, lat0 = sub.lng - math.rad(75.0), math.rad(24.0)
local best = nil
for _, c in ipairs(oapi.get_stormcells(hJ, lng0, lat0, 3000e3)) do
	if not best or c.strength > best.strength then best = c end
end
local lng, lat = lng0, lat0
if best then lat, lng = best.lat, best.lng end
local v = vessel.get_focusinterface()
local s = v:get_status(2)
s.rbody = hJ
s.status = 0
s.rpos = vec.sub(oapi.equ_to_global(hJ, {lng = lng, lat = lat, rad = R - 20e3}), oapi.get_globalpos(hJ))
s.rvel = vec.sub(oapi.get_groundvector(hJ, lng, lat, 3), oapi.get_globalvel(hJ))
s.vrot = {x = 0, y = 0, z = 0}
v:defset_status(s)
local clat = lat - 150e3 / R
local t0 = oapi.get_simtime()
while true do
	local t = oapi.get_simtime() - t0
	local alt = math.max(50e3 - 14.3e3 * t, -150e3)
	oapi.set_cameramode({mode = 'ground', ref = 'Jupiter', lng = math.deg(lng), lat = math.deg(clat), alt = alt,
		alt_above_ground = 0})
	proc.wait_frames(1)
end
