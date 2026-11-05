// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#define EXPORT_IMGUI_CONTEXT
#define OAPI_IMPLEMENTATION
#define IMGUI_DEFINE_MATH_OPERATORS
#include <stdio.h>
#include "OrbiterAPI.h"
#include "UiTheme.h"
#include "IconsLucide.h"
#include "DlgMgr.h"
#include "UiAnchor.h"
#include "resource.h"
#include "Orbiter.h"
#include "Log.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_extras.h"
#ifdef _WIN32
#include "imgui_impl_win32.h"
#endif
#include "implot.h"
#include "IconsFontAwesome6.h"
#include <chrono>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_map>

using namespace oapi;

extern char DBG_MSG[256];
extern Orbiter *g_pOrbiter;

#ifdef _WIN32
static int x_sizeframe = GetSystemMetrics (SM_CXSIZEFRAME);
static int y_sizeframe = GetSystemMetrics (SM_CYSIZEFRAME);
static int x_fixedframe = GetSystemMetrics (SM_CXFIXEDFRAME);
static int y_fixedframe = GetSystemMetrics (SM_CYFIXEDFRAME);
#endif

static bool doflip = true;
static void RenderNotifications();

// dialog thread messages
#define TM_OPENDIALOG WM_USER
#define DLG_CAPTIONBUTTON (DLG_CAPTIONCLOSE|DLG_CAPTIONHELP)

struct THREADDATA {
	HINSTANCE hInst;
	HWND hParent;
	int id;
	DWORD flag;
	DLGPROC procDlg;
	void *context;
} g_tdata;

HWND g_hDlg;       // dialog handle passed from dlg to main thread
HANDLE hCreateDlg; // used for synchronising dialog creation
static DIALOGENTRY *de_create = 0;

// ==================================================================
// class DialogManager

DialogManager::DialogManager (Orbiter *orbiter, HWND hAppWnd)
{
	pOrbiter = orbiter;
	gc = orbiter->GetGraphicsClient();
	hWnd = hAppWnd;
	nEntry = 0;
	nList = nListBuf = 0;
	firstEntry = NULL;
	lastEntry = NULL;
	InitImGui();
}

DialogManager::~DialogManager ()
{
	Clear();
	ShutdownImGui();
}


void DialogManager::Init (HWND hAppWnd)
{
	Clear();
	hWnd        = hAppWnd;
}


void DialogManager::Clear ()
{
#ifdef _WIN32 // Win32 dialog windows (none exist on other platforms)
	DIALOGENTRY *tmp;
	while (firstEntry) {
		tmp = firstEntry;
		firstEntry = firstEntry->next;
		delete tmp->dlg;
		delete tmp;
	}
#endif
	lastEntry = NULL;
	if (nListBuf) {
		delete []DlgList;
		DlgList = NULL;
		nListBuf = 0;
	}
	nList = 0;

	nEntry = 0;
}

// =======================================================================

HWND DialogManager::OpenDialogEx (HINSTANCE hInst, int id, HWND hParent, DLGPROC pDlg, DWORD flag, void *context)
{
	if ((flag & DLG_ALLOWMULTI) == 0)
		if (IsEntry (hInst, id)) return NULL; // already open, and multiple instances not allowed
	return AddEntry (hInst, id, hParent, pDlg, flag, context);
}

void DialogManager::OpenDialogAsync (HINSTANCE hInst, int id, HWND hParent, DLGPROC pDlg, DWORD flag, void *context)
{
	if ((flag & DLG_ALLOWMULTI) == 0)
		if (IsEntry (hInst, id)) return; // already open, and multiple instances not allowed
	AddEntryAsync (hInst, id, hParent, pDlg, flag, context);
}

bool DialogManager::CloseDialog (HWND hDlg)
{
	if (DelEntry (hDlg, 0, 0)) {
		//DestroyWindow (hDlg);
		return true;
	}
	return false;
}

void *DialogManager::GetDialogContext (HWND hDlg)
{
#ifdef _WIN32
	DialogWin *dlg = (DialogWin*)GetWindowLongPtr (hDlg, DWLP_USER);
	return (dlg ? dlg->GetContext() : 0);
#else
	return 0;
#endif
}

DIALOGENTRY *DialogManager::AddWindow (HINSTANCE hInst, HWND hWnd, HWND hParent, DWORD flag)
{
#ifdef _WIN32
	DIALOGENTRY *tmp = new DIALOGENTRY; TRACENEW
	de_create = tmp;
	tmp->dlg = new DialogWin (hInst, hWnd, hParent, flag);

	tmp->prev = lastEntry;
	tmp->next = NULL;

	if (lastEntry)
		lastEntry->next = tmp;
	else
		firstEntry = tmp;

	lastEntry = tmp;
	nEntry++;

	de_create = 0;
	AddList (tmp->dlg->GetHwnd());

	return tmp;
#else
	return NULL;
#endif
}

HWND DialogManager::AddEntry (HINSTANCE hInst, int id, HWND hParent, DLGPROC pDlg, DWORD flag, void *context)
{
#ifdef _WIN32
	return AddEntry (new DialogWin (hInst, hParent, id, pDlg, flag, context));
#else
	return NULL;
#endif
}

HWND DialogManager::AddEntry (DialogWin *dlg)
{
#ifdef _WIN32
	DIALOGENTRY *tmp = new DIALOGENTRY; TRACENEW
	de_create = tmp;
	tmp->dlg = dlg;
	tmp->prev = lastEntry;
	tmp->next = NULL;
	if (lastEntry)
		lastEntry->next = tmp;
	else
		firstEntry = tmp;
	lastEntry = tmp;
	nEntry++;
	de_create = 0;
	HWND hWnd = tmp->dlg->OpenWindow(); // is this the best place to create the window?
	AddList (hWnd);
	return hWnd;
#else
	return NULL;
#endif
}

