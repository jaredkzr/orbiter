-- The scenario builder's test scenario (Launchpad.Builder: ScenarioBuilder::MakeTestScenario):
-- a ship on a pad, on a runway, on the surface, in orbit (half fuelled), beside the one in orbit,
-- docked to it, and a station set to pass over the base -
-- are they where they were put?
local ok = true
local function check(name, cond, msg)
	io.stdout:write(string.format("Scenario builder: %s %s %s\n", cond and "ok" or "WRONG", name, msg))
	if not cond then ok = false end
end
proc.skip()
local E = oapi.get_objhandle("Earth")
local R = oapi.get_size(E)
local function ship(n) local h = oapi.get_objhandle(n); return h and vessel.get_interface(n) end
local pad, rwy, surf, orb, near = ship("PAD"), ship("RWY"), ship("SURF"), ship("ORB"), ship("NEAR")
check("ships", pad and rwy and surf and orb and near, "all five made")
if pad and rwy and surf and orb and near then
	-- on the ground: at Cape Canaveral (two of them), at 117 W 35 N
	local function equ(v) local e = oapi.global_to_equ(E, oapi.get_globalpos(v:get_handle())); return math.deg(e.lng), math.deg(e.lat), e.rad - R end
	local lng, lat, alt = equ(pad)
	check("pad", math.abs(lng + 80.65) < 0.2 and math.abs(lat - 28.58) < 0.2 and alt < 200, string.format("%.3f %.3f, %.0f m", lng, lat, alt))
	local lng2, lat2, alt2 = equ(rwy)
	check("runway", math.abs(lng2 + 80.65) < 0.2 and math.abs(lat2 - 28.58) < 0.2 and alt2 < 200 and (math.abs(lng2 - lng) + math.abs(lat2 - lat)) > 1e-4,
		string.format("%.3f %.3f, %.0f m", lng2, lat2, alt2))
	lng, lat, alt = equ(surf)
	check("surface", math.abs(lng + 117.0) < 0.01 and math.abs(lat - 35.0) < 0.01 and alt < 5000, string.format("%.3f %.3f, %.0f m", lng, lat, alt))
	-- in orbit: 300 km up, 51.6 degrees to the equator, eastwards
	local p0, v0 = orb:get_relativepos(E), orb:get_relativevel(E)
	local r = vec.length(p0)
	local axis = oapi.get_rotationmatrix(E)
	axis = { x = axis.m12, y = axis.m22, z = axis.m32 }
	local hv = vec.crossp(p0, v0)
	local inc = math.deg(math.acos(math.abs(vec.dotp(hv, axis)) / vec.length(hv)))
	local l0 = equ(orb)
	for i = 1, 10 do proc.skip() end
	local l1 = equ(orb)
	local east = ((l1 - l0 + 540) % 360) - 180
	check("orbit", math.abs(r - R - 300e3) < 5e3 and math.abs(inc - 51.6) < 0.3 and east > 0 and math.abs(vec.length(v0) - 7726) < 30,
		string.format("%.1f km up, %.2f deg, %.0f m/s, %s", (r - R) / 1e3, inc, vec.length(v0), east > 0 and "eastwards" or "westwards"))
	local d = vec.length(vec.sub(near:get_relativepos(E), orb:get_relativepos(E)))
	check("beside", math.abs(d - 500) < 25, string.format("%.0f m from it", d))
	-- docked to the one in orbit, which is half fuelled (a ship on a pad is refuelled there)
	local dock = ship("DOCK")
	check("docked ship", dock ~= nil, "made")
	if dock then
		local mate = dock:get_dockstatus(dock:get_dockhandle(0))
		local dd = vec.length(vec.sub(dock:get_relativepos(E), orb:get_relativepos(E)))
		check("docked", mate ~= nil and dd < 60, string.format("%s, %.1f m from it", mate and "docked" or "not docked", dd))
	end
	local ph = orb:get_propellanthandle(0)
	local lvl = orb:get_propellantmass(ph) / orb:get_propellantmaxmass(ph)
	check("fuel", math.abs(lvl - 0.5) < 0.01, string.format("%.0f %%", lvl * 100))
	-- a station set to pass over Cape Canaveral 10 minutes after the start: its two-body
	-- path to then, and the Earth turned on to then
	local over = ship("OVER")
	check("station", over ~= nil, "made")
	if over then
		local mu = 6.6743e-11 * oapi.get_mass(E)
		local p, v = over:get_relativepos(E), over:get_relativevel(E)
		local T = 600 - oapi.get_simtime()
		local function acc(q) local r = vec.length(q); return vec.mul(q, -mu / (r * r * r)) end
		local n = math.floor(T / 0.5)
		local h = T / n
		for i = 1, n do -- (Runge-Kutta, fourth order)
			local k1v, k1p = acc(p), v
			local k2v, k2p = acc(vec.add(p, vec.mul(k1p, h / 2))), vec.add(v, vec.mul(k1v, h / 2))
			local k3v, k3p = acc(vec.add(p, vec.mul(k2p, h / 2))), vec.add(v, vec.mul(k2v, h / 2))
			local k4v, k4p = acc(vec.add(p, vec.mul(k3p, h))), vec.add(v, vec.mul(k3v, h))
			p = vec.add(p, vec.mul(vec.add(vec.add(k1p, vec.mul(vec.add(k2p, k3p), 2)), k4p), h / 6))
			v = vec.add(v, vec.mul(vec.add(vec.add(k1v, vec.mul(vec.add(k2v, k3v), 2)), k4v), h / 6))
		end
		local R0 = oapi.get_rotationmatrix(E)
		local q = { x = R0.m11 * p.x + R0.m21 * p.y + R0.m31 * p.z, y = R0.m12 * p.x + R0.m22 * p.y + R0.m32 * p.z, z = R0.m13 * p.x + R0.m23 * p.y + R0.m33 * p.z }
		local a = 2 * math.pi / 86164.10132 * T
		local c, s = math.cos(a), math.sin(a)
		local x, z = c * q.x + s * q.z, -s * q.x + c * q.z
		local slng, slat = math.deg(math.atan2(z, x)), math.deg(math.asin(q.y / vec.length(p)))
		local blng, blat = equ(pad)
		check("overflight", math.abs(slng - blng) < 0.5 and math.abs(slat - blat) < 0.5 and v.y > -1e9,
			string.format("over %.2f %.2f at 10 min (the base: %.2f %.2f), %.0f km up", slng, slat, blng, blat, (vec.length(p) - R) / 1e3))
	end
end
io.stdout:write("Scenario builder: check " .. (ok and "passed" or "failed") .. "\n")
io.stdout:flush()
