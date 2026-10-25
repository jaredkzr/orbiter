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
         BRIGHTNESS  brightness factor

Later lines override earlier ones. Example (Config/VK/Plumes/DeltaGlider.cfg):

  THRUSTER main TYPE hydrolox SCALE 1.2
  THRUSTER hover 0 BRIGHTNESS 0.5
  EXHAUST 7 8 TYPE texture
