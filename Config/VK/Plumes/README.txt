Engine plume settings for the Vulkan client
===========================================

The Vulkan client draws each engine exhaust as a ray-marched plume volume
whose shape follows the ambient pressure (a narrow jet with shock diamonds
at sea level, a wide faint bell in vacuum). The engine class (propellant)
is inferred from the thruster:

  Isp < 1500 m/s                    coldgas (compressed gas, e.g. MMU)
  attitude thrusters only           rcs
  Isp >= 15000 m/s                  ion (thrust < 5 kN) or plasma
  Isp >= 4000 m/s                   hydrolox
  Isp >= 3400 m/s                   methalox
  Isp >= 2900 m/s                   kerolox (thrust >= 300 kN) or hypergolic
  lower                             solid (thrust >= 300 kN) or hypergolic
  exhausts without a thruster       texture (the add-on's exhaust texture)

A file <vessel class>.cfg in this folder overrides this per exhaust:

  THRUSTER <group> [<index> ...] <key> <value> ...
  EXHAUST <index> ... <key> <value> ...

  group: main, retro, hover, rcs (all attitude thrusters) or all; the
         indices count the thrusters within the group (none: all of them)
  EXHAUST indices count the vessel's exhausts in the order they were added

  keys:  TYPE        ion, plasma, hydrolox, methalox, kerolox, hypergolic,
                     solid, rcs, coldgas, or texture (keep the add-on's flame texture)
         SCALE       size factor (nozzle radius and length)
         LENGTH      length factor
         BRIGHTNESS  brightness factor (of the flame drawn and of the light
                     it casts)

The light a flame casts on the vessel, the ground and the smoke has the
class's radiance, far above the drawn one (at full thrust in sea-level air,
in units of a sunlit white surface: solid 300, kerolox 150, hypergolic 60,
methalox 40, rcs 30, hydrolox 20; ion and plasma as drawn; coldgas none):
at night a launch lights the ground for kilometres. It is set per class,
not per vessel (BRIGHTNESS scales it with the flame).

For the whole class:

  ReentryLegacy = TRUE   keep the add-on's reentry particle streams (its
                         own texture) instead of the client's plasma sheath
                         on the windward hull (default FALSE: the streams
                         only scale the sheath and its wake: their
                         amin/amax over the friction power decide when
                         it starts)

Later lines override earlier ones. Example (Config/VK/Plumes/DeltaGlider.cfg):

  THRUSTER main TYPE hydrolox SCALE 1.2
  THRUSTER hover 0 BRIGHTNESS 0.5
  EXHAUST 7 8 TYPE texture
