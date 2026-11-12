-- Sound travelling through the air (Tests/Audio/Flyby*.scn, rendered with
-- ORBITER_AUDIO_RENDER and ORBITER_AUDIO_TEST_TONE=SRC): a vessel with a
-- 440 Hz tone flown along a straight line past a listener on the ground
-- (a ground camera at sea level, looking north), at a steady speed in the
-- planet's rotating frame (the air's frame), from the start (it appears
-- there: no sound from before). The render's
-- check (Tests/CMakeLists.txt) finds in the sound the travel time, the
-- Doppler shift and, for the supersonic pass, the silence ahead of the
-- shock cone.
--
-- The scenario's description names the pass: "AUDIO_PASS sub" (Mach 0.5,
-- 100 m above the listener) or "AUDIO_PASS super" (Mach 2, 1000 m).

local earth = oapi.get_objhandle("Earth")
local R = oapi.get_size(earth)
local LNG, LAT = math.rad(-80.0), math.rad(28.6)   -- (the Atlantic: sea level)
local T0 = 0.0                                      -- the pass starts [s] (from the first frame)

local passes = {
	sub   = {h = 100.0,  v = 170.0, x0 = -3000.0,  t = 36.0},
	super = {h = 1000.0, v = 680.0, x0 = -13600.0, t = 45.0},
}

local function log(s) oapi.write_log("AudioFlyby: " .. s) end

local name = "sub"
if vessel.get_interface("SUPER") then name = "super" end
local p = passes[name]
local v = vessel.get_interface("SRC")

-- the position at time t: east along the parallel, h above sea level
local function pos(t)
	local x = p.x0 + p.v * math.max(0.0, t - T0)
	local lng = LNG + x / (R * math.cos(LAT))
	return oapi.equ_to_global(earth, {lng = lng, lat = LAT, rad = R + p.h})
end

-- the planet's angular velocity (global frame): about its local y axis
local function omega()
	local w = 2.0 * math.pi / oapi.get_planetperiod(earth)
	return mat.mul(oapi.get_rotationmatrix(earth), {x = 0, y = w, z = 0})
end

local function place(t)
	local g = pos(t)
	local ge = oapi.get_globalpos(earth)
	local st = v:get_status(2)
	st.status = 0
	st.rbody = earth
	st.rpos = vec.sub(g, ge)
	-- (the velocity: along the path, plus the turning of the planet's frame)
	local along = vec.mul(vec.sub(pos(t + 0.01), g), 100.0)
	st.rvel = vec.add(along, vec.crossp(st.rpos, omega())) -- (left-handed frame)
	st.vrot = {x = 0, y = 0, z = 0}
	v:defset_status(st)
end

oapi.set_cameramode({mode = "ground", ref = "Earth", lng = math.deg(LNG), lat = math.deg(LAT), alt = 2.0,
	phi = math.pi / 2, tht = 0.0})
log(string.format("pass %s: %.0f m up at %.0f m/s, overhead at %.3f s", name, p.h, p.v, T0 - p.x0 / p.v))

local nlog = 0
while oapi.get_simtime() < p.t do
	local t = oapi.get_simtime()
	if t > 5 and nlog < 3 then -- (how closely the vessel follows the path)
		nlog = nlog + 1
		local g = oapi.get_globalpos(v:get_handle())
		local e = vec.sub(g, pos(t))
		local dir = vec.unit(vec.sub(pos(t + 1), pos(t)))
		log(string.format("t %.3f: along-path error %.2f m (%.2f steps)", t, vec.dotp(e, dir), vec.dotp(e, dir) / (p.v * oapi.get_simstep())))
	end
	place(t)
	proc.wait_frames(1)
end
log("done")
oapi.exit(0)
