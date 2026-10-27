Vessel damage: structure of vessel classes
==========================================

With the damage model on (Launchpad: Options > Damage, or DamageModel = 1
in Orbiter.cfg), vessels can break up: when a joint of their structure is
overloaded, the parts beyond it break off.

A vessel's structure is made automatically from its external meshes: their
surface is divided into parts of similar size which hang together
(vessels symmetric about their x = 0 plane get symmetric parts). The parts
form a tree: the core part, at the vessel's origin (its centre of
gravity), and the others hanging from it by joints, one part from
another. The empty mass is shared out by surface area.

A joint's strength is the force it carries when the parts beyond it are
accelerated at the design load factor, times the safety factor (each joint
varies by up to 10%, so that symmetric parts do not fail together).

A file named after the vessel class overrides the defaults:

    Config/Damage/<classname>.cfg

    LOADFACTOR 12        ; design load factor [g] (default 6)
    SAFETY 1.5           ; safety factor (default 1.5)
    PARTS 16             ; number of automatic parts (default: from the size)
    INDESTRUCTIBLE       ; the class never breaks up

Parts can be given explicitly (before the automatic ones are made from the
rest), by a box in the vessel frame [m] or by mesh groups (mesh:group,
indices from 0), optionally with their own load factor or a joint strength
[N]:

    PART LeftWing BOX -9 -1 -8  -3 1 0  LOADFACTOR 9
    PART Gear GROUPS 0:12 0:13 STRENGTH 2e5

The parts of a vessel can be listed with VESSEL::GetPartCount and
GetPartInfo (in Lua vessel:get_partcount, get_partinfo).
