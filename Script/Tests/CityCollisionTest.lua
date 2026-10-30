-- Collisions with city buildings (Scenarios/Tests/CityCollisionTest.scn).
-- GL (delta-glider) is set down 45 m from a building in a suburb of
-- Houston, facing it with a clear path (no other building or tree near it),
-- and driven towards it with the main engines (cut half way). It must be
-- stopped by the building: come near it, never reach its wall, end slow.

local failed = false

local function log(s)
	oapi.write_log("CityCollisionTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

-- local east/north coordinates [m] of a point relative to (lng0, lat0)
local function en(R, lng0, lat0, lng, lat)
	return (lng - lng0) * R * math.cos(lat0), (lat - lat0) * R
end

local D = 45.0

-- a building with a clear path of length D to its face along its x axis
local function find_target(h, lng, lat)
	local R = oapi.get_size(h)
	local bl = oapi.city_buildings(h, lng, lat, 600)
	local trees = oapi.scatter_objects(h, lng, lat, 600, true)
	log(string.format("%d buildings, %d other obstacles within 600 m", #bl, #trees))
	-- (the other obstacles as buildings: trunks and boulders)
	local obst = {}
	for _, o in ipairs(bl) do obst[#obst+1] = o end
	for _, o in ipairs(trees) do
		local r = o.kind == 3 and 0.5 or o.radius
		obst[#obst+1] = {lng = o.lng, lat = o.lat, width = 2 * r, depth = 2 * r}
	end
	for _, t in ipairs(bl) do
		if t.width >= 8 and t.width <= 30 and t.depth >= 8 and t.height >= 4 and t.dist < 600 - D - 60 then -- (the obstacles about its path are in the lists)
			for _, side in ipairs({1, -1}) do
				local b = t.hdg + (side < 0 and math.pi or 0) -- bearing of the start point from the building
				local sx, sy = D * math.sin(b), D * math.cos(b)
				local clear = true
				for _, o in ipairs(obst) do
					if o ~= t then
						local ox, oy = en(R, t.lng, t.lat, o.lng, o.lat)
						local r = 0.5 * math.sqrt(o.width ^ 2 + o.depth ^ 2)
						if ox * ox + oy * oy < (D + r + 20) ^ 2 then
							local f = math.max(0, math.min(1, (ox * sx + oy * sy) / (D * D)))
							local dx, dy = ox - sx * f, oy - sy * f
							if math.sqrt(dx * dx + dy * dy) < 12.0 + r then clear = false break end
						end
					end
				end
				if clear then
					local slng = t.lng + sx / (R * math.cos(t.lat))
					local slat = t.lat + sy / R
					local de = oapi.surface_elevation(h, slng, slat) - oapi.surface_elevation(h, t.lng, t.lat)
					if math.abs(de) < 3.0 then
						return t, slng, slat, b + math.pi
					end
				end
			end
		end
	end
	return nil
end

local function main()
	proc.wait_frames(2)
	local ves = vessel.get_interface("GL")
	local h = oapi.get_objhandle("Earth")
	local R = oapi.get_size(h)
	local t, slng, slat, hdg = find_target(h, math.rad(-95.6), math.rad(29.9))
	if not t then
		check("GL", false, "finds a building with a clear path")
		return
	end
	log(string.format("target: %.0f x %.0f m, %.1f m tall, at %.5f %.5f; heading %.0f deg",
		t.width, t.depth, t.height, math.deg(t.lng), math.deg(t.lat), math.deg(hdg) % 360))
	local ux, uy = math.sin(hdg), math.cos(hdg)
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
		local x, y = en(R, slng, slat, e.lng, e.lat)
		local s = x * ux + y * uy
		smax = math.max(smax, s)
		vmax = math.max(vmax, ves:get_groundspeed())
		if s > 0.5 * D then ves:set_thrustergrouplevel(THGROUP.MAIN, 0) end
	end
	local v = ves:get_groundspeed()
	local wall = D - 0.5 * t.width
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
