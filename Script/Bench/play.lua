-- benchmark: a recorded flight played back, the time acceleration by
-- BENCH_SCHED ("t:w,t:w,..." from sim time t [s], acceleration w),
-- ending at BENCH_END [s]
local sched = {}
for t, w in string.gmatch(os.getenv("BENCH_SCHED") or "0:1", "([%d%.]+):([%d%.]+)") do sched[#sched + 1] = {tonumber(t), tonumber(w)} end
local tend = tonumber(os.getenv("BENCH_END") or "1e9")
local function want(t)
	local w = 1
	for _, s in ipairs(sched) do if t >= s[1] then w = s[2] end end
	return w
end
local tw = oapi.get_systime()
-- BENCH_PROBE="t0:t1:name,name,...": the vessels' states relative to Earth each frame in [t0, t1]
local probe = os.getenv("BENCH_PROBE")
local p0, p1, pnames
if probe then
	local a, b, c = string.match(probe, "([%d%.]+):([%d%.]+):(.+)")
	p0, p1, pnames = tonumber(a), tonumber(b), {}
	for n in string.gmatch(c, "[^,]+") do pnames[#pnames + 1] = n end
end
local earth = oapi.get_objhandle("Earth")
while true do
	local t = oapi.get_simtime()
	if probe and t >= p0 and t <= p1 then
		for _, n in ipairs(pnames) do
			local h = oapi.get_objhandle(n)
			if h then
				local p, v = oapi.get_relativepos(h, earth), oapi.get_relativevel(h, earth)
				oapi.write_log(string.format("probe t=%.3f dt=%.3f %s r=%.6e pos=(%.6e %.6e %.6e) v=(%.4e %.4e %.4e)", t, oapi.get_simstep(), n,
					math.sqrt(p.x*p.x+p.y*p.y+p.z*p.z), p.x, p.y, p.z, v.x, v.y, v.z))
			end
		end
	end
	if t >= tend then break end
	local w = want(t)
	-- (not past the end in one frame)
	if w > 1 and (tend - t) < w * 0.2 then w = math.max(1, (tend - t) / 0.2) end
	if math.abs(oapi.get_tacc() - w) > 1e-6 * w then oapi.set_tacc(w) end
	proc.wait_frames(1)
end
oapi.write_log(string.format("bench: played to %.1f s in %.0f s wall", oapi.get_simtime(), oapi.get_systime() - tw))
oapi.exit(0)
