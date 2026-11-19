-- The scenario builder's test scenario (Launchpad.Builder: ScenarioBuilder::MakeTestScenario):
-- a ship on a pad, on a runway, on the surface, in orbit, and beside the one in orbit -
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
end
io.stdout:write("Scenario builder: check " .. (ok and "passed" or "failed") .. "\n")
io.stdout:flush()
