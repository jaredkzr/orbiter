-- The Solar System's neighbours (Scenarios/Tests/SystemsTest.scn): the
-- stars of Config/Systems at their catalogue places, Alpha Centauri A and B
-- on their orbit, a vessel's orbit about Epsilon Eridani with the origin moved there;
-- Sgr A* and the S-stars, Nyx and Hemera; interstellar travel to a star of
-- the world.

local failed = false
local function log(s) oapi.write_log("SystemsTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end
local LY = 299792458.0 * 365.25 * 86400.0
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
local function wait_sim(dt)
	local t = oapi.get_simtime()
	repeat proc.wait_frames(1) until oapi.get_simtime() - t >= dt
end

local function main()
	proc.wait_frames(3)
	local sun = oapi.get_objhandle("Sun")
	local sp = oapi.get_globalpos(sun)

	-- 1. at the named stars' places (the catalogue: oapi.get_namedstar)
	local stars = {AlphaCentauriA = "Rigil Kentaurus", AlphaCentauriB = "Toliman", Proxima = "Proxima Centauri",
		Barnard = "Barnard's Star", EpsilonEridani = "Ran", ["TRAPPIST-1"] = "TRAPPIST-1"}
	for body, named in pairs(stars) do
		local h = oapi.get_objhandle(body)
		local s = oapi.get_namedstar(named)
		if not h or not s then check("body " .. body, false, "missing")
		else
			local d = len(sub(oapi.get_globalpos(h), sp)) / LY
			local dn = len(sub(s.pos, sp)) / LY
			-- (A and B: their barycentre's place, each on the orbit: within 30 AU)
			check("place " .. body, math.abs(d - dn) < 1e-3, string.format("%.5f ly (named star %.5f)", d, dn))
		end
	end

	-- 2. Alpha Centauri A and B: 4.0" apart on the sky in 2016.0 (their
	-- closest, Pourbaix & Boffin 2016's orbit), 28.8 AU in space
	local a, b = oapi.get_globalpos(oapi.get_objhandle("AlphaCentauriA")), oapi.get_globalpos(oapi.get_objhandle("AlphaCentauriB"))
	local ab, d = sub(a, b), len(sub(a, sp))
	local u = {x = (a.x - sp.x) / d, y = (a.y - sp.y) / d, z = (a.z - sp.z) / d}
	local along = ab.x * u.x + ab.y * u.y + ab.z * u.z
	local sky = math.deg(math.sqrt(len(ab) ^ 2 - along ^ 2) / d) * 3600
	log(string.format("Alpha Centauri A-B: %.3f AU, %.3f arcsec on the sky", len(ab) / 1.495978707e11, sky))
	check("binary", math.abs(sky - 4.02) < 0.05 and math.abs(len(ab) / 1.495978707e11 - 28.83) < 0.05, "on their orbit")

	-- 3. an orbit about Epsilon Eridani: the origin at its system, the orbit
	-- flown to the metre over half of it at time warp (not about Proxima:
	-- its planets pull it kilometres off its conic)
	local prx = vessel.get_interface("PRX")
	local hp = oapi.get_objhandle("EpsilonEridani")
	local function r() return len(sub(prx:get_globalpos(), oapi.get_globalpos(hp))) end
	local leo = vessel.get_interface("LEO")
	local earth = oapi.get_objhandle("Earth")
	local function rl() return len(sub(leo:get_globalpos(), oapi.get_globalpos(earth))) end
	local r0, rl0 = r(), rl()
	local rmin, rmax = r0, r0
	oapi.set_tacc(1000)
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 30000 do
		proc.wait_frames(1)
		local x = r()
		rmin, rmax = math.min(rmin, x), math.max(rmax, x)
	end
	oapi.set_tacc(1)
	log(string.format("orbit about Epsilon Eridani at %.0f km: %.3f-%.3f m about it over %.0f s; low Earth orbit %.3f km (from %.3f)",
		r0 / 1e3, rmin - r0, rmax - r0, oapi.get_simtime() - t0, rl() / 1e3, rl0 / 1e3))
	check("orbit", rmax - rmin < 2.0, "held to the metre: positions near the origin")
	check("parked", math.abs(rl() - rl0) < 10.0, "the Earth orbit waits on its conic")

	-- 4. the Galactic Centre: Sgr A* at the sky model's centre, the S-stars
	-- on their orbits about it (S2: GRAVITY 2020): speed as Kepler's
	-- (vis-viva, a from its 0.125058" at the centre's distance)
	local AU, PC, G = 1.495978707e11, 3.0856775814913673e16, 6.67430e-11
	local sgra = oapi.get_objhandle("SgrA")
	local gc = oapi.get_namedstar(0)
	check("sgra", sgra ~= nil and gc ~= nil and gc.body == sgra, "Sgr A* the body at the Galactic Centre")
	if sgra then
		local s2 = oapi.get_objhandle("S2")
		local r = sub(oapi.get_globalpos(s2), oapi.get_globalpos(sgra))
		local v = sub(oapi.get_globalvel(s2), oapi.get_globalvel(sgra))
		local a = 0.125058 * len(sub(oapi.get_globalpos(sgra), sp)) / PC * AU
		local vk = math.sqrt(G * oapi.get_mass(sgra) * (2 / len(r) - 1 / a))
		log(string.format("S2: %.1f AU from Sgr A*, %.1f km/s (Kepler %.1f)", len(r) / AU, len(v) / 1e3, vk / 1e3))
		check("s2", math.abs(len(v) / vk - 1) < 0.01, "on its orbit")
	end

	-- 5. Nyx (fictional), 24.5 pc away, Hemera on its 2.44 h orbit: 0.198 AU,
	-- Kepler's speed, prograde about the hole's spin (its disc's sense)
	local nyx, hem = oapi.get_objhandle("Nyx"), oapi.get_objhandle("Hemera")
	check("nyx", nyx ~= nil and hem ~= nil and math.abs(len(sub(oapi.get_globalpos(nyx), sp)) / PC - 24.5) < 0.01, "Nyx at 24.5 pc")
	if nyx and hem then
		local r = sub(oapi.get_globalpos(hem), oapi.get_globalpos(nyx))
		local v = sub(oapi.get_globalvel(hem), oapi.get_globalvel(nyx))
		local vk = math.sqrt(G * oapi.get_mass(nyx) / len(r))
		-- (the spin axis: Obliquity 2.0765, LAN -1.0873; right-handed (x, z, y))
		local e, L = 2.0765, -1.0873
		local A = {x = -math.sin(L) * math.sin(e), y = math.cos(L) * math.sin(e), z = math.cos(e)}
		local R, V = {x = r.x, y = r.z, z = r.y}, {x = v.x, y = v.z, z = v.y}
		local Lm = {x = R.y * V.z - R.z * V.y, y = R.z * V.x - R.x * V.z, z = R.x * V.y - R.y * V.x}
		local pro = (Lm.x * A.x + Lm.y * A.y + Lm.z * A.z) / len(Lm)
		log(string.format("Hemera: %.4f AU from Nyx, %.0f km/s (Kepler %.0f), its orbit's axis %.4f along the spin", len(r) / AU, len(v) / 1e3, vk / 1e3, pro))
		check("hemera", math.abs(len(r) / AU - 0.198) < 1e-3 and math.abs(len(v) / vk - 1) < 0.01 and pro > 0.999, "on its orbit, prograde")
		local nh = oapi.get_namedstar("Hemera")
		check("hemera_named", nh ~= nil and nh.body == hem and nh.index == -1, "a named star (a target) of its own")
	end

	-- 6. interstellar travel to a star of the world: its body the target
	local hv = prx:get_handle()
	oapi.reltrip_set(hv, {star = "Rigil Kentaurus", accel = 9.80665})
	local ri = oapi.reltrip_info(hv)
	check("starbody", ri.target == oapi.get_objhandle("AlphaCentauriA"), "Rigil Kentaurus: the body AlphaCentauriA")

	oapi.exit(failed and 1 or 0)
end

main()
