-- Docking with collisions (Scenarios/Tests/DockingTest.scn): DG2 is placed
-- nose to nose with DG1, its docking port 3 m from DG1's, closing at
-- 0.2 m/s. They must dock within 30 s.

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

local function main()
	local dg1, dg2 = vessel.get_interface("DG1"), vessel.get_interface("DG2")
	proc.wait_frames(2)

	-- DG2: turned by 180 degrees about DG1's vertical axis, its port 3 m from
	-- DG1's port along DG1's longitudinal axis
	local R = dg1:get_rotationmatrix()
	local R2 = {m11 = -R.m11, m12 = R.m12, m13 = -R.m13,
	            m21 = -R.m21, m22 = R.m22, m23 = -R.m23,
	            m31 = -R.m31, m32 = R.m32, m33 = -R.m33}
	local s1 = dg1:get_status(2)
	local s = dg2:get_status(2)
	s.rpos = vec.add(s1.rpos, mat.mul(R, {x = 0, y = 0, z = 2 * 10.076 + 3.0}))
	s.rvel = vec.add(s1.rvel, mat.mul(R, {x = 0, y = 0, z = -0.2}))
	s.arot = {x = math.atan2(R2.m23, R2.m33), y = -math.asin(R2.m13), z = math.atan2(R2.m12, R2.m11)}
	s.vrot = {x = 0, y = 0, z = 0}
	dg2:defset_status(s)

	local docked_at = nil
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 30.0 do
		proc.wait_frames(1)
		if not docked_at and dg1:get_dockstatus(dg1:get_dockhandle(0)) then
			docked_at = oapi.get_simtime() - t0
		end
	end
	log(string.format("docked after %s s", docked_at and string.format("%.2f", docked_at) or "-"))
	check("DG", docked_at ~= nil, "dock")
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