void DialogManager::AddEntryAsync (HINSTANCE hInst, int id, HWND hParent, DLGPROC pDlg, DWORD flag, void *context)
{
#ifdef _WIN32
#ifdef USEDLGTHREAD
	g_tdata.hInst = hInst;
	g_tdata.hParent = hParent;
	g_tdata.id = id;
	g_tdata.flag = flag;
	g_tdata.procDlg = pDlg;
	g_tdata.context = context;
	PostThreadMessage (thid, TM_OPENDIALOG, (WPARAM)&g_tdata, 0);
#else
	AddEntry (hInst, id, hParent, pDlg, flag, context);
#endif
#endif
}

bool DialogManager::DelEntry (HWND hDlg, HINSTANCE hInst, int id)
{
#ifdef _WIN32
	DIALOGENTRY *tmp;
	for (tmp = firstEntry; tmp; tmp = tmp->next) {
		DialogWin *dlg = tmp->dlg;
		if (hDlg && hDlg != dlg->GetHwnd()) continue;
		if (hInst && hInst != dlg->GetHinst()) continue;
		if (id && id != dlg->GetResId()) continue;
		delete dlg;
		DelList (hDlg);
		if (tmp == firstEntry) firstEntry = tmp->next;
		if (tmp == lastEntry)  lastEntry  = tmp->prev;
		if (tmp->prev) tmp->prev->next = tmp->next;
		if (tmp->next) tmp->next->prev = tmp->prev;
		delete tmp;
		nEntry--;
		return true;
	}
	return false;
#else
	return false;
#endif
}

void DialogManager::AddList (HWND hWnd)
{
#ifdef _WIN32
	if (nList == nListBuf) { // grow buffer
		HWND *tmp = new HWND[nListBuf += 16];
		if (nList) {
			memcpy (tmp, DlgList, nList*sizeof(HWND));
			delete []DlgList;
		}
		DlgList = tmp;
	}
	DlgList[nList++] = hWnd;
#endif
}

void DialogManager::DelList (HWND hWnd)
{
#ifdef _WIN32
	DWORD i;
	for (i = 0; i < nList; i++) {
		if (DlgList[i] == hWnd) break;
	}
	if (i < nList) {
		for (; i < nList-1; i++)
			DlgList[i] = DlgList[i+1];
		DlgList[i] = NULL;
		nList--;
	}
#endif
}

HWND DialogManager::GetNextEntry (HWND hWnd) const
{
#ifdef _WIN32
	if (!hWnd) {
		if (firstEntry) {
			searchEntry = firstEntry;
			return firstEntry->dlg->GetHwnd();
		} else {
			searchEntry = NULL;
			return NULL;
		}
	} else {
		if (!searchEntry || searchEntry->dlg->GetHwnd() != hWnd) {
			for (searchEntry = firstEntry; searchEntry && searchEntry->dlg->GetHwnd() != hWnd; searchEntry = searchEntry->next);
		}
		if (searchEntry) searchEntry = searchEntry->next;
		return (searchEntry ? searchEntry->dlg->GetHwnd() : NULL);
	}
#else
	return NULL;
#endif
}

HWND DialogManager::IsEntry (HINSTANCE hInst, int id)
{
#ifdef _WIN32
	DIALOGENTRY *tmp = firstEntry;
	while (tmp) {
		DialogWin *dlg = tmp->dlg;
		if (dlg->GetHinst() == hInst && dlg->GetResId() == id)
			return tmp->dlg->GetHwnd();
		tmp = tmp->next;
	}
	return 0;
#else
	return NULL;
#endif
}

bool DialogManager::AddTitleButton (DWORD msg, HBITMAP hBmp, DWORD flag)
{
#ifdef _WIN32
	return DialogWin::Create_AddTitleButton (msg, hBmp, flag);
#else
	return false;
#endif
}

DWORD DialogManager::GetTitleButtonState (HWND hDlg, DWORD msg)
{
#ifdef _WIN32
	DIALOGENTRY *tmp;
	for (tmp = firstEntry; tmp; tmp = tmp->next)
		if (hDlg == tmp->dlg->GetHwnd())
			return tmp->dlg->GetTitleButtonState (msg);
	return 0;
#else
	return 0;
#endif
}

bool DialogManager::SetTitleButtonState (HWND hDlg, DWORD msg, DWORD state)
{
#ifdef _WIN32
	state = (state ? 1:0);
	if (DialogWin::Create_SetTitleButtonState (msg, state)) return true;

	DIALOGENTRY *de;
	for (de = firstEntry; de; de = de->next)
		if (hDlg == de->dlg->GetHwnd())
			return de->dlg->SetTitleButtonState (msg, state);
	return false;
#else
	return false;
#endif
}

void DialogManager::UpdateDialogs ()
{
#ifdef _WIN32
	for (DIALOGENTRY *tmp = firstEntry; tmp; tmp = tmp->next) {
		if (tmp->dlg->UpdateContinuously())
			tmp->dlg->Update();
	}
#endif
}

void DialogManager::BroadcastMessage (DWORD msg, void *data)
{
#ifdef _WIN32
	for (DIALOGENTRY *tmp = firstEntry; tmp; tmp = tmp->next)
		tmp->dlg->Message (msg, data);
#endif
}

