// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "MfdWindow.h"
#include "Orbiter.h"
#include "Pane.h"
#include "Mfd.h"
#include "DlgMgr.h"
#include "UiAnchor.h"
#include "imgui.h"
#include "imgui_extras.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

extern Pane *g_pane;
extern TimeData td;

namespace {
	const int MINPX = 128;     // smallest display [pixels]
	const double SETTLE = 0.3; // [s] a new size must hold before the MFD is recreated

	MFDSPEC Spec (int px)
	{
		MFDSPEC s;
		s.pos = { 0, 0, (LONG)px, (LONG)px };
		s.nbt_left = s.nbt_right = 6;
		s.bt_yofs = px / 6;
		s.bt_ydist = px / 7;
		return s;
	}

	std::string Title (int slot) { return "MFD " + std::to_string (slot + 1); }
}

// =======================================================================
// The external MFD of a window

class PopoutMFD: public ExternMFD {
public:
	PopoutMFD (DlgMfdWindow *w, int px): ExternMFD (Spec (px)), win(w) {}
	~PopoutMFD () { if (win) win->ext = nullptr; } // (deleted with the pane)
	DlgMfdWindow *win;
};

// =======================================================================

DlgMfdWindow::DlgMfdWindow (int _slot)
	: ImGuiDialog (("###mfdwin" + std::to_string (_slot)).c_str(), { 420.0f, 395.0f }), slot(_slot)
{}

DlgMfdWindow::~DlgMfdWindow ()
{
	OnClose ();
}

ExternMFD *DlgMfdWindow::MFD () const { return ext; }

int DlgMfdWindow::Mode () const
{
	return (ext && ext->instr) ? ext->instr->Type () : MFD_NONE;
}

bool DlgMfdWindow::Open (int mode)
{
	if (!g_pane) return false;
	if (!ext) {
		if (!px) px = 400;
		ext = new PopoutMFD (this, px);
		g_pane->RegisterExternMFD (ext, Spec (px)); // (opens the Orbit MFD)
	}
	if (mode != MFD_NONE && Mode () != mode) ext->SetMode (mode);
	if (Mode () != MFD_NONE) lastMode = Mode ();
	active = true;
	return Mode () != MFD_NONE;
}

void DlgMfdWindow::OnClose ()
{
	active = false;
	if (ext) {
		if (Mode () != MFD_NONE) lastMode = Mode ();
		ext->win = nullptr;
		ExternMFD *e = ext;
		ext = nullptr;
		if (g_pane) g_pane->UnregisterExternMFD (e); // (deletes it)
	}
	ptrDown[0] = ptrDown[1] = false;
	hovered = false;
}

void DlgMfdWindow::Resize (int newPx)
{
	px = newPx;
	if (ext) {
		ext->Resize (Spec (px));
		if (Mode () == MFD_NONE && lastMode != MFD_NONE) ext->SetMode (lastMode);
	}
}

void DlgMfdWindow::Pointer (int event, float x, float y, int button, float wheel)
{
	if (!ext) return;
	MFDPOINTER p = {};
	p.event = event;
	p.x = x, p.y = y;
	p.button = button;
	p.buttons = (ptrDown[0] ? 1 : 0) | (ptrDown[1] ? 2 : 0);
	p.wheel = wheel;
	ext->ProcessPointer (p);
}

