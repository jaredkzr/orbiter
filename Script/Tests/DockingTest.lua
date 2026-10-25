-- Docking with collisions (Scenarios/Tests/DockingTest.scn): in each pair,
-- the second delta-glider is placed nose to nose with the first, its
-- docking port 3 m from the first's, closing along the docking axis.
-- A: 0.2 m/s, lined up: must dock. B: 1.5 m/s: too fast, must not dock
-- (the vessels collide). C: 0.2 m/s, turned by 25 degrees about the
-- docking axis: must not dock.

local failed = false

local function log(s)
	oapi.write_log("DockingTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

-- place v2 nose to nose with v1, closing at speed, turned by roll about the
-- docking axis
local function approach(v1, v2, speed, roll)
	local R = v1:get_rotationmatrix()
	-- v2's frame: v1's turned by 180 degrees about its vertical, then by roll
	-- about the longitudinal axis
	local c, s = math.cos(roll), math.sin(roll)
	local T = {m11 = -c, m12 = s, m13 = 0, m21 = s, m22 = c, m23 = 0, m31 = 0, m32 = 0, m33 = -1}
	local R2 = mat.mmul(R, T)
	local s1 = v1:get_status(2)
	local st = v2:get_status(2)
	-- (ports at (0,-0.49,10.076) in each vessel)
	local port1 = mat.mul(R, {x = 0, y = -0.49, z = 10.076})
	local port2 = mat.mul(R2, {x = 0, y = -0.49, z = 10.076})
	local gap = mat.mul(R, {x = 0, y = 0, z = 3.0})
	st.rpos = vec.sub(vec.add(vec.add(s1.rpos, port1), gap), port2)
	st.rvel = vec.add(s1.rvel, mat.mul(R, {x = 0, y = 0, z = -speed}))
	st.arot = {x = math.atan2(R2.m23, R2.m33), y = -math.asin(R2.m13), z = math.atan2(R2.m12, R2.m11)}
	st.vrot = {x = 0, y = 0, z = 0}
	v2:defset_status(st)
end

local function main()
	local v = {}
	for _, n in ipairs({"A1", "A2", "B1", "B2", "C1", "C2"}) do v[n] = vessel.get_interface(n) end
	proc.wait_frames(2)
	approach(v.A1, v.A2, 0.2, 0)
	approach(v.B1, v.B2, 1.5, 0)
	approach(v.C1, v.C2, 0.2, math.rad(25))

	local docked = {}
	local dmin = {B = 1e9}
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 30.0 do
		proc.wait_frames(1)
		for _, p in ipairs({"A", "B", "C"}) do
			if not docked[p] and v[p .. "1"]:get_dockstatus(v[p .. "1"]:get_dockhandle(0)) then
				docked[p] = oapi.get_simtime() - t0
			end
		end
		dmin.B = math.min(dmin.B, vec.dist(oapi.get_globalpos(v.B1:get_handle()), oapi.get_globalpos(v.B2:get_handle())))
	end
	for _, p in ipairs({"A", "B", "C"}) do
		log(string.format("%s: docked after %s s", p, docked[p] and string.format("%.2f", docked[p]) or "-"))
	end
	log(string.format("B: closest centres %.2f m", dmin.B))
	check("A", docked.A ~= nil, "docks")
	check("B", docked.B == nil, "does not dock (too fast)")
	check("B", dmin.B > 18.0, "collides (does not pass through)")
	check("C", docked.C == nil, "does not dock (misaligned)")

	-- the refusals are reported (in the log, and to the vessel modules)
	local f = io.open("Orbiter.log", "r")
	local txt = f and f:read("*a") or ""
	if f then f:close() end
	local function count(pat) local n = 0; for _ in txt:gmatch(pat) do n = n + 1 end; return n end
	local nB = count("B[12]: docking with B[12] refused: closing at")
	local nC = count("C[12]: docking with C[12] refused: rolled by")
	log(string.format("refusals reported: B %d, C %d", nB, nC))
	check("B", nB == 2, "refusal reported once by each vessel")
	check("C", nC == 2, "refusal reported once by each vessel")
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
