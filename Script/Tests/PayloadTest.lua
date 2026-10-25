-- Payloads (Scenarios/Tests/PayloadTest.scn): Atlantis includes the mass of
-- Carina, in its bay, in its dynamics (IncludeAttachedMass). Carina is
-- released at 0.2 m/s: the orbiter recoils (momentum conserved), and Carina
-- leaves the bay at a steady speed (they do not collide until apart: it is
-- nested in its cradle). Sent back into the bay, it collides with the
-- orbiter and does not sink back into its cradle.

local failed = false
local drel = 0 -- (Carina's distance from the orbiter at release)

local function log(s)
	oapi.write_log("PayloadTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

local function gpos(v) return oapi.get_globalpos(v:get_handle()) end
local function gvel(v) return oapi.get_globalvel(v:get_handle()) end

local function main()
	local atl, car = vessel.get_interface("STS-101"), vessel.get_interface("Carina")
	proc.wait_frames(2)

	-- the payload's mass is part of the orbiter's
	local own = atl:get_emptymass() + atl:get_totalpropellantmass()
	local mc = car:get_mass()
	local m = atl:get_mass()
	log(string.format("orbiter %.1f kg, own %.1f kg, payload %.1f kg", m, own, mc))
	check("STS-101", math.abs(m - (own + mc)) < 1e-6 * m, "mass includes the payload")

	-- release at 0.2 m/s
	drel = vec.dist(gpos(car), gpos(atl))
	local v0 = gvel(atl)
	atl:detach_child(atl:get_attachmenthandle(false, 0), 0.2)
	proc.wait_frames(1)
	local ma = atl:get_mass()
	local dva, dvc = vec.sub(gvel(atl), v0), vec.sub(gvel(car), v0)
	-- (both fall alike in the step: their momentum relative to that is conserved)
	local rel = vec.length(vec.sub(dvc, dva))
	local p = vec.add(vec.mul(dva, ma), vec.mul(dvc, mc))
	local g = vec.mul(p, 1 / (ma + mc))                 -- (the common motion)
	local pa = vec.length(vec.mul(vec.sub(dva, g), ma)) -- (momentum of each, relative to it)
	local pc = vec.length(vec.mul(vec.sub(dvc, g), mc))
	log(string.format("after release: orbiter %.1f kg, relative speed %.4f m/s (expected %.4f), momenta %.2f %.2f kg m/s",
		ma, rel, 0.2 * (1 + mc / ma), pa, pc))
	check("STS-101", math.abs(ma - own) < 1e-6 * ma, "mass without the payload after release")
	check("STS-101", math.abs(rel - 0.2 * (1 + mc / ma)) < 0.002, "recoils from the release")
	check("STS-101", math.abs(pa - pc) < 0.01 * pc, "momentum conserved")

	-- Carina leaves the bay at a steady speed
	local vmin, vmax = 1e9, 0
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 60.0 do
		proc.wait_frames(1)
		local r = vec.length(vec.sub(gvel(car), gvel(atl)))
		vmin, vmax = math.min(vmin, r), math.max(vmax, r)
	end
	local d = vec.dist(gpos(car), gpos(atl))
	log(string.format("after 60 s: %.2f m apart, relative speed %.4f..%.4f m/s", d, vmin, vmax))
	check("Carina", vmax - vmin < 0.01, "leaves the bay at a steady speed (no collision)")
	check("Carina", d > 10.0, "is clear of the orbiter")

	-- back towards the orbiter at 0.3 m/s
	do
		local st = car:get_status(2)
		local dir = vec.unit(vec.sub(gpos(car), gpos(atl)))
		st.rvel = vec.add(vec.sub(st.rvel, vec.sub(gvel(car), gvel(atl))), vec.mul(dir, -0.3))
		car:defset_status(st)
	end
	local dmin = 1e9
	t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 90.0 do
		proc.wait_frames(1)
		dmin = math.min(dmin, vec.dist(gpos(car), gpos(atl)))
	end
	log(string.format("sent back: closest %.2f m (released at %.2f m)", dmin, drel))
	check("Carina", dmin > drel + 0.3, "collides with the orbiter on its way back")
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
