-- benchmark (nightly): a soak of BENCH_END [s wall] (default 1800) in low Earth
-- orbit (Tests/Bench/Soak.scn = HighWarp's orbit): every BENCH_CUT [s wall]
-- (default 30) the next of four views (cockpit, above in the orbit's frame,
-- a close chase, far behind), every BENCH_BURST [s wall] (default 120) a time
-- warp burst of 20 s at 100x or 1000x in turn. The heap's use and budget are in
-- the frame log every frame ("@mem"), with the process memory: after the first
-- minutes they should not grow (analyze.py: mem_slope, rss_slope).
local tend = tonumber(os.getenv("BENCH_END") or "1800")
local cut = tonumber(os.getenv("BENCH_CUT") or "30")
local burst = tonumber(os.getenv("BENCH_BURST") or "120")
local views = {
	function () oapi.set_cameramode({mode='cockpit'}); oapi.set_cameraaperture(math.rad(40) / 2) end,
	function () oapi.set_cameramode({mode='track', trackmode='chase', reldist=4, phi=0, tht=-0.45}); oapi.set_cameraaperture(math.rad(50) / 2) end,
	function () oapi.set_cameramode({mode='track', trackmode='locked', reldist=3, phi=2.6, tht=0.3}); oapi.set_cameraaperture(math.rad(40) / 2) end,
	function () oapi.set_cameramode({mode='track', trackmode='orbital', reldist=60, phi=3.0, tht=0.4}); oapi.set_cameraaperture(math.rad(30) / 2) end,
}
proc.wait_frames(30)
oapi.write_log(string.format("bench: measure at %.3f", oapi.get_simtime()))
local w0 = oapi.get_systime()
local ncut, nburst, bursting = -1, 0, false
while true do
	local T = oapi.get_systime() - w0
	if T >= tend then break end
	local n = math.floor(T / cut)
	if n > ncut then
		ncut = n
		views[n % #views + 1]()
		oapi.write_log(string.format("bench: cut %d at %.3f", n, oapi.get_simtime()))
	end
	local b = math.floor(T / burst)
	if not bursting and b > nburst then
		nburst = b
		bursting = true
		local w = (b % 2 == 1) and 100 or 1000
		oapi.set_tacc(w)
		oapi.write_log(string.format("bench: warp %g at %.3f", w, oapi.get_simtime()))
	elseif bursting and T - nburst * burst >= 20 then
		bursting = false
		oapi.set_tacc(1)
		oapi.write_log(string.format("bench: warp 1 at %.3f", oapi.get_simtime()))
	end
	proc.wait_frames(1)
end
oapi.set_tacc(1)
local v = vessel.get_focusinterface()
oapi.write_log(string.format("bench: soak of %.0f s wall to %.0f s sim, altitude %.0f km", oapi.get_systime() - w0, oapi.get_simtime(), v:get_altitude() / 1000))
oapi.exit(0)
