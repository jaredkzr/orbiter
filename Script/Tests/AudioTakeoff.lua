-- A vessel's own sounds (Tests/Audio/Takeoff.scn, rendered with
-- ORBITER_AUDIO_RENDER): a delta-glider at the start of KSC's runway 33,
-- heard from the ground 300 m to its side. Main engines to full at 2 s;
-- the roar arrives 300 m / 340 m/s = 0.88 s later. Ends at 12 s.
local v = vessel.get_interface("GL-01")
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
local g = oapi.global_to_equ(oapi.get_objhandle("Earth"), oapi.get_globalpos(v:get_handle()))
-- 300 m west of the vessel (the runway runs north-north-west)
local R = oapi.get_size(oapi.get_objhandle("Earth"))
oapi.set_cameramode({mode = 'ground', ref = 'Earth', lng = math.deg(g.lng - 300.0 / (R * math.cos(g.lat))), lat = math.deg(g.lat), alt = 2})
oapi.write_log("AudioTakeoff: listening")
at(2.0); v:set_thrustergrouplevel(THGROUP.MAIN, 1.0); oapi.write_log("AudioTakeoff: main engines full")
at(12.0); oapi.exit(0)
