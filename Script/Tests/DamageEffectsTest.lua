-- Damage effects (Scenarios/Tests/DamageEffectsTest.scn): what losing parts
-- does to Delta-gliders, each compared with an intact twin.

local failed = false

local function log(s)
	oapi.write_log("DamageEffectsTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

local earth = oapi.get_objhandle("Earth")
local function gpos(v) return oapi.get_globalpos(v:get_handle()) end
local function gvel(v) return oapi.get_globalvel(v:get_handle()) end
local function up(v) return vec.unit(vec.sub(gpos(v), oapi.get_globalpos(earth))) end
-- the local vertical in the vessel's frame
local function localup(v) return vec.sub(v:global2local(vec.add(gpos(v), up(v))), v:global2local(gpos(v))) end
local function wait(t)
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < t do proc.wait_frames(1) end
end

-- landed: the nose (with the nose gear) breaks off; the glider tips onto its nose
-- (onto the piece under it, which pushes it up again once they settle: the
-- lowest pitch)
local function nose()
	local v = vessel.get_interface("NOSE")
	local p0 = v:get_pitch()
	local part = v:get_partat({x=0, y=-1, z=9})
	local n = v:break_part(part, 0)
	local p1 = p0
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 8 do
		proc.wait_frames(1)
		p1 = math.min(p1, v:get_pitch())
	end
	log(string.format("NOSE: %d parts lost ('%s'), pitch %.2f -> %.2f deg (lowest)", n, v:get_partinfo(part).name, math.deg(p0), math.deg(p1)))
	check("NOSE", n >= 1 and p1 < p0 - math.rad(2), "tips forward without its nose gear")
end

-- in flight at 3 km, 200 m/s: the left wing tip breaks off; it rolls left
local function flight()
	local air, ref = vessel.get_interface("AIR"), vessel.get_interface("REF")
	local rot = {}
	for i, v in ipairs({air, ref}) do
		local st = v:get_status(2)
		local fwd = vec.sub(v:local2global({x=0, y=0, z=1}), gpos(v))
		-- (its axes on the ground: level)
		local g = gpos(v)
		local x = vec.sub(v:local2global({x=1, y=0, z=0}), g)
		local y = vec.sub(v:local2global({x=0, y=1, z=0}), g)
		local z = vec.sub(v:local2global({x=0, y=0, z=1}), g)
		rot[i] = {m11=x.x, m12=y.x, m13=z.x, m21=x.y, m22=y.y, m23=z.y, m31=x.z, m32=y.z, m33=z.z}
		st.status = 0
		st.rpos = vec.add(st.rpos, vec.mul(up(v), 3000))
		st.rvel = vec.add(st.rvel, vec.mul(fwd, 200))
		v:defset_status(st)
	end
	proc.wait_frames(1)
	for i, v in ipairs({air, ref}) do v:set_rotationmatrix(rot[i]) end -- (level, as on the ground)
	proc.wait_frames(1)
	local part = air:get_partat({x=-8.5, y=0, z=-4})
	air:break_part(part, 0)
	wait(2)
	local ua, ur = localup(air), localup(ref)
	log(string.format("AIR: local vertical x %.4f (intact twin %.4f), bank %.2f (%.2f) deg",
		ua.x, ur.x, math.deg(air:get_bank()), math.deg(ref:get_bank())))
	check("AIR", ua.x - ur.x > 0.02, "rolls to the left without its left wing tip")
end

-- in orbit: main engines lost; left wing tip lost (yaws under main thrust)
local function orbit()
	local r, e, w = vessel.get_interface("ORB-R"), vessel.get_interface("ORB-E"), vessel.get_interface("ORB-W")
	-- the parts of the main engines
	local lost = 0
	for i = 0, e:get_groupthrustercount(THGROUP.MAIN) - 1 do
		local th = e:get_groupthruster(THGROUP.MAIN, i)
		local part = e:get_partat(e:get_thrusterpos(th))
		lost = lost + e:break_part(part, 0)
		log(string.format("ORB-E: main engine %d at part %d '%s'", i, part, e:get_partinfo(part).name))
	end
	local pmi0 = w:get_pmi()
	w:break_part(w:get_partat({x=-8.5, y=0, z=-4}), 0)
	local pmi1 = w:get_pmi()
	log(string.format("ORB-W: pmi (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)", pmi0.x, pmi0.y, pmi0.z, pmi1.x, pmi1.y, pmi1.z))
	check("ORB-W", pmi1.x < pmi0.x and pmi1.y < pmi0.y and pmi1.z < pmi0.z, "smaller moments of inertia without its wing tip")

	for _, v in ipairs({r, e, w}) do v:set_thrustergrouplevel(THGROUP.MAIN, 1) end
	proc.wait_frames(2)
	local tr, te = vec.length(r:get_thrustvector()), vec.length(e:get_thrustvector())
	wait(2)
	local wr, ww = r:get_angvel(), w:get_angvel()
	log(string.format("main thrust %.0f kN (intact), %.0f kN (engines lost); after 2 s yaw rate %.4f (intact), %.4f (wing tip lost) rad/s",
		tr / 1000, te / 1000, wr.y, ww.y))
	check("ORB-E", lost >= 1 and tr > 0 and te == 0, "no main thrust without its engines")
	check("ORB-W", math.abs(ww.y) > math.abs(wr.y) + 0.01, "yaws under main thrust (centre of gravity off the thrust line)")
end

local function main()
	proc.wait_frames(2)
	nose()
	flight()
	orbit()
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