void DlgMfdWindow::EditKeys ()
{
	// a field of the display is being edited: the keys typed in this window
	if (!ext || !ext->instr || !ext->instr->Editing ()) return;
	static const struct { ImGuiKey k; DWORD key; } MAP[] = {
		{ ImGuiKey_0, OAPI_KEY_0 }, { ImGuiKey_1, OAPI_KEY_1 }, { ImGuiKey_2, OAPI_KEY_2 }, { ImGuiKey_3, OAPI_KEY_3 },
		{ ImGuiKey_4, OAPI_KEY_4 }, { ImGuiKey_5, OAPI_KEY_5 }, { ImGuiKey_6, OAPI_KEY_6 }, { ImGuiKey_7, OAPI_KEY_7 },
		{ ImGuiKey_8, OAPI_KEY_8 }, { ImGuiKey_9, OAPI_KEY_9 },
		{ ImGuiKey_Keypad0, OAPI_KEY_NUMPAD0 }, { ImGuiKey_Keypad1, OAPI_KEY_NUMPAD1 }, { ImGuiKey_Keypad2, OAPI_KEY_NUMPAD2 },
		{ ImGuiKey_Keypad3, OAPI_KEY_NUMPAD3 }, { ImGuiKey_Keypad4, OAPI_KEY_NUMPAD4 }, { ImGuiKey_Keypad5, OAPI_KEY_NUMPAD5 },
		{ ImGuiKey_Keypad6, OAPI_KEY_NUMPAD6 }, { ImGuiKey_Keypad7, OAPI_KEY_NUMPAD7 }, { ImGuiKey_Keypad8, OAPI_KEY_NUMPAD8 },
		{ ImGuiKey_Keypad9, OAPI_KEY_NUMPAD9 },
		{ ImGuiKey_Period, OAPI_KEY_PERIOD }, { ImGuiKey_KeypadDecimal, OAPI_KEY_DECIMAL }, { ImGuiKey_Comma, OAPI_KEY_COMMA },
		{ ImGuiKey_Minus, OAPI_KEY_MINUS }, { ImGuiKey_KeypadSubtract, OAPI_KEY_SUBTRACT }, { ImGuiKey_E, OAPI_KEY_E },
		{ ImGuiKey_Backspace, OAPI_KEY_BACK }, { ImGuiKey_Enter, OAPI_KEY_RETURN }, { ImGuiKey_KeypadEnter, OAPI_KEY_NUMPADENTER },
		{ ImGuiKey_Escape, OAPI_KEY_ESCAPE }, { ImGuiKey_UpArrow, OAPI_KEY_UP }, { ImGuiKey_DownArrow, OAPI_KEY_DOWN } };
	for (const auto &m : MAP)
		if (ImGui::IsKeyPressed (m.k, false)) ext->instr->EditKey (m.key);
}

void DlgMfdWindow::Display ()
{
	ImGui::SetNextWindowSize (ImVec2 (defaultSize.width, defaultSize.height), ImGuiCond_FirstUseEver);
	{	// first placement: side by side from the left, below the menu bar
		ImVec2 vs = ImGui::GetMainViewport ()->Size;
		int perRow = std::max (1, (int)((vs.x - 40.0f) / (defaultSize.width + 16.0f)));
		float x = 40.0f + (slot % perRow) * (defaultSize.width + 16.0f);
		float y = 90.0f + (slot / perRow % 2) * (defaultSize.height + 16.0f);
		ImGui::SetNextWindowPos (ImVec2 (x, y), ImGuiCond_FirstUseEver);
	}
	ImGui::SetNextWindowSizeConstraints (ImVec2 (240.0f, 260.0f), ImVec2 (4096.0f, 4096.0f));
	// title: the mode, with a stable window id
	std::string title = Title (slot);
	if (ext && ext->instr) {
		const char *mn = Instrument::ModeName (ext->instr->Type ());
		if (mn) title = std::string (mn) + " - " + title;
	}
	title += name; // ("###mfdwin<slot>")
	ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (4.0f, 4.0f));
	if (ImGui::Begin (title.c_str(), &active, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
		if (uianchor::Enabled ()) { // (tests: the window's corners and title bar)
			ImVec2 p = ImGui::GetWindowPos (), s = ImGui::GetWindowSize ();
			std::string pre = "mfdwin" + std::to_string (slot) + ".window.";
			uianchor::Set ((pre + "title").c_str(), p.x + s.x * 0.5f, p.y + ImGui::GetFrameHeight () * 0.5f);
			uianchor::Set ((pre + "br").c_str(), p.x + s.x - 2.0f, p.y + s.y - 2.0f);
		}
		OnDraw ();
	}
	ImGui::End ();
	ImGui::PopStyleVar ();
	if (!active) OnClose ();
}

