-- The view of Tests/Lens/Lens.scn: a circular orbit at 30 M about the test
-- hole (prograde, in its equatorial plane: Kerr's Omega = 1/(r^1.5 + a)),
-- the nose 10 degrees off the hole
local C, G = 299792458.0, 6.67430e-11
local function sub(a, b) return {x = a.x - b.x, y = a.y - b.y, z = a.z - b.z} end
local function add(a, b) return {x = a.x + b.x, y = a.y + b.y, z = a.z + b.z} end
local function mul(a, f) return {x = a.x * f, y = a.y * f, z = a.z * f} end
local function len(a) return math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z) end
local function unit(a) return mul(a, 1 / len(a)) end
local function cross(a, b) return {x = a.y * b.z - a.z * b.y, y = a.z * b.x - a.x * b.z, z = a.x * b.y - a.y * b.x} end
local hole = oapi.get_objhandle("TestHole")
local M = G * oapi.get_mass(hole) / (C * C)
local r, s = 30.0, 0.9
local W = 1 / (r ^ 1.5 + s) * C / M
local v = vessel.get_interface("BH")
local st = v:get_status(2)
st.status = 0
st.rbody = hole
st.rpos = {x = r * M, y = 0, z = s * M}
st.rvel = {x = -W * s * M, y = 0, z = W * r * M}
v:defset_status(st)
proc.wait_frames(2)
local zs = unit(sub(oapi.get_globalpos(hole), v:get_globalpos()))
local xs = unit(cross({x = 0, y = 1, z = 0}, zs))
local off = math.rad(10)
local z = unit(add(mul(zs, math.cos(off)), mul(xs, math.sin(off))))
local x = unit(cross({x = 0, y = 1, z = 0}, z))
local y = cross(z, x)
v:set_rotationmatrix({m11 = x.x, m12 = y.x, m13 = z.x, m21 = x.y, m22 = y.y, m23 = z.y, m31 = x.z, m32 = y.z, m33 = z.z})
v:set_angvel({x = 0, y = 0, z = 0})
