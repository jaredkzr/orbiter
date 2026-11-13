// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Lua's audio table: sound files on vessels and the interface, animations
// with a motor's sound, a vessel's sounds tuned (OrbiterAPI.h: oapiAudio*)

#define INTERPRETER_IMPLEMENTATION
#include "Interpreter.h"
#include "VesselAPI.h"

/***
Sound.

A vessel's engines, thrusters, air, contacts and docking sound by
themselves. These functions add sounds, give animations a motor's sound
and tune a vessel's sounds.
@module audio
*/

/// (a vessel argument: a handle or a vessel object; nil: none)
OBJHANDLE Interpreter::audio_vessel (lua_State *L, int idx)
{
	if (lua_isnil (L, idx) || lua_isnone (L, idx)) return nullptr;
	if (lua_islightuserdata (L, idx)) return lua_toObject (L, idx);
	if (VESSEL *v = lua_tovessel (L, idx)) return v->GetHandle ();
	return nullptr;
}

/***
Plays a sound file on a vessel, or on the interface.
@function play
@param vessel the vessel (handle or object), or nil: the interface
@tparam string file a WAV, FLAC or MP3 file (as given, or under Sound/)
@tparam[opt] table opt pos (vector, vessel frame [m]), level (number, default 1), loop (boolean)
@treturn int a handle (0: not played)
*/
int Interpreter::audio_play (lua_State *L)
{
	OBJHANDLE h = audio_vessel (L, 1);
	ASSERT_SYNTAX (lua_isnil (L, 1) || h, "Argument 1: invalid vessel");
	ASSERT_STRING (L, 2);
	const char *file = lua_tostring (L, 2);
	VECTOR3 pos = _V(0, 0, 0);
	double level = 1.0;
	bool loop = false;
	if (lua_istable (L, 3)) {
		lua_getfield (L, 3, "pos");
		if (lua_isvector (L, -1)) pos = lua_tovector (L, -1);
		lua_pop (L, 1);
		lua_getfield (L, 3, "level");
		if (lua_isnumber (L, -1)) level = lua_tonumber (L, -1);
		lua_pop (L, 1);
		lua_getfield (L, 3, "loop");
		if (lua_isboolean (L, -1)) loop = lua_toboolean (L, -1) != 0;
		lua_pop (L, 1);
	}
	lua_pushnumber (L, oapiAudioPlay (h, file, &pos, level, loop));
	return 1;
}

/***
Stops a sound.
@function stop
@tparam int handle from play
@treturn bool false: no such sound (any more)
*/
int Interpreter::audio_stop (lua_State *L)
{
	ASSERT_NUMBER (L, 1);
	lua_pushboolean (L, oapiAudioStop ((int)lua_tonumber (L, 1)));
	return 1;
}

/***
Sets a sound's level and pitch.
@function set
@tparam int handle from play
@tparam table opt level (number), pitch (number)
@treturn bool false: no such sound
*/
int Interpreter::audio_set (lua_State *L)
{
	ASSERT_NUMBER (L, 1);
	ASSERT_TABLE (L, 2);
	double level = -1.0, pitch = 0.0;
	lua_getfield (L, 2, "level");
	if (lua_isnumber (L, -1)) level = lua_tonumber (L, -1);
	lua_pop (L, 1);
	lua_getfield (L, 2, "pitch");
	if (lua_isnumber (L, -1)) pitch = lua_tonumber (L, -1);
	lua_pop (L, 1);
	lua_pushboolean (L, oapiAudioSet ((int)lua_tonumber (L, 1), level, pitch));
	return 1;
}

/***
Is a sound still playing?
@function playing
@tparam int handle from play
@treturn bool
*/
int Interpreter::audio_playing (lua_State *L)
{
	ASSERT_NUMBER (L, 1);
	lua_pushboolean (L, oapiAudioPlaying ((int)lua_tonumber (L, 1)));
	return 1;
}

