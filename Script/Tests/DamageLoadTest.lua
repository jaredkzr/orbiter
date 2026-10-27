-- Loading damage (Scenarios/Tests/DamageRoundtrip.scn, saved by
-- DamageSaveTest in the build tree): the vessels have the parts lost and
-- the masses they had, the pieces of debris are where they were, those at
-- rest still at rest.

local failed = false
local function log(s) oapi.write_log("DamageLoadTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local function main()
	local e = dofile("Script/Tests/DamageRoundtrip_expect.lua")
	proc.wait_frames(1)
	for n, ev in pairs(e.vessels) do if n ~= "GONE" then
		local v = vessel.get_interface(n)
		local lost = {}
		for i = 0, v:get_partcount() - 1 do lost[#lost + 1] = v:get_partinfo(i).flags % 2 end
		lost = table.concat(lost, "")
		check(n, lost == ev.lost, string.format("parts lost %s (saved %s)", lost, ev.lost))
		check(n, math.abs(v:get_mass() - ev.mass) < 1e-3, string.format("mass %.3f kg (saved %.3f)", v:get_mass(), ev.mass))
	end end
	-- (the vessel that broke up is not saved, not being the focus vessel;
	-- its wreck is)
	local nw = 0
	for i = 0, oapi.get_debriscount() - 1 do
		if oapi.get_debrisinfo(oapi.get_debris(i)).source == "GONE" then nw = nw + 1 end
	end
	check("GONE", oapi.get_objhandle("GONE") == nil and nw > 0, string.format("not saved; its wreck of %d pieces is", nw))
	check("debris", oapi.get_debriscount() == #e.pieces, string.format("%d pieces (saved %d)", oapi.get_debriscount(), #e.pieces))
	for i = 0, oapi.get_debriscount() - 1 do
		local h = oapi.get_debris(i)
		local d = oapi.get_debrisinfo(h)
		local p = e.pieces[i + 1]
		check(d.name, d.name == p.name and math.abs(d.mass - p.mass) < 1e-3 and d.atrest == p.atrest,
			string.format("%.1f kg, at rest %s", d.mass, tostring(d.atrest)))
		if d.atrest then -- (on the ground: where it was)
			local q = oapi.global_to_equ(oapi.get_objhandle("Earth"), oapi.get_globalpos(h))
			local dr = math.sqrt(((q.lng - p.r.x) * math.cos(q.lat) * q.rad)^2 + ((q.lat - p.r.y) * q.rad)^2 + (q.rad - p.r.z)^2)
			check(d.name, dr < 0.01, string.format("%.4f m from where it lay", dr))
		else -- (in orbit, relative to GL-02: the saved states are those of one
		     -- frame later than the time saved, and a frame has passed)
			local r = vec.sub(oapi.get_globalpos(h), oapi.get_globalpos(oapi.get_objhandle("GL-02")))
			local dr = math.sqrt((r.x-p.r.x)^2 + (r.y-p.r.y)^2 + (r.z-p.r.z)^2)
			local tol = 0.05 + 2.5 * oapi.get_simstep() * p.vr
			check(d.name, dr < tol, string.format("%.4f m from where it was, relative to GL-02 (moving at %.1f m/s)", dr, p.vr))
		end
	end
end

local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
