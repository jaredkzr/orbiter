-- The view of Tests/Lens/Donor.scn: a delta-glider 20 M from Nyx's donor
-- Hemera (inwards of it, a little out of the disc's plane), at rest
-- relative to Nyx or (DONOR_COMOVE) moving with Hemera, 0.07 c: the
-- relativistic view. Hemera drawn where its light left it (light-time;
-- aberrated when moving) against where the lens trace sees it
-- (ORBITER_VK_LENS_DONOR_CHECK)
local C, G = 299792458.0, 6.67430e-11
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function add(a, b) return {x = a.x + b.x, y = a.y + b.y, z = a.z + b.z} end
local function mul(a, f) return {x = a.x * f, y = a.y * f, z = a.z * f} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
local function unit(a) return mul(a, 1 / len(a)) end
local function cross(a, b) return {x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x} end
local hole, don = oapi.get_objhandle("Nyx"), oapi.get_objhandle("Hemera")
local M = G * oapi.get_mass(hole) / (C * C)
local hp, dp = oapi.get_globalpos(hole), oapi.get_globalpos(don)
local r = sub(dp, hp)
local rh = unit(r)
local dv = sub(oapi.get_globalvel(don), oapi.get_globalvel(hole))
local n = unit(cross(r, dv))
local t = unit(cross(n, rh))
local cam = add(add(add(dp, mul(rh, -18 * M)), mul(t, 6 * M)), mul(n, 5 * M))
local v = vessel.get_interface("BH")
local st = v:get_status(2)
st.status = 0
st.rbody = hole
st.rpos = sub(cam, hp)
st.rvel = os.getenv("DONOR_COMOVE") and dv or {x = 0, y = 0, z = 0}
v:defset_status(st)
proc.wait_frames(2)
local z = unit(sub(dp, v:get_globalpos()))
local x = unit(cross(n, z))
local y = cross(z, x)
v:set_rotationmatrix({m11 = x.x, m12 = y.x, m13 = z.x, m21 = x.y, m22 = y.y, m23 = z.y, m31 = x.z, m32 = y.z, m33 = z.z})
v:set_angvel({x = 0, y = 0, z = 0})
