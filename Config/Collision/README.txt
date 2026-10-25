Collision hulls of vessel classes
=================================

Vessels collide with each other using convex hulls made automatically from
their external meshes (one for each mesh group). A vessel class can replace
them with its own hulls in a file named after the class:

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

Contacts are not made close to two free docking ports lined up with each
other, so that vessels can dock.