//-----------------------------------------------------------------------------
// Name: OrbiterDefDialogProc()
// Desc: Default message handler for orbiter dialog boxes
//-----------------------------------------------------------------------------
INT_PTR OrbiterDefDialogProc (HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
#ifdef _WIN32
	switch (uMsg) {
	case WM_SETCURSOR:
		// implements "focus follows mouse" behaviour
		if (!g_pOrbiter->StickyFocus() && g_pOrbiter->Cfg()->CfgUIPrm.MouseFocusMode == 2 && GetFocus() != hDlg &&
			!IsChild (hDlg, GetFocus()) && GetParent (hDlg) == g_pOrbiter->GetRenderWnd()) {
				SetFocus (hDlg);
				return FALSE;
		}
		break;
	case WM_NCLBUTTONDBLCLK: {
		// implements window minimisation on title bar double-click
		DialogWin *dlg = (DialogWin*)GetWindowLongPtr (hDlg, DWLP_USER);
		if (dlg) dlg->ToggleShrink();
		} break;

	// *** Create a timer to force frame updates during dialog box moves ***
	case WM_ENTERSIZEMOVE:
		SetTimer (hDlg, 0xff, 1, NULL);
		return 0;
	case WM_EXITSIZEMOVE:
		KillTimer (hDlg, 0xff);
		return 0;
	case WM_TIMER:
		if (wParam == 0xff) g_pOrbiter->SingleFrame();
		return 0;

	// *** Provide custom buttons in the window title bar ***
//	case WM_SETTEXT:
//	case WM_ACTIVATE:
//	case WM_NCPAINT:
//		PostMessage (hDlg, WM_USER+2, 0, 0);
//		break;
//	case WM_USER+2: {
//		DialogWin *dlg = (DialogWin*)GetWindowLongPtr (hDlg, DWLP_USER);
//		if (dlg) dlg->PaintTitleButtons();
//		} return 0;
//	case WM_NCLBUTTONDOWN:
//		if (wParam == HTCAPTION) {
//			DialogWin *dlg = (DialogWin*)GetWindowLongPtr (hDlg, DWLP_USER);
//			if (dlg) dlg->CheckTitleButtons (MAKEPOINTS (lParam));
//		}
//		return 0;
	//case WM_CTLCOLORDLG: {
	//	HDC hDC = (HDC)wParam;
	//	SetBkColor (hDC, 0x808080);
	//	} return (INT_PTR)GetStockObject (GRAY_BRUSH);
	}
	return FALSE;
#else
	return FALSE;
#endif
}

// ====================================================================
// Tread management for dialog thread
// ====================================================================

void DialogManager::StartDialogThread ()
{
#ifdef _WIN32
	DWORD WINAPI DlgThreadProc (void *data);
	hThread = CreateThread (NULL, 2048, DlgThreadProc, this, 0, &thid);
	hCreateDlg = CreateEvent (NULL, FALSE, FALSE, NULL);
#endif
}

void DialogManager::DestroyDialogThread ()
{
#ifdef _WIN32
	PostThreadMessage (thid, WM_QUIT, 0, 0);
	CloseHandle (hThread);
	CloseHandle (hCreateDlg);
#endif
}

#ifdef _WIN32
DWORD WINAPI DlgThreadProc (void *data)
{
	DialogManager *dlgmgr = (DialogManager*)data;
	MSG msg;
	void DoDialog (THREADDATA *tdata);

	while (GetMessage (&msg, NULL, 0, 0)) {
		switch (msg.message) {
		case TM_OPENDIALOG: {
			THREADDATA *tdata = (THREADDATA*)msg.wParam;
			dlgmgr->AddEntry (tdata->hInst, tdata->id, tdata->hParent, tdata->procDlg, tdata->flag, tdata->context);
			} break;
		default:
			TranslateMessage (&msg);
			DispatchMessage (&msg);
			break;
		}
	}

	return 0;
}
#endif // _WIN32

// ====================================================================
// End tread management
// ====================================================================
// 
// ====================================================================
// ImGui
// ====================================================================

DLLEXPORT ImGuiContext* GImGui = NULL;
DLLEXPORT ImPlotContext* GImPlot = NULL;
void DialogManager::InitImGui()
{
	if(!gc) return;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImPlot::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	// Viewports don't play nice when in full screen mode
	if(!pOrbiter->IsFullscreen())
		io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	//io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	// the look (UiTheme.h): palette, sizes, fonts
	ui::SetUserScale (pOrbiter->Cfg()->CfgUIPrm.UIScale);
	ui::Apply ();

	const CFG_FONTPRM &prm = g_pOrbiter->Cfg()->CfgFontPrm;

	const ui::Fonts &uf = ui::LoadFonts (prm.ImGui_DefaultFontFile, prm.ImGui_MonospacedFontFile, ui::SIZE_BODY);
	defaultFont = uf.body;
	monoFont = uf.mono;
	consoleFont = io.Fonts->AddFontFromFileTTF(prm.ImGui_ConsoleFontFile, ui::SIZE_BODY);
	if (!consoleFont) consoleFont = monoFont;
	manuscriptFont = io.Fonts->AddFontFromFileTTF(prm.ImGui_ManuscriptFontFile, ui::SIZE_BODY);
	if (!manuscriptFont) manuscriptFont = defaultFont;

#ifdef _WIN32
	ImGui_ImplWin32_Init(hWnd);
#endif
	gc->clbkImGuiInit();
}

void DialogManager::ShutdownImGui()
{
	if(!gc) return;

	gc->clbkImGuiShutdown();
#ifdef _WIN32
	ImGui_ImplWin32_Shutdown();
#endif
	ImPlot::DestroyContext();
	ImGui::DestroyContext();
}

