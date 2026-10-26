-- A plunge through Saturn's cloud decks (a camera flight for test videos):
-- the camera, 300000 m west of the strongest storm cell near a point in
-- daylight (as saturn_storm), descends from 80000 m to -230000 m about the
-- 1-bar level in 14 s, looking at the storm
local hP = oapi.get_objhandle("Saturn")
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
local best = strongest()
local lng, lat = lng0, lat0
if best then lng, lat = best.lng, best.lat end
place(lng, lat, 0)
local clng = lng - 300000 / (R * math.cos(lat))
local t0 = oapi.get_simtime()
while true do
	local t = oapi.get_simtime() - t0
	local alt = math.max(80000 - 22143 * t, -230000)
	place(lng, lat, 0)
	oapi.set_cameramode({mode = 'ground', ref = 'Saturn', lng = math.deg(clng), lat = math.deg(lat), alt = alt,
		alt_above_ground = 0})
	proc.wait_frames(1)
end
