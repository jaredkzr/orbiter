-- Launch windows (Scenarios/Tests/LaunchPlanTest.scn): transfers to the Moon
-- from Baikonur and to Mars from Cape Canaveral, planned from the ground. For each, the chosen
-- launch opportunity's parking orbit is flown into (the glider is put into
-- it at the insertion time), and the transfer is planned again from there:
-- its ejection must need no more than the coplanar estimate (no plane change).

local failed = false
local function log(s) oapi.write_log("LaunchPlanTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local function wait_done(hv, serial, timeout)
	local t0 = oapi.get_systime()
	repeat
		proc.wait_frames(1)
		local ti = oapi.transfer_info(hv)
		if ti and ti.job == 0 and ti.serial > serial then return ti end
	until oapi.get_systime() - t0 > timeout
	return oapi.transfer_info(hv)
end

local deg = 180 / math.pi

local function plan(v, target, name, alt, lat)
	local hv = v:get_handle()
	local earth = oapi.get_objhandle("Earth")
	local ti = oapi.transfer_info(hv)
	local s = ti and ti.serial or 0
	oapi.transfer_search(hv, target, 0, alt, 0)
	ti = wait_done(hv, s, 60)
	check(name .. "_search", ti.valid, ti.message)
	local coplanar = ti.dv_eject
	local L = oapi.transfer_launches(hv)
	check(name .. "_launches", #L >= 1, string.format("%d launch opportunities", #L))
	if #L == 0 then return end
	local o
	for _, x in ipairs(L) do
		log(string.format("%s: lift-off MJD %.4f, azimuth %.1f, inclination %.1f, coast %.0f min, ejection %.0f m/s, penalty %.0f m/s, total %.0f m/s%s",
			name, x.launch_mjd, x.azimuth * deg, x.incl * deg, (x.burn_mjd - x.ins_mjd) * 1440, x.dv_eject, x.dv_penalty, x.dv_total, x.selected and " (chosen)" or ""))
		if x.selected then o = x end
	end
	check(name .. "_incl", o.incl * deg > lat - 0.5, string.format("inclination %.2f deg (site latitude %.2f)", o.incl * deg, lat))
	check(name .. "_azimuth", o.azimuth * deg > 0 and o.azimuth * deg < 180, string.format("azimuth %.1f deg (eastward)", o.azimuth * deg))
	local ci = math.cos(lat / deg) * math.sin(o.azimuth)
	check(name .. "_spherical", math.abs(math.acos(ci) - o.incl) < 0.5 / deg,
		string.format("inclination %.2f deg from azimuth and latitude: %.2f deg", o.incl * deg, math.acos(ci) * deg))
	check(name .. "_order", o.launch_mjd < o.ins_mjd and o.ins_mjd < o.burn_mjd, "lift-off, insertion, ejection in order")
	check(name .. "_eject", math.abs(o.dv_eject - coplanar) < 0.03 * coplanar,
		string.format("ejection %.0f m/s from the planned orbit (coplanar estimate %.0f)", o.dv_eject, coplanar))

	-- into the planned parking orbit at insertion, and plan from there
	oapi.set_simmjd(o.ins_mjd)
	proc.wait_frames(2)
	local vs = v:get_status()
	vs.status = 0
	vs.rbody = earth
	vs.rpos = o.rins
	vs.rvel = o.vins
	v:defset_status(vs)
	proc.wait_frames(5)
	s = oapi.transfer_info(hv).serial
	oapi.transfer_search(hv, target, 0, alt, 0)
	ti = wait_done(hv, s, 60)
	oapi.transfer_select(hv, o.burn_mjd, o.tof)
	proc.wait_frames(2)
	ti = oapi.transfer_info(hv)
	log(string.format("%s from the parking orbit: ejection %.0f m/s at MJD %.4f (planned %.0f m/s at %.4f)", name, ti.dv_eject, ti.burn_mjd, o.dv_eject, o.burn_mjd))
	check(name .. "_inplane", math.abs(ti.dv_eject - o.dv_eject) < 0.03 * o.dv_eject and math.abs(ti.burn_mjd - o.burn_mjd) * 1440 < 30,
		string.format("ejection %.0f m/s (planned %.0f), %.0f min from the planned time", ti.dv_eject, o.dv_eject, (ti.burn_mjd - o.burn_mjd) * 1440))
end

local function main()
	-- (the Moon first: its flight starts within days, the Mars window weeks later)
	plan(vessel.get_interface("GL-B"), oapi.get_objhandle("Moon"), "moon", 100e3, 45.920)
	plan(vessel.get_interface("GL-L"), oapi.get_objhandle("Mars"), "mars", 300e3, 28.5969630)
	oapi.exit(failed and 1 or 0)
end

main()
