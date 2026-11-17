-- A delta-glider's landing on KSC's runway 33, flown by a simple autopilot,
-- for listening to its contact sounds in context (Tests/Audio/LandingVideo.scn):
-- final approach from LV_HEIGHT m (default 35), sinking 3 m/s at LV_SPEED m/s (105),
-- flare to about LV_SINK m/s (default 1.3: the wheels close at about 1.2 m/s), thrust cut, the nose lowered,
-- the brakes (LV_BRAKE) from 2 s after touchdown to a stop.
--   LV_VIEW=ground (default: beside the runway where it comes down) | chase | cockpit
--   LV_GEAR=up: a belly landing (Tests/Audio/LandingVideoBelly.scn: the gear up)
--     - flared to LV_SINK (1.0), sliding to a stop on the hull, no brakes
--   LV_CRASH=1: a crash (LandingVideoCrash.scn, with ORBITER_DAMAGE=1): no
--     flare, flown into the runway sinking LV_CRASH_SINK (12 m/s), nose down
--   LV_TD: where the ground camera stands, past the threshold [m] (1350)
--   LV_SIDE: the approach that far to the right of the runway's centreline
--     [m] (0; e.g. 120: down on the grass beside it - the ground's sounds)
local v = vessel.get_interface("LAND")
local earth = v:get_surfaceref()
local earthName = oapi.get_objname(earth)
local function gpos() return oapi.get_globalpos(v:get_handle()) end
local function env(n, d) return tonumber(os.getenv(n) or "") or d end
local BELLY = os.getenv("LV_GEAR") == "up"
local CRASH = os.getenv("LV_CRASH") == "1"
local H0, SINK = env("LV_HEIGHT", 35.0), env("LV_SINK", BELLY and 1.0 or 1.3)
local CRASH_SINK = env("LV_CRASH_SINK", 12.0)
local VREF, BACK = env("LV_SPEED", 105.0), -200.0   -- [m/s] approach speed, [m] start short of where it stands (the threshold; < 0: past it - over the runway all the way: no terrain under the approach)
local view = os.getenv("LV_VIEW") or "ground"
local SIDE = env("LV_SIDE", 0.0)
LV_ROLL = env("LV_ROLL", 30.0)   -- [s] of the rollout shown (at most: it ends stopped)
LV_BRAKE = env("LV_BRAKE", 0.2)  -- the brakes' level (full: about 1.3 g - far harder than an airliner's)

