-- Every landing pad of every base (Scenarios/Tests/BasePadsTest.scn): PAD (a
-- delta-glider) is set down on each in turn and must settle there: still
-- landed, near the pad's centre, level, at rest. Catches pads on slopes,
-- pads inside structures (the collision hulls push the vessel out) and pads
-- off the ground (a floating base's deck).

local failed = false
local npad, nbad = 0, 0

local function log(s)
	oapi.write_log("BasePadsTest: " .. s)
end

local TILT_MAX = 6.0   -- [deg] (on level ground: ~0)
local DRIFT_MAX = 1.0  -- [m] (sliding after touchdown)
local SPEED_MAX = 0.2  -- [m/s]
local SETTLE = 4.0     -- [s]

local function main()
	proc.wait_frames(2)
	local v = vessel.get_interface("PAD")
	local bodies = {}
	for i = 0, oapi.get_objcount() - 1 do
		local h = oapi.get_objhandle(i)
		if oapi.get_objecttype(h) == OBJTP.PLANET and oapi.get_basecount(h) > 0 then bodies[#bodies + 1] = h end
	end
	for _, hp in ipairs(bodies) do
		local R = oapi.get_size(hp)
		local pname = oapi.get_objname(hp)
		for b = 0, oapi.get_basecount(hp) - 1 do
			local hb = oapi.get_basebyindex(hp, b)
			local bname = oapi.get_objname(hb)
			local only = os.getenv("PADS_BASE") -- (one base only, when looking into it)
			for p = 0, (only and bname ~= only) and -1 or oapi.get_basepadcount(hb) - 1 do
				local lng, lat = oapi.get_basepadequpos(hb, p)
				local st = v:get_status(2)
				st.status = 1
				st.rbody = hp
				st.base = hb
				st.port = p
				st.surf_lng, st.surf_lat, st.surf_hdg = lng, lat, 0.0
				-- (light before it is set down, and all the time: the pad refuels
				-- the vessel on it, and a mass changed at once has the gear
				-- spring it up at lift-off)
				v:set_propellantmass(v:get_propellanthandle(0), 650.0)
				do
					v:defset_status(st)
					local tw = oapi.get_simtime()
					while oapi.get_simtime() - tw < (p == 0 and 2.0 or 0.06) do
						-- (light, all the time: the pad refuels the vessel on it, and
						-- a mass changed at once would have the gear spring it up)
						v:set_propellantmass(v:get_propellanthandle(0), 650.0)
						proc.wait_frames(1)
					end
				end
				-- (a hop: a landed vessel is held where it was put; dropped
				-- from a few decimetres it comes to rest on its own)
				local drift
				do
					v:set_wheelbrakelevel(1.0, 0, true) -- (else it rolls on)
					local a0 = v:get_altitude(ALTMODE.GROUND)
					local t0 = oapi.get_simtime()
					local up = true
					local td = nil -- (where it touched down)
					local prev = nil
					while oapi.get_simtime() - t0 < 40.0 do
						if up then
							-- (up until it rises at 0.3 m/s: a few centimetres, on any body)
							v:set_thrustergrouplevel(THGROUP.HOVER, 1.0)
							local a = v:get_altitude(ALTMODE.GROUND)
							local dt = oapi.get_simstep()
							if (prev and dt > 0 and (a - prev) / dt > 0.3) or a > a0 + 0.3 or oapi.get_simtime() - t0 > 3.0 then up = false; t0 = oapi.get_simtime() end
							prev = a
						else
							v:set_thrustergrouplevel(THGROUP.HOVER, 0.0)
							if not td and v:get_altitude(ALTMODE.GROUND) < a0 + 0.05 then td = v:get_equpos() end
							if oapi.get_simtime() - t0 > SETTLE and v:get_flightstatus() % 2 == 1 then break end
						end
						proc.wait_frames(1)
					end
					v:set_thrustergrouplevel(THGROUP.HOVER, 0.0)
					local e = v:get_equpos()
					td = td or e
					drift = math.sqrt(((e.lat - td.lat) * R) ^ 2 + ((e.lng - td.lng) * R * math.cos(lat)) ^ 2)
				end
				local tilt = math.deg(math.acos(math.cos(v:get_pitch()) * math.cos(v:get_bank())))
				local spd = v:get_groundspeed()
				-- (the ground's slope under the pad: terrain, or a structure?)
				local d = 10.0
				local dlat, dlng = d / R, d / (R * math.cos(lat))
				local sx = (oapi.surface_elevation(hp, lng + dlng, lat) - oapi.surface_elevation(hp, lng - dlng, lat)) / (2 * d)
				local sz = (oapi.surface_elevation(hp, lng, lat + dlat) - oapi.surface_elevation(hp, lng, lat - dlat)) / (2 * d)
				local slope = math.deg(math.atan(math.sqrt(sx * sx + sz * sz)))
				local landed = v:get_flightstatus() % 2 == 1
				local ok = landed and drift < DRIFT_MAX and tilt < TILT_MAX and spd < SPEED_MAX
				npad = npad + 1
				local line = string.format("%s %s pad %d: slid %.2f m, tilt %.1f deg (ground %.1f deg), speed %.2f m/s, %s",
					pname, bname, p + 1, drift, tilt, slope, spd, landed and "landed" or "NOT LANDED")
				if ok then log("ok   " .. line) else log("FAIL " .. line); nbad = nbad + 1; failed = true end
			end
		end
	end
	log(string.format("%d pads, %d failed", npad, nbad))
end

local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
