-- The floating base on Venus (Scenarios/Tests/AerostatTest.scn). Aphrodite's
-- deck (55 km up, radius 75 m about the base's origin) is the ground for
-- vessels over it, and only there. A1 sits on pad 1; A2 hops off pad 2 on
-- its hover engines and comes down again.

local failed = false

local function log(s)
	oapi.write_log("AerostatTest: " .. s)
end

local function check(name, cond, msg)
	if cond then
		log("PASS " .. name .. " " .. msg)
	else
		log("FAIL " .. name .. " " .. msg)
		failed = true
	end
end

local ALT = 55000.0 -- the deck [m]

local function main()
	proc.wait_frames(2)
	local h = oapi.get_objhandle("Venus")
	local f = io.open("Config/Venus/Base/Aphrodite.cfg", "r")
	check("base", f ~= nil, "Config/Venus/Base/Aphrodite.cfg is there")
	if not f then return end
	local txt = f:read("*a")
	f:close()
	local lng, lat = txt:match("Location%s*=%s*([-+%d.]+)%s+([-+%d.]+)")
	local blng, blat = math.rad(tonumber(lng)), math.rad(tonumber(lat))
	-- the ground: the deck over the base, the surface 55 km below beside it
	local R = oapi.get_size(h)
	local e0 = oapi.surface_elevation(h, blng, blat)
	local e1 = oapi.surface_elevation(h, blng + 200.0 / (R * math.cos(blat)), blat)
	log(string.format("ground at the base's origin %.1f m, 200 m east %.1f m", e0, e1))
	check("deck", math.abs(e0 - ALT) < 0.01, "is the ground over the base")
	check("deck", math.abs(e1) < 2000.0, "ends at its edge")

	local a1, a2 = vessel.get_interface("A1"), vessel.get_interface("A2")
	local p2 = a2:get_equpos()
	local amin = { 1e9, 1e9 }
	local amax2 = -1e9
	local t0 = oapi.get_simtime()
	-- A2: up on its hover engines for 4 s, then down again (light: the pad
	-- refuels the vessels on it)
	a2:set_propellantmass(a2:get_propellanthandle(0), 650.0)
	-- A2: up on its hover engines for 4 s, then down again
	a2:set_thrustergrouplevel(THGROUP.HOVER, 1.0)
	local left = false
	while oapi.get_simtime() - t0 < 40.0 do
		proc.wait_frames(1)
		local t = oapi.get_simtime() - t0
		a2:set_thrustergrouplevel(THGROUP.HOVER, t < 4.0 and 1.0 or 0.0) -- (every frame, as ContactTest)
		local x1, x2 = a1:get_altitude(ALTMODE.MEANRAD), a2:get_altitude(ALTMODE.MEANRAD)
		amin[1] = math.min(amin[1], x1)
		amin[2] = math.min(amin[2], x2)
		amax2 = math.max(amax2, x2)
		if a2:get_flightstatus() % 2 == 0 then left = true end
	end
	local e2 = a2:get_equpos()
	local moved = math.sqrt(((e2.lat - p2.lat) * R) ^ 2 + ((e2.lng - p2.lng) * R * math.cos(p2.lat)) ^ 2)
	log(string.format("A1 lowest %.2f m; A2 lowest %.2f m, highest %.2f m, %.1f m from its pad, landed %s",
		amin[1], amin[2], amax2, moved, tostring(a2:get_flightstatus() % 2 == 1)))
	check("A1", a1:get_flightstatus() % 2 == 1, "stays landed")
	check("A1", amin[1] > ALT - 0.5, "stays on the deck")
	check("A2", left and amax2 > ALT + 10.0, "lifted off")
	check("A2", a2:get_flightstatus() % 2 == 1, "landed again")
	check("A2", amin[2] > ALT - 0.5, "never fell through the deck")
	check("A2", moved < 40.0, "came down near its pad")
end

local ok, err = xpcall(main, debug.traceback)
if not ok then
	log("ERROR " .. tostring(err))
	failed = true
end
oapi.exit(failed and 1 or 0)
