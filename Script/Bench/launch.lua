-- benchmark: a Shuttle launch (AscentAP, Shift+L) 3 s after the start, run
-- to T+BENCH_END [s] (default 60), at time acceleration 1 (BENCH_WARP from
-- T+BENCH_WARP_AT); the particle budget check: Tests/Bench/budget.py
proc.wait_simdt(3)
oapi.simulatebufferedkey(OAPI_KEY.L, OAPI_KEY.LSHIFT)
local t0 = oapi.get_simtime()
local tend = tonumber(os.getenv("BENCH_END") or "60")
local w1, t1 = tonumber(os.getenv("BENCH_WARP") or "1"), tonumber(os.getenv("BENCH_WARP_AT") or "1e9")
while true do
	local T = oapi.get_simtime() - t0
	if T >= tend then break end
	local w = (T >= t1) and w1 or 1
	if math.abs(oapi.get_tacc() - w) > 1e-6 then oapi.set_tacc(w) end
	proc.wait_frames(1)
end
oapi.write_log(string.format("bench: launch at %.1f, played to T+%.1f s", t0, oapi.get_simtime() - t0))
oapi.exit(0)
