-- The cockpit and the vacuum (Tests/Audio/Cockpit.scn, rendered with
-- ORBITER_AUDIO_RENDER and ORBITER_AUDIO_TEST_TONE=SRC): a delta-glider in
-- orbit with a 440 Hz tone. In steps of 150 frames (3 s of sound at
-- --fixedstep=0.02):
--   0-3 s   cockpit: heard through the hull
--   3-6 s   outside: silence (Physical), heard (Cinematic)
--   6-9 s   cockpit at 20x time acceleration: faded out
--   9-12 s  cockpit at 1x: heard again

local function log(s) oapi.write_log("AudioCockpit: " .. s) end
-- one time step (proc.skip may come back within the same one)
local function next_frame()
	local t = oapi.get_simtime()
	while oapi.get_simtime() == t do proc.skip() end
end
local steps = {
	function () oapi.set_cameramode({mode = "cockpit"}) end,
	function () oapi.set_cameramode({mode = "track", trackmode = "targetrelative", reldist = 4.0, phi = 0.3, tht = 0.2}) end,
	function () oapi.set_cameramode({mode = "cockpit"}); oapi.set_tacc(20) end,
	function () oapi.set_tacc(1) end,
}
for i, f in ipairs(steps) do
	f()
	log(string.format("step %d at %.2f s sim time", i, oapi.get_simtime()))
	for k = 1, 150 do next_frame() end
end
oapi.exit(0)
