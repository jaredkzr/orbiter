Vessel damage: structure of vessel classes
==========================================

With the damage model on (Launchpad: Options > Damage, or DamageModel = 1
in Orbiter.cfg; ORBITER_DAMAGE=1 in the environment turns it on without
changing the configuration, for tests), vessels can break up: when a
joint of their structure is overloaded, the parts beyond it break off.

A vessel's structure is made automatically from its external meshes: their
surface is divided into parts of similar size which hang together
(vessels symmetric about their x = 0 plane get symmetric parts). The parts
form a tree: the core part, at the vessel's origin (its centre of
gravity), and the others hanging from it by joints, one part from
another. The empty mass is shared out by surface area.

A joint's strength is the largest force it carries in the design load
cases, times the safety factor (each joint varies by up to 10%, so that
symmetric parts do not fail together):

  - the parts beyond it accelerated at the design load factor,
  - aerodynamic forces at the design load factor along each axis (shared
    by the parts' projected areas: wings carry their lift),
  - each group of engines (main, retro, hover) at full thrust,
  - landing at 3 g on the gear (the first three touchdown points).

In flight (damage model on), each step the forces on each part (thrust on
the engines' parts, aerodynamic forces by projected area, control
surfaces, ground and vessel contacts where they touch) and the vessel's
motion (acceleration, turning) give the load of every joint. A joint
loaded beyond its strength breaks: the part and those beyond it fly off
as a piece of debris. A joint at the core part loaded beyond 3 times its
strength breaks the whole vessel up. Impacts are taken to decelerate the
structure over a crush stroke of 0.3 m.

Each part also has a skin temperature: heated by the air's stagnation
heat flux over its area facing the flow, cooled by radiation. A part
hotter than MAXTEMP burns off (the core part: the vessel breaks up).

Below a gas giant's crush pressure (GGCrushPressure in the planet's
configuration, 23 bar) vessels are crushed.

A file named after the vessel class overrides the defaults:

    Config/Damage/<classname>.cfg

    LOADFACTOR 12        ; design load factor [g] (default 6)
    SAFETY 1.5           ; safety factor (default 1.5)
    PARTS 16             ; number of automatic parts (default: from the size)
    INDESTRUCTIBLE       ; the class never breaks up
    MAXTEMP 1800         ; [K] skin temperature at which a part fails (default 1800)
    CRUSHPRESSURE 5e6    ; [Pa] pressure it withstands (default: the planet's)

Parts can be given explicitly (before the automatic ones are made from the
rest), by a box in the vessel frame [m] or by mesh groups (mesh:group,
indices from 0), optionally with their own load factor or a joint strength
[N]:

    PART LeftWing BOX -9 -1 -8  -3 1 0  LOADFACTOR 9
    PART Gear GROUPS 0:12 0:13 STRENGTH 2e5

The parts of a vessel can be listed with VESSEL::GetPartCount and
GetPartInfo (in Lua vessel:get_partcount, get_partinfo: also the load of
each joint and each part's temperature). VESSEL::BreakPart breaks a part
off (vessel:break_part); the module is told of parts lost by
VMSG_PARTLOST (clbkGeneric).
