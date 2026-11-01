// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Implementation of class StarSpec

#include <fstream>
#include <stdio.h>
#include "Orbiter.h"
#include "Config.h"
#include "Star.h"
#include "Camera.h"
#include "Log.h"
#include "Psys.h"

using namespace std;

extern Orbiter *g_pOrbiter;
extern Camera *g_camera;
extern char DBG_MSG[256];


Star::Star (double _mass, double _mean_radius)
: CelestialBody (_mass, _mean_radius)
{
	Setup ();
}

Star::Star (char *fname)
: CelestialBody (fname)
{
	ifstream ifs (g_pOrbiter->ConfigPath (fname));
	if (!ifs) return;
	bDynamicPosVel = false;
	// read star-specific parameters here
	InitDeviceObjects ();
	Setup ();
}

Star::~Star ()
{
	DestroyDeviceObjects ();
}

void Star::Setup ()
{
	CelestialBody::Setup ();
	ExternPosition(); // try to init from module
}

void Star::Update (bool force)
{
	if (bDynamicPosVel || module && module->bEphemeris())
		CelestialBody::Update (force);
	else {
		// otherwise keep the star at rest at the origin of the global
		// coordinate system: its state carried over to the step's end, its
		// rotation turned (the new state's: else a zero matrix after the
		// states swap)
		s1->pos = s0->pos, s1->vel = s0->vel;
		UpdatePrecession ();
		UpdateRotation ();
	}
}

Vector Star::Pos2Barycentre (Vector &pos)
{
	// by definition, the barycentre of stars (root objects) is the
	// barycentre of the system: the global frame's origin unless it moved
	extern PlanetarySystem *g_psys;
	return g_psys ? -g_psys->Origin () : Vector();
}

Vector4 Star::GetLightColor ()
{
	return {1,1,1,1};
}