void DialogManager::ImGuiNewFrame()
{
	if(!gc) return;

	gc->clbkImGuiNewFrame();
#ifdef _WIN32
	ImGui_ImplWin32_NewFrame();
#endif
	ui::SetUserScale (pOrbiter->Cfg()->CfgUIPrm.UIScale);
	ui::UpdateScale (ImGui::GetIO().DisplaySize.y);
	ImGui::NewFrame();
	uianchor::NewFrame();

	// Focus-follows-mouse: when the mouse is not over any ImGui window,
	// clear ImGui's internal focused window. Without this, clicking an
	// ImGui window permanently sets WantCaptureKeyboard (due to
	// NavEnableKeyboard), blocking all DirectInput keyboard processing
	// even after the mouse leaves the ImGui window.
	if (g_pOrbiter->Cfg()->CfgUIPrm.MouseFocusMode != 0) {
		ImGuiIO& io = ImGui::GetIO();
		if (!io.WantCaptureMouse) {
			ImGui::SetWindowFocus(static_cast<const char*>(nullptr));
		}
	}

	if (!exclusive) RenderNotifications();

	// We can't use a range-based loop here because Display() may unregister the current dialog
	for (auto it = DlgImGuiList.begin(); it != DlgImGuiList.end();)
	{
		auto current = it++;
		if (exclusive && *current != exclusive) continue;
		if ((*current)->IsActive()) {
			(*current)->Display();
		}
	}

	ImGui::EndFrame();
}

ImFont *DialogManager::GetFont(ImGuiFont f)
{
	switch(f) {
	case ImGuiFont::MONO: return monoFont;
	case ImGuiFont::CONSOLE: return consoleFont;
	case ImGuiFont::DEFAULT: return defaultFont;
	case ImGuiFont::MANUSCRIPT: return manuscriptFont;
	default: return defaultFont;
	}
}

void DialogManager::SetMainColor(COLORREF col)
{
	// (the dialogs no longer take the HUD's colour: UiTheme.h)
}

ImGuiDialog::~ImGuiDialog(){
	// Make sure this dialog is no longer referenced in the DialogManager
	oapiCloseDialog(this);
}

bool ImGuiDialog::HandleHelpButton() {
	if(!helpfile.empty()) {
		HELPCONTEXT hc;
		hc.helpfile = const_cast<char *>(helpfile.c_str());
		hc.topic = helptopic.empty() ? NULL : const_cast<char *>(helptopic.c_str());
		hc.toc = (char*)"html/orbiter.chm::/orbiter.hhc";
		hc.index = (char*)"html/orbiter.chm::/orbiter.hhk";

		if(ImGui::MenuButton(ICON_FA_CIRCLE_QUESTION, "Help")) {
			g_pOrbiter->OpenHelp(&hc);
		}
		return true;
	}
	return false;
}

namespace {
	// Where a dialog opens: its remembered place, unless that is off the
	// screen, ImGui's default or on another window; then the first free
	// place down the right edge, then the left, below the info bars, clear
	// of the other windows and the middle of the view (the vessel, the HUD)
	ImVec2 DialogPlace (const char *name, ImVec2 size, bool &place)
	{
		ImGuiContext &g = *GImGui;
		const ImGuiViewport *vp = ImGui::GetMainViewport ();
		ImVec2 V0 = vp->Pos, V1 (vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
		float m = ui::Px (16);
		auto overlaps = [&](ImVec2 p, ImVec2 s) {
			ImRect r (p, ImVec2(p.x + s.x, p.y + s.y));
			r.Expand (ui::Px (6));
			for (ImGuiWindow *w : g.Windows) {
				if (!w->WasActive || w->Hidden || (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_Popup))) continue;
				if (strcmp (w->Name, name) == 0) continue;
				if (r.Overlaps (w->Rect ())) return true;
			}
			return false;
		};
		auto inside = [&](ImVec2 p, ImVec2 s) {
			return p.x >= V0.x && p.y >= V0.y && p.x + s.x <= V1.x && p.y + s.y <= V1.y;
		};
		ImRect middle (ImVec2(V0.x + vp->Size.x * 0.32f, V0.y + vp->Size.y * 0.22f), ImVec2(V0.x + vp->Size.x * 0.68f, V0.y + vp->Size.y * 0.78f));

		// the remembered place
		ImVec2 pos;
		bool have = false;
		if (ImGuiWindow *w = ImGui::FindWindowByName (name)) pos = w->Pos, size = w->SizeFull, have = true;
		else if (ImGuiWindowSettings *s = ImGui::FindWindowSettingsByID (ImHashStr (name))) {
			pos = ImVec2((float)s->Pos.x, (float)s->Pos.y);
			if (s->Size.x > 0 && s->Size.y > 0) size = ImVec2((float)s->Size.x, (float)s->Size.y);
			have = true;
		}
		place = !have || !inside (pos, size) || (pos.x == 60.0f && pos.y == 60.0f) || overlaps (pos, size);
		if (!place) return pos;

		float top = V0.y + ui::Px (84), step = ui::Px (12);
		for (int side = 0; side < 2; side++) {
			float x = side == 0 ? V1.x - m - size.x : V0.x + m;
			for (float y = top; y + size.y <= V1.y - m; y += step) {
				ImVec2 p (x, y);
				if (overlaps (p, size)) continue;
				if (ImRect(p, ImVec2(p.x + size.x, p.y + size.y)).Overlaps (middle)) continue;
				return p;
			}
		}
		// (the edges taken: anywhere free, the middle too, from the top)
		for (float y = top; y + size.y <= V1.y - m; y += step * 2)
			for (float x = V1.x - m - size.x; x >= V0.x + m; x -= step * 2) {
				ImVec2 p (x, y);
				if (!overlaps (p, size)) return p;
			}
		// (no free place: cascade from the top right)
		static int n = 0;
		n = (n + 1) % 8;
		return ImVec2(std::max (V0.x + m, V1.x - m - size.x - n * ui::Px (28)), top + n * ui::Px (28));
	}
}

