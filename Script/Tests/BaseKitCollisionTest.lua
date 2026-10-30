-- Collisions with the launch complex kit (Scenarios/Tests/BaseKitCollisionTest.scn).
-- GL (delta-glider) is set down 100 m from the centre of the assembly
-- building of Jiuquan (60 or 90 m wide; which, its position and turn read
-- from the base's configuration), facing its side wall (without the annex),
-- and driven towards it with the main engines (cut 15 m before the wall). It must
-- be stopped by the building: come near it, never reach its wall, end slow.

local failed = false

local function log(s)
	oapi.write_log("BaseKitCollisionTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

local D = 100.0 -- start from the building's centre [m]

-- the base's location and the assembly building's position (base frame:
-- x south, z east) and turn [deg]
local function read_cfg()
	local f = io.open("Config/Earth/Base/Jiuquan.cfg", "r")
	if not f then return nil end
	local t = f:read("*a")
	f:close()
	local lng, lat = t:match("Location%s*=%s*([-+%d.]+)%s+([-+%d.]+)")
	local m, x, z, r = t:match("FILE BaseKit\\(assembly[%w_]*)%s+POS ([-%d.]+) [-%d.]+ ([-%d.]+)%s+ROT ([-%d.]+)")
	if not (lng and x) then return nil end
	local half = m == "assembly_small" and 30.0 or 45.0 -- (half its width [m])
	return tonumber(lng), tonumber(lat), tonumber(x), tonumber(z), tonumber(r), half
end

local function main()
	proc.wait_frames(2)
	local ves = vessel.get_interface("GL")
	local h = oapi.get_objhandle("Earth")
	local R = oapi.get_size(h)
	local lng0, lat0, bx, bz, rot, HALF = read_cfg()
	if not lng0 then
		check("GL", false, "finds the assembly building in Config/Earth/Base/Jiuquan.cfg")
		return
	end
	-- the building's -x axis (Orbiter turns x' = x cos r - z sin r, z' = x sin r + z cos r)
	local r = math.rad(rot)
	local ax, az = -math.cos(r), -math.sin(r)
	local sx, sz = bx + D * ax, bz + D * az -- start (base frame)
	local slat = math.rad(lat0) - sx / R
	local slng = math.rad(lng0) + sz / (R * math.cos(math.rad(lat0)))
	-- heading: towards the building (north: -x, east: z)
	local hdg = (math.atan2 or math.atan)(-az, ax) -- (atan2 (east, north) towards the building)
	local un, ue = ax, -az -- (north, east of the direction towards the building: n = -x, e = z)
	log(string.format("building at x %.1f z %.1f, turned %.1f deg; start %.1f m from its centre, heading %.0f deg",
		bx, bz, rot, D, math.deg(hdg) % 360))
	local st = ves:get_status(2)
	st.status = 1
	st.rbody = h
	st.surf_lng, st.surf_lat, st.surf_hdg = slng, slat, hdg % (2 * math.pi)
	st.arot = {x = 10, y = 0, z = 0}
	ves:defset_status(st)
	proc.wait_frames(2)
	ves:set_thrustergrouplevel(THGROUP.MAIN, 0.15)
	local smax, vmax = -1e9, 0
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 40.0 do
		proc.wait_frames(1)
		local e = ves:get_equpos()
		local n = (e.lat - slat) * R
		local ea = (e.lng - slng) * R * math.cos(slat)
		local s = n * un + ea * ue -- (along the path, towards the building)
		smax = math.max(smax, s)
		vmax = math.max(vmax, ves:get_groundspeed())
		if s > D - HALF - 15.0 then ves:set_thrustergrouplevel(THGROUP.MAIN, 0) end
	end
	local v = ves:get_groundspeed()
	local wall = D - HALF
	log(string.format("furthest %.2f m along the path (wall at %.1f m), top speed %.1f m/s, end speed %.2f m/s", smax, wall, vmax, v))
	check("GL", vmax > 3.0, "got going")
	check("GL", smax > wall - 25.0, "came near the building")
	check("GL", smax < wall, "was stopped by it (its centre never reached the wall)")
	check("GL", v < 2.0, "ends slow")
end

local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
