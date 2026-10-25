-- Ground contact on a slope (Scenarios/Tests/ContactSlopeTest.scn): three
-- delta-gliders set down on a 12 degree slope are released (a short engine
-- pulse ends their landed state). UPHILL (braked, facing uphill) and ACROSS
-- (braked, across the slope) must stay at rest on their gear, tilted with
-- the slope; DOWNHILL (no brakes, facing downhill) must roll down it,
-- upright. Skipped (passes) without elevation data.

local failed = false

local function log(s)
	oapi.write_log("ContactSlopeTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

-- position on the surface (planet frame) [m]
local function pos_of(ves)
	local e = ves:get_equpos()
	return {x = e.rad * math.cos(e.lat) * math.cos(e.lng), y = e.rad * math.sin(e.lat), z = e.rad * math.cos(e.lat) * math.sin(e.lng)}
end

-- slope of the terrain under a vessel [deg]
local function terrain_slope(ves)
	local h = oapi.get_objhandle("Moon")
	local e = ves:get_equpos()
	local d = 10.0 / oapi.get_size(h)
	local e0 = oapi.surface_elevation(h, e.lng, e.lat)
	local ex = oapi.surface_elevation(h, e.lng + d / math.cos(e.lat), e.lat)
	local ey = oapi.surface_elevation(h, e.lng, e.lat + d)
	return math.deg(math.atan(math.sqrt((ex-e0)^2 + (ey-e0)^2) / 10.0))
end

local function main()
	local names = {"UPHILL", "ACROSS", "DOWNHILL"}
	local ves = {}
	for _, n in ipairs(names) do ves[n] = vessel.get_interface(n) end
	proc.wait_frames(2)

	local slope = terrain_slope(ves.UPHILL)
	log(string.format("terrain slope %.1f deg", slope))
	if slope < 5.0 then
		log("SKIPPED: no elevation data for the Moon")
		return
	end

	-- release: a short pulse of the main engines
	local alt0, hdg0 = {}, {}
	for _, n in ipairs(names) do
		alt0[n] = ves[n]:get_altitude(ALTMODE.GROUND)
		hdg0[n] = math.deg(ves[n]:get_yaw())
		if n ~= "DOWNHILL" then ves[n]:set_wheelbrakelevel(1.0, 0, true) end
		ves[n]:set_thrustergrouplevel(THGROUP.MAIN, 0.01)
	end
	proc.wait_frames(1)
	for _, n in ipairs(names) do ves[n]:set_thrustergrouplevel(THGROUP.MAIN, 0) end

	local m = {}
	for _, n in ipairs(names) do m[n] = { amin = 1e9, vmax_late = 0, p_late = nil } end

	local t0 = oapi.get_simtime()
	local T = 60.0
	while oapi.get_simtime() - t0 < T do
		proc.wait_frames(1)
		local t = oapi.get_simtime() - t0
		for _, n in ipairs(names) do
			local v = ves[n]
			local a = v:get_altitude(ALTMODE.GROUND)
			m[n].amin = math.min(m[n].amin, a)
			if t > T - 20.0 then
				if not m[n].p_late then m[n].p_late = pos_of(v) end
				m[n].vmax_late = math.max(m[n].vmax_late, v:get_groundspeed())
			end
		end
	end

	for _, n in ipairs(names) do
		local r, v = m[n], ves[n]
		r.moved_late = vec.dist(pos_of(v), r.p_late)
		r.pitch, r.bank = math.deg(v:get_pitch()), math.deg(v:get_bank())
		log(string.format("%s: alt min %.3f, final %.3f (start %.3f), moved in the last 20 s %.3f m, speed %.3f m/s, pitch %.2f, bank %.2f, heading %.1f (start %.1f), landed %s",
			n, r.amin, v:get_altitude(ALTMODE.GROUND), alt0[n], r.moved_late, v:get_groundspeed(), r.pitch, r.bank,
			math.deg(v:get_yaw()), hdg0[n], tostring(v:is_landed() and true or false)))
	end

	for _, n in ipairs(names) do
		local r, v = m[n], ves[n]
		check(n, r.amin > 1.0, "does not sink into the ground")
		check(n, math.abs(r.bank) < 30 and math.abs(r.pitch) < 30, "stays upright")
	end
	check("UPHILL", m.UPHILL.moved_late < 0.05, "holds on its brakes")
	check("UPHILL", math.abs(m.UPHILL.pitch - slope) < 4, "is pitched up with the slope")
	check("ACROSS", m.ACROSS.moved_late < 0.05, "holds on its brakes")
	check("ACROSS", math.abs(math.abs(m.ACROSS.bank) - slope) < 4, "is banked with the slope")
	check("UPHILL", m.UPHILL.amin > alt0.UPHILL - 0.3, "does not settle deeper than its gear")
	check("DOWNHILL", m.DOWNHILL.moved_late > 5, "rolls down the slope")
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
