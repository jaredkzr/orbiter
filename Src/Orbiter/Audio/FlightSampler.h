// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// The vessel in focus read for FlightState (FlightSample): Orbiter's API,
// in one place. ORBITER_AUDIO_FLIGHT_TRACE=<file>: every sample written
// there (a trace for Tests/Audio.Flight.cpp).
// =======================================================================

#ifndef __AUDIOFLIGHTSAMPLER_H
#define __AUDIOFLIGHTSAMPLER_H

#include "FlightState.h"
#include <cstdio>
#include <string>

class Vessel;

namespace audio {

class System;

/// A vessel's sample (false: none to take - destroyed, no module)
bool SampleFlight (System &sys, const Vessel *v, double simt, double simdt, FlightSample &s);

/// The gear's state (0 up .. 1 down; < 0: not known): the animation given
/// the gear's sound, else the one its XRSound cfg names
double GearState (System &sys, const Vessel *v);

} // namespace audio

#endif // !__AUDIOFLIGHTSAMPLER_H
