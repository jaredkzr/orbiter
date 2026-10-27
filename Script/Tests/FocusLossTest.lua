-- Losing the focus vessel (Scenarios/Tests/FocusLossTest.scn, run with
-- ORBITER_DAMAGE=1): the focus vessel breaks up and becomes a ghost
-- following its wreck, which the camera follows; switching away removes it.

local failed = false

local function log(s)
	oapi.write_log("FocusLossTest: " .. s)
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
local function gpos(h) return oapi.get_globalpos(h) end

local function main()
	proc.wait_frames(2)
	local v = vessel.get_interface("GL-01")
	local hv = v:get_handle()
	-- dropped flat from 150 m
	local st = v:get_status(2)
	local g = gpos(hv)
	local up = vec.unit(vec.sub(g, gpos(earth)))
	local x = vec.sub(v:local2global({x=1, y=0, z=0}), g)
	local y = vec.sub(v:local2global({x=0, y=1, z=0}), g)
	local z = vec.sub(v:local2global({x=0, y=0, z=1}), g)
	st.arot = {x=math.atan2(y.z, z.z), y=-math.asin(z.x), z=math.atan2(y.x, x.x)}
	st.vrot = {x=0, y=0, z=0}
	st.status = 0
	st.rpos = vec.add(st.rpos, vec.mul(up, 150))
	v:defset_status(st)

	local t0 = oapi.get_simtime()
	while oapi.get_simtime() - t0 < 10 and oapi.get_debriscount() == 0 do proc.wait_frames(1) end
	proc.wait_frames(5)
	check("GL-01", oapi.get_debriscount() > 1, string.format("broke up (%d pieces)", oapi.get_debriscount()))
	check("GL-01", oapi.get_objhandle("GL-01") == hv, "is still there (a ghost)")
	local cam = oapi.get_cameratarget()
	check("GL-01", oapi.get_objecttype(cam) == OBJTP.DEBRIS, "the camera follows a piece of the wreck")
	local info = oapi.get_debrisinfo(cam)
	local largest = 0
	for i = 0, oapi.get_debriscount() - 1 do largest = math.max(largest, oapi.get_debrisinfo(oapi.get_debris(i)).mass) end
	check("GL-01", info and info.mass == largest, "the largest piece")
	proc.wait_frames(2)
	local d = vec.dist(gpos(hv), gpos(cam))
	log(string.format("ghost %.2f m from the piece", d))
	check("GL-01", d < 1.0, "the ghost moves with it")

	-- after switching to another vessel the ghost stays while the instant
	-- replay reaches back to its destruction (not offered as a focus vessel)
	oapi.set_focusobject(oapi.get_objhandle("PB"))
	proc.wait_frames(3)
	check("GL-01", oapi.get_objhandle("GL-01") == hv and not v:get_enablefocus(), "kept for the replay, not offered as a focus vessel")
end

-- (a script error ends the session too, as a failure)
local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
