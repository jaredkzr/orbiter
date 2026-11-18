-- The bases' trains as the core describes them (oapi.get_basetrains:
-- oapiGetBaseTrains; the Vulkan client draws and moves its own from them):
-- as many as the bases' files have (TRAIN1, TRAIN2), their tracks' ends on
-- the ground, their heights, speeds and turning points as the files give
-- them (Brighton Beach: a hangrail; Habana: a monorail and two hangrails).

local failed = false
local function log(s) oapi.write_log("BaseTrainTest: " .. s) end
local function check(name, cond, msg)
	log((cond and "PASS " or "FAIL ") .. name .. " " .. msg)
	if not cond then failed = true end
end

-- (a base frame point (x south, z east) as longitude, latitude: exact, as Base::Rel_EquPos (the
-- tangent plane at the base); its ground's height in the base frame: the elevation there over the
-- base's, and the plane's height over the sphere there)
local function ground_y(hp, blng, blat, x, z)
	local R = oapi.get_size(hp)
	local bx, bz = x, z
	local sn, cn, st, ct = math.sin(blng), math.cos(blng), math.sin(blat), math.cos(blat)
	local px = R * ct * cn + bx * st * cn - bz * sn
	local py = R * st - bx * ct
	local pz = R * ct * sn + bx * st * sn + bz * cn
	local lng, lat = math.atan2(pz, px), math.atan2(py, math.sqrt(px * px + pz * pz))
	return oapi.surface_elevation(hp, lng, lat) - oapi.surface_elevation(hp, blng, blat) + (R - math.sqrt(R * R + x * x + z * z))
end

local function file_trains(path)
	local f = io.open(path, "r")
	if not f then return nil end
	local n = 0
	for line in f:lines() do
		if line:match("^%s*TRAIN[12]%s*$") then n = n + 1 end
	end
	f:close()
	return n
end

local function base(planet, name, cfg, expect)
	local hp = oapi.get_objhandle(planet)
	local hb = oapi.get_basebyname(hp, name)
	check(name, hb ~= nil, "is there")
	if not hb then return end
	local blng, blat = oapi.get_baseequpos(hb)
	local tr = oapi.get_basetrains(hb)
	check(name, #tr == file_trains(cfg), string.format("%d trains, as its file", #tr))
	for i, t in ipairs(tr) do
		local e = expect[i]
		local tag = string.format("%s train %d", name, i)
		if not e then check(tag, false, "unexpected"); break end
		-- (the track's length as the core measures it: from the file's ends, before they are set on the ground)
		local len = math.sqrt((t.end2.x - t.end1.x) ^ 2 + (t.end2.z - t.end1.z) ^ 2)
		check(tag, t.type == e.type, string.format("type %d", t.type))
		check(tag, math.abs(t.end1.x - e.end1[1]) < 0.01 and math.abs(t.end1.z - e.end1[2]) < 0.01
			and math.abs(t.end2.x - e.end2[1]) < 0.01 and math.abs(t.end2.z - e.end2[2]) < 0.01, "its ends as its file")
		for k, p in ipairs({ t.end1, t.end2 }) do
			local g = ground_y(hp, blng, blat, p.x, p.z)
			check(tag, math.abs(p.y - g) < 1.0, string.format("end %d on the ground (%.2f m, the ground %.2f m)", k, p.y, g))
		end
		check(tag, math.abs(t.height - e.height) < 1e-6 and math.abs(t.maxspeed - e.maxspeed) < 1e-6
			and math.abs(t.slowzone - e.slowzone) < 1e-6, string.format("height %.0f, %.0f m/s, slow zone %.0f m", t.height, t.maxspeed, t.slowzone))
		check(tag, math.abs(t.minpos - 10) < 1e-3 and math.abs(t.maxpos - (len - 10)) < 0.01, "cars turn 10 m from its ends")
		check(tag, t.tex:lower() == e.tex:lower(), "texture " .. t.tex)
	end
end

local function main()
	proc.wait_frames(2)
	base("Moon", "Brighton Beach", "Config/Moon/Base/Brighton.cfg", {
		{ type = 2, end1 = { -77.9423, -45.0 }, end2 = { -1169.1, -675.0 }, height = 12, maxspeed = 50, slowzone = 200, tex = "Hangrail" } })
	base("Earth", "Habana", "Config/Earth/Base/Habana.cfg", {
		{ type = 1, end1 = { -1200, 380 }, end2 = { 300, 380 }, height = 0, maxspeed = 30, slowzone = 100, tex = "Monorail" },
		{ type = 2, end1 = { -170, -1000 }, end2 = { -170, 1000 }, height = 11, maxspeed = 50, slowzone = 200, tex = "Hangrail" },
		{ type = 2, end1 = { -1700, 430 }, end2 = { 300, -270 }, height = 20, maxspeed = 40, slowzone = 200, tex = "Hangrail" } })
end

local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
