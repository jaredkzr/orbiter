-- Exoplanets (Scenarios/Tests/ExoplanetTest.scn): the planets of the
-- neighbouring systems on their orbits (periods as measured), a transit of
-- TRAPPIST-1 e in front of its star seen from the Sun at the measured
-- epoch, tidal locking (meridian 0 to the star), the procedural ground of
-- TRAPPIST-1 e (seas over their share of it), and a vessel standing on it.

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
	local t = os.clock()
	-- (coarse tiles, level 6: a query below the finest level makes its tile
	-- each time)
	for i = 0, 23 do
		local lat = math.asin(-1 + (i + 0.5) / 12)
		for j = 0, 47 do
			local lng = -math.pi + (j + 0.5) * math.pi / 24
			local e = oapi.surface_elevation(he, lng, lat, 9)
			n = n + 1
			if math.abs(e) < 0.5 then below = below + 1 end
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

	oapi.exit(failed and 1 or 0)
end

main()
