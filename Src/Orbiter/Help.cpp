// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Help.h"
#ifdef _WIN32
#include <htmlhelp.h>
#endif
#include <stdio.h>

void OpenHelp (HWND hWnd, const char *file, const char *topic)
{
	char topic_file[256];
	if (strlen(topic) < 4 || strnicmp(topic + strlen(topic) - 4, ".htm", 4))
		sprintf(topic_file, "%s.htm", topic);
	else
		strcpy(topic_file, topic);
#ifdef _WIN32
	HtmlHelp (hWnd, file, HH_DISPLAY_TOPIC, (DWORD_PTR)topic_file);
#endif
}

void OpenDefaultHelp (HWND hWnd, const char *topic)
{
	OpenHelp (hWnd, "html\\orbiter.chm", topic);
}
