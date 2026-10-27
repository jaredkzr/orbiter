-- Saving damage (Scenarios/Tests/DamageSaveTest.scn): delta-gliders lose
-- parts, the scenario is saved as Scenarios/Tests/DamageRoundtrip.scn (in
-- the build tree, run by Scenario.DamageLoadTest), with what it must show
-- in Script/Tests/DamageRoundtrip_expect.lua. Run with ORBITER_DAMAGE=1
-- (GONE, spun up, breaks up).

local failed = false
local function log(s) oapi.write_log("DamageSaveTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local function main()
	proc.wait_frames(2)
	local g1, g2 = vessel.get_interface("GL-01"), vessel.get_interface("GL-02")
	g1:break_part(g1:get_partat({x=-8.5, y=0, z=-4}), 0)
	g2:break_part(g2:get_partat({x=0, y=0, z=8}), 1.0)
	local gone = vessel.get_interface("GONE")
	gone:set_angvel({x=0, y=0, z=15})
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 10 and not oapi.get_debrisinfo(oapi.get_debris(0)).atrest do proc.wait_frames(1) end
	check("GL-01", oapi.get_debrisinfo(oapi.get_debris(0)).atrest, "its wing tip comes to rest")

	-- what the loaded scenario must show: the vessels' masses and parts lost,
	-- the pieces (mass, at rest), the positions of the pieces relative to
	-- their vessels
	local e = {vessels = {}, pieces = {}}
	check("GONE", gone:get_partinfo(0).flags % 2 == 1, "broke up spun at 15 rad/s")
	for _, v in ipairs({g1, g2, gone}) do
		local lost = {}
		for i = 0, v:get_partcount() - 1 do lost[#lost + 1] = v:get_partinfo(i).flags % 2 end
		e.vessels[v:get_name()] = {mass = v:get_mass(), lost = table.concat(lost, "")}
	end
	for i = 0, oapi.get_debriscount() - 1 do
		local h = oapi.get_debris(i)
		local d = oapi.get_debrisinfo(h)
		-- (at rest: its planet-frame position; in motion: relative to GL-02, in
		-- orbit with them)
		local r, vr = nil, 0
		if d.atrest then
			local q = oapi.global_to_equ(oapi.get_objhandle("Earth"), oapi.get_globalpos(h))
			r = {x=q.lng, y=q.lat, z=q.rad}
		else
			r = vec.sub(oapi.get_globalpos(h), oapi.get_globalpos(oapi.get_objhandle("GL-02")))
			vr = vec.length(vec.sub(oapi.get_globalvel(h), oapi.get_globalvel(oapi.get_objhandle("GL-02"))))
		end
		e.pieces[#e.pieces + 1] = string.format("{name=%q, mass=%.6f, atrest=%s, r={x=%.12f, y=%.12f, z=%.6f}, vr=%.4f}",
			d.name, d.mass, tostring(d.atrest), r.x, r.y, r.z, vr)
	end
	local f = io.open("Script/Tests/DamageRoundtrip_expect.lua", "w")
	f:write("return {\n  vessels = {\n")
	for n, v in pairs(e.vessels) do f:write(string.format("    [%q] = {mass=%.6f, lost=%q},\n", n, v.mass, v.lost)) end
	f:write("  },\n  pieces = {\n")
	for _, p in ipairs(e.pieces) do f:write("    " .. p .. ",\n") end
	f:write("  }\n}\n")
	f:close()

	check("save", oapi.savescenario("Tests/DamageRoundtrip", "Saved by DamageSaveTest"), "scenario saved")
	-- (the saved scenario runs the load test's script)
	local fn = "Scenarios/Tests/DamageRoundtrip.scn"
	local fi = io.open(fn)
	local s = fi:read("*a")
	fi:close()
	local n
	s, n = s:gsub("Script Tests/DamageSaveTest", "Script Tests/DamageLoadTest")
	local fo = io.open(fn, "w")
	fo:write(s)
	fo:close()
	check("save", n == 1 and s:find("BEGIN_DEBRIS") and s:find("DAMAGE"), "with the damage and the debris")
end

local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
