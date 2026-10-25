-- Ground contact of the stock vessels (Scenarios/Tests/ContactVesselsTest.scn):
-- each vessel is released from its landed state (a short engine pulse) and,
-- once settled (after 5 s: a vessel may be set down on other touchdown
-- points than it has, e.g. the delta-glider with its gear up), must stay at
-- rest on its touchdown points: not sinking, tipping or creeping.

local failed = false

local function log(s)
	oapi.write_log("ContactVesselsTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

-- position on the surface (planet frame) [m]
local function pos_of(ves)
	local e = ves:get_equpos()
	return {x = e.rad * math.cos(e.lat) * math.cos(e.lng), y = e.rad * math.sin(e.lat), z = e.rad * math.cos(e.lat) * math.sin(e.lng)}
end

local function main()
	local names = {"SHA", "PB", "DGS", "BELLY", "QUAD"}
	local ves, m = {}, {}
	for _, n in ipairs(names) do ves[n] = vessel.get_interface(n) end
	proc.wait_frames(2)

	for _, n in ipairs(names) do
		local v = ves[n]
		m[n] = { amin = 1e9, vmax_late = 0 }
		-- (release: a short pulse of any thruster)
		if v:get_thrustercount() > 0 then v:set_thrusterlevel(v:get_thrusterhandle(0), 0.01) end
	end
	proc.wait_frames(1)
	for _, n in ipairs(names) do
		local v = ves[n]
		if v:get_thrustercount() > 0 then v:set_thrusterlevel(v:get_thrusterhandle(0), 0) end
	end

	local t0 = oapi.get_simtime()
	local T = 30.0
	while oapi.get_simtime() - t0 < T do
		proc.wait_frames(1)
		local t = oapi.get_simtime() - t0
		for _, n in ipairs(names) do
			local v, r = ves[n], m[n]
			if t >= 5.0 then
				if not r.alt0 then
					r.alt0, r.pitch0, r.bank0 = v:get_altitude(ALTMODE.GROUND), math.deg(v:get_pitch()), math.deg(v:get_bank())
				end
				r.amin = math.min(r.amin, v:get_altitude(ALTMODE.GROUND))
			end
			if t > T - 10.0 then
				if not r.p_late then r.p_late = pos_of(v) end
				r.vmax_late = math.max(r.vmax_late, v:get_groundspeed())
			end
		end
	end

	for _, n in ipairs(names) do
		local v, r = ves[n], m[n]
		r.creep = vec.dist(pos_of(v), r.p_late)
		r.pitch, r.bank = math.deg(v:get_pitch()), math.deg(v:get_bank())
		log(string.format("%s (%s): alt settled %.3f min %.3f final %.3f, pitch %.2f -> %.2f, bank %.2f -> %.2f, creep %.4f m, speed %.4f m/s, landed %s",
			n, v:get_classname(), r.alt0, r.amin, v:get_altitude(ALTMODE.GROUND), r.pitch0, r.pitch, r.bank0, r.bank, r.creep, r.vmax_late,
			tostring(v:is_landed() and true or false)))
		check(n, r.amin > r.alt0 - 0.02, "does not sink into the ground")
		check(n, math.abs(r.pitch - r.pitch0) < 0.5 and math.abs(r.bank - r.bank0) < 0.5, "keeps its attitude")
		check(n, r.creep < 0.05 and r.vmax_late < 0.05, "is at rest")
	end
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
