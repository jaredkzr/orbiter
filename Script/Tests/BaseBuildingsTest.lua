-- The bases' generic buildings as the core describes them
-- (oapi.get_basebuildings: oapiGetBaseBuildings; the Vulkan client draws its
-- own from them): as many as the bases' files have (BLOCK, HANGAR, HANGAR2,
-- HANGAR3, TANK), tanks' sizes their radii, all standing on the ground
-- (at the height their files give them over it: some are sunk).

local failed = false
local function log(s) oapi.write_log("BaseBuildingsTest: " .. s) end
local function check(name, cond, msg)
	log((cond and "PASS " or "FAIL ") .. name .. " " .. msg)
	if not cond then failed = true end
end

local TYPES = { BLOCK = 0, HANGAR = 1, HANGAR2 = 2, HANGAR3 = 3, TANK = 4 }

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

-- the file's buildings: their types in order, their heights (POS y), the tanks' SCALE
local function file_buildings(path)
	local f = io.open(path, "r")
	if not f then return nil end
	local out, cur = {}, nil
	for line in f:lines() do
		local tag = line:match("^(%u+%d?)%s*$")
		if tag and TYPES[tag] then
			cur = { type = TYPES[tag], y = 0 }
			out[#out + 1] = cur
		elseif cur and line:match("^%s*POS") then
			cur.y = tonumber(line:match("^%s*POS%s+[-%d.]+%s+([-%d.]+)")) or 0
		elseif cur and cur.type == 4 then
			local sx, sy, sz = line:match("^%s*SCALE%s+([-%d.]+)%s+([-%d.]+)%s+([-%d.]+)")
			if sx then cur.scale = { tonumber(sx), tonumber(sy), tonumber(sz) } end
		end
		if line:match("^%s*END%s*$") then cur = nil end
	end
	f:close()
	return out
end

local function base(planet, name, cfg)
	local hp = oapi.get_objhandle(planet)
	local hb = oapi.get_basebyname(hp, name)
	check(name, hb ~= nil, "is there")
	if not hb then return end
	local blng, blat = oapi.get_baseequpos(hb)
	local b = oapi.get_basebuildings(hb)
	local fb = file_buildings(cfg)
	check(name, fb ~= nil and #b == #fb, string.format("%d buildings, as its file (%d)", #b, fb and #fb or -1))
	if not fb or #b ~= #fb then return end
	local types, tanks, worst, wi = true, true, 0.0, 0
	for i, s in ipairs(b) do
		if s.type ~= fb[i].type then types = false end
		if s.type == 4 and fb[i].scale then
			-- (a tank's SCALE: radius, height, radius)
			if math.abs(s.size.x - fb[i].scale[1]) > 1e-3 or math.abs(s.size.z - fb[i].scale[3]) > 1e-3 or math.abs(s.size.y - fb[i].scale[2]) > 1e-3 then tanks = false end
		end
		local off = math.abs(s.pos.y - fb[i].y - ground_y(hp, blng, blat, s.pos.x, s.pos.z))
		if off > worst then worst, wi = off, i end
	end
	check(name, types, "types in the file's order")
	check(name, tanks, "tanks' sizes: radius, height, radius (their SCALE)")
	check(name, worst < 1.0, string.format("on the ground, as high as their files give (at most %.2f m off: building %d at %.0f %.0f, its ground %.2f, it %.2f)",
		worst, wi, b[math.max(wi, 1)].pos.x, b[math.max(wi, 1)].pos.z,
		wi > 0 and ground_y(hp, blng, blat, b[wi].pos.x, b[wi].pos.z) or 0, wi > 0 and b[wi].pos.y or 0))
end

local function main()
	proc.wait_frames(2)
	base("Moon", "Brighton Beach", "Config/Moon/Base/Brighton.cfg")
	base("Mars", "Olympus", "Config/Mars/Base/Olympus.cfg")
	base("Earth", "Edwards", "Config/Earth/Base/Edwards.cfg")
	base("Earth", "Baikonur", "Config/Earth/Base/Baikonur.cfg")
end

local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
