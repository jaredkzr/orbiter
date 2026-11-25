-- Boosters into the sea at time acceleration (Scenarios/Tests/SrbSplashTest.scn):
-- the two spent SRBs come down into the Atlantic at 10x, damage model on.
-- They must break up there and what is left of them must stay in the water,
-- at sane speeds, with finite states (a session ended in a segmentation
-- fault here).

local failed = false
local function log(s) oapi.write_log("SrbSplashTest: " .. s) end
local function check(name, cond, msg)
	if cond then log("PASS " .. name .. " " .. msg) else log("FAIL " .. name .. " " .. msg); failed = true end
end
local function finite(x) return x == x and x ~= math.huge and x ~= -math.huge end

local function main()
	proc.wait_frames(2)
	local earth = oapi.get_objhandle("Earth")
	local R = oapi.get_size(earth)
	local names = { "STS-101-SRB1", "STS-101-SRB2" }
	-- (steps of 1/6 s: 10x at 60 fps, as in the session; the test's fixed
	-- step is 0.02 s)
	oapi.set_tacc(1 / 6 / 0.02)
	local t0 = oapi.get_simtime()
	local fastest = 0
	while oapi.get_simtime() - t0 < 90 do
		proc.wait_frames(1)
		if oapi.get_simtime() - t0 > 40 then -- (in the water by then)
			for _, n in ipairs(names) do
				local h = oapi.get_objhandle(n)
				if h then
					local v = vessel.get_interface(h)
					local s = v:get_groundspeed()
					fastest = math.max(fastest, finite(s) and s or math.huge)
				end
			end
		end
	end
	oapi.set_tacc(1)
	check("break-up", oapi.get_debriscount() > 0, string.format("the boosters broke up: %d pieces", oapi.get_debriscount()))
	for _, n in ipairs(names) do
		local h = oapi.get_objhandle(n)
		if h then
			local v = vessel.get_interface(h)
			local g = v:get_globalpos()
			local alt = oapi.global_to_equ(earth, g).rad - R
			local w = v:get_angvel()
			log(string.format("%s: %.1f m above the datum, %.1f m/s, turning %.2f rad/s", n, alt, v:get_groundspeed(), vec.length(w)))
			check(n, finite(g.x) and finite(g.y) and finite(g.z) and finite(alt), "has a finite state")
			check(n, math.abs(alt) < 50, "is at the surface")
			check(n, vec.length(w) < 5, "does not spin up")
		else
			log(n .. ": gone")
		end
	end
	check("speeds", fastest < 100, string.format("the remains stay below 100 m/s in the water (%.1f)", fastest))
end

local ok, err = xpcall(main, debug.traceback)
if not ok then log("ERROR " .. tostring(err)); failed = true end
oapi.exit(failed and 1 or 0)