void DlgMfdWindow::OnDraw ()
{
	if (!ext) { // (closed with the session's pane, or switched off)
		if (!Open (lastMode)) return;
	}
	ImVec2 avail = ImGui::GetContentRegionAvail ();
	ImVec2 org = ImGui::GetCursorScreenPos ();
	ImVec2 org0 = ImGui::GetCursorPos ();
	float bw = std::clamp (avail.x * 0.11f, 30.0f, 64.0f), bh = bw * 0.72f, gap = 4.0f;
	float D = std::min (avail.x - 2.0f * (bw + gap), avail.y - bh - gap);
	D = std::max (D, (float)MINPX * 0.5f);

	// recreate the MFD at the new size once it holds
	int want = std::max (MINPX, ((int)D / 4) * 4);
	if (want != px) {
		if (want != wantPx) wantPx = want, wantSince = td.SysT0;
		else if (td.SysT0 - wantSince >= SETTLE && !ptrDown[0] && !ptrDown[1]) Resize (want);
	}

	float x0 = std::floor ((avail.x - D - 2.0f * (bw + gap)) * 0.5f);
	ImVec2 dpos (org0.x + x0 + bw + gap, org0.y);

	// side buttons
	for (int side = 0; side < 2; side++)
		for (int i = 0; i < 6; i++) {
			int bt = side * 6 + i;
			const char *lbl = ext->GetButtonLabel (bt);
			char id[32];
			snprintf (id, sizeof (id), "%s##bt%d", lbl ? lbl : "", bt);
			float y = dpos.y + D / 6.0f + i * D / 7.0f - bh * 0.5f;
			float x = side ? dpos.x + D + gap : dpos.x - gap - bw;
			ImGui::SetCursorPos (ImVec2 (x, y));
			ImGui::Button (id, ImVec2 (bw, bh));
			if (ImGui::IsItemActivated ()) ext->ProcessButton (bt, PANEL_MOUSE_LBDOWN), heldButton = bt;
			if (ImGui::IsItemDeactivated ()) ext->ProcessButton (bt, PANEL_MOUSE_LBUP), heldButton = -1;
			if (!ext) return;
		}

	// the display: pointer events from an invisible button over the image
	SURFHANDLE surf = ext->GetDisplaySurface ();
	ImVec2 sp (org.x + (dpos.x - org0.x), org.y + (dpos.y - org0.y));
	ImGui::SetCursorPos (dpos);
	ImGui::InvisibleButton ("##display", ImVec2 (D, D), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	ImDrawList *dl = ImGui::GetWindowDrawList ();
	if (surf) dl->AddImage (ImTextureRef (ImGui::GetImTextureID (surf)), sp, ImVec2 (sp.x + D, sp.y + D));
	else dl->AddRectFilled (sp, ImVec2 (sp.x + D, sp.y + D), IM_COL32 (0, 0, 0, 255));
	if (uianchor::Enabled ()) {
		std::string pre = "mfdwin" + std::to_string (slot) + ".";
		uianchor::Set ((pre + "tl").c_str(), sp.x, sp.y);
		uianchor::Set ((pre + "br").c_str(), sp.x + D, sp.y + D);
		if (ext->instr && px > 0) {
			std::vector<std::pair<std::string, oapi::FVECTOR2>> list;
			ext->instr->Anchors (list);
			for (auto &a : list)
				uianchor::Set ((pre + a.first).c_str(), sp.x + a.second.x / px * D, sp.y + a.second.y / px * D);
		}
	}

	ImGuiIO &io = ImGui::GetIO ();
	bool hov = ImGui::IsItemHovered ();
	float mx = (io.MousePos.x - sp.x) / D * px, my = (io.MousePos.y - sp.y) / D * px;
	if (hov || ptrDown[0] || ptrDown[1]) {
		if (mx != lastX || my != lastY) Pointer (MFD_POINTER_MOVE, mx, my);
		lastX = mx, lastY = my;
		for (int b = 0; b < 2; b++) {
			if (hov && ImGui::IsMouseClicked (b)) { ptrDown[b] = true; Pointer (MFD_POINTER_DOWN, mx, my, b); }
			if (ptrDown[b] && ImGui::IsMouseReleased (b)) { ptrDown[b] = false; Pointer (MFD_POINTER_UP, mx, my, b); }
		}
		if (hov && io.MouseWheel != 0.0f) Pointer (MFD_POINTER_WHEEL, mx, my, 0, io.MouseWheel);
	} else if (hovered) {
		Pointer (MFD_POINTER_LEAVE, -1.0f, -1.0f);
		lastX = lastY = -1.0f;
	}
	hovered = hov;
	if (!ext) return;
	if (ImGui::IsWindowFocused ()) EditKeys ();

	// bottom buttons: power, mode selection, menu
	static const char *bl[3] = { "PWR", "SEL", "MNU" };
	float bx = dpos.x + D * 0.5f - 1.5f * bw - gap;
	for (int i = 0; i < 3; i++) {
		ImGui::SetCursorPos (ImVec2 (bx + i * (bw + gap), dpos.y + D + gap));
		char id[16];
		snprintf (id, sizeof (id), "%s##b%d", bl[i], i);
		if (ImGui::Button (id, ImVec2 (bw, bh))) {
			switch (i) {
			case 0:
				if (Mode () != MFD_NONE) { lastMode = Mode (); ext->SetMode (MFD_NONE); }
				else ext->SetMode (lastMode);
				break;
			case 1: ext->SendKey (OAPI_KEY_F1); break;
			case 2: ext->SendKey (OAPI_KEY_GRAVE); break;
			}
			if (!ext) return;
		}
		if (uianchor::Enabled ()) {
			ImVec2 c = ImGui::GetItemRectMin (), d = ImGui::GetItemRectMax ();
			std::string nm = "mfdwin" + std::to_string (slot) + ".btn." + bl[i];
			uianchor::Set (nm.c_str(), 0.5f * (c.x + d.x), 0.5f * (c.y + d.y));
		}
	}
}

// =======================================================================

MfdWindows::MfdWindows (DialogManager *_dm): dm(_dm) {}

MfdWindows::~MfdWindows ()
{
	CloseAll ();
}

DlgMfdWindow *MfdWindows::Open (int mode)
{
	DlgMfdWindow *w = nullptr;
	for (auto &p : wins)
		if (!p->IsActive ()) { w = p.get(); break; }
	if (!w) {
		wins.push_back (std::make_unique<DlgMfdWindow> ((int)wins.size()));
		w = wins.back().get();
		dm->AddEntry (w);
	}
	w->Open (mode);
	w->Activate ();
	return w;
}

DlgMfdWindow *MfdWindows::Window (int slot) const
{
	return (slot >= 0 && slot < (int)wins.size()) ? wins[slot].get() : nullptr;
}

void MfdWindows::CloseAll ()
{
	for (auto &p : wins) {
		dm->DelEntry (p.get());
		p->OnClose ();
	}
	wins.clear ();
}

void MfdWindows::Save (const char *path) const
{
	std::ofstream ofs (path);
	if (!ofs) return;
	ofs << "; MFD windows open at the end of the last session (slot, mode)" << std::endl;
	for (auto &p : wins) {
		if (!p->IsActive ()) continue;
		const char *mn = Instrument::ModeName (p->Mode () != MFD_NONE ? p->Mode () : MFD_ORBIT);
		ofs << "WINDOW " << p->Slot () << ' ' << (mn ? mn : "Orbit") << std::endl;
	}
}

void MfdWindows::Restore (const char *path)
{
	std::ifstream ifs (path);
	std::string line;
	while (std::getline (ifs, line)) {
		if (line.compare (0, 7, "WINDOW ")) continue;
		int slot = -1;
		char name[256] = "";
		if (sscanf (line.c_str() + 7, "%d %255[^\n]", &slot, name) < 2 || slot < 0 || slot > 63) continue;
		while (strlen (name) && (name[strlen (name)-1] == '\r' || name[strlen (name)-1] == ' ')) name[strlen (name)-1] = 0;
		int mode = Instrument::ModeFromName (name);
		if (mode == MFD_NONE) mode = MFD_ORBIT;
		while ((int)wins.size() <= slot) {
			wins.push_back (std::make_unique<DlgMfdWindow> ((int)wins.size()));
			dm->AddEntry (wins.back().get());
		}
		wins[slot]->Open (mode);
	}
}
