-- Water entry (Scenarios/Tests/WaterEntryTest.scn): a ditching delta-glider
-- and a ShuttlePB dropped flat at 9 m/s, in the default sea.

local failed = false
local function log(s) oapi.write_log("WaterEntryTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg) else log("FAIL " .. name .. " " .. msg); failed = true end
end

local earth, R

local function height(v) return oapi.global_to_equ(earth, v:get_globalpos()).rad - R end

-- set a vessel going: height alt [m], speed along a path dive [deg] below
-- the horizon, its nose pitch [deg] above the horizon, wings level
local function launch(v, alt, speed, dive, pitch)
	local g = v:get_globalpos()
	local up = vec.unit(vec.sub(g, oapi.get_globalpos(earth)))
	local x = vec.sub(v:local2global({x=1, y=0, z=0}), g)
	local z = vec.sub(v:local2global({x=0, y=0, z=1}), g)
	local y = vec.crossp(z, x)
	if vec.dotp(y, up) < 0 then y = vec.mul(y, -1) end
	local function tilt(a) return vec.add(vec.mul(z, math.cos(a)), vec.mul(y, math.sin(a))), vec.sub(vec.mul(y, math.cos(a)), vec.mul(z, math.sin(a))) end
	local path = (dive >= 89.9) and vec.mul(up, -1) or tilt(-math.rad(dive))
	local zA, yA = tilt(math.rad(pitch))
	local st = v:get_status(2)
	st.status = 0; st.vrot = {x=0, y=0, z=0}
	st.rpos = vec.add(st.rpos, vec.mul(up, alt - height(v)))
	st.rvel = vec.add(st.rvel, vec.mul(path, speed))
	v:defset_status(st)
	return {m11=x.x, m12=yA.x, m13=zA.x, m21=x.y, m22=yA.y, m23=zA.y, m31=x.z, m32=yA.z, m33=zA.z}
end

local function main()
	proc.wait_frames(2)
	earth = oapi.get_objhandle("Earth"); R = oapi.get_size(earth)
	local sp = oapi.get_seaparams(earth)
	log(string.format("sea: wind %.1f m/s, Hs %.2f m, Tp %.1f s", sp.wind, sp.hs, sp.tp))
	local dg, pb = vessel.get_interface("DITCH"), vessel.get_interface("DROP")
	local rdg = launch(dg, 2.5, 70, 2, 8)
	local rpb = launch(pb, 1.6, 9, 90, 0)
	proc.wait_frames(1)
	dg:set_rotationmatrix(rdg); pb:set_rotationmatrix(rpb)
	local e0 = dg:get_equpos()
	local t0 = oapi.get_simtime()
	local m = { DITCH = { low = 1e9, bank = 0 }, DROP = { low = 1e9, bank = 0 } }
	local T = 40
	while oapi.get_simtime() - t0 < T do
		proc.wait_frames(1)
		for n, v in pairs({ DITCH = dg, DROP = pb }) do
			local r = m[n]
			r.low = math.min(r.low, height(v))
			if oapi.get_simtime() - t0 > T - 10 then
				local gv = v:get_groundspeedvector(REFFRAME.HORIZON)
				r.late = math.max(r.late or 0, math.sqrt(gv.x^2 + gv.z^2))
			end
		end
	end
	local e1 = dg:get_equpos()
	local dist = R * math.sqrt((e1.lat - e0.lat)^2 + ((e1.lng - e0.lng) * math.cos(e1.lat))^2)
	for n, v in pairs({ DITCH = dg, DROP = pb }) do
		local w, r = v:get_waterstate(), m[n]
		log(string.format("%s: peak %.1f g, entry %.1f m/s, slam %.0f kPa, lowest %.2f m, now %.2f m, pitch %.1f bank %.1f deg, late speed %.2f m/s, submerged %.2f of %.1f m3",
			n, w.accmax / 9.81, w.entryspeed, w.slammax / 1000, r.low, height(v), math.deg(v:get_pitch()), math.deg(v:get_bank()), r.late, w.submerged, w.volume))
		check(n, w.near and w.wet, "is in the water")
		check(n, r.low > -5, "does not go under")
		check(n, math.abs(height(v)) < v:get_size(), "floats at the surface")
		check(n, math.abs(v:get_bank()) < math.rad(25), "is upright")
		check(n, r.late < 1.0, "has stopped")
	end
	log(string.format("DITCH: ran out %.0f m", dist))
	check("DITCH", dist > 40 and dist < 1500, "runs out a plausible distance")
	check("DITCH", m.DITCH and dg:get_waterstate().accmax < 30 * 9.81, "decelerates below 30 g")
	check("DROP", pb:get_waterstate().accmax > 3 * 9.81 and pb:get_waterstate().accmax < 60 * 9.81, "is slammed between 3 and 60 g")
end

local ok, err = xpcall(main, debug.traceback)
if not ok then log("ERROR " .. tostring(err)); failed = true end
oapi.exit(failed and 1 or 0)
