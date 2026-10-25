Collision hulls of vessel classes
=================================

Vessels collide with each other using convex hulls made automatically from
their external meshes (one for each mesh group). A vessel module can define
its own hulls instead (VESSEL::AddCollisionHull, ClearCollisionHulls,
ResetCollisionHulls; in Lua vessel:add_collisionhull etc.). A file named
after the vessel class replaces both:

    Config/Collision/<classname>.cfg

with one or more hulls, each given by points in the vessel frame [m] (the
hull is the convex shape around them):

    ; collision hulls of MyShip
    HULL
      -2.0 -1.0 -6.0
       2.0 -1.0 -6.0
       ...
    END
    HULL
      ...
    END

A file containing just

    NONE

gives the class no hulls: its vessels pass through other vessels, as they
did before Orbiter had vessel collisions.

Docking: two free docking ports capture each other only within the limits
set in Orbiter.cfg (defaults shown):

    DockingSpeedLimit = 0.5    ; relative speed of the ports [m/s]
    DockingAngleLimit = 10     ; misalignment, approach and roll [deg]

Contacts are not made close to two ports that would capture each other, so
that vessels can dock; too fast or askew, they collide.
