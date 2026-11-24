-- benchmark: Atlantis undocks from the ISS (Tests/Bench/Dock.scn) and fires
-- its RCS jets group by group, from the cockpit until BENCH_SWITCH [s]
-- (default 30), then from outside (the camera switch, and the station and
-- the shuttle's jets in view); ends at BENCH_END [s] (default 60)
local v = vessel.get_focusinterface()
local tend = tonumber(os.getenv("BENCH_END") or "60")
local tsw = tonumber(os.getenv("BENCH_SWITCH") or "30")
proc.wait_simdt(2)
v:undock(0)
local t0 = oapi.get_simtime()
oapi.write_log(string.format("bench: measure at %.3f", oapi.get_simtime()))
-- (back away first; then each pair of opposite groups in turn, so that the
-- shuttle stays near the station)
local seq = {
	{THGROUP.ATT_BACK, 4},
	{THGROUP.ATT_UP, 1.5}, {THGROUP.ATT_DOWN, 1.5}, {THGROUP.ATT_LEFT, 1.5}, {THGROUP.ATT_RIGHT, 1.5},
	{THGROUP.ATT_PITCHUP, 1}, {THGROUP.ATT_PITCHDOWN, 1}, {THGROUP.ATT_YAWLEFT, 1}, {THGROUP.ATT_YAWRIGHT, 1},
	{THGROUP.ATT_BANKLEFT, 1}, {THGROUP.ATT_BANKRIGHT, 1}, {THGROUP.ATT_FORWARD, 1}, {THGROUP.ATT_BACK, 1},
}
local i, tNext, switched = 0, t0, false
while true do
	local T = oapi.get_simtime() - t0
	if T >= tend then break end
	if not switched and T >= tsw then
		oapi.set_cameramode({mode='track', trackmode='locked', reldist=1.4, phi=2.4, tht=0.3}) -- (reldist: in the shuttle's size)
		oapi.write_log(string.format("bench: camera outside at %.3f", oapi.get_simtime())) -- (T+ in the sim time: event windows, analyze.py -e)
		switched = true
	end
	if oapi.get_simtime() >= tNext then
		if i > 0 then v:set_thrustergrouplevel(seq[i][1], 0) end
		i = i % #seq + 1
		if i == 1 and T > 1 then i = 2 end -- (the long back-away only once)
		v:set_thrustergrouplevel(seq[i][1], 1)
		tNext = oapi.get_simtime() + seq[i][2]
	end
	proc.wait_frames(1)
end
oapi.write_log(string.format("bench: dock from %.1f to T+%.1f s", t0, oapi.get_simtime() - t0))
oapi.exit(0)