-- where it stands: the threshold; its height there: the wheels on the ground
local alt0 = v:get_altitude(ALTMODE.GROUND)
local altM0 = v:get_altitude(ALTMODE.MEANRAD)   -- (flown by: smooth - the ground's height under the runway is uneven)
local up = vec.unit(vec.sub(gpos(), oapi.get_globalpos(earth)))
local fwd = vec.unit(vec.sub(v:local2global({x=0, y=0, z=1}), gpos()))
local right = vec.unit(vec.sub(v:local2global({x=1, y=0, z=0}), gpos()))
local function onground(p) local e = oapi.global_to_equ(earth, p); return e end
-- (where it comes down: about 1350 m past the threshold)
local thrE = oapi.global_to_equ(earth, gpos())   -- (the threshold, on the turning planet)
local Rp = oapi.get_size(earth)
local function past()   -- [m] past the threshold (along the runway, roughly: its great circle)
	local e = oapi.global_to_equ(earth, gpos())
	local c = math.sin(thrE.lat) * math.sin(e.lat) + math.cos(thrE.lat) * math.cos(e.lat) * math.cos(e.lng - thrE.lng)
	return Rp * math.acos(math.max(-1, math.min(1, c)))
end
local td = vec.add(gpos(), vec.mul(fwd, env("LV_TD", 1350)))
if view == "ground" then
	local e = oapi.global_to_equ(earth, vec.add(td, vec.mul(right, SIDE + 45)))
	oapi.set_cameramode({mode = 'ground', ref = earthName, lng = math.deg(e.lng), lat = math.deg(e.lat), alt = 2})
elseif view == "cockpit" then
	oapi.set_cameramode({mode = 'cockpit'})
end

-- into the air: BACK m short, H0 up, level, VREF along, sinking 3 m/s
local R
do
	local x, y, z = right, up, fwd
	local p0 = math.rad(2)   -- (nose up a little: about the angle it glides at)
	y, z = vec.sub(vec.mul(y, math.cos(p0)), vec.mul(z, math.sin(p0))), vec.add(vec.mul(z, math.cos(p0)), vec.mul(y, math.sin(p0)))
	R = {m11=x.x, m12=y.x, m13=z.x, m21=x.y, m22=y.y, m23=z.y, m31=x.z, m32=y.z, m33=z.z}
end
proc.skip()
local st = v:get_status(2)
st.status = 0
st.arot = {x=math.atan2(R.m23, R.m33), y=-math.asin(R.m13), z=math.atan2(R.m12, R.m11)}
st.rpos = vec.add(st.rpos, vec.add(vec.add(vec.mul(fwd, -BACK), vec.mul(up, H0)), vec.mul(right, SIDE)))
st.rvel = vec.add(st.rvel, vec.add(vec.mul(fwd, VREF), vec.mul(up, -3.0)))
st.vrot = {x = 0, y = 0, z = 0}
v:defset_status(st)
local t0 = oapi.get_simtime()
while oapi.get_simtime() == t0 do proc.skip() end
v:set_rotationmatrix(R)
if view == "chase" then
	oapi.set_cameramode({mode = 'track', trackmode = 'targetrelative', reldist = 0.06, phi = math.rad(200), tht = math.rad(8)})
end
v:set_adcmode(ADCMODE.ON)   -- (the control surfaces on: the selector may be off on the ground)
local tstart = oapi.get_simtime()
oapi.write_log("LandingVideo: released at " .. tstart)

QD = env("LV_QD", -5.0)   -- (the pitch loop: elevator per rad of pitch error, per rad/s of pitch rate; pitch per m/s of sink error)
KP = env("LV_KP", 3.0)
KV = env("LV_KV", 0.015)
local function clamp(x, a, b) return math.max(a, math.min(b, x)) end
local pitchI, tdown, braked = math.rad(2), nil, false
local nextLog = 0
local LIMIT = env("LV_LIMIT", 90.0)   -- [s] at the most (a video run must end, whatever happens)
local function step()   -- (one frame's flying; true: done)
	local t = oapi.get_simtime()
	if t > tstart + LIMIT then return true end
	local hg = v:get_altitude(ALTMODE.GROUND) - alt0         -- the wheels' height over the ground (touchdown)
	local h = v:get_altitude(ALTMODE.MEANRAD) - altM0        -- ... over the threshold's level (flown by)
	local sv = v:get_horizonairspeedvector()
	local vs, as = sv.y, v:get_airspeed()
	local w = v:get_angvel()                                -- (x pitch, y yaw, z roll) [rad/s]
	local pitch, bank = v:get_pitch(), v:get_bank()
	-- the heading's error: the nose's direction across the runway
	local nose = vec.unit(vec.sub(v:local2global({x=0, y=0, z=1}), gpos()))
	local cross = vec.dotp(nose, right)
	if not tdown and hg < 0.05 and t > tstart + 1 then
		tdown = t
		oapi.write_log(string.format("LandingVideo: touchdown at %.2f s, %.1f m/s, sinking %.2f m/s, %.0f m past the threshold", t - tstart, as, vs, past()))
	end
	if not tdown then
		-- sink: 3 m/s down to the flare (from 8 m), then easing to SINK; a
		-- crash: down at CRASH_SINK from 25 m, no flare
		local vt = -clamp(SINK + (3.0 - SINK) * (h - 1.0) / 7.0, SINK, 3.0)
		if CRASH and h < 25 then vt = -CRASH_SINK end
		local pt = clamp(KV * (vt - vs) + pitchI, math.rad(-5), math.rad(14))
		pitchI = clamp(pitchI + 0.02 * (vt - vs) * 0.0333, math.rad(-3), math.rad(10))
		v:set_adclevel(AIRCTRL.ELEVATOR, clamp(KP * (pt - pitch) + QD * w.x, -1, 1))
		v:set_thrustergrouplevel(THGROUP.MAIN, h > 5 and clamp(0.05 * (VREF - as), 0, 1) or 0)
	else
		-- on the wheels: the nose lowered, the brakes from 2 s on
		v:set_thrustergrouplevel(THGROUP.MAIN, 0)
		v:set_adclevel(AIRCTRL.ELEVATOR, BELLY and 0 or clamp(-0.3 * (t - tdown), -0.6, 0))
		if not BELLY and not braked and t > tdown + 2.0 then v:set_wheelbrakelevel(LV_BRAKE, 0, true); braked = true end   -- (permanent: a transient level lasts one frame)
		if (v:get_groundspeed() < 0.5 and t > tdown + 4) or t > tdown + LV_ROLL then return true end
	end
	v:set_adclevel(AIRCTRL.AILERON, clamp(2.0 * bank + 0.8 * w.z, -1, 1))
	v:set_adclevel(AIRCTRL.RUDDER, clamp(-3.0 * cross + 1.0 * w.y, -1, 1))
	if t >= nextLog then
		oapi.write_log(string.format("LandingVideo: t %.2f h %.2f vs %.2f air %.1f pitch %.1f bank %.1f cross %.3f elev %.2f mode %d", t - tstart, h, vs, as, math.deg(pitch), math.deg(bank), cross, v:get_adclevel(AIRCTRL.ELEVATOR), v:get_adcmode()))
		nextLog = t + 0.5
	end
	if CRASH and tdown and t > tdown + 8 then return true end
	return false
end
while true do
	-- (the vessel destroyed: its interface may fail - the flight over, heard out below)
	local ok, done = pcall(step)
	if not ok then oapi.write_log("LandingVideo: the vessel is gone (" .. tostring(done) .. ")"); break end
	if done then break end
	proc.skip()
	if not oapi.get_objhandle("LAND") then break end
end
oapi.write_log(string.format("LandingVideo: stopped at %.2f s", oapi.get_simtime() - tstart))
local tend = oapi.get_simtime() + (CRASH and env("LV_HEAROUT", 6.0) or 1.5)   -- (a crash heard out: its debris, the wreck's fire, smoulder, ticking - LV_HEAROUT [s])
while oapi.get_simtime() < tend do proc.skip() end
oapi.exit(0)