void PlaceDialog (const char *name, float width, float height)
{
	ImVec2 size (width * ui::Scale (), height * ui::Scale ());
	ImGuiWindow *prev = ImGui::FindWindowByName (name);
	if (!prev || !prev->WasActive) { // (opening)
		bool place;
		ImVec2 pos = DialogPlace (name, size, place);
		if (place) ImGui::SetNextWindowPos (pos, ImGuiCond_Always);
	}
	ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
}

void ImGuiDialog::Display() {
	PlaceDialog (name.c_str(), defaultSize.width, defaultSize.height);

	// (a taller title bar, the title clear of the window's edge)
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::Px(14), ui::Px(9)));
	bool open = ImGui::Begin(name.c_str(), &active);
	ImGui::PopStyleVar();
	if(open) {
		HandleHelpButton();
		OnDraw();
		// Esc closes the dialog that has the focus (not while typing)
		if (ImGui::IsWindowFocused (ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
			ImGui::IsKeyPressed (ImGuiKey_Escape, false))
			active = false;
		ui::ScrollFade();
	}
	ImGui::End();
	if (!active) OnClose();
}

void ImGuiDialog::Activate() {
	active = true;
	ImGui::SetWindowFocus(name.c_str());
}

/* 
Notification handling, borrowed heavily from https://github.com/patrickcjk/imgui-notify
Added:
- permanent discardable notifications
- copy text to clipboard (permanent notifications)
- deduplication
- fall animation
- limit notification box to the main viewport

MIT License

Copyright (c) 2021-2026 Patrick

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

using namespace std::chrono_literals;
using duration_t = std::chrono::duration<double>;
using time_point_t = std::chrono::time_point<std::chrono::steady_clock, duration_t>;

const float NOTIFY_PADDING_X = 20.f;            // Bottom-left X padding
const float NOTIFY_PADDING_Y = 20.f;            // Bottom-left Y padding
const float NOTIFY_PADDING_MESSAGE_Y = 10.f;    // Padding Y between each message
const uint32_t NOTIFICATION_FLAGS = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing;
const duration_t FADE_TIME = 0.5s;
static const ImVec4 notifcolors[4] = {
	{ 0, 255, 0, 255 },     // Success
	{ 255, 255, 0, 255 },   // Warning
	{ 255, 0, 0, 255 },     // Error
	{ 0, 157, 255, 255 }    // Info
};

static const char *notificons[4] = {
	ICON_LC_CIRCLE_CHECK,    // Success
	ICON_LC_TRIANGLE_ALERT,  // Warning
	ICON_LC_CIRCLE_X,        // Error
	ICON_LC_INFO             // Info
};

// the severity's colour (UiTheme.h)
static const ui::Colour notifcolour[4] = { ui::SUCCESS, ui::CAUTION, ui::WARNING, ui::ACCENT };

static duration_t notifduration[4] = {
	3.0s,  // Success
	10.0s, // Warning
	86400.0s, // Error - basically permanent and needs to be acknowledged
	7.0s   // Info
};


struct Notification
{
	uint32_t occurrences;
	std::string title;
	std::string content;
	size_t hash;
	ImVec4 color;
	bool expired = false;
	bool permanent = false;
	const char *icon;
	duration_t duration;
	time_point_t creation;
	float height;
	float speed;
	uint32_t type;

	Notification(uint32_t type_, const char *title_, const char *content_, size_t hash_):title(title_),content(content_),hash(hash_),type(type_)
	{
		if(type >= OAPINOTIF_INFO) type = OAPINOTIF_INFO;
		color = notifcolors[type];
		icon = notificons[type];
		duration = notifduration[type];
		creation = std::chrono::steady_clock::now();
		occurrences = 1;
		expired = false;
		permanent = type == OAPINOTIF_ERROR;
		speed = 0.0f;
		height = -1.0;
	}
	void ReTrigger() {
		occurrences++;
		creation = std::chrono::steady_clock::now() - FADE_TIME;
		duration = notifduration[type];
	}
	void UpdateState(time_point_t now, float h, duration_t dt) {
		duration_t elapsed = now - creation;
		// Update alpha/expiration base on elapsed time
		if(elapsed > FADE_TIME + duration + FADE_TIME) { // expired
			color.w = 0.0f;
			expired = true;
		} else if(elapsed < FADE_TIME) { // appearing
			color.w = elapsed / FADE_TIME;
		} else if(elapsed > FADE_TIME + duration) { // disappearing
			color.w = 1.0f - (elapsed - FADE_TIME - duration) / FADE_TIME;
		} else { // steady
			color.w = 1.0; 
		}

		if(h < height) {
			height -= speed;
			speed += std::chrono::duration_cast<std::chrono::milliseconds>(dt).count()/100.0f;
			if(height <= h) {
				height = h;
				speed = 0.0f;
			}
		} else {
			height = h;
			speed = 0.0f;
		}
	}
};

static std::vector<Notification> notifications;

static void RenderNotifications()
{
	ImVec2 vp_size = ImGui::GetMainViewport()->Size + ImGui::GetMainViewport()->Pos;
	float height = 0.0f;

	int i = 0;
	static time_point_t last = std::chrono::steady_clock::now();
	auto now = std::chrono::steady_clock::now();
	duration_t dt = now - last;
	last = now;
	for (auto &notif: notifications)
	{
		notif.UpdateState(now, height, dt);

		// a card at the bottom right: a strip and an icon in the severity's
		// colour, the title, the message dimmed below it
		float m = ui::Px(16);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, notif.color.w);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::Px(10));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Px(16), ui::Px(12)));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Px(8), ui::Px(3)));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::PANEL, 0.98f));
		ImGui::SetNextWindowPos(ImVec2(vp_size.x - m, vp_size.y - m - notif.height), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Px(300), 0), ImVec2(ui::Px(420), FLT_MAX));

		// Generate new unique name for this notification
		char window_name[32];
		sprintf(window_name, "##NOTIF%d", i);
		i++;
		ImGuiWindowFlags wf = NOTIFICATION_FLAGS;
		if(notif.permanent) {
			wf &= ~ImGuiWindowFlags_NoInputs;
		}

		// Prevent the notification from spawning outside the main window
		ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
		ImGui::Begin(window_name, NULL, wf);
		ui::Colour sev = notifcolour[notif.type < 4 ? notif.type : OAPINOTIF_INFO];
		{
			ImGuiWindow *w = ImGui::GetCurrentWindow();
			float r = ui::Px(10);
			w->DrawList->AddRectFilled(w->Pos, ImVec2(w->Pos.x + ui::Px(4), w->Pos.y + w->Size.y), ui::Col32(sev), r, ImDrawFlags_RoundCornersLeft);
		}

		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui::Px(360));

		ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(sev));
		ImGui::TextUnformatted(notif.icon);
		ImGui::PopStyleColor();

		ImGui::SameLine(0, ui::Px(10));
		ui::PushFont(ui::GetFonts().medium);
		if(notif.occurrences > 1) {
			ImGui::Text("%s (\xC3\x97%d)", notif.title.c_str(), notif.occurrences);
		} else {
			ImGui::TextUnformatted(notif.title.c_str());
		}
		ui::PopFont();

		if(notif.permanent)
		{
			ImGui::SameLine(0, ui::Px(12));
			if (ui::IconButton(ICON_LC_X, "Dismiss"))
			{
				// Change duration so that the notification will expire right now;
				notif.duration = now - notif.creation - FADE_TIME;
			}
		}

		if (!notif.content.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::TEXT_DIM));
			ImGui::TextUnformatted(notif.content.c_str()); // Render content text
			ImGui::PopStyleColor();

			// Allow copying error messages to the clipboard
			if(notif.permanent) {
				char popup_name[32];
				sprintf(popup_name, "##POPUP%d", i);

				if (ImGui::BeginPopupContextItem(popup_name))
				{
					if (ImGui::MenuItem("Copy")) {
						ImGui::SetClipboardText(notif.content.c_str());
					}
					ImGui::EndPopup();
				}
			}
		}

		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(4);
		// Save height for next notification
		height += ImGui::GetWindowHeight() + ui::Px(NOTIFY_PADDING_MESSAGE_Y);

		// End
		ImGui::End();
	}
    // Remove expired notifications
    notifications.erase(std::remove_if(notifications.begin(), notifications.end(),
										[=](auto &notif){return notif.expired;}),
						notifications.end());
}


// OAPI implementation
DLLEXPORT void oapiAddNotification(int type, const char *title, const char *content) {
	if(content == NULL) {
		content = "(null)";
	}
	const size_t hash = std::hash<std::string>{}(title) ^ std::hash<std::string>{}(content);
	for (auto &notif: notifications) {
		if(notif.hash == hash && notif.title == title && notif.content == content && notif.type == type) {
			notif.ReTrigger();
			return;
		}
	}
	notifications.emplace_back(type, title, content, hash);
}


// ImGui utils
namespace ImGui {
	// Resettable slider from https://github.com/ocornut/imgui/issues/1751
	DLLEXPORT bool SliderFloatReset(const char* label, float* v, float v_min, float v_max, float v_default, const char* display_format)
	{
		bool ret = ImGui::SliderFloat(label, v, v_min, v_max, display_format);
		if (ImGui::BeginPopupContextItem(label))
		{
			char buf[64];
			sprintf(buf, "Reset to %f", v_default);
			if (ImGui::MenuItem(buf))
				*v = v_default;
			ImGui::MenuItem("Close");
			ImGui::EndPopup();
		}
		return ret;
	}

	// Cheap double slider
	// It's provided only to be able to use sliders where a double is used but a float would have been enough
	// e.g. the stars brightness configuration
	DLLEXPORT bool SliderDouble(const char* label, double* v, double v_min, double v_max, const char* display_format)
	{
		float tmp = *v;
		bool ret = ImGui::SliderFloat(label, &tmp, v_min, v_max, display_format);
		if(ret) *v = tmp;
		return ret;
	}

	DLLEXPORT bool InputIntEx(const char* label, int* v, int v_min, int v_max, int step, int step_fast, ImGuiInputTextFlags flags)
	{
		bool ret = ImGui::InputInt(label, v, step, step_fast, flags);
		if(ret) {
			if(*v < v_min) *v = v_min;
			else if (*v > v_max) *v = v_max;
		}
		return ret;
	}
	DLLEXPORT bool InputDoubleEx(const char* label, double* v, double v_min, double v_max, double step, double step_fast, const char *fmt, ImGuiInputTextFlags flags)
	{
		bool ret = ImGui::InputDouble(label, v, step, step_fast, fmt, flags);
		if(ret) {
			if(*v < v_min) *v = v_min;
			else if (*v > v_max) *v = v_max;
		}
		return ret;
	}


	DLLEXPORT void PushFont(ImGuiFont f, float scale)
	{
		ImGuiContext& g = *GImGui;
		const ImGuiStyle& style = g.Style;
		ImGui::PushFont(g_pOrbiter->DlgMgr()->GetFont(f), style.FontSizeBase * scale);
	}

	// From imgui_demo.cpp, with added sameline argument
	DLLEXPORT void HelpMarker(const char* desc, bool sameline)
	{
		if(sameline)
			ImGui::SameLine();
		ImGui::TextDisabled(ICON_FA_CIRCLE_QUESTION);
		if (ImGui::BeginItemTooltip())
		{
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::TextUnformatted(desc);
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	DLLEXPORT bool MenuButton(const char *label, const char *tooltip, float xoffset)
	{
		ImGuiContext& g = *GImGui;
		const ImGuiWindow* window = ImGui::GetCurrentWindow();
		const ImRect titleBarRect = window->TitleBarRect();
		const ImGuiStyle& style = g.Style;
        ImGuiLastItemData last_item_backup = g.LastItemData;
		ImGui::PushClipRect( titleBarRect.Min, titleBarRect.Max, false );
		float width = (strlen(label)+1) * g.FontSize;
		float offset = titleBarRect.Max.x - width - titleBarRect.Min.x - g.Style.FramePadding.x;
		offset -= xoffset;
		// (centred in the title bar, whatever its height)
		float y = std::max(0.0f, (titleBarRect.GetHeight() - (g.FontSize + 2.0f * style.FramePadding.y)) * 0.5f);
		ImGui::SetCursorPos( ImVec2( offset, y ) );
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);   // no border
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0,0,0,0));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0,0,0,0));    // invisible border color		bool ret = ImGui::Button( label );
		bool ret = ImGui::Button( label );
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
		if(tooltip && ImGui::IsItemHovered())
			ImGui::SetTooltip(tooltip);
		ImGui::PopClipRect();
        g.LastItemData = last_item_backup;
		return ret;
	}

	DLLEXPORT ImTextureID GetImTextureID (SURFHANDLE surf)
	{
		oapi::GraphicsClient *gc = g_pOrbiter->GetGraphicsClient();
		if (gc && surf)
			return gc->clbkImGuiSurfaceTexture (surf);
		return 0;
	}

	DLLEXPORT void Image(SURFHANDLE surf, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1, const ImVec4& tint_col, const ImVec4& border_col)
	{
		ImTextureID tex_id = GetImTextureID(surf);
		Image(tex_id, image_size, uv0, uv1, tint_col, border_col);
	}

	DLLEXPORT bool ImageButton(const char* str_id, SURFHANDLE surf, const ImVec2& image_size, const ImVec2& uv0, const ImVec2& uv1, const ImVec4& bg_col, const ImVec4& tint_col)
	{
		ImTextureID tex_id = GetImTextureID(surf);
		return ImageButton(str_id, tex_id, image_size, uv0, uv1, bg_col, tint_col);
	}

	DLLEXPORT bool SliderEnum(const char *label, int *v, const char *values[], int nvalues)
	{
        const char* elem_name = (*v >= 0 && *v < nvalues) ? values[*v] : "Unknown";
        return ImGui::SliderInt(label, v, 0, nvalues - 1, elem_name);
	}


	// "Animated" widgets for Combos and Popups

	struct FadingState
	{
		enum State { Closed, Opening, Open } state = Closed;
		float animstate = 0.0f;
	};
	static std::unordered_map<ImGuiID, FadingState> g_popupStates;

	// A popup that will fade-in when open (making it fade-out is too tricky)
	DLLEXPORT bool BeginAnimatedPopup(const char* name, float duration)
	{
		ImGuiContext& g = *GImGui;
		ImGuiID id = g.CurrentWindow->GetID(name);
		FadingState& S = g_popupStates[id];

		if (ImGui::IsPopupOpen(name) && S.state == FadingState::Closed)	{
			S.state = FadingState::Opening;
			S.animstate = 0.0f;
		}

		if (S.state == FadingState::Opening) {
			S.animstate += ImGui::GetIO().DeltaTime;
			if (S.animstate >= duration)
				S.state = FadingState::Open;
		}

		float x = ImClamp(S.animstate / duration, 0.0f, 1.0f);
		float alpha = x * x * (3.0f - 2.0f * x);

		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

		if (!ImGui::BeginPopup(name)) {
			ImGui::PopStyleVar();
			S.state = FadingState::Closed;
			return false;
		}

		return true;
	}

	DLLEXPORT void EndAnimatedPopup()
	{
		ImGui::PopStyleVar();
		ImGui::EndPopup();
	}

	DLLEXPORT bool BeginAnimatedCombo(const char* label, const char* preview_value, ImGuiComboFlags flags, float duration)
	{
		ImGuiContext& g = *GImGui;
		ImGuiID id = g.CurrentWindow->GetID(label);
		FadingState& S = g_popupStates[id];

		float alpha = 1.0f;
		if (S.state == FadingState::Opening) {
			S.animstate += ImGui::GetIO().DeltaTime;
			if (S.animstate >= duration) {
				S.state = FadingState::Open;
			} else {
				float x = ImClamp(S.animstate / duration, 0.0f, 1.0f);
				alpha = x * x * (3.0f - 2.0f * x);
			}
		}

		ImVec4 bc = ImGui::GetStyleColorVec4(ImGuiCol_Border);
		ImVec4 bs = ImGui::GetStyleColorVec4(ImGuiCol_BorderShadow);
		bc.w *= alpha;
		bs.w *= alpha;
		ImGui::PushStyleColor(ImGuiCol_Border, bc);
		ImGui::PushStyleColor(ImGuiCol_BorderShadow, bs);
		ImGui::SetNextWindowBgAlpha(alpha);

		if (ImGui::BeginCombo(label, preview_value, flags)) {
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);

			if(S.state == FadingState::Closed) {
				S.state = FadingState::Opening;
				S.animstate = 0.0f;
			}
		} else {
			S.state = FadingState::Closed;
			ImGui::PopStyleColor(2);
			return false;
		}

		return true;
	}

    DLLEXPORT void EndAnimatedCombo()
	{
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
		ImGui::EndCombo();
	}

	struct AnimatedHeaderState
	{
		enum State { Closed, Opening, Closing, Open } state = Closed;
		float speed;
		float height = 0.0f;
		ImVec2 startPos;
		std::string childname;
	};
	static std::unordered_map<ImGuiID, AnimatedHeaderState> g_headerStates;
	static std::vector<ImGuiID> g_stackHeaders;
	DLLEXPORT bool BeginAnimatedCollapsingHeader(const char* label, ImGuiTreeNodeFlags flags, float speed)
	{
		ImGuiWindow* window = GetCurrentWindow();
		if (window->SkipItems)
			return false;

		ImGuiID id = ImGui::GetID(label);
		auto found = g_headerStates.find(id);
		auto& state = g_headerStates[id];
		state.speed = speed;

		// First time with see this widget,
		if(found == g_headerStates.end()) {
			state.childname = std::string(label) + "_child";
			// Check to see if it should be open
			if(flags & ImGuiTreeNodeFlags_DefaultOpen)
				state.state = AnimatedHeaderState::Open;
		}

		// (a neutral bar, the label in the medium weight: the accent marks selections)
		ImGui::PushStyleColor(ImGuiCol_Header, ui::Col(ui::RAISED, 0.55f));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ui::Col(ui::RAISED_HOVER, 0.8f));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ui::Col(ui::RAISED_ACTIVE));
		ui::PushFont(ui::GetFonts().medium, ImGui::GetStyle().FontSizeBase);
		bool visible = ImGui::CollapsingHeader(label, flags);
		ui::PopFont();
		ImGui::PopStyleColor(3);

		if(visible) {
			switch(state.state) {
				case AnimatedHeaderState::Closed:
					state.state = AnimatedHeaderState::Opening;
					break;
				case AnimatedHeaderState::Closing:
					state.state = AnimatedHeaderState::Opening;
					break;
				case AnimatedHeaderState::Opening:
				case AnimatedHeaderState::Open:
					break;
			}
		} else {
			switch(state.state) {
				case AnimatedHeaderState::Open:
					state.state = AnimatedHeaderState::Closing;
					break;
				case AnimatedHeaderState::Opening:
					state.state = AnimatedHeaderState::Closing;
					break;
				case AnimatedHeaderState::Closing:
				case AnimatedHeaderState::Closed:
					break;
			}
		}

		// If it's closed, EndAnimatedCollapsingHeader won't be called
		// so we need to exit before pushing the id to the stack
		if(state.state == AnimatedHeaderState::Closed) return false;
		g_stackHeaders.push_back(id);

		// If the block is fully deployed, we use a BeginGroup/EndGroup section
		// To compute the height of the whole block in EndAnimatedCollapsingHeader
		if(state.state == AnimatedHeaderState::Open) {
			state.startPos = ImGui::GetCursorScreenPos();
			ImGui::BeginGroup();
			return true;
		}

		// Use a child during the animation, we clamp its height to create
		// a sliding effect
		ImGui::BeginChild(state.childname.c_str(),
							ImVec2(-FLT_MIN, state.height), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoDecoration);
			
		return true;
	}

	DLLEXPORT void EndAnimatedCollapsingHeader()
	{
		auto id = g_stackHeaders.back();
		g_stackHeaders.pop_back();

		auto& state = g_headerStates[id];
		// Compute the height of the block
		if(state.state == AnimatedHeaderState::Open) {
			ImGui::EndGroup();

			ImVec2 endPos = ImGui::GetItemRectMax();
			state.height = endPos.y - state.startPos.y;
			return;
		}

		ImVec2 available = ImGui::GetContentRegionAvail();

		switch(state.state) {
			case AnimatedHeaderState::Opening:
				// A negative available.y indicates that the content is
				// too tall to fit in the given height -> we increase it
				// Otherwise we can stop the animation
				if(available.y < 0) {
					state.height += ImGui::GetIO().DeltaTime * state.speed;
				} else {
					state.state = AnimatedHeaderState::Open;
					// Prevent too much overshooting
					float ypos = ImGui::GetCursorPosY();
					ypos -= available.y;
					ImGui::SetCursorPosY(ypos);
					ImGui::Dummy(ImVec2(0,0));
				}
				break;
			case AnimatedHeaderState::Closing:
				state.height -= ImGui::GetIO().DeltaTime * state.speed;
				if(state.height <= 0.0) {
					state.height = 0;
					state.state = AnimatedHeaderState::Closed;
				}
				break;
			case AnimatedHeaderState::Open:
			case AnimatedHeaderState::Closed:
				break; // should not be here
		}
		ImGui::EndChild();
	}
}

