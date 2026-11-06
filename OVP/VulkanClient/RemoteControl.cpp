// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "RemoteControl.h"
#include "TestInput.h"
#include "OrbiterAPI.h"
#include "posix/OrbiterInput.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

OAPIFUNC void oapiExitOrbiter (int code); // implemented in the Orbiter core

namespace {
	std::string Json (const std::string &s)
	{
		std::string r = "\"";
		for (unsigned char c : s) {
			if (c == '"' || c == '\\') r += '\\', r += (char)c;
			else if (c == '\n') r += "\\n";
			else if (c == '\t') r += "\\t";
			else if (c < 0x20) { char b[8]; snprintf (b, sizeof (b), "\\u%04x", c); r += b; }
			else r += (char)c;
		}
		return r + "\"";
	}

	std::string Trim (const std::string &s)
	{
		size_t a = s.find_first_not_of (" \t\r\n"), b = s.find_last_not_of (" \t\r\n");
		return a == std::string::npos ? std::string() : s.substr (a, b - a + 1);
	}
}

#ifdef _WIN32

// (posix only, for now)
RemoteControl::~RemoteControl () {}
bool RemoteControl::Start (const std::string &, bool, TestInput *)
{
	oapiWriteLogError ("RemoteControl: not available on this platform");
	return false;
}
void RemoteControl::Poll (SDL_Window *) {}
void RemoteControl::Accept () {}
bool RemoteControl::Read (SDL_Window *) { return false; }
void RemoteControl::Handle (SDL_Window *, const std::string &) {}
void RemoteControl::Send (const std::string &) {}
std::string RemoteControl::NewLog () { return std::string(); }

#else

RemoteControl::~RemoteControl ()
{
	if (clientFd >= 0) close (clientFd);
	if (listenFd >= 0) {
		close (listenFd);
		unlink (path.c_str());
	}
}

bool RemoteControl::Start (const std::string &p, bool ls, TestInput *in)
{
	path = p, lockstep = ls, input = in;
	sockaddr_un addr = {};
	addr.sun_family = AF_UNIX;
	if (path.size() >= sizeof (addr.sun_path)) {
		oapiWriteLogError ("RemoteControl: socket path too long: %s", path.c_str());
		return false;
	}
	strcpy (addr.sun_path, path.c_str());
	unlink (path.c_str());
	listenFd = socket (AF_UNIX, SOCK_STREAM, 0);
	if (listenFd < 0 || bind (listenFd, (sockaddr*)&addr, sizeof (addr)) < 0 || listen (listenFd, 4) < 0) {
		oapiWriteLogError ("RemoteControl: cannot listen on %s: %s", path.c_str(), strerror (errno));
		return false;
	}
	fcntl (listenFd, F_SETFL, O_NONBLOCK);
	// (the log from here on is the first "log")
	std::ifstream f ("Orbiter.log", std::ios::ate);
	logOffset = f ? (long long)f.tellg () : 0;
	// (anchors are published once someone asks for them)
	float x, y;
	oapiInputUiAnchor ("", &x, &y);
	oapiWriteLogV ("RemoteControl: listening on %s (%s)", path.c_str(), lockstep ? "lockstep" : "realtime");
	return true;
}

void RemoteControl::Accept ()
{
	int fd = accept (listenFd, nullptr, nullptr);
	if (fd < 0) return;
	if (clientFd >= 0) close (clientFd); // (the newest client)
	clientFd = fd;
	fcntl (clientFd, F_SETFL, O_NONBLOCK);
	inbuf.clear();
}

bool RemoteControl::Read (SDL_Window *window)
{
	if (clientFd < 0) return false;
	char buf[4096];
	bool got = false;
	for (;;) {
		ssize_t n = recv (clientFd, buf, sizeof (buf), 0);
		if (n > 0) { inbuf.append (buf, n); got = true; continue; }
		if (n == 0) { close (clientFd); clientFd = -1; } // (the client left)
		break;
	}
	size_t e;
	while ((e = inbuf.find ('\n')) != std::string::npos) {
		std::string line = Trim (inbuf.substr (0, e));
		inbuf.erase (0, e + 1);
		if (!line.empty()) Handle (window, line);
	}
	return got;
}

