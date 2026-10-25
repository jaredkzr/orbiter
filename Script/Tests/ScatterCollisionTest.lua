-- Collisions with the ground scatter (Scenarios/Tests/ScatterCollisionTest.scn).
-- MOON (delta-glider) is set down 70 m from a boulder on the Moon, EARTH
-- 35 m from a tree in a dense Finnish forest, each facing it with a clear
-- path (no other obstacles near it), and driven towards it with the main
-- engines (cut half way). Each must be stopped by its obstacle: it must
-- come near it, but never reach its centre, and end slow. Small rocks,
-- shrubs and grass on the way are not obstacles.

local failed = false

local function log(s)
	oapi.write_log("ScatterCollisionTest: " .. s)
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

-- an obstacle with a clear path of length D towards it; the start point and heading
local function find_target(name, h, ves, wanted, D)
	local R = oapi.get_size(h)
	local e = ves:get_equpos()
	local objs = oapi.scatter_objects(h, e.lng, e.lat, 400, true)
	log(string.format("%s: %d obstacles within 400 m", name, #objs))
	for _, t in ipairs(objs) do
		if wanted(t) and t.dist < 400 - D - 30 then -- (the obstacles on its path are in the list)
			local near = {} -- (the obstacles that may be on the path)
			for _, o in ipairs(objs) do
				local ox, oy = en(R, t.lng, t.lat, o.lng, o.lat)
				if o ~= t and ox * ox + oy * oy < (D + 30) ^ 2 then
					near[#near+1] = {x = ox, y = oy, r = (o.kind == 3 and 0.5 or o.radius)} -- (trees: their trunks)
				end
			end
			for k = 0, 15 do
				local b = k * math.pi / 8.0 -- bearing of the start point from the obstacle
				local sx, sy = D * math.sin(b), D * math.cos(b)
				local clear = true
				for _, o in ipairs(near) do
					-- distance from the segment start -> obstacle
					local f = math.max(0, math.min(1, (o.x * sx + o.y * sy) / (D * D)))
					local dx, dy = o.x - sx * f, o.y - sy * f
					if math.sqrt(dx * dx + dy * dy) < 11.0 + o.r then clear = false break end
				end
				if clear then
					local slng = t.lng + sx / (R * math.cos(t.lat))
					local slat = t.lat + sy / R
					local de = oapi.surface_elevation(h, slng, slat) - oapi.surface_elevation(h, t.lng, t.lat)
					if math.abs(de) < 4.0 then
						return t, slng, slat, b + math.pi
					end
				end
			end
		end
	end
	return nil
end

local function main()
	local runs = {
		{ name = "MOON", body = "Moon", D = 70.0, wanted = function(t) return t.kind == 0 and t.height >= 1.5 end },
		{ name = "EARTH", body = "Earth", D = 35.0, wanted = function(t) return t.kind == 3 end }, -- (a dense forest)
	}
	proc.wait_frames(2)
	for _, r in ipairs(runs) do
		r.ves = vessel.get_interface(r.name)
		r.h = oapi.get_objhandle(r.body)
		r.R = oapi.get_size(r.h)
		local t, slng, slat, hdg = find_target(r.name, r.h, r.ves, r.wanted, r.D)
		if not t then
			check(r.name, false, "finds an obstacle with a clear path")
		else
			r.t, r.slng, r.slat = t, slng, slat
			r.ux, r.uy = math.sin(hdg), math.cos(hdg) -- direction of travel (east, north)
			log(string.format("%s: target %s (radius %.2f m, height %.2f m) at %.5f %.5f; heading %.0f deg",
				r.name, t.name, t.radius, t.height, math.deg(t.lng), math.deg(t.lat), math.deg(hdg) % 360))
			local st = r.ves:get_status(2)
			st.status = 1
			st.rbody = r.h
			st.surf_lng, st.surf_lat, st.surf_hdg = slng, slat, hdg % (2 * math.pi)
			st.arot = {x = 10, y = 0, z = 0} -- (no attitude of the old place: set down level on the gear)
			r.ves:defset_status(st)

		end
	end
	proc.wait_frames(2)
	for _, r in ipairs(runs) do
		if r.t then r.ves:set_thrustergrouplevel(THGROUP.MAIN, 0.15) end
		r.smax, r.vmax = -1e9, 0
	end

	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 45.0 do
		proc.wait_frames(1)
		for _, r in ipairs(runs) do
			if r.t then
				local e = r.ves:get_equpos()
				local x, y = en(r.R, r.slng, r.slat, e.lng, e.lat)
				local s = x * r.ux + y * r.uy
				r.smax = math.max(r.smax, s)
				r.vmax = math.max(r.vmax, r.ves:get_groundspeed())
				if s > 0.5 * r.D then r.ves:set_thrustergrouplevel(THGROUP.MAIN, 0) end
			end
		end
	end
	for _, r in ipairs(runs) do
		if r.t then
			local v = r.ves:get_groundspeed()
			log(string.format("%s: furthest %.2f m along the path (obstacle at %.0f m), top speed %.1f m/s, end speed %.2f m/s",
				r.name, r.smax, r.D, r.vmax, v))
			check(r.name, r.vmax > 3.0, "got going")
			check(r.name, r.smax > r.D - r.t.radius - 25.0, "came near the obstacle")
			check(r.name, r.smax < r.D - 0.5 * r.t.radius, "was stopped by it (never reached its centre)")
			check(r.name, v < 2.0, "ends slow")
		end
	end
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
