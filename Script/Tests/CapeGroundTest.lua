-- Cape Canaveral's ground (Scenarios/Tests/CapeGroundTest.scn): elevation
-- profiles about the base, as the physics reads them (the core's
-- elevation, with the detail and the base's levelling).
--  - the runways (SLF 15/33, the Skid Strip 13/31): their centrelines,
--    extended 8 km past each end (the approaches): spikes (the largest
--    deviation from the neighbours' mean at 10 m) and the range within any
--    500 m; on the runway itself the range across its length's steps
--  - the graded strips flush with the ground about them: the runway's
--    mean against the ground's 150-400 m to either side
--  - 8 radial lines from the base out to 40 km (land only): the same
--  - the levels the physics reads at heights of 30 m to 2 km agree
--  - launch complex 39's pads keep the data's ground (their mounds), the woods
--    about them levelled
-- Skipped (passes) without elevation data.

local failed = false
local function log(s) oapi.write_log("CapeGroundTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local E = oapi.get_objhandle("Earth")
local R = oapi.get_size(E)
local r = math.rad

-- the base's frame (x south, z east [m]) to longitude, latitude [rad]
local LNG0, LAT0 = r(-80.65165), r(28.58438)
local function base2equ(x, z)
	local lat = LAT0 - x / R
	return LNG0 + z / (R * math.cos(lat)), lat
end

-- elevations from (lng, lat) along azimuth az [rad], len [m] at step [m]
-- (great circle, small-angle steps); lvl: the level asked (0: the deepest)
local function profile(lng, lat, az, len, step, lvl)
	local h, pts = {}, {}
	for i = 0, math.floor(len / step) do
		local d = i * step
		local la = lat + d * math.cos(az) / R
		local lo = lng + d * math.sin(az) / (R * math.cos(la))
		h[#h+1] = oapi.surface_elevation(E, lo, la, lvl or 0)
		pts[#pts+1] = { lo, la }
	end
	return h, pts
end

-- runs of land (|h| > 1 m: not the sea, lagoons, canals and their edges), at least n samples; trim samples
-- left out at the ends next to the water (the shores: banks, dunes)
local function landRuns(h, n, trim)
	trim = trim or 0
	local runs, cur = {}, {}
	for i = 1, #h + 1 do
		if i <= #h and math.abs(h[i]) > 1.0 then cur[#cur+1] = h[i]
		else
			local a = (#cur < i - 1) and trim or 0   -- (water before it: trimmed)
			local b = (i <= #h) and trim or 0         -- (water after it)
			if #cur - a - b >= n then
				local run = {}
				for k = a + 1, #cur - b do run[#run+1] = cur[k] end
				run.at = i - #cur + a
				runs[#runs+1] = run
			end
			cur = {}
		end
	end
	return runs
end

-- spike: the largest deviation from the neighbours' mean; win: the largest
-- range within w samples; range: the whole range
local function stats(h, w)
	local spike, win, mn, mx = 0, 0, math.huge, -math.huge
	local si, wi = 0, 0
	for i = 2, #h - 1 do
		local s = math.abs(h[i] - 0.5 * (h[i-1] + h[i+1]))
		if s > spike then spike, si = s, i end
	end
	for i = 1, #h do
		mn, mx = math.min(mn, h[i]), math.max(mx, h[i])
		local a, b = math.huge, -math.huge
		for k = i, math.min(#h, i + w - 1) do a = math.min(a, h[k]); b = math.max(b, h[k]) end
		if b - a > win then win, wi = b - a, i end
	end
	return spike, win, mx - mn, si, wi
end

local function mean(h) local s = 0; for _, v in ipairs(h) do s = s + v end; return s / #h end

-- a runway from its ends (base frame): azimuth, length, its first end
local function runway(x1, z1, x2, z2)
	local lng1, lat1 = base2equ(x1, z1)
	local lng2, lat2 = base2equ(x2, z2)
	local dn, de = (lat2 - lat1) * R, (lng2 - lng1) * R * math.cos(lat1)
	return math.atan2(de, dn), math.sqrt(dn * dn + de * de), lng1, lat1
end

-- thresholds (bare earth: the Cape's ground a few metres up, gently varying;
-- before it, with the data's canopy: spikes of 4.7 m at 10 m on the SLF's
-- approaches and 8 m at 25 m on the radials, ranges of 8-27 m in 500 m, the
-- runways 2.4-3 m below the canopy beside them, the levels up to 4.8 m apart)
local SPIKE_MAX = 0.3   -- [m] at 10 m
local WIN_MAX = 2.0     -- [m] within 500 m (the approaches)
local RADIAL_SPIKE_MAX = 1.0 -- [m] at 25 m (ditches and canals at the datum in KSC: dips under 1 m)
local RADIAL_WIN_MAX = 4.0   -- [m] within 500 m (the mainland's own relief, 16-40 km west)
local RWY_RANGE_MAX = 1.5 -- [m] along a runway
local FLUSH_MAX = 1.0   -- [m] a runway's mean above/below the ground beside it
local LEVEL_MAX = 1.0   -- [m] the levels' disagreement

local function runwayChecks(name, x1, z1, x2, z2)
	local az, len, lng1, lat1 = runway(x1, z1, x2, z2)
	local EXT = 8000
	-- the centreline, from EXT short of end 1 to EXT past end 2
	local lng0 = lng1 - EXT * math.sin(az) / (R * math.cos(lat1))
	local lat0 = lat1 - EXT * math.cos(az) / R
	local h = profile(lng0, lat0, az, len + 2 * EXT, 10)
	local i1, i2 = math.floor(EXT / 10) + 1, math.floor((EXT + len) / 10) + 1
	local on = {}
	for i = i1, i2 do on[#on+1] = h[i] end
	local sp, wn, rg = stats(on, 50)
	log(string.format("%s runway: %.0f m at %.1f deg, mean %.2f m, range %.2f m, spike %.3f m", name, len, math.deg(az), mean(on), rg, sp))
	check(name .. "_runway", rg < RWY_RANGE_MAX and sp < SPIKE_MAX, string.format("range %.2f m, spike %.3f m", rg, sp))
	-- the approaches (land only)
	local worstS, worstW = 0, 0
	for _, part in ipairs({ { 1, i1 - 1 }, { i2 + 1, #h } }) do
		local seg = {}
		for i = part[1], part[2] do seg[#seg+1] = h[i] end
		for _, run in ipairs(landRuns(seg, 10, 10)) do
			local s, w = stats(run, 50)
			worstS, worstW = math.max(worstS, s), math.max(worstW, w)
		end
	end
	check(name .. "_approaches", worstS < SPIKE_MAX and worstW < WIN_MAX, string.format("spike %.3f m, range in 500 m %.2f m", worstS, worstW))
	-- flush: the runway's mean against the ground 150-400 m to either side
	-- (along its middle half, land only)
	local side = {}
	for _, off in ipairs({ -400, -300, -200, -150, 150, 200, 300, 400 }) do
		local pa = az + math.pi / 2
		local la = lat1 + off * math.cos(pa) / R
		local lo = lng1 + off * math.sin(pa) / (R * math.cos(la))
		local hs = profile(lo, la, az, len, 50)
		for i = math.floor(#hs / 4), math.floor(3 * #hs / 4) do
			if math.abs(hs[i]) > 0.25 then side[#side+1] = hs[i] end
		end
	end
	if #side > 10 then
		local d = mean(on) - mean(side)
		check(name .. "_flush", math.abs(d) < FLUSH_MAX, string.format("runway %.2f m, beside it %.2f m: %.2f m", mean(on), mean(side), d))
	end
	-- the levels: physics reads 32 - log2(altitude); those of an approach (30 m .. 2 km)
	-- against the deepest (higher up, coarser tiles' coasts move the sea's edge: metres
	-- there matter little)
	local worst, wl = 0, 0
	for _, lvl in ipairs({ 27, 25, 23, 21 }) do
		local hl = profile(lng0, lat0, az, len + 2 * EXT, 100, lvl)
		local hd = profile(lng0, lat0, az, len + 2 * EXT, 100)
		for i = 1, #hl do
			if math.abs(hd[i]) > 0.25 then
				local d = math.abs(hl[i] - hd[i])
				if d > worst then worst, wl = d, lvl end
			end
		end
	end
	check(name .. "_levels", worst < LEVEL_MAX, string.format("largest difference %.2f m (level %d)", worst, wl))
end

local function main()
	-- (no data: a flat Earth here)
	local probe = profile(r(7.6586), r(45.9763), r(30), 20000, 250)
	local _, _, arange = stats(probe, 2)
	if arange < 100 then
		log("SKIPPED: no elevation data for Earth")
		oapi.exit(0)
	end

	-- launch complex 39's pads keep the data's hardstands (BEGIN_BAREEARTH_KEEP:
	-- level at 5 m; their meshes and the vessels on them set on that)
	for _, pad in ipairs({ { "39A", -2659, 4635 }, { "39B", -4759.2, 3001 } }) do
		local lng, lat = base2equ(pad[2], pad[3])
		local h = oapi.surface_elevation(E, lng, lat)
		check("pad" .. pad[1], math.abs(h - 5.0) < 0.3, string.format("%.2f m", h))
		-- (and only the mound and its ramp: the woods about it levelled, no canopy standing
		-- 5-10 m as hills; rings of 250-450 m, the highest of them)
		local top = -1e9
		for _, d in ipairs({ 250, 350, 450 }) do
			for a = 0, 355, 5 do
				local l, t = base2equ(pad[2] + d * math.cos(r(a)), pad[3] + d * math.sin(r(a)))
				top = math.max(top, oapi.surface_elevation(E, l, t))
			end
		end
		check("pad" .. pad[1] .. "woods", top < 6.5, string.format("highest %.2f m", top))
	end
	-- (the runways' ends, as in Canaveral.cfg: the SLF's with its overruns)
	runwayChecks("slf", -1141.69, -2877.44, -5644.87, -5462.46)
	runwayChecks("skid", 13919.90, 9514.59, 12047.64, 7109.42)

	-- radial lines from the base out to 40 km (its bare earth's radius; land only), at 25 m
	local worstS, worstW, worstAz = 0, 0, 0
	local wS = 0
	for k = 0, 7 do
		local az = r(45 * k)
		local h = profile(LNG0, LAT0, az, 40000, 25)
		local azS, azW = 0, 0
		local sAt, wAt = 0, 0
		for _, run in ipairs(landRuns(h, 10, 4)) do
			local s, w, _, si, wi = stats(run, 20)
			if s > azS then azS, sAt = s, run.at + si - 1 end
			if w > azW then azW, wAt = w, run.at + wi - 1 end
		end
		if os.getenv("CG_VERBOSE") then
			local function ctx(i) local t = {}; for k = math.max(1, i - 3), math.min(#h, i + 3) do t[#t+1] = string.format("%.2f", h[k]) end; return table.concat(t, " ") end
			log(string.format("  radial %d: spike at %.0f m [%s]; window at %.0f m", 45 * k, (sAt - 1) * 25, ctx(sAt), (wAt - 1) * 25))
		end
		log(string.format("radial %3d deg: spike %.3f m, range in 500 m %.2f m", 45 * k, azS, azW))
		if azW > worstW then worstW, worstAz = azW, 45 * k end
		worstS = math.max(worstS, azS)
	end
	check("radials", worstS < RADIAL_SPIKE_MAX and worstW < RADIAL_WIN_MAX, string.format("spike %.3f m, range in 500 m %.2f m (worst at %d deg)", worstS, worstW, worstAz))

	oapi.exit(failed and 1 or 0)
end

main()
