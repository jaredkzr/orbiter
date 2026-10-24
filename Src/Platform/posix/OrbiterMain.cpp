// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Application entry point for non-Windows platforms.
// The simulation core is built as a shared library (OrbiterCore), which
// plugin modules and tests link against; this executable only starts it.
// =======================================================================

#include "Orbiter.h"
#include "cmdline.h"
#include "Log.h"
#include <clocale>
#include <cstdio>
#include <string>

extern Orbiter *g_pOrbiter;
extern char cwd[512];

// Re-assemble the command line from its arguments, in the form expected by
// orbiter::CommandLine (Windows-style single command line string)
static std::string JoinArguments (int argc, char *argv[])
{
	std::string cmdline;
	for (int i = 1; i < argc; i++) {
		if (i > 1) cmdline += ' ';
		std::string arg(argv[i]);
		if (arg.find_first_of(" \t") != std::string::npos && arg.find('"') == std::string::npos)
			cmdline += '"' + arg + '"';
		else
			cmdline += arg;
	}
	return cmdline;
}

int main (int argc, char *argv[])
{
	if (!getcwd (cwd, sizeof(cwd))) cwd[0] = '\0';

	g_pOrbiter = new Orbiter; // application instance

	// Parse command line
	std::string cmdline = JoinArguments (argc, argv);
	orbiter::CommandLine::Parse (g_pOrbiter, &cmdline[0]);

	// Initialise the log
	INITLOG("Orbiter.log", g_pOrbiter->Cfg()->CfgCmdlinePrm.bAppendLog);
	LOGOUT("Build %s [v.%06d] (headless)", __DATE__, g_pOrbiter->GetVersion());

	// Initialise random number generator
	srand (12345);

	if (FAILED (g_pOrbiter->Create (NULL))) {
		LOGOUT_ERR("Application creation failed");
		fprintf (stderr, "Orbiter: application creation failed. See Orbiter.log for details.\n");
		return 1;
	}

	setlocale (LC_CTYPE, "");

	int res = g_pOrbiter->Run ();
	delete g_pOrbiter;
	return res;
}
