-- A delta-glider's animations heard (Tests/Audio/AnimVideo.scn), on KSC's
-- runway: the nose cone and the top hatch opening together (two motors at
-- once), the outer airlock opening, the hatch closing, the airlock closing
-- and, half way, opening again (turned back on the way); then the end.
--   ANIM_VIEW=cockpit (default) | ground (25 m off its nose, to the side)
local v = vessel.get_interface("LAND")
local view = os.getenv("ANIM_VIEW") or "cockpit"
local function log(s) oapi.write_log("AnimVideo: " .. s) end
local t0 = oapi.get_simtime()
local function at(t) while oapi.get_simtime() - t0 < t do proc.skip() end end
local function key(k, what) v:send_bufferedkey(k); log(string.format("%.2f s: %s", oapi.get_simtime() - t0, what)) end

if view == "cockpit" then
	oapi.set_cameramode({mode = 'cockpit'})
else
	local earth = v:get_surfaceref()
	local function gpos() return oapi.get_globalpos(v:get_handle()) end
	local fwd = vec.unit(vec.sub(v:local2global({x=0, y=0, z=1}), gpos()))
	local right = vec.unit(vec.sub(v:local2global({x=1, y=0, z=0}), gpos()))
	local e = oapi.global_to_equ(earth, vec.add(gpos(), vec.add(vec.mul(fwd, 12), vec.mul(right, 22))))
	oapi.set_cameramode({mode = 'ground', ref = oapi.get_objname(earth), lng = math.deg(e.lng), lat = math.deg(e.lat), alt = 2})
end
at(1.0);  key(OAPI_KEY.K, "nose cone opening")
at(1.5);  key(OAPI_KEY.U, "top hatch opening")
at(12.0); key(OAPI_KEY.O, "outer airlock opening")
at(24.0); key(OAPI_KEY.U, "top hatch closing")
at(27.0); key(OAPI_KEY.O, "outer airlock closing")
at(30.5); key(OAPI_KEY.O, "outer airlock opening again (turned back on the way)")
at(42.0)
oapi.exit(0)
