-- Debris (Scenarios/Tests/DebrisTest.scn): parts broken off vessels.
-- GL-01 (landed) loses its left wing tip: the piece takes the part's mass,
-- falls to the ground beside the vessel and comes to rest there. GL-02 (in
-- orbit) loses a part pushed off at 2 m/s: the two separate at that speed,
-- momentum is conserved, and they do not collide.

local failed = false

local function log(s)
	oapi.write_log("DebrisTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

local function gpos(h) return oapi.get_globalpos(h) end
local function gvel(h) return oapi.get_globalvel(h) end

-- the newest piece of debris
local function newest()
	return oapi.get_debris(oapi.get_debriscount() - 1)
end

local function landed()
	local v = vessel.get_interface("GL-01")
	local hv = v:get_handle()
	local part = v:get_partat({x=-8.5, y=0, z=-4})
	local info = v:get_partinfo(part)
	local m0 = v:get_mass()
	local n = v:break_part(part, 0)
	check("GL-01", n == 1 and oapi.get_debriscount() == 1, string.format("the wing tip broke off (%d part, %d pieces)", n, oapi.get_debriscount()))
	check("GL-01", v:get_partinfo(part).flags % 2 == 1, "the part is flagged as lost")
	check("GL-01", v:break_part(part, 0) == 0, "a lost part does not break off again")
	local h = newest()
	local d = oapi.get_debrisinfo(h)
	log(string.format("piece '%s' of %s: %.1f kg, %.2f m", d.name, d.source, d.mass, d.size))
	check("GL-01", math.abs(d.mass - info.mass) < 1e-6 * info.mass, "the piece has the part's mass")
	check("GL-01", math.abs(v:get_mass() - (m0 - info.mass)) < 1e-6 * m0, "the vessel lost the part's mass")
	check("GL-01", oapi.get_objecttype(h) == OBJTP.DEBRIS, "the piece is debris")

	-- it falls beside the vessel and comes to rest
	local earth = oapi.get_objhandle("Earth")
	local function radius(x) return vec.dist(gpos(x), gpos(earth)) end
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 30 and not oapi.get_debrisinfo(h).atrest do proc.wait_frames(1) end
	local dt = oapi.get_simtime() - t0
	local dh = radius(h) - radius(hv)
	local dd = vec.dist(gpos(h), gpos(hv))
	log(string.format("at rest after %.1f s: %.2f m from the vessel, %.2f m below its centre", dt, dd, -dh))
	check("GL-01", oapi.get_debrisinfo(h).atrest, "the piece comes to rest")
	check("GL-01", dd < 15 and dh < 0 and dh > -3, "it lies on the ground beside the vessel")
	local r0 = radius(h)
	proc.wait_frames(100)
	check("GL-01", math.abs(radius(h) - r0) < 0.01, "it stays there")
end

local function orbit()
	local v = vessel.get_interface("GL-02")
	local hv = v:get_handle()
	local part = v:get_partat({x=0, y=0, z=8})
	local m0 = v:get_mass()
	local v0 = gvel(hv)
	local n = v:break_part(part, 2.0)
	check("GL-02", n >= 1, string.format("the nose broke off (%d parts)", n))
	proc.wait_frames(1)
	local h = newest()
	local md, mv = oapi.get_debrisinfo(h).mass, v:get_mass()
	local dvv, dvd = vec.sub(gvel(hv), v0), vec.sub(gvel(h), v0)
	local rel = vec.length(vec.sub(dvd, dvv))
	-- (both fall alike in the step: their momentum relative to that is conserved)
	local g = vec.mul(vec.add(vec.mul(dvv, mv), vec.mul(dvd, md)), 1 / (mv + md))
	local pv = vec.length(vec.mul(vec.sub(dvv, g), mv))
	local pd = vec.length(vec.mul(vec.sub(dvd, g), md))
	log(string.format("vessel %.1f kg, piece %.1f kg (of %.1f), relative speed %.4f m/s, momenta %.2f %.2f kg m/s", mv, md, m0, rel, pv, pd))
	check("GL-02", math.abs(mv + md - m0) < 1e-6 * m0, "mass conserved")
	check("GL-02", math.abs(rel - 2.0) < 0.01, "pushed apart at 2 m/s")
	check("GL-02", math.abs(pv - pd) < 0.01 * pd, "momentum conserved")
	local vmin, vmax = 1e9, 0
	for i = 1, 300 do
		proc.wait_frames(1)
		local r = vec.length(vec.sub(gvel(h), gvel(hv)))
		vmin, vmax = math.min(vmin, r), math.max(vmax, r)
	end
	log(string.format("after %d frames: %.1f m apart, relative speed %.4f..%.4f m/s", 300, vec.dist(gpos(h), gpos(hv)), vmin, vmax))
	check("GL-02", vmax - vmin < 0.01, "they leave each other at a steady speed (no collision)")
end

local function main()
	proc.wait_frames(2)
	landed()
	orbit()
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
