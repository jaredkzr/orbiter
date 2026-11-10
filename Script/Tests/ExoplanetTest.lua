-- Exoplanets (Scenarios/Tests/ExoplanetTest.scn): the planets of the
-- neighbouring systems on their orbits (periods as measured), a transit of
-- TRAPPIST-1 e in front of its star seen from the Sun at the measured
-- epoch, tidal locking (meridian 0 to the star), the procedural ground of
-- TRAPPIST-1 e (seas over their share of it), a vessel standing on it,
-- the default ground scatter of the procedural worlds by surface class, and
-- their site materials (coarse, fine, flat: shares, off elsewhere, rocks on the mask).

local failed = false
local function log(s) oapi.write_log("ExoplanetTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function dot(a, b) return a.x * b.x + a.y * b.y + a.z * b.z end
local function len(a) return math.sqrt(dot(a, a)) end
local G = 6.67259e-11

-- name, host, period [d] (NASA Exoplanet Archive)
local planets = {
	{"TRAPPIST-1 b", "TRAPPIST-1", 1.510826}, {"TRAPPIST-1 c", "TRAPPIST-1", 2.421937},
	{"TRAPPIST-1 d", "TRAPPIST-1", 4.049219}, {"TRAPPIST-1 e", "TRAPPIST-1", 6.101013},
	{"TRAPPIST-1 f", "TRAPPIST-1", 9.20754}, {"TRAPPIST-1 g", "TRAPPIST-1", 12.352446},
	{"TRAPPIST-1 h", "TRAPPIST-1", 18.772866},
	{"Proxima b", "Proxima", 11.18465}, {"Proxima d", "Proxima", 5.12338},
	{"Barnard b", "Barnard", 3.1542}, {"Barnard c", "Barnard", 4.1244},
	{"Barnard d", "Barnard", 2.3402}, {"Barnard e", "Barnard", 6.7392},
	{"Epsilon Eridani b", "EpsilonEridani", 2680.0},
}

local function main()
	proc.wait_frames(3)

	-- 1. periods: from the state (vis-viva), the archive's
	local worst = 0
	local missing = {}
	for _, p in ipairs(planets) do
		local h, s = oapi.get_objhandle(p[1]), oapi.get_objhandle(p[2])
		if not h or not s then missing[#missing + 1] = p[1]
		else
			local r = sub(oapi.get_globalpos(h), oapi.get_globalpos(s))
			local v = sub(oapi.get_globalvel(h), oapi.get_globalvel(s))
			local mu = G * (oapi.get_mass(h) + oapi.get_mass(s))
			local a = 1 / (2 / len(r) - dot(v, v) / mu)
			local T = 2 * math.pi * math.sqrt(a ^ 3 / mu) / 86400
			worst = math.max(worst, math.abs(T / p[3] - 1))
		end
	end
	log(string.format("%d planets, periods within %.2e of the archive's", #planets - #missing, worst))
	check("planets", #missing == 0, "all loaded" .. (#missing > 0 and (" (missing " .. table.concat(missing, ", ") .. ")") or ""))
	check("periods", worst < 1e-6, "as measured")

	-- 2. a transit of TRAPPIST-1 e: at an epoch of the measured series, in
	-- front of the star seen from the Sun, within its disc
	local he, ht, sun = oapi.get_objhandle("TRAPPIST-1 e"), oapi.get_objhandle("TRAPPIST-1"), oapi.get_objhandle("Sun")
	local mjd0, P = 2457660.36766210 - 2400000.5, 6.101013
	local mjd = mjd0 + math.ceil((oapi.get_simmjd() - mjd0) / P) * P
	-- (the glider on TRAPPIST-1 e kept on its ground: PROP_ORBITAL_FIXEDSURF
	-- and PROP_SORBITAL_FIXEDSURF)
	local FIXEDSURF = 0x22
	oapi.set_simmjd(mjd, FIXEDSURF)
	proc.wait_frames(3)
	local rel = sub(oapi.get_globalpos(he), oapi.get_globalpos(ht))
	local los = sub(oapi.get_globalpos(ht), oapi.get_globalpos(sun))
	local u = {x = los.x / len(los), y = los.y / len(los), z = los.z / len(los)}
	local along = dot(rel, u)
	local b = math.sqrt(math.max(0, dot(rel, rel) - along ^ 2)) / oapi.get_size(ht)
	log(string.format("TRAPPIST-1 e at MJD %.5f: %.0f km towards the Sun, %.3f stellar radii off the star's centre on the sky", oapi.get_simmjd(), -along / 1e3, b))
	check("transit", along < 0 and b < 0.5, "in front of TRAPPIST-1")

	-- 3. tidal locking: the star over meridian 0, the equator, now and a
	-- third of an orbit on
	local function facing()
		local q = oapi.global_to_equ(he, oapi.get_globalpos(ht))
		return math.max(math.abs(q.lng), math.abs(q.lat))
	end
	local f0 = facing()
	oapi.set_simmjd(oapi.get_simmjd() + P / 3, FIXEDSURF)
	proc.wait_frames(3)
	local f1 = facing()
	log(string.format("the star %.4f and %.4f rad from the substellar point (lng 0, lat 0)", f0, f1))
	check("locked", f0 < 0.02 and f1 < 0.02, "meridian 0 to the star")

	-- 4. the ground of TRAPPIST-1 e: seas (their surface the ground, at the
	-- datum) over about their share (0.65), the relief within its
	-- configured range
	local below, n, emin, emax = 0, 0, 1e9, -1e9
	local seaPt = nil
	local t = os.clock()
	-- (coarse tiles, level 6: a query below the finest level makes its tile
	-- each time)
	for i = 0, 23 do
		local lat = math.asin(-1 + (i + 0.5) / 12)
		for j = 0, 47 do
			local lng = -math.pi + (j + 0.5) * math.pi / 24
			local e = oapi.surface_elevation(he, lng, lat, 9)
			n = n + 1
			if math.abs(e) < 0.5 then
				below = below + 1
				if not seaPt then -- (a sea point: its neighbours are sea as well)
					local all = true
					for _, d in ipairs({{0.02, 0}, {-0.02, 0}, {0, 0.02}, {0, -0.02}}) do
						if math.abs(oapi.surface_elevation(he, lng + d[1], lat + d[2], 9)) >= 0.5 then all = false end
					end
					if all then seaPt = {lng = lng, lat = lat} end
				end
			end
			emin, emax = math.min(emin, e), math.max(emax, e)
		end
	end
	log(string.format("ground of TRAPPIST-1 e: %.3f of %d points sea, %.0f to %.0f m (%.2f s)", below / n, n, emin, emax, os.clock() - t))
	check("seas", math.abs(below / n - 0.65) < 0.08 and emin > -100 and emax < 12250 and emax > 3000, "about their share, relief in range")
	t = os.clock()
	local e1 = oapi.surface_elevation(he, 0.35, 0.17)
	log(string.format("finest ground at 20.05E 9.74N: %.1f m (%.3f s)", e1, os.clock() - t))

	-- 5. a delta-glider landed at 28.6W 22.9S (lowlands, 380 m): standing on
	-- the ground
	local gl = vessel.get_interface("GL-E")
	local t1 = oapi.get_simtime()
	while oapi.get_simtime() - t1 < 10 and gl:get_flightstatus() % 2 == 0 do proc.wait_frames(1) end
	local alt = gl:get_altitude(ALTMODE.GROUND)
	local vs = gl:get_groundspeed()
	log(string.format("GL-E: %.2f m above the ground, %.3f m/s, status %d", alt, vs, gl:get_flightstatus()))
	check("landed", gl:get_flightstatus() % 2 == 1 and math.abs(alt) < 5 and vs < 0.1, "standing on the procedural ground")

	-- 6. ground scatter by surface class (Config/Scatter/Default_<Class>.cfg,
	-- gated by the client's climate: none on water, sea ice, slush)
	t = os.clock()
	local D = math.pi / 180
	local function scatter(h, lngd, latd, r)
		local t0 = os.clock()
		local objs = oapi.scatter_objects(h, lngd * D, latd * D, r, false)
		local n = #objs
		return objs, string.format("%d objects (%.2f s)", n, os.clock() - t0)
	end
	local function allNames(objs, ...)
		local keys = {...}
		for _, o in ipairs(objs) do
			local ok = false
			for _, k in ipairs(keys) do if o.name:find(k, 1, true) then ok = true end end
			if not ok then return false, o.name end
		end
		return true
	end
	local hf, hp, hb = oapi.get_objhandle("TRAPPIST-1 f"), oapi.get_objhandle("Proxima b"), oapi.get_objhandle("Barnard b")
	-- a. TRAPPIST-1 e (Ocean) around the glider: Titan and Earth sets, on land
	local ge = gl:get_equpos()
	local objs, msg = scatter(he, ge.lng / D, ge.lat / D, 300)
	local ok, bad = allNames(objs, "Titan", "Earth")
	local minElev = 1e9
	for _, o in ipairs(objs) do minElev = math.min(minElev, o.elev) end
	log("TRAPPIST-1 e at GL-E: " .. msg .. ", lowest elevation " .. string.format("%.1f", minElev))
	check("scatter-e-land", #objs > 50 and ok and minElev > 1.0, "land objects only, Titan/Earth sets" .. (bad and (" (" .. bad .. ")") or ""))
	-- b. TRAPPIST-1 e: none on the sea
	if seaPt then
		objs, msg = scatter(he, seaPt.lng / D, seaPt.lat / D, 300)
		log(string.format("TRAPPIST-1 e sea at %.2fE %.2fN: %s", seaPt.lng / D, seaPt.lat / D, msg))
		check("scatter-e-sea", #objs == 0, "none on the sea")
	else
		check("scatter-e-sea", false, "no sea point found in the grid")
	end
	-- c. TRAPPIST-1 f (Ice, tidally locked, open water to 14 deg)
	objs, msg = scatter(hf, 0, 0, 300)
	log("TRAPPIST-1 f substellar: " .. msg)
	check("scatter-f-water", #objs == 0, "none on the open water")
	objs, msg = scatter(hf, 14.0, 0, 300)
	log("TRAPPIST-1 f slush at 14.0E: " .. msg)
	check("scatter-f-slush", #objs == 0, "none on the slush")
	objs, msg = scatter(hf, 0, 30, 300)
	ok, bad = allNames(objs, "Ice")
	log("TRAPPIST-1 f ice at 30N: " .. msg)
	check("scatter-f-ice", #objs > 20 and ok, "ice blocks on the ice sheet" .. (bad and (" (" .. bad .. ")") or ""))
	-- d. Proxima b (Ocean): sea ice site, land site
	objs, msg = scatter(hp, -30, -20, 300)
	log("Proxima b sea ice (-30, -20): " .. msg)
	check("scatter-b-sea", #objs == 0, "none on the sea ice")
	objs, msg = scatter(hp, -26, -8, 300)
	log("Proxima b land (-26, -8): " .. msg)
	check("scatter-b-land", #objs > 20, "rocks on the land")
	-- e. Barnard b (Rock, airless): Moon set
	objs, msg = scatter(hb, 10, 10, 300)
	ok, bad = allNames(objs, "Moon")
	log("Barnard b (10, 10): " .. msg)
	check("scatter-barnard", #objs > 20 and ok, "Moon rocks" .. (bad and (" (" .. bad .. ")") or ""))
	log(string.format("ground scatter: %.2f s", os.clock() - t))

	-- 7. site materials (F3): the core's mask of coarse/fine/flat sub-materials on the
	-- procedural worlds; its shares, off on other worlds, the rocks following it
	t = os.clock()
	local hearth, hmars = oapi.get_objhandle("Earth"), oapi.get_objhandle("Mars")
	local function shares(h, name, want)
		math.randomseed(7)
		local n, sum, bad = 3000, {0, 0, 0}, 0
		for _ = 1, n do
			local lng, lat = (math.random() * 2 - 1) * math.pi, math.asin(math.random() * 2 - 1)
			local w = oapi.site_material(h, lng, lat)
			if not w then return false, name .. ": no site materials" end
			local tot = 0
			for i = 1, 3 do
				if w[i] < 0 or w[i] > 1 then bad = bad + 1 end
				sum[i] = sum[i] + w[i]; tot = tot + w[i]
			end
			if tot > 1 + 1e-9 then bad = bad + 1 end
		end
		local ok = bad == 0
		for i = 1, 3 do if math.abs(sum[i] / n - want[i]) > 0.04 then ok = false end end
		return ok, string.format("%s: mean weights %.3f %.3f %.3f (want %.2f %.2f %.2f), %d out of range", name, sum[1] / n, sum[2] / n, sum[3] / n, want[1], want[2], want[3], bad)
	end
	local ok7, msg7 = shares(he, "TRAPPIST-1 e", {0.22, 0.30, 0.04})
	log(msg7)
	check("site-shares-e", ok7, "the Ocean class' shares")
	ok7, msg7 = shares(hb, "Barnard b", {0.18, 0.30, 0.0})
	log(msg7)
	check("site-shares-barnard", ok7, "the Rock class' shares")
	-- b. off elsewhere
	check("site-off", oapi.site_material(hearth, 0.3, 0.5) == nil and oapi.site_material(hmars, 0.3, 0.5) == nil, "none on Earth and Mars")
	-- c. the rocks follow the mask: around GL-E, rocks on the coarse material over its share
	-- of the area, few on sand
	local function rockShares(h, lng0, lat0, tag)
		local R, rad = oapi.get_size(h), 1000
		local objs = oapi.scatter_objects(h, lng0, lat0, rad, false)
		local nr, nc, nf = 0, 0, 0
		for _, o in ipairs(objs) do
			if o.kind == 0 then
				local w = oapi.site_material(h, o.lng, o.lat)
				nr = nr + 1
				if w[1] > 0.5 then nc = nc + 1 end
				if w[2] > 0.5 then nf = nf + 1 end
			end
		end
		local na, ac, af = 0, 0, 0
		for i = -20, 20 do
			for j = -20, 20 do
				local de, dn = i * rad / 20, j * rad / 20
				if de * de + dn * dn <= rad * rad then
					local lat = lat0 + dn / R
					local w = oapi.site_material(h, lng0 + de / (R * math.cos(lat)), lat)
					na = na + 1
					if w[1] > 0.5 then ac = ac + 1 end
					if w[2] > 0.5 then af = af + 1 end
				end
			end
		end
		local r = {nr = nr, nobj = #objs, oc = nr > 0 and nc / nr or 0, of = nr > 0 and nf / nr or 0, ac = ac / na, af = af / na}
		log(string.format("%s rocks within %d m: %d of %d objects; coarse share %.3f of the rocks, %.3f of the area; fine share %.3f of the rocks, %.3f of the area",
			tag, rad, nr, r.nobj, r.oc, r.ac, r.of, r.af))
		return r
	end
	local wg = oapi.site_material(he, ge.lng, ge.lat)
	log(string.format("site materials at GL-E: coarse %.2f fine %.2f flat %.2f", wg[1], wg[2], wg[3]))
	local rs, where = rockShares(he, ge.lng, ge.lat, "TRAPPIST-1 e at GL-E"), "GL-E"
	if rs.ac <= 0.02 then
		rs, where = rockShares(hb, 10 * D, 10 * D, "Barnard b (10, 10)"), "Barnard b"
	end
	check("site-rocks-coarse", rs.nr >= 20 and rs.ac > 0.02 and rs.oc >= 1.5 * rs.ac, string.format("rocks cluster on the coarse material at %s (%.3f vs %.3f of the area)", where, rs.oc, rs.ac))
	check("site-rocks-fine", rs.af > 0.02 and rs.of <= 0.6 * rs.af, string.format("rocks thin on the fine material at %s (%.3f vs %.3f of the area)", where, rs.of, rs.af))
	log(string.format("site materials: %.2f s", os.clock() - t))

	oapi.exit(failed and 1 or 0)
end

main()
