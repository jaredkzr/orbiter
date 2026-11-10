-- Wrecks in the water (Scenarios/Tests/WaterSinkTest.scn): run with ORBITER_DAMAGE=1.

local failed = false
local function log(s) oapi.write_log("WaterSinkTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg) else log("FAIL " .. name .. " " .. msg); failed = true end
end

local earth, R
local function height(h) return oapi.global_to_equ(earth, oapi.get_globalpos(h)).rad - R end

local function launch(v, alt, speed, dive, pitch)
	local g = v:get_globalpos()
	local up = vec.unit(vec.sub(g, oapi.get_globalpos(earth)))
	local x = vec.sub(v:local2global({x=1, y=0, z=0}), g)
	local z = vec.sub(v:local2global({x=0, y=0, z=1}), g)
	local y = vec.crossp(z, x)
	if vec.dotp(y, up) < 0 then y = vec.mul(y, -1) end
	local function tilt(a) return vec.add(vec.mul(z, math.cos(a)), vec.mul(y, math.sin(a))), vec.sub(vec.mul(y, math.cos(a)), vec.mul(z, math.sin(a))) end
	local path = tilt(-math.rad(dive))
	local zA, yA = tilt(math.rad(pitch))
	local st = v:get_status(2)
	st.status = 0; st.vrot = {x=0, y=0, z=0}
	st.rpos = vec.add(st.rpos, vec.mul(up, alt - height(v:get_handle())))
	st.rvel = vec.add(st.rvel, vec.mul(path, speed))
	v:defset_status(st)
	return {m11=x.x, m12=yA.x, m13=zA.x, m21=x.y, m22=yA.y, m23=zA.y, m31=x.z, m32=yA.z, m33=zA.z}
end

local function main()
	proc.wait_frames(2)
	earth = oapi.get_objhandle("Earth"); R = oapi.get_size(earth)
	local sp = oapi.get_seaparams(earth)
	-- 1. the dive
	local dv = vessel.get_interface("DIVE")
	local rot = launch(dv, 30, 150, 30, -30)
	proc.wait_frames(1)
	dv:set_rotationmatrix(rot)
	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 60 do proc.wait_frames(1) end
	local n, deep, rest, lowest, floated = oapi.get_debriscount(), 0, 0, 0, 0
	for i = 0, n - 1 do
		local d = oapi.get_debris(i)
		local info = oapi.get_debrisinfo(d)
		local h = height(d)
		lowest = math.min(lowest, h)
		if h < -sp.floordepth + 3 then deep = deep + 1 end
		if h > -2 then floated = floated + 1 end
		if info.atrest then rest = rest + 1 end
	end
	log(string.format("DIVE: %d pieces: %d on the seabed, %d at rest, %d afloat, lowest %.1f m (floor %.0f m)", n, deep, rest, floated, lowest, -sp.floordepth))
	check("DIVE", n >= 3, "broke up")
	check("DIVE", deep >= n / 2, "its pieces sank to the seabed")
	check("DIVE", lowest > -sp.floordepth - 3, "and no deeper")
	check("DIVE", rest >= deep / 2, "where they came to rest")
	-- 2. the long-step fall
	local fv = vessel.get_interface("FALL")
	launch(fv, 5000, 0, 90, 0)
	proc.wait_frames(2)
	oapi.set_tacc(1000)
	local t1 = oapi.get_simtime()
	local gone = false
	while oapi.get_simtime() - t1 < 120 do
		proc.wait_frames(1)
		if not oapi.get_objhandle("FALL") or fv:get_flightstatus() ~= 0 then break end
	end
	oapi.set_tacc(1)
	proc.wait_frames(3)
	local h = oapi.get_objhandle("FALL")
	log(string.format("FALL: %s after %.0f s", h and string.format("status %d at %.1f m", fv:get_flightstatus(), height(h)) or "gone", oapi.get_simtime() - t1))
	check("FALL", h == nil or fv:get_partinfo(0).flags % 2 == 1 or oapi.get_debriscount() > n, "was destroyed by the long step's impact on the sea")
end

local ok, err = xpcall(main, debug.traceback)
if not ok then log("ERROR " .. tostring(err)); failed = true end
oapi.exit(failed and 1 or 0)
