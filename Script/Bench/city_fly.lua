-- benchmark: a camera flying low over a city at night (Tests/Bench/NightCity.scn):
-- a ground observer moved along a straight line at BENCH_ALT [m] (default 600)
-- and BENCH_SPEED [m/s] (default 200), looking ahead and down; ends at
-- BENCH_END [s] (default 60). (City fields, night lights, terrain tiles
-- streaming in under a moving view; no vessel physics)
local tend = tonumber(os.getenv("BENCH_END") or "60")
local alt = tonumber(os.getenv("BENCH_ALT") or "600")
local spd = tonumber(os.getenv("BENCH_SPEED") or "200")
-- from the south-west outskirts across downtown Houston, heading north-east
local lng0, lat0, hdg = -95.52, 29.62, math.rad(40)
local R = 6371000
proc.wait_simdt(1)
local t0 = oapi.get_simtime()
oapi.write_log(string.format("bench: measure at %.3f", oapi.get_simtime()))
while true do
	local T = oapi.get_simtime() - t0
	if T >= tend then break end
	local d = spd * T
	local lat = lat0 + math.deg(d * math.cos(hdg) / R)
	local lng = lng0 + math.deg(d * math.sin(hdg) / (R * math.cos(math.rad(lat))))
	-- (phi from east towards north: heading 40 deg from north is 50 deg from east)
	oapi.set_cameramode({mode='ground', ref='Earth', lng=lng, lat=lat, alt=alt, phi=math.pi / 2 - hdg, tht=-0.3})
	proc.wait_frames(1)
end
oapi.write_log(string.format("bench: city flight from %.1f for %.1f s", t0, oapi.get_simtime() - t0))
oapi.exit(0)
