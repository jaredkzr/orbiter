-- A vessel's own sounds in vacuum (Tests/Audio/MoonLiftoff.scn, rendered
-- with ORBITER_AUDIO_RENDER): a delta-glider lifting off at Brighton Beach
-- on its hover engines. Outside (0-6 s): silence (physical). In the cockpit
-- (6-12 s): the engines through the structure, an attitude pulse at 9 s.
local v = vessel.get_focusinterface()
local function at(t) while oapi.get_simtime() < t do proc.skip() end end
oapi.set_cameramode({mode = 'track', trackmode = 'locked', reldist = 3, phi = 0.5, tht = 0.2})
at(1.0); v:set_thrustergrouplevel(THGROUP.HOVER, 0.35)
at(6.0); oapi.set_cameramode({mode = 'cockpit'})
at(9.0); v:set_thrustergrouplevel(THGROUP.ATT_YAWLEFT, 1.0)
at(9.3); v:set_thrustergrouplevel(THGROUP.ATT_YAWLEFT, 0.0)
at(12.0); oapi.exit(0)
