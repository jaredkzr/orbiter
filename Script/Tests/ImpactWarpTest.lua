-- Impacts under high time acceleration (Scenarios/Tests/ImpactWarpTest.scn):
-- vessels hitting the Moon in long steps are set down and landed, quickly
-- and without their states going bad.

local failed = false
local function log(s) oapi.write_log("ImpactWarpTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end

local moon

local function state(name)
	local v = vessel.get_interface(name)
	local r = len(sub(v:get_globalpos(), oapi.get_globalpos(moon)))
	return v, r - oapi.get_size(moon), v:get_flightstatus()
end

local function run(name, warp, simt, wallmax)
	local t0, w0 = oapi.get_simtime(), oapi.get_systime()
	oapi.set_tacc(warp)
	local bad = false
	while oapi.get_simtime() - t0 < simt and oapi.get_systime() - w0 < wallmax do
		proc.wait_frames(1)
		local _, alt = state(name)
		if alt ~= alt then bad = true; break end
		local _, _, st = state(name)
		if st == 1 then break end
	end
	oapi.set_tacc(1)
	proc.wait_frames(3)
	local v, alt, st = state(name)
	local vr = len(sub(v:get_globalvel(), oapi.get_globalvel(moon)))
	log(string.format("%s at %gx: status %d, altitude %.3f km, %.1f m/s, %.1f s wall", name, warp, st, alt / 1000, vr, oapi.get_systime() - w0))
	return not bad and alt == alt, st, alt, vr, oapi.get_systime() - w0
end

local function main()
	proc.wait_frames(2)
	moon = oapi.get_objhandle("Moon")

	-- 1. from rest 1,000 km up, at 1,000x (20 s steps; ~1.4 km/s at the ground)
	local v = vessel.get_interface("FALL")
	local st = v:get_status(2)
	st.status = 0; st.rbody = moon
	st.rpos = {x = 1738e3 + 1000e3, y = 0, z = 0}; st.rvel = {x = 0, y = 0, z = 0}
	v:defset_status(st)
	proc.wait_frames(5)
	local ok, s, alt, vr, wall = run("FALL", 1000, 4000, 60)
	-- (landed: on the terrain, the Moon's surface within +-10 km of its mean radius)
	check("fall", ok and s == 1 and math.abs(alt) < 11e3 and vr < 10 and wall < 20,
		string.format("landed at %.3f km, %.1f m/s, %.1f s", alt / 1000, vr, wall))

	-- 2. an orbit through the ground, at 10,000x (stabilised steps of 200 s)
	ok, s, alt, vr, wall = run("DIVE", 10000, 20000, 60)
	check("dive", ok and s == 1 and math.abs(alt) < 11e3 and vr < 10 and wall < 20,
		string.format("landed at %.3f km, %.1f m/s, %.1f s", alt / 1000, vr, wall))

	-- 3. the high orbit is undisturbed
	local _, ahigh, shigh = state("HIGH")
	check("high", shigh == 0 and ahigh > 1200e3, string.format("in orbit at %.0f km", ahigh / 1000))

	oapi.exit(failed and 1 or 0)
end

main()
