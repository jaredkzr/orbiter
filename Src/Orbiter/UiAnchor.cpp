// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "UiAnchor.h"
#include "imgui.h"
#include <map>
#include <mutex>
#include <string>

namespace {
	struct Pos { float x, y; };
	std::map<std::string, Pos> cur, prev;
	std::mutex mtx;
	bool enabled = false;
}

bool uianchor::Enabled ()
{
	return enabled;
}

void uianchor::Set (const char *name, float x, float y)
{
	if (!enabled) return;
	std::lock_guard<std::mutex> lock (mtx);
	cur[name] = {x, y};
}

void uianchor::SetItem (const char *name)
{
	if (!enabled) return;
	ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	Set (name, (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

void uianchor::NewFrame ()
{
	if (!enabled) return;
	std::lock_guard<std::mutex> lock (mtx);
	prev.swap (cur);
	cur.clear();
}

bool uianchor::Get (const char *name, float &x, float &y)
{
	std::lock_guard<std::mutex> lock (mtx);
	enabled = true;
	auto it = cur.find (name);
	if (it == cur.end()) {
		it = prev.find (name);
		if (it == prev.end()) return false;
	}
	x = it->second.x, y = it->second.y;
	return true;
}
