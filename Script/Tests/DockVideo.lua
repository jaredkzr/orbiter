-- Docking heard (Tests/Audio/DockVideo.scn): delta-glider A2 placed nose to
-- nose with A1, its docking port 3 m off, closing at 0.2 m/s along the
-- docking axis (as Tests/DockingTest's pair A); docked, then undocked 6 s
-- later; 6 s after that, the end. From A1's cockpit or outside:
--   DOCK_VIEW=cockpit (default) | outside
local v1, v2 = vessel.get_interface("A1"), vessel.get_interface("A2")
local view = os.getenv("DOCK_VIEW") or "cockpit"
local function log(s) oapi.write_log("DockVideo: " .. s) end
local function wait(dt) local t = oapi.get_simtime() + dt; while oapi.get_simtime() < t do proc.skip() end end

proc.wait_frames(2)
-- (as DockingTest's approach: v2's frame v1's turned 180 degrees about its vertical)
local R = v1:get_rotationmatrix()
local T = {m11 = -1, m12 = 0, m13 = 0, m21 = 0, m22 = 1, m23 = 0, m31 = 0, m32 = 0, m33 = -1}
local R2 = mat.mmul(R, T)
local s1 = v1:get_status(2)
local st = v2:get_status(2)
local port1 = mat.mul(R, {x = 0, y = -0.49, z = 10.076})
local port2 = mat.mul(R2, {x = 0, y = -0.49, z = 10.076})
st.rpos = vec.sub(vec.add(vec.add(s1.rpos, port1), mat.mul(R, {x = 0, y = 0, z = 3.0})), port2)
st.rvel = vec.add(s1.rvel, mat.mul(R, {x = 0, y = 0, z = -0.2}))
st.arot = {x = math.atan2(R2.m23, R2.m33), y = -math.asin(R2.m13), z = math.atan2(R2.m12, R2.m11)}
st.vrot = {x = 0, y = 0, z = 0}
v2:defset_status(st)
if view == "cockpit" then
	oapi.set_cameramode({mode = 'cockpit'})
else
	-- (beside the two: 60 m, from the side and a little above)
	oapi.set_cameramode({mode = 'track', trackmode = 'targetrelative', reldist = 3.0, phi = math.rad(80), tht = math.rad(15)})
end
local t0 = oapi.get_simtime()
while not v1:get_dockstatus(v1:get_dockhandle(0)) do
	if oapi.get_simtime() - t0 > 40 then log("no docking"); oapi.exit(1) end
	proc.skip()
end
log(string.format("docked after %.2f s", oapi.get_simtime() - t0))
wait(6.0)
v1:undock(0)
log(string.format("undocked at %.2f s", oapi.get_simtime() - t0))
wait(6.0)
oapi.exit(0)
