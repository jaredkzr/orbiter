-- Procedural terrain detail (Scenarios/Tests/TerrainDetailTest.scn):
-- elevation profiles. In the Alps the detail adds relief below the
-- resolution of the elevation data (cells of about 150 m: without detail
-- the profile is smooth at 25 m), without steps; the open sea and the
-- ground at Cape Canaveral (a surface base) stay as flat as the data. On
-- the Moon's mare near Hadley Rille craters roughen the flat ground.
-- Skipped (passes) without elevation data.

local failed = false
local function log(s) oapi.write_log("TerrainDetailTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg)
	else log("FAIL " .. name .. " " .. msg); failed = true end
end

local E = oapi.get_objhandle("Earth")
local r = math.rad

-- elevations along a great circle from (lng, lat) in direction az [rad]
local function profile(lng, lat, az, len, step, body)
	body = body or E
	local R = oapi.get_size(body)
	local h = {}
	for i = 0, math.floor(len / step) do
		local d = i * step / R
		h[#h+1] = oapi.surface_elevation(body, lng + d * math.sin(az) / math.cos(lat), lat + d * math.cos(az))
	end
	return h
end

-- largest step between samples, rms deviation from the neighbours' mean
-- (roughness at the sample spacing), range
local function stats(h)
	local maxd, rr, mn, mx = 0, 0, math.huge, -math.huge
	for i = 1, #h - 1 do maxd = math.max(maxd, math.abs(h[i+1] - h[i])) end
	for i = 2, #h - 1 do local d = h[i] - 0.5 * (h[i-1] + h[i+1]); rr = rr + d * d end
	for i = 1, #h do mn = math.min(mn, h[i]); mx = math.max(mx, h[i]) end
	return maxd, math.sqrt(rr / (#h - 2)), mn, mx
end

local function main()
	local _, _, amin, amax = stats(profile(r(7.6586), r(45.9763), r(30), 20000, 25))
	if amax - amin < 100 then
		log("SKIPPED: no elevation data for Earth")
		oapi.exit(0)
	end

	-- the Alps near the Matterhorn: rough at 25 m (about 1 m without the
	-- detail), and continuous at 0.1 m (slopes of 80 degrees would give
	-- steps of 0.6 m; a seam would be a step of metres)
	local _, rough = stats(profile(r(7.6586), r(45.9763), r(30), 20000, 25))
	check("relief", rough > 2.0, string.format("roughness at 25 m: %.2f m", rough))
	local step = stats(profile(r(7.6586), r(45.9763), r(30), 600, 0.1))
	check("continuous", step < 0.6, string.format("largest step at 0.1 m: %.3f m", step))

	-- the same point gives the same elevation
	local e1 = oapi.surface_elevation(E, r(7.66), r(45.97))
	local e2 = oapi.surface_elevation(E, r(7.66), r(45.97))
	check("deterministic", e1 == e2, string.format("%.6f m, %.6f m", e1, e2))

	-- the open sea stays flat
	local _, _, smin, smax = stats(profile(r(-40), r(30), r(30), 5000, 25))
	check("sea", smax - smin < 1e-6, string.format("range %.6f m", smax - smin))

	-- no detail at a surface base (the roughness of the data alone)
	local _, krough = stats(profile(r(-80.675), r(28.5), 0, 3000, 25))
	check("base", krough < 0.6, string.format("roughness at Cape Canaveral: %.3f m", krough))

	-- the Moon's mare near Hadley Rille: craters (about 0.03 m roughness at
	-- 5 m without the detail), and continuous through them
	local M = oapi.get_objhandle("Moon")
	if M then
		local _, mrough = stats(profile(r(3.70), r(26.05), r(90), 4000, 5, M))
		check("craters", mrough > 0.1, string.format("roughness at 5 m on the mare: %.3f m", mrough))
		local mstep = stats(profile(r(3.70), r(26.05), r(90), 600, 0.1, M))
		check("craters_continuous", mstep < 0.6, string.format("largest step at 0.1 m: %.3f m", mstep))
	end

	oapi.exit(failed and 1 or 0)
end

main()
