-- Gas giants (Scenarios/Tests/GasGiantTest.scn): Jupiter's atmosphere
-- model (1 bar and 165 K at altitude 0, the dry adiabat below), no solid
-- surface, and its jets: NORTH is dropped into the eastward jet at 24 N
-- (about 150 m/s), SOUTH into the westward jet at 20 S (about -60 m/s),
-- both 20 km above the 1-bar level, moving with the planet's rotation.

local failed = false

local function log(s)
	oapi.write_log("GasGiantTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

local hJ = oapi.get_objhandle("Jupiter")
local R = oapi.get_size(hJ)

-- put v at (lng, lat, alt), at rest relative to the planet's rotation
local function place(v, lng, lat, alt)
	local s = v:get_status(2)
	s.rbody = hJ
	s.status = 0
	s.rpos = vec.sub(oapi.equ_to_global(hJ, {lng = lng, lat = lat, rad = R + alt}), oapi.get_globalpos(hJ))
	s.rvel = vec.sub(oapi.get_groundvector(hJ, lng, lat, 3), oapi.get_globalvel(hJ))
	s.vrot = {x = 0, y = 0, z = 0}
	v:defset_status(s)
end

local function main()
	-- the atmosphere model
	local a0 = oapi.get_planetatmparams(hJ, 0.0, 0.0, 0.0)
	local am = oapi.get_planetatmparams(hJ, -100e3, 0.0, 0.0)
	local ap = oapi.get_planetatmparams(hJ, 50e3, 0.0, 0.0)
	log(string.format("atmosphere: 0 km %.0f Pa %.1f K %.3f kg/m3; -100 km %.0f Pa %.1f K; +50 km %.0f Pa %.1f K",
		a0.p, a0.T, a0.rho, am.p, am.T, ap.p, ap.T))
	check("1 bar", math.abs(a0.p - 1e5) < 1 and math.abs(a0.T - 165) < 0.1, "(at altitude 0)")
	check("adiabat", am.T > 340 and am.T < 380 and am.p > 1e6, "(warmer and denser below)")
	check("above", ap.p < 3e4 and ap.T < 165, "(thinner and colder above)")
	check("no surface", not oapi.planet_hassurface(hJ) and oapi.planet_hassurface(oapi.get_objhandle("Earth")), "(Jupiter none, Earth one)")
	check("crush pressure", math.abs(oapi.get_crushpressure(hJ) - 23e5) < 1, string.format("(%.0f Pa)", oapi.get_crushpressure(hJ)))
	-- convective storms: active somewhere, not everywhere (a grid of points
	-- in the water clouds); their updrafts positive and bounded
	local n, active, upmax = 0, 0, 0
	for lat = -60, 60, 2 do
		for lng = -180, 178, 2 do
			local s, u = oapi.get_storm(hJ, math.rad(lng), math.rad(lat), -30e3)
			n = n + 1
			if s > 0.2 then active = active + 1 end
			upmax = math.max(upmax, u)
		end
	end
	check("storms", active > 0.005 * n and active < 0.5 * n and upmax > 5 and upmax <= 40.5,
		string.format("(%.1f percent of points active, strongest updraft %.1f m/s)", 100 * active / n, upmax))
	local w = oapi.get_windvector(hJ, 0.0, math.rad(24.0), 0.0, 0)
	check("jet", w.x > 130 and w.x < 170, string.format("(24 N: %.1f m/s eastward)", w.x))

	-- the drops
	local north, south = vessel.get_interface("NORTH"), vessel.get_interface("SOUTH")
	place(north, 0.0, math.rad(24.0), 20e3)
	place(south, 0.0, math.rad(-20.0), 20e3)
	proc.wait_frames(2)
	local t0 = oapi.get_simtime()
	local amin, vs_at = {}, {}
	while oapi.get_simtime() - t0 < 600.0 do
		proc.wait_frames(1)
		for n, v in pairs({NORTH = north, SOUTH = south}) do
			local a = v:get_altitude(ALTMODE.MEANRAD)
			amin[n] = math.min(amin[n] or 1e9, a)
			local vs = v:get_groundspeedvector(REFFRAME.HORIZON).y
			-- (sinking speeds 20 km and 40 km below the 1-bar level)
			if not vs_at[n .. "20"] and a < -20e3 then vs_at[n .. "20"] = vs end
			if not vs_at[n .. "40"] and a < -40e3 then vs_at[n .. "40"] = vs end
		end
	end
	for n, v in pairs({NORTH = north, SOUTH = south}) do
		local g = v:get_groundspeedvector(REFFRAME.HORIZON)
		log(string.format("%s: lowest %.1f km, now %.1f km; sinking %.1f m/s at -20 km, %.1f m/s at -40 km; ground speed east %.1f m/s, landed %s",
			n, amin[n] / 1e3, v:get_altitude(ALTMODE.MEANRAD) / 1e3, vs_at[n .. "20"] or 0, vs_at[n .. "40"] or 0, g.x, tostring(v:is_landed())))
		check(n .. " sinks", amin[n] < -40e3 and not v:is_landed(), "(through the 1-bar level, not landed)")
		check(n .. " slows", vs_at[n .. "20"] and vs_at[n .. "40"] and math.abs(vs_at[n .. "40"]) < math.abs(vs_at[n .. "20"]),
			"(slower in the denser air below)")
	end
	local en = north:get_groundspeedvector(REFFRAME.HORIZON).x
	local es = south:get_groundspeedvector(REFFRAME.HORIZON).x
	check("drift", en > 100 and es < -30, string.format("(NORTH east %.1f m/s, SOUTH east %.1f m/s)", en, es))
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
