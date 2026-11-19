-- The Shuttle stack on pad 39B (Scenarios/Tests/Cape39StackTest.scn): Atlantis
-- as "Atlantis satellite Launch" has it, its boosters' skirts on the mobile
-- launcher platform's deck (Canaveral.cfg: MESH MLP on pad 39B). It must stay
-- landed, still (under 5 cm in 10 s) and at its height; where it stands in the
-- pad's frame and the deck's height are logged (Utils/BaseKit/blender_kit.py
-- cape: the platform's deck under the skirts).

local failed = false
local function log(s) oapi.write_log("Cape39StackTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local LNG0, LAT0 = math.rad(-80.65165), math.rad(28.58438) -- (Cape Canaveral's origin)
local PAD = { -4759.2, 3001.0 }                              -- (pad 39B in the base's frame: x south, z east [m])

local function main()
	proc.wait_frames(2)
	local E = oapi.get_objhandle("Earth")
	local R = oapi.get_size(E)
	local v = vessel.get_interface("STS-101")
	local p0 = v:get_equpos()
	local a0 = v:get_altitude(ALTMODE.GROUND)
	local x = (LAT0 - p0.lat) * R - PAD[1]
	local z = (p0.lng - LNG0) * R * math.cos(p0.lat) - PAD[2]
	log(string.format("stack at x %.2f z %.2f m from pad 39B's origin; %.3f m above the ground (its skirts %.3f m)",
		x, z, a0, a0 - 18.962))
	local plng = LNG0 + PAD[2] / (R * math.cos(LAT0 - PAD[1] / R))
	local e0 = oapi.surface_elevation(E, plng, LAT0 - PAD[1] / R)
	local e1 = oapi.surface_elevation(E, p0.lng, p0.lat)
	log(string.format("ground: %.3f m at the pad's origin (the pad's meshes' y = 0), %.3f m under the stack: the skirts at the pad's y %.3f m",
		e0, e1, e1 - e0 + a0 - 18.962))
	local t0 = oapi.get_simtime()
	local amin, amax = a0, a0
	while oapi.get_simtime() - t0 < 10.0 do
		proc.wait_frames(1)
		local a = v:get_altitude(ALTMODE.GROUND)
		amin, amax = math.min(amin, a), math.max(amax, a)
	end
	local p1 = v:get_equpos()
	local moved = math.sqrt(((p1.lat - p0.lat) * R) ^ 2 + ((p1.lng - p0.lng) * R * math.cos(p0.lat)) ^ 2)
	log(string.format("after 10 s: moved %.4f m, height %.3f..%.3f m, landed %s",
		moved, amin, amax, tostring(v:get_flightstatus() % 2 == 1)))
	check("stack", v:get_flightstatus() % 2 == 1, "stays landed")
	check("stack", moved < 0.05, "stays still")
	check("stack", amax - amin < 0.05, "keeps its height")
	check("stack", math.abs(a0 - 47.361) < 0.5, "stands at the scenario's height")
end

local ok, err = xpcall(main, debug.traceback)
if not ok then log("ERROR " .. tostring(err)); failed = true end
oapi.exit(failed and 1 or 0)
