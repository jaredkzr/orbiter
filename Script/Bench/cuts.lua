-- benchmark: camera cuts around a Delta-glider parked at Mojave (Tests/Bench/CamCuts.scn).
-- Every BENCH_HOLD [s] (default 4) the next view: the cockpit, a ground observer
-- zoomed to 4 deg, a close chase, a high observer; each round places the
-- observers elsewhere (new tiles), the last (fourth) round repeats the first
-- (tiles cached). Ends at BENCH_END [s] (default 64). BENCH_VIEWS="1,3": only
-- these views (1-based), in turn; BENCH_FIXED=1: the same places every round
-- (CamToggle: cockpit and chase every second at one place: keeping the view
-- just left). Logs "bench: cut <k> view <v> round <r> at <sim t>" (analyze.py
-- -e cut: the hitch excess and recovery per cut).
local tend = tonumber(os.getenv("BENCH_END") or "64")
local hold = tonumber(os.getenv("BENCH_HOLD") or "4")
local lng0, lat0 = -118.152, 35.059
local views = {
	function (k) oapi.set_cameramode({mode='cockpit'}); oapi.set_cameraaperture(math.rad(20)) end,
	function (k) local a = 0.9 * k
		oapi.set_cameramode({mode='ground', ref='Earth', lng=lng0 + 0.03 * math.cos(a), lat=lat0 + 0.03 * math.sin(a), alt=30})
		oapi.set_cameraaperture(math.rad(4)) end,
	-- (reldist: in vessel sizes)
	function (k) oapi.set_cameramode({mode='track', trackmode='locked', reldist=3, phi=1.0 + k, tht=0.3}); oapi.set_cameraaperture(math.rad(25)) end,
	function (k) local a = 2.0 + 1.3 * k
		oapi.set_cameramode({mode='ground', ref='Earth', lng=lng0 + 0.1 * math.cos(a), lat=lat0 + 0.1 * math.sin(a), alt=2000})
		oapi.set_cameraaperture(math.rad(30)) end,
}
local only = {}
for v in string.gmatch(os.getenv("BENCH_VIEWS") or "", "%d+") do only[#only + 1] = views[tonumber(v)] end
if #only > 0 then views = only end
local fixed = os.getenv("BENCH_FIXED") == "1"
proc.wait_simdt(2)
local t0 = oapi.get_simtime()
oapi.write_log(string.format("bench: measure at %.3f", t0))
local i = 0
while true do
	local T = oapi.get_simtime() - t0
	if T >= tend then break end
	local n = math.floor(T / hold)
	if n >= i then
		local v = n % #views
		local k = math.floor(n / #views)
		if k == 3 or fixed then k = 0 end -- (the last round: the first again)
		views[v + 1](k)
		oapi.write_log(string.format("bench: cut %d view %d round %d at %.3f", n, v, k, oapi.get_simtime()))
		i = n + 1
	end
	proc.wait_frames(1)
end
oapi.write_log(string.format("bench: cuts from %.1f for %.1f s", t0, oapi.get_simtime() - t0))
oapi.exit(0)