void RemoteControl::Handle (SDL_Window *window, const std::string &line)
{
	int id = nextId++;
	std::string cmd = line.substr (0, line.find_first_of (" \t"));
	std::string args = Trim (line.substr (cmd.size()));
	char head[48];
	snprintf (head, sizeof (head), "{\"id\":%d,\"ok\":true,\"result\":", id);
	if (cmd == "ping") {
		Send (std::string (head) + "\"pong\"}");
	} else if (cmd == "info") {
		int w = 0, h = 0;
		if (window) SDL_GetWindowSize (window, &w, &h);
		char b[160];
		snprintf (b, sizeof (b), "{\"window\":[%d,%d],\"frame\":%llu,\"session\":%s,\"pace\":\"%s\"}}",
			w, h, frame, session ? "true" : "false", lockstep ? "lockstep" : "realtime");
		Send (std::string (head) + b);
	} else if (cmd == "log") {
		Send (std::string (head) + Json (NewLog ()) + "}");
	} else if (cmd == "pace") {
		if (args == "lockstep" || args == "realtime") {
			lockstep = (args == "lockstep");
			Send (std::string (head) + Json (args) + "}");
		} else {
			char b[160];
			snprintf (b, sizeof (b), "{\"id\":%d,\"ok\":false,\"error\":\"pace: lockstep or realtime\"}", id);
			Send (b);
		}
	} else if (cmd == "quit") {
		Send (std::string (head) + "null}");
		oapiExitOrbiter (0);
	} else {
		input->Append (line, id); // (answered when it has run)
	}
}

void RemoteControl::Send (const std::string &json)
{
	if (clientFd < 0) return;
	std::string s = json + "\n";
	size_t off = 0;
	while (off < s.size()) {
		ssize_t n = send (clientFd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
		if (n > 0) { off += n; continue; }
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			pollfd p = { clientFd, POLLOUT, 0 };
			poll (&p, 1, 100);
			continue;
		}
		close (clientFd);
		clientFd = -1;
		return;
	}
}

std::string RemoteControl::NewLog ()
{
	std::ifstream f ("Orbiter.log");
	if (!f) return std::string();
	f.seekg (0, std::ios::end);
	long long end = (long long)f.tellg ();
	if (logOffset < 0 || logOffset > end) logOffset = 0; // (a new log)
	f.seekg (logOffset);
	std::stringstream ss;
	ss << f.rdbuf();
	logOffset = end;
	return ss.str();
}

void RemoteControl::Poll (SDL_Window *window)
{
	if (listenFd < 0) return;
	Accept ();
	Read (window);
	// (answers and queries now, between frames: they take none)
	input->Step (window, true);
	// lockstep: nothing to run, the simulation waits for the next command
	while (lockstep && input->Idle ()) {
		pollfd p[2] = { { listenFd, POLLIN, 0 }, { clientFd, POLLIN, 0 } };
		poll (p, clientFd >= 0 ? 2 : 1, 100);
		SDL_PumpEvents (); // (a visible window stays alive)
		Accept ();
		Read (window);
		input->Step (window, true);
	}
}

#endif

void RemoteControl::Answer (int id, bool ok, const std::string &text)
{
	char head[48];
	if (ok) {
		snprintf (head, sizeof (head), "{\"id\":%d,\"ok\":true,\"result\":", id);
		Send (std::string (head) + (text.empty() ? "null" : text) + "}");
	} else {
		snprintf (head, sizeof (head), "{\"id\":%d,\"ok\":false,\"error\":", id);
		Send (std::string (head) + Json (text) + "}");
	}
}
