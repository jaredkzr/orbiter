// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Remote control for playtesting (ORBITER_VK_REMOTE=<socket>): a local
// (Unix domain) socket takes commands, one per line, and answers each with
// one line of JSON once it has taken effect:
//   {"id":n,"ok":true,"result":<json>}  or  {"id":n,"ok":false,"error":"..."}
//
// The commands are TestInput's (mouse, keys, waits, screenshots, Lua, UI
// anchors: see TestInput.h), pushed into the window's event queue like a
// player's input, and these, answered at once:
//   ping                 alive?
//   info                 window size, frame, whether a session runs, pace
//   log                  the lines Orbiter.log gained since the last "log"
//   pace lockstep|realtime
//
// Pace (ORBITER_VK_REMOTE_PACE): lockstep (default) runs frames only while
// a command runs (a key press, "wait n"): between commands the simulation
// stands still, however long the player takes to decide. realtime runs
// on, taking commands as they come. Driven by Utils/Playtest/orbctl.py.
// =======================================================================

#ifndef __REMOTECONTROL_H_VKCLIENT
#define __REMOTECONTROL_H_VKCLIENT

#include <string>

struct SDL_Window;
class TestInput;

class RemoteControl {
public:
	~RemoteControl ();
	/// Listen on the socket at path; input: the interpreter of the commands
	bool Start (const std::string &path, bool lockstep, TestInput *input);
	/// Before a frame's input: take the commands that came; in lockstep with
	/// none running, wait here for one
	void Poll (SDL_Window *window);
	/// Answer a command that has run (TestInput's reply)
	void Answer (int id, bool ok, const std::string &text);
	/// The frame counter and whether a session runs (for "info")
	void SetFrame (unsigned long long frame, bool session) { this->frame = frame, this->session = session; }

private:
	void Accept ();
	bool Read (SDL_Window *window); // false: nothing came
	void Handle (SDL_Window *window, const std::string &line);
	void Send (const std::string &json);
	std::string NewLog ();

	TestInput *input = nullptr;
	std::string path;
	int listenFd = -1, clientFd = -1;
	std::string inbuf;
	bool lockstep = true;
	int nextId = 1;
	unsigned long long frame = 0;
	bool session = false;
	long long logOffset = -1;
};

#endif // !__REMOTECONTROL_H_VKCLIENT
