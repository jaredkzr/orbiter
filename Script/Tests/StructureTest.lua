-- Vessel structure (Scenarios/Tests/StructureTest.scn): the structural
-- parts of some vessels. Their masses add up to the empty mass, part 0 is
-- the core part (at the origin), parents come before their children, and
-- each joint's strength is at least the load of the parts beyond it at the
-- design load factor times the safety factor (within the spread of
-- strengths).

local failed = false

local function log(s)
	oapi.write_log("StructureTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

local SAFETY, SPREAD, G0 = 1.5, 0.1, 9.81

local function structure(name, nmin, nmax)
	local v = vessel.get_interface(name)
	local n = v:get_partcount()
	local parts, msum = {}, 0
	for i = 0, n - 1 do
		local p = v:get_partinfo(i)
		parts[i] = p
		msum = msum + p.mass
		log(string.format("%s part %d '%s': %.0f kg at (%.2f %.2f %.2f) r %.2f, parent %d, %d hulls, strength %.0f kN",
			name, i, p.name, p.mass, p.cg.x, p.cg.y, p.cg.z, p.radius, p.parent, p.nhull, p.strength / 1000))
	end
	check(name, n >= nmin and n <= nmax, string.format("has %d parts (%d..%d)", n, nmin, nmax))
	local em = v:get_emptymass()
	check(name, math.abs(msum - em) < 1e-6 * em, string.format("part masses add up to the empty mass (%.1f of %.1f kg)", msum, em))
	check(name, parts[0].parent == -1 and parts[0].strength == 0, "part 0 is the core part")
	check(name, v:get_partat({x=0, y=0, z=0}) == 0, "the core part is at the origin")
	local order, strength = true, true
	local sub = {}
	for i = n - 1, 0, -1 do
		sub[i] = (sub[i] or 0) + parts[i].mass
		local pa = parts[i].parent
		if i > 0 then
			if pa < 0 or pa >= i then order = false else sub[pa] = (sub[pa] or 0) + sub[i] end
		end
	end
	-- (at least the inertial case; more where the vessel's own forces load it)
	for i = 1, n - 1 do
		local s0 = parts[i].loadfactor * SAFETY * G0 * sub[i]
		if parts[i].strength / s0 < 1 - SPREAD - 1e-9 then strength = false end
	end
	check(name, order, "parents come before their children")
	check(name, strength, "joint strengths follow the load factor")
	return v, parts
end

local function main()
	proc.wait_frames(2)
	local dg = structure("GL-01", 6, 32)
	-- the wing tips are parts of their own, not the core
	local l, r = dg:get_partat({x=-8.5, y=0, z=-4}), dg:get_partat({x=8.5, y=0, z=-4})
	log(string.format("GL-01 wing tips: parts %d and %d", l, r))
	check("GL-01", l ~= r and l > 0 and r > 0, "the wing tips are separate parts")
	-- (mirrored: same mass, centres mirrored)
	local pl, pr = dg:get_partinfo(l), dg:get_partinfo(r)
	check("GL-01", math.abs(pl.mass - pr.mass) < 1e-6 * pl.mass and math.abs(pl.cg.x + pr.cg.x) < 1e-6
		and math.abs(pl.cg.z - pr.cg.z) < 1e-6, "the wing tips are mirrored")
	check("GL-01", pl.loadfactor == 12, "load factor from Config/Damage/DeltaGlider.cfg")
	local _, pb = structure("PB-01", 1, 16)
	check("PB-01", pb[0].loadfactor == 6, "default load factor")
	structure("STS-101", 6, 40)
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
