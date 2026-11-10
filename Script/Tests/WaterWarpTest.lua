-- Resting afloat (Scenarios/Tests/WaterWarpTest.scn)

local failed = false
local function log(s) oapi.write_log("WaterWarpTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg) else log("FAIL " .. name .. " " .. msg); failed = true end
end

local earth, R
local function height(v) return oapi.global_to_equ(earth, v:get_globalpos()).rad - R end
local function wait(T) local t0 = oapi.get_simtime(); while oapi.get_simtime() - t0 < T do proc.wait_frames(1) end end

local function main()
	proc.wait_frames(3)
	earth = oapi.get_objhandle("Earth"); R = oapi.get_size(earth)
	local set, drop = vessel.get_interface("SET"), vessel.get_interface("DROP")
	-- 1. set down on the water by the scenario: afloat at once
	local w = set:get_waterstate()
	log(string.format("SET: status %d, afloat %s, height %.2f m", set:get_flightstatus(), tostring(w.afloat), height(set)))
	check("SET", set:get_flightstatus() == 1 and w.afloat, "starts afloat")
	check("SET", height(set) > 0.3 and height(set) < 2.5, "at its draft (not on its gear on the surface)")
	-- 2. let go above the water: comes to rest afloat
	local st = drop:get_status(2)
	local up = vec.unit(vec.sub(drop:get_globalpos(), oapi.get_globalpos(earth)))
	st.status = 0; st.vrot = {x=0, y=0, z=0}
	st.rpos = vec.add(st.rpos, vec.mul(up, 3.0 - height(drop)))
	drop:defset_status(st)
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 90 and not drop:get_waterstate().afloat do proc.wait_frames(1) end
	log(string.format("DROP: afloat %s after %.1f s, height %.2f m", tostring(drop:get_waterstate().afloat), oapi.get_simtime() - t0, height(drop)))
	check("DROP", drop:get_waterstate().afloat and drop:get_flightstatus() == 1, "comes to rest afloat")
	-- 3. an hour at 100x and 1000x
	local hmin, hmax = 1e9, -1e9
	for _, warp in ipairs({ 100, 1000 }) do
		oapi.set_tacc(warp)
		local t1 = oapi.get_simtime()
		while oapi.get_simtime() - t1 < 1800 do
			proc.wait_frames(1)
			for _, v in ipairs({ set, drop }) do
				local h = height(v)
				hmin = math.min(hmin, h); hmax = math.max(hmax, h)
			end
		end
	end
	oapi.set_tacc(1)
	proc.wait_frames(3)
	log(string.format("warp: heights %.2f .. %.2f m, SET status %d afloat %s, DROP status %d afloat %s", hmin, hmax,
		set:get_flightstatus(), tostring(set:get_waterstate().afloat), drop:get_flightstatus(), tostring(drop:get_waterstate().afloat)))
	check("warp", hmin == hmin and hmin > -1.0 and hmax < 3.5, "keeps them on the water")
	check("warp", set:get_waterstate().afloat and drop:get_waterstate().afloat, "keeps them resting afloat")
	-- 4. at 1x, held on the swell: they move with it
	local h0, dh = height(set), 0
	local t2 = oapi.get_simtime()
	while oapi.get_simtime() - t2 < 10 do proc.wait_frames(1); dh = math.max(dh, math.abs(height(set) - h0)) end
	log(string.format("SET rides the swell: heave %.2f m in 10 s", dh))
	check("SET", dh > 0.05, "rides the swell")
	-- 5. the engines take it away
	set:set_thrustergrouplevel(THGROUP.MAIN, 0.5)
	wait(8)
	set:set_thrustergrouplevel(THGROUP.MAIN, 0)
	local gs = set:get_groundspeed()
	log(string.format("SET under power: status %d, %.1f m/s", set:get_flightstatus(), gs))
	check("SET", set:get_flightstatus() == 0 and not set:get_waterstate().afloat and gs > 5, "is set free by its engines")
end

local ok, err = xpcall(main, debug.traceback)
if not ok then log("ERROR " .. tostring(err)); failed = true end
oapi.exit(failed and 1 or 0)
