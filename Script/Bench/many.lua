-- benchmark: many vessels (Tests/Bench/ManyVessels.scn): 50 vessels of six
-- classes parked in rows at the Cape. A ground observer 25 m up starts 12 km
-- south-west of them and flies towards them at BENCH_SPEED [m/s] (default
-- 400), looking at the rows, their visuals made as they come within range;
-- then BENCH_PAN seconds (default 20) of it panning along the rows. Visual
-- creation and its texture uploads, the steady cost per vessel. Logs "bench:
-- approach at <sim t>", "bench: pan at <sim t>". (Made at run time with
-- oapi.create_vessel, from the script, the session hung after the first.)
local speed = tonumber(os.getenv("BENCH_SPEED") or "400")
local pan = tonumber(os.getenv("BENCH_PAN") or "20")
local deg = math.pi / 180
local R = 6371000
local tlng, tlat = -80.6785, 28.5985 -- (the rows' middle)
local lng1, lat1 = -80.6801, 28.5968 -- (the end of the approach: 250 m from them)
local brg = math.rad(40)             -- (the approach's bearing, from the north)
local dist0 = 12000
local function at(d) -- (d metres before the end of the approach)
	return lng1 - math.deg(d * math.sin(brg) / (R * math.cos(lat1 * deg))), lat1 - math.deg(d * math.cos(brg) / R)
end
local function look(lng, lat) -- (phi from east towards north, at the rows)
	local e = (tlng - lng) * math.cos(lat * deg)
	local n = tlat - lat
	return math.atan2(n, e)
end
oapi.set_cameraaperture(math.rad(50) / 2)
local lng, lat = at(dist0)
oapi.set_cameramode({mode='ground', ref='Earth', lng=lng, lat=lat, alt=25, phi=look(lng, lat), tht=math.rad(-3)})
proc.wait_frames(30)
local t0 = oapi.get_simtime()
oapi.write_log(string.format("bench: measure at %.3f", t0))
oapi.write_log(string.format("bench: approach at %.3f", t0))
while true do
	local d = dist0 - speed * (oapi.get_simtime() - t0)
	if d <= 0 then break end
	lng, lat = at(d)
	oapi.set_cameramode({mode='ground', ref='Earth', lng=lng, lat=lat, alt=25, phi=look(lng, lat), tht=math.rad(-3)})
	proc.wait_frames(1)
end
lng, lat = at(0)
local phi0 = look(lng, lat)
local t1 = oapi.get_simtime()
oapi.write_log(string.format("bench: pan at %.3f", t1))
while oapi.get_simtime() - t1 < pan do
	local u = (oapi.get_simtime() - t1) / pan
	oapi.set_cameramode({mode='ground', ref='Earth', lng=lng, lat=lat, alt=25, phi=phi0 + math.rad(35) - math.rad(70) * u, tht=math.rad(-3)})
	proc.wait_frames(1)
end
oapi.write_log(string.format("bench: many vessels, %d objects", oapi.get_objcount()))
oapi.exit(0)
