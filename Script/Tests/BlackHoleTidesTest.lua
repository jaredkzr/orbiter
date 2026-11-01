-- Black holes: tides (Scenarios/Tests/BlackHoleTidesTest.scn). A fall
-- from rest on a 10 solar mass hole's axis, the damage model on: parts
-- torn off where the tides reach the joints' strength (thousands of km
-- out), the vessel broken up before the horizon.

local failed = false
local function log(s) oapi.write_log("BlackHoleTidesTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local C, G = 299792458.0, 6.67430e-11
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end

local function lost(v)
	local n = 0
	for i = 0, v:get_partcount() - 1 do
		if v:get_partinfo(i).flags % 2 == 1 then n = n + 1 end
	end
	return n
end

local function main()
	proc.wait_frames(2)
	local hole = oapi.get_objhandle("SmallHole")
	local M = G * oapi.get_mass(hole) / (C * C)
	local rh = oapi.get_size(hole)
	local v = vessel.get_interface("TIDE")
	local np = v:get_partcount()
	check("parts", np > 1, string.format("%d parts", np))
	local st = v:get_status(2)
	st.status = 0
	st.rbody = hole
	st.rpos = {x = 0, y = 2e7, z = 0}
	st.rvel = {x = 0, y = 0, z = 0}
	v:defset_status(st)
	proc.wait_frames(2)
	-- (the fall from 20,000 km takes ~2.7 s: slowed to 1/10)
	oapi.set_tacc(0.1)
	local rfirst, rgone, tidal, rlast
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 10 do
		proc.wait_frames(1)
		local h = oapi.get_objhandle("TIDE")
		if not h or v:get_partinfo(0).flags % 2 == 1 then
			rgone = rgone or rlast
			break
		end
		local d = len(sub(v:get_globalpos(), oapi.get_globalpos(hole)))
		local rs = oapi.get_relstate(h)
		if rs.tidal then tidal = rs.tidal end
		if not rfirst and lost(v) > 0 then rfirst = d end
		rlast = d
	end
	oapi.set_tacc(1)
	log(string.format("10 solar masses (horizon %.1f km): first part torn off at %.0f km, the vessel broken up at %.0f km (tides there %.3g g)",
		rh / 1e3, (rfirst or -1) / 1e3, (rgone or -1) / 1e3, (tidal or -1) / 9.80665))
	-- (Newton: 2GM L / r^3 = 6 g x 1.5 over ~10 m at ~5,000 km)
	check("first", rfirst and rfirst > 1e6 and rfirst < 3e7, "parts torn off thousands of km out")
	check("gone", rgone and rgone > 10 * rh, "broken up long before the horizon")
	oapi.exit(failed and 1 or 0)
end

main()