/***
Gives one of a vessel's animations a motor's sound.
@function animation
@param vessel handle or object
@tparam int anim the animation's index
@tparam int kind audio.MOTOR, audio.DOOR, audio.GEAR (audio.NONE: none)
@tparam[opt] number level loudness (default 1)
*/
int Interpreter::audio_animation (lua_State *L)
{
	OBJHANDLE h = audio_vessel (L, 1);
	ASSERT_SYNTAX (h, "Argument 1: invalid vessel");
	ASSERT_NUMBER (L, 2);
	ASSERT_NUMBER (L, 3);
	double level = lua_isnumber (L, 4) ? lua_tonumber (L, 4) : 1.0;
	oapiAudioAnimation (h, (UINT)lua_tonumber (L, 2), (int)lua_tonumber (L, 3), level);
	return 0;
}

/***
Tunes a vessel's own sounds.
@function tune
@param vessel handle or object
@tparam string key engine_level, engine_pitch, rumble, whine, crackle, rcs_level, rcs_pitch, air_level, contact_level
@tparam number value a factor (1: as usual)
@treturn bool false: no such key
*/
int Interpreter::audio_tune (lua_State *L)
{
	OBJHANDLE h = audio_vessel (L, 1);
	ASSERT_SYNTAX (h, "Argument 1: invalid vessel");
	ASSERT_STRING (L, 2);
	ASSERT_NUMBER (L, 3);
	lua_pushboolean (L, oapiAudioTune (h, lua_tostring (L, 2), lua_tonumber (L, 3)));
	return 1;
}

/***
The voice's callouts said lately (for the vessel in focus).
@function callouts
@treturn table the files, newest first
*/
int Interpreter::audio_callouts (lua_State *L)
{
	lua_newtable (L);
	for (int i = 0; ; i++) {
		const char *f = oapiAudioCallout (i);
		if (!f) break;
		lua_pushstring (L, f);
		lua_rawseti (L, -2, i + 1);
	}
	return 1;
}

/***
A lightning flash in a planet's air: its thunder, heard as its sound arrives.
@function lightning
@param planet handle
@tparam vector start where its channel starts (global frame) [m]
@tparam vector dir its way (global frame)
@tparam number length the channel's length [m]
@tparam[opt=2e5] number energy per metre [J/m]
@tparam[opt=3] int strokes
@tparam[opt=0.3] number duration of its strokes [s]
@tparam[opt=1] int seed its channel's kinks
*/
int Interpreter::audio_lightning (lua_State *L)
{
	ASSERT_SYNTAX (lua_islightuserdata (L, 1), "Argument 1: planet handle expected");
	OBJHANDLE h = lua_toObject (L, 1);
	VECTOR3 start = lua_tovector (L, 2), dir = lua_tovector (L, 3);
	ASSERT_NUMBER (L, 4);
	double El = lua_isnumber (L, 5) ? lua_tonumber (L, 5) : 2e5;
	int strokes = lua_isnumber (L, 6) ? (int)lua_tonumber (L, 6) : 3;
	double dur = lua_isnumber (L, 7) ? lua_tonumber (L, 7) : 0.3;
	unsigned seed = lua_isnumber (L, 8) ? (unsigned)lua_tonumber (L, 8) : 1u;
	oapiAudioLightning (h, start, dir, lua_tonumber (L, 4), El, strokes, dur, seed);
	return 0;
}

void Interpreter::LoadAudioAPI ()
{
	static const struct luaL_reg audioLib[] = {
		{"play", audio_play},
		{"stop", audio_stop},
		{"set", audio_set},
		{"playing", audio_playing},
		{"animation", audio_animation},
		{"tune", audio_tune},
		{"callouts", audio_callouts},
		{"lightning", audio_lightning},
		{NULL, NULL}
	};
	luaL_openlib (L, "audio", audioLib, 0);
	lua_pushnumber (L, AUDIO_ANIM_NONE);  lua_setfield (L, -2, "NONE");
	lua_pushnumber (L, AUDIO_ANIM_MOTOR); lua_setfield (L, -2, "MOTOR");
	lua_pushnumber (L, AUDIO_ANIM_DOOR);  lua_setfield (L, -2, "DOOR");
	lua_pushnumber (L, AUDIO_ANIM_GEAR);  lua_setfield (L, -2, "GEAR");
	lua_pop (L, 1);
}
