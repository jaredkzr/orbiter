-- Instant replay of damage (Scenarios/Tests/ReplayDamageTest.scn): a
-- delta-glider in orbit loses its left wing tip, pushed off at 3 m/s.
-- Before the break the replay shows the glider whole and no debris; after
-- it the piece where it was recorded; leaving the replay restores the live
-- state.

local failed = false
local function log(s) oapi.write_log("ReplayDamageTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end
local function dist(a, b) return math.sqrt((a.x-b.x)^2 + (a.y-b.y)^2 + (a.z-b.z)^2) end
local function hidden(v, part) return math.floor(v:get_partinfo(part).flags / 2) % 2 == 1 end
-- (in a script step the states are those at the end of the step)
local function now() return oapi.get_simmjd() + oapi.get_simstep() / 86400 end

local function main()
	local v = vessel.get_interface("GL-D")
	proc.wait_frames(2)
	proc.wait_simdt(1)
	local part = v:get_partat({x=-8.5, y=0, z=-4})
	local mjd0 = now()
	v:break_part(part, 3.0)
	local h = oapi.get_debris(oapi.get_debriscount() - 1)
	proc.wait_simdt(1)
	-- (relative to the glider: MJD's resolution, about 1 us, is a centimetre
	-- at orbital speed)
	local p1, mjd1 = vec.sub(oapi.get_globalpos(h), v:get_globalpos()), now()
	proc.wait_simdt(2)
	local plive = oapi.get_globalpos(h)

	-- (scripts do not run while the simulation is held: the checks are made
	-- at once, in the same step)
	check("enter", oapi.set_replay(true), "replay started")
	oapi.replay_seek(mjd0 - 0.5 / 86400)
	check("before", not hidden(v, part), "the wing tip is shown on the glider before the break")
	check("before", oapi.get_debrisinfo(h).hidden, "the piece is not shown before the break")
	oapi.replay_seek(mjd1)
	local d = dist(vec.sub(oapi.get_globalpos(h), v:get_globalpos()), p1)
	check("after", hidden(v, part) and not oapi.get_debrisinfo(h).hidden, "after the break the piece is shown, not the wing tip")
	check("after", d < 0.01, string.format("the piece %.5f m from where it was recorded (relative to the glider)", d))
	oapi.set_replay(false)
	local dl = dist(oapi.get_globalpos(h), plive)
	check("restored", dl < 1e-6 and hidden(v, part) and not oapi.get_debrisinfo(h).hidden,
		string.format("live state: the piece %.3g m from where it was", dl))
	oapi.exit(failed and 1 or 0)
end

local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	oapi.exit(1)
end
