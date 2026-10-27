-- Destruction (Scenarios/Tests/DestructionTest.scn, run with
-- ORBITER_DAMAGE=1): structural loads break parts off, and break vessels up.

local failed = false

local function log(s)
	oapi.write_log("DestructionTest: " .. s)
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
local function up(v) return vec.unit(vec.sub(gpos(v), oapi.get_globalpos(earth))) end
local function wait(t, until_)
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < t do
		proc.wait_frames(1)
		if until_ and until_() then break end
	end
end
local function lost(v)
	local n = 0
	for i = 0, v:get_partcount() - 1 do
		if v:get_partinfo(i).flags % 2 == 1 then n = n + 1 end
	end
	return n
end
local function maxload(v)
	local m = 0
	for i = 1, v:get_partcount() - 1 do m = math.max(m, v:get_partinfo(i).load) end
	return m
end
local function exists(name) return oapi.get_objhandle(name) ~= nil end

-- vessels on the ground placed in the air, level as on the ground: alt
-- [m] up, speed [m/s] forward, vs [m/s] vertical
local function airborne(list)
	local rot = {}
	for i, e in ipairs(list) do
		local v = e.v
		local st = v:get_status(2)
		local g = gpos(v)
		local x = vec.sub(v:local2global({x=1, y=0, z=0}), g)
		local y = vec.sub(v:local2global({x=0, y=1, z=0}), g)
		local z = vec.sub(v:local2global({x=0, y=0, z=1}), g)
		local R = {m11=x.x, m12=y.x, m13=z.x, m21=x.y, m22=y.y, m23=z.y, m31=x.z, m32=y.z, m33=z.z}
		rot[i] = R
		st.arot = {x=math.atan2(R.m23, R.m33), y=-math.asin(R.m13), z=math.atan2(R.m12, R.m11)}
		st.vrot = {x=0, y=0, z=0}
		st.status = 0
		st.rpos = vec.add(st.rpos, vec.mul(up(v), e.alt))
		st.rvel = vec.add(st.rvel, vec.add(vec.mul(z, e.speed), vec.mul(up(v), e.vs)))
		v:defset_status(st)
	end
	proc.wait_frames(1)
	for i, e in ipairs(list) do e.v:set_rotationmatrix(rot[i]) end
	proc.wait_frames(1)
end

local function main()
	proc.wait_frames(2)
	local cruise, pull = vessel.get_interface("CRUISE"), vessel.get_interface("PULL")
	local drop, hard = vessel.get_interface("DROP"), vessel.get_interface("HARD")
	local nparts = drop:get_partcount()
	local heat = vessel.get_interface("HEAT")
	airborne({{v=cruise, alt=3000, speed=200, vs=0}, {v=pull, alt=3000, speed=320, vs=0},
		{v=drop, alt=150, speed=0, vs=0}, {v=hard, alt=3, speed=0, vs=-4},
		{v=heat, alt=90000, speed=11000, vs=-2000}})

	-- Jupiter: below the crush depth (23 bar)
	local jup = oapi.get_objhandle("Jupiter")
	local deep = vessel.get_interface("DEEP")
	local st = deep:get_status(2)
	st.rbody = jup
	st.status = 0
	st.rpos = {x=oapi.get_size(jup) - 300e3, y=0, z=0}
	st.rvel = {x=0, y=0, z=0}
	deep:defset_status(st)

	pull:set_adclevel(AIRCTRL.ELEVATOR, 1)
	local spin = vessel.get_interface("SPIN")
	spin:set_angvel({x=0, y=0, z=6})
	local peak, tcruise, theat, tfail = 0, 0, 0, -1
	local ndeb0 = oapi.get_debriscount()
	local t0 = oapi.get_simtime()
	local function maxtemp(v)
		local m = 0
		for i = 0, v:get_partcount() - 1 do m = math.max(m, v:get_partinfo(i).temp) end
		return m
	end
	local function heating()
		if exists("HEAT") then
			theat = math.max(theat, maxtemp(heat))
			if tfail < 0 and lost(heat) > 0 then tfail = oapi.get_simtime() - t0 end
		elseif tfail < 0 then tfail = oapi.get_simtime() - t0 end
	end
	wait(10, function()
		if exists("CRUISE") then peak = math.max(peak, maxload(cruise)); tcruise = math.max(tcruise, maxtemp(cruise)) end
		heating()
		return false
	end)

	log(string.format("CRUISE: %d parts lost, peak joint load %.2f of strength, hottest part %.0f K", exists("CRUISE") and lost(cruise) or -1, peak, tcruise))
	check("CRUISE", exists("CRUISE") and lost(cruise) == 0 and peak < 0.5, "cruises without damage")
	check("CRUISE", tcruise < 400, "stays cool")
	check("PULL", exists("PULL") and lost(pull) == 0, "pulls up at full elevator at 320 m/s without damage")
	local ns = exists("SPIN") and lost(spin) or -1
	local tips = exists("SPIN") and spin:get_partinfo(spin:get_partat({x=-8.5, y=0, z=-4})).flags % 2 == 1
		and spin:get_partinfo(spin:get_partat({x=8.5, y=0, z=-4})).flags % 2 == 1
	log(string.format("SPIN: %d parts lost", ns))
	check("SPIN", tips, "loses its wing tips spun at 6 rad/s")
	check("HARD", exists("HARD") and lost(hard) == 0, "lands hard on its gear without damage")
	check("DROP", not exists("DROP"), "breaks up dropped flat from 150 m")
	check("DEEP", not exists("DEEP"), "is crushed below 23 bar in Jupiter")
	local nd = oapi.get_debriscount() - ndeb0
	log(string.format("%d pieces of debris", nd))
	check("DROP", nd >= 2 * nparts, "the broken-up vessels are debris")

	-- (lunar return speed: heating grows as v^3, drag as v^2)
	wait(50, function() heating() return false end)
	log(string.format("HEAT: hottest part %.0f K, failed after %.1f s (%s)", theat, tfail, exists("HEAT") and (lost(heat) .. " parts lost") or "destroyed"))
	check("HEAT", tfail >= 0 and theat > 1700, "heats up and fails entering steeply at 11 km/s")
	check("PB", exists("PB"), "the focus vessel is still there")
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
