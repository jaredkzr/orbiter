// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "UiAnchor.h"
#include "ImGuiItemHooks.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <vector>
#include <map>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
	struct Pos { float x, y; };
	std::map<std::string, Pos> cur, prev;
	std::mutex mtx;
	bool enabled = false;

	// ---- ImGui's widgets (test engine hooks) --------------------------------
	struct Widget {
		std::string window, label;
		float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		int flags = 0;
		bool visible = false;
	};
	std::unordered_map<ImGuiID, Widget> wcur, wprev; // this frame's, the last's

	// a label without "##id" and icon glyphs (private use: U+E000-U+F8FF)
	std::string Clean (const char *label)
	{
		std::string s;
		for (const char *p = label; *p; ) {
			if (p[0] == '#' && p[1] == '#') break;
			unsigned char c = (unsigned char)*p;
			if (c == 0xEE || (c == 0xEF && (unsigned char)p[1] <= 0xA3)) { // (3-byte private-use glyph)
				p += 3;
				continue;
			}
			s += *p++;
		}
		size_t a = s.find_first_not_of (" \t"), b = s.find_last_not_of (" \t");
		return a == std::string::npos ? std::string() : s.substr (a, b - a + 1);
	}

	void ItemAdd (ImGuiContext *ctx, ImGuiID id, const ImRect &bb, const ImGuiLastItemData *)
	{
		ImGuiWindow *w = ctx->CurrentWindow;
		if (!w || !id || w->IsFallbackWindow) return; // (not ImGui's implicit "Debug" window)
		Widget &it = wcur[id];
		it.window = Clean (w->Name);
		it.x0 = bb.Min.x, it.y0 = bb.Min.y, it.x1 = bb.Max.x, it.y1 = bb.Max.y;
		it.visible = !w->Hidden && !w->SkipItems && bb.Overlaps (w->ClipRect);
	}

	void ItemInfo (ImGuiContext *, ImGuiID id, const char *label, ImGuiItemStatusFlags flags)
	{
		if (!id || !label) return;
		Widget &it = wcur[id]; // (some widgets, e.g. tabs, report their label before they are added)
		it.label = Clean (label);
		it.flags = (int)flags;
	}

	const ImGuiItemHooks hooks = { ItemAdd, ItemInfo };

	std::string Lower (std::string s)
	{
		std::transform (s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower (c); });
		return s;
	}

	// the widgets on screen: the last complete frame's (and this one's so far)
	std::vector<Widget> Visible ()
	{
		std::unordered_map<ImGuiID, Widget> all (wprev);
		for (auto &kv : wcur) all[kv.first] = kv.second;
		std::vector<Widget> v;
		for (auto &kv : all)
			if (kv.second.visible && !kv.second.label.empty()) v.push_back (kv.second);
		std::sort (v.begin(), v.end(), [](const Widget &a, const Widget &b) { return a.y0 != b.y0 ? a.y0 < b.y0 : a.x0 < b.x0; });
		return v;
	}

	bool FindWidget (const std::string &spec, float &x, float &y)
	{
		std::string s = spec, win;
		int nth = 1;
		size_t br = s.rfind ('[');
		if (br != std::string::npos && s.back() == ']') {
			nth = std::max (1, atoi (s.c_str() + br + 1));
			s.erase (br);
		}
		size_t sl = s.find ('/');
		if (sl != std::string::npos) win = Lower (s.substr (0, sl)), s = s.substr (sl + 1);
		s = Lower (s);
		for (const Widget &w : Visible ()) {
			if (Lower (w.label) != s) continue;
			if (!win.empty() && Lower (w.window).find (win) == std::string::npos) continue;
			if (--nth > 0) continue;
			x = (w.x0 + w.x1) * 0.5f, y = (w.y0 + w.y1) * 0.5f;
			return true;
		}
		return false;
	}
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
	wprev.swap (wcur);
	wcur.clear();
	if (ImGuiContext *g = ImGui::GetCurrentContext()) { // (the widgets of this frame, reported)
		g->TestEngine = (void*)&hooks;
		g->TestEngineHookItems = true;
	}
}

std::string uianchor::Widgets (const std::string &filter)
{
	std::lock_guard<std::mutex> lock (mtx);
	enabled = true;
	std::string f = Lower (filter), s;
	char buf[96];
	for (const Widget &w : Visible ()) {
		if (!f.empty() && Lower (w.label).find (f) == std::string::npos && Lower (w.window).find (f) == std::string::npos) continue;
		const char *state = "";
		if (w.flags & ImGuiItemStatusFlags_Checkable) state = (w.flags & ImGuiItemStatusFlags_Checked) ? "checked" : "unchecked";
		else if (w.flags & ImGuiItemStatusFlags_Inputable) state = "input";
		else if (w.flags & ImGuiItemStatusFlags_Openable) state = (w.flags & ImGuiItemStatusFlags_Opened) ? "open" : "closed";
		snprintf (buf, sizeof (buf), "%.0f %.0f %.0f %.0f\t", (w.x0 + w.x1) * 0.5f, (w.y0 + w.y1) * 0.5f, w.x1 - w.x0, w.y1 - w.y0);
		s += buf + w.window + "\t" + w.label + "\t" + state + "\n";
	}
	return s;
}

std::string uianchor::List (const std::string &prefix)
{
	std::lock_guard<std::mutex> lock (mtx);
	enabled = true;
	std::map<std::string, Pos> all (prev);
	for (auto &kv : cur) all[kv.first] = kv.second;
	std::string s;
	char buf[64];
	for (auto &kv : all) {
		if (kv.first.compare (0, prefix.size(), prefix)) continue;
		snprintf (buf, sizeof (buf), " %.0f %.0f\n", kv.second.x, kv.second.y);
		s += kv.first + buf;
	}
	return s;
}

bool uianchor::Get (const char *name, float &x, float &y)
{
	std::lock_guard<std::mutex> lock (mtx);
	enabled = true;
	if (!strncmp (name, "ui:", 3)) return FindWidget (name + 3, x, y); // (a widget, by its label)
	auto it = cur.find (name);
	if (it == cur.end()) {
		it = prev.find (name);
		if (it == prev.end()) return false;
	}
	x = it->second.x, y = it->second.y;
	return true;
}
