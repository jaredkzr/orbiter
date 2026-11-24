-- Vessels created from a script (Scenarios/Tests/CreateVesselTest.scn):
-- oapi.create_vessel at the script's start and between frames, from raw
-- statuses (VESSELSTATUS, VESSELSTATUS2) and a table, several in a row, a
-- missing class. Each new vessel is put in free flight 2 km up, 80 m east
-- of the last (no ground, no collisions).
-- A hang fails by the test's timeout.

local failed = false
local function log(s) oapi.write_log("CreateVesselTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg) else log("FAIL " .. name .. " " .. msg); failed = true end
end

local dg = vessel.get_interface("DG")
local made = {}
local slot = 0
-- (the DG's position relative to the Earth, with its velocity; the up and east
-- directions there)
local function place(rpos)
	slot = slot + 1
	local up = vec.unit(rpos)
	local east = vec.unit(vec.crossp({x = 0, y = 1, z = 0}, up))
	return vec.add(vec.add(rpos, vec.mul(up, 2000)), vec.mul(east, 80 * slot))
end

-- a raw VESSELSTATUS of the DG's, in flight at the next place
local function raw1()
	local st = dg:get_rawstatus(1)
	local s = st:get()
	st:set({status = 0, rpos = place(s.rpos), vrot = {x = 0, y = 0, z = 0}})
	return st
end

local function create(name, class, st)
	local h = oapi.create_vessel(name, class, st)
	check(name, h ~= nil, "created (" .. class .. ")")
	if h then made[#made + 1] = name end
	return h
end

-- at the script's start (before any frame)
create("START_PB", "ShuttlePB", raw1())
create("START_DG", "DeltaGlider", raw1())

proc.wait_frames(2)
local t0 = oapi.get_simtime()

-- between frames: one a frame (the Delta-glider's interpreter: the hang)
for i = 1, 3 do
	create("FRAME_DG" .. i, "DeltaGlider", raw1())
	proc.wait_frames(1)
end
-- several in a row
for i = 1, 3 do create("BURST_DG" .. i, "DeltaGlider", raw1()) end
-- from a raw VESSELSTATUS2
do
	local st = dg:get_rawstatus(2)
	local s = st:get()
	st:set({status = 0, rpos = place(s.rpos), vrot = {x = 0, y = 0, z = 0}})
	create("VS2_DG", "DeltaGlider", st)
end
-- from a status table (as get_status(1) gives)
do
	local st = dg:get_status(1)
	st.status = 0
	st.rpos = place(st.rpos)
	create("TABLE_PB", "ShuttlePB", st)
end
-- a class that does not exist: nil, and the session goes on
check("NOCLASS", oapi.create_vessel("NOCLASS", "NoSuchVesselClass", raw1()) == nil, "gives nil")
-- not a status: an error, caught
check("NOSTATUS", not pcall(oapi.create_vessel, "NOSTATUS", "ShuttlePB", 42), "is an error")

-- the session goes on
proc.wait_frames(20)
check("frames", oapi.get_simtime() > t0 + 0.3, string.format("sim time went on (%.2f s)", oapi.get_simtime() - t0))
for _, n in ipairs(made) do check(n, oapi.get_objhandle(n) ~= nil, "is there") end
-- (parked apart: each at least 40 m from the others and the DG)
local all = { "DG" }
for _, n in ipairs(made) do all[#all + 1] = n end
local dmin, pair = 1e30, ""
for i = 1, #all do
	for j = i + 1, #all do
		local a, b = vessel.get_interface(all[i]), vessel.get_interface(all[j])
		local d = vec.length(vec.sub(a:get_globalpos(), b:get_globalpos()))
		if d < dmin then dmin, pair = d, all[i] .. "-" .. all[j] end
	end
end
check("apart", dmin > 40, string.format("closest %s %.1f m", pair, dmin))
log(failed and "FAILED" or "all passed")
oapi.exit(failed and 1 or 0)
