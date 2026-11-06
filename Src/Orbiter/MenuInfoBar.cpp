// Copyright (c) Martin Schweiger
// Licensed under the MIT License
#define IMGUI_DEFINE_MATH_OPERATORS
#include <algorithm>
#include <functional>
#include <vector>
#include <cstdarg>
#include <chrono>
#include <string>
#include "OrbiterAPI.h"
#include "imgui.h"
#include "imgui_internal.h"

#include "MenuInfoBar.h"
#include "UiTheme.h"
#include "IconsLucide.h"
#include "Camera.h"
#include "Dialogs.h"
#include "DlgMenuCfg.h"
#include "Log.h"

// =======================================================================
// Externs

extern Orbiter *g_pOrbiter;
extern Camera *g_camera;

namespace {
	float g_menuBottom = 0.0f;  // the menu bar's lower edge this frame (screen; 0: none)
	float g_labelBottom = 0.0f; // the camera mode label's lower edge this frame (0: none)
}
extern TimeData td;

const ImGuiWindowFlags INFOFLAGS = ImGuiWindowFlags_NoTitleBar |
	ImGuiWindowFlags_NoInputs |
	ImGuiWindowFlags_NoNav |
	ImGuiWindowFlags_NoScrollbar |
	ImGuiWindowFlags_AlwaysAutoResize |
	ImGuiWindowFlags_NoCollapse |
	ImGuiWindowFlags_NoMove |
	ImGuiWindowFlags_NoSavedSettings |
	ImGuiWindowFlags_NoFocusOnAppearing |
	ImGuiWindowFlags_NoBringToFrontOnFocus |
	ImGuiWindowFlags_NoDocking;

template<std::size_t N>
struct MovingAverage {
    float buf[N]{};
    std::size_t idx = 0;
    std::size_t count = 0;
    float sum = 0.0;

    float add(float x) {
        sum -= buf[idx];
        buf[idx] = x;
        sum += x;

        idx = (idx + 1) % N;
        if (count < N) ++count;

        return sum / count;
    }
};

// the icons of Orbiter's own menu commands (Orbiter.cpp, RegisterMenuCmd)
static const char *BuiltinGlyph(const char *label)
{
	static const struct { const char *label, *glyph; } g[] = {
		{"Ship", ICON_LC_ROCKET}, {"Camera", ICON_LC_CAMERA}, {"Speed", ICON_LC_FAST_FORWARD},
		{"Pause", ICON_LC_PAUSE}, {"Function", ICON_LC_SQUARE_FUNCTION}, {"Info", ICON_LC_INFO},
		{"Options", ICON_LC_SETTINGS}, {"Map", ICON_LC_MAP}, {"MFD", ICON_LC_MONITOR},
		{"Record", ICON_LC_CIRCLE_DOT}, {"Help", ICON_LC_CIRCLE_HELP}, {"Save", ICON_LC_SAVE},
		{"Exit", ICON_LC_LOG_OUT},
	};
	for (auto &e : g) if (!strcmp(label, e.label)) return e.glyph;
	return nullptr;
}

// a menu button's icon: a glyph centred in bb, a light shade when hovered
static void DrawGlyph(ImDrawList *dl, const ImRect &bb, const char *glyph, bool hovered, bool held, ImVec2 offset = ImVec2(0,0))
{
	ImVec2 a = bb.Min - offset, b = bb.Max - offset;
	if (hovered || held) dl->AddRectFilled(a, b, ui::Col32(ui::TEXT, held ? 0.14f : 0.08f), ui::Px(8));
	float sz = (b.y - a.y) * 0.56f;
	ImGui::PushFont(ui::GetFonts().body, sz / ui::Scale()); // (sizes are given at 1080 lines)
	ImVec2 ts = ImGui::CalcTextSize(glyph);
	dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), ui::Col32(ui::TEXT, hovered ? 1.0f : 0.85f), glyph);
	ImGui::PopFont();
}

class DynamicMenuBar: public ImGuiDialog
{
	static inline const float strength = 0.3f;
	CFG_UIPRM &prm;

	// Until we get it from C++20
	static inline float lerp(float a, float b, float t) {
			return a + t * (b - a);
	}
public:
	struct MenuItem {
		std::string label;
		SURFHANDLE texture;
		CustomFunc func;
		void *context;
		ImTextureID texId;
		const char *glyph;   // the icon of a built-in command (else the texture's)
		int id;
		float lastItemSize;
		bool dragging;
		float animStateScroll;
		float animStateScale;
		bool enabled;
		MenuItem(const char *label_, const char *path, int id_, CustomFunc func_, void *context_):label(label_),func(func_),context(context_),id(id_) {
			glyph = BuiltinGlyph(label_);
			texture = glyph ? NULL : oapiLoadTexture(path);
			if(!texture && !glyph) oapiAddNotification(OAPINOTIF_ERROR, "Cannot find menu item texture", path);
			texId = texture ? ImGui::GetImTextureID(texture) : ImTextureID();
			lastItemSize = g_pOrbiter->Cfg()->CfgUIPrm.MenuButtonSize;
			dragging = false;
			enabled = true;
			animStateScroll = 0.0f;
			animStateScale = 0.0f;
		}
		~MenuItem() {
			if(texture)
				oapiDestroySurface(texture);
		}

		// We don't want to be able to copy because of the SURFHANDLE
		MenuItem(const MenuItem&) = delete;
		MenuItem& operator=(const MenuItem&) = delete;
		MenuItem(MenuItem&& other) noexcept {
			*this = std::move(other);
		}
		MenuItem& operator=(MenuItem&& other) noexcept {
			label = std::move(other.label);
			texture = other.texture;
			other.texture = NULL;
			func = other.func;
			context = other.context;
			texId = other.texId;
			glyph = other.glyph;
			lastItemSize = other.lastItemSize;
			id = other.id;
			dragging = other.dragging;
			enabled = other.enabled;
			animStateScroll = other.animStateScroll;
			animStateScale = other.animStateScale;
			return *this;
		}
	};

	const ImGuiWindowFlags MENUFLAGS = ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoBackground |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_DockNodeHost |
		ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoDocking |
		ImGuiWindowFlags_NoNav;
	void OnDraw() {}
	std::vector<MenuItem> items;
	float animState;
	float animStateSpacing;
	std::vector<MenuInfoBar::MenuPreference> preferedOrder;

	// Dragging
	ImVec2 dragPos = {0,0};
	bool isDragging;
	bool oneDrag;
	bool isDraggingOutside;
	int swapId = -1;
	ImTextureID dragTexId = 0;
	const char *dragGlyph = nullptr;
	int dragId = -1;
	float holePos;


	DynamicMenuBar(): ImGuiDialog("DynamicMenu"), prm(g_pOrbiter->Cfg()->CfgUIPrm) {
		animState = 0.0f;
		isDragging = false;
		oneDrag = false;
		isDraggingOutside = false;
		animStateSpacing = (prm.MenuButtonSpacing * ui::Scale());

		FILEHANDLE f = oapiOpenFile("MenuInfoBar.cfg", FILE_IN, CONFIG);
		if(f) {
			for(int i = 1;;i++) {
				char buf[64];
				char dst[256];
				bool state;
				sprintf(buf, "MenuItem%d", i);
				bool s = oapiReadItem_string (f, buf, dst);
				sprintf(buf, "MenuItemEnabled%d", i);
				s &= oapiReadItem_bool (f, buf, state);
				if(!s)
					break;
				preferedOrder.emplace_back(MenuInfoBar::MenuPreference{dst, state});
			}

			oapiCloseFile(f, FILE_IN);
		}

		items.reserve(g_pOrbiter->menuitems.size());
		for(auto &item: g_pOrbiter->menuitems) {
			AddMenuItem(item.label.c_str(), item.imagepath.c_str(), item.id, item.func, item.context);
		}
		// Save the current state if the file did not exist
		if(!f) {
			SavePreferedOrder();
		}
	}
	// Test if labela appears before labelb in preferedOrder 
	bool Before(const char *labela, const char *labelb) {
		for(const auto &item: preferedOrder) {
			if(item.label == labela)
				return true;
			if(item.label == labelb)
				return false;
		}
		return false;
	}

	void AddMenuItem(const char *label, const char *imagepath, int id, CustomFunc func, void *context = NULL) {
		const auto &known = std::find_if(preferedOrder.begin(), preferedOrder.end(), [label](const auto &item){ return item.label == label;});
		if(known != preferedOrder.end()) {
			// Insert the new item so that it respects the order provided by preferedOrder
			for(auto it = items.begin(); it!=items.end();++it) {
				if(!Before(it->label.c_str(), label)) {
					auto newItem = items.emplace(it, label, imagepath, id, func, context);
					newItem->enabled = known->enabled;
					return;
				}
			}
			// Add to the back if no candidate was found
			auto &newItem = items.emplace_back(label, imagepath, id, func, context);
			newItem.enabled = known->enabled;
		} else {
			// Unknown item, push to the back
			items.emplace_back(label, imagepath, id, func, context);
		}
	}
	void DelMenuItem(int id) {
		items.erase(std::remove_if(items.begin(), items.end(), [id](const auto &item) { return item.id == id; }), items.end());
		SavePreferedOrder();
	}
	void SyncPreferedOrder() {
		// Refresh item state from preferedOrder
		// Used when changing properties from DlgMenuCfg
		for(auto &item: items) {
			const auto &known = std::find_if(preferedOrder.begin(), preferedOrder.end(), [&item](const auto &el){ return el.label == item.label;});
			if(known != preferedOrder.end()) {
				if(item.enabled != known->enabled) {
					if(!known->enabled) {
						item.animStateScale = 1.0f;
					} else {
						item.animStateScale = -1.0f;
						item.enabled = true;
					}
				}
			}
		}
		SavePreferedOrder();
	}
	void SavePreferedOrder() {
		preferedOrder.clear();
		FILEHANDLE f = oapiOpenFile("MenuInfoBar.cfg", FILE_OUT, CONFIG);
		int i = 0;
		for(const auto &item:items) {
			char buf[64];
			i++;
			preferedOrder.emplace_back(MenuInfoBar::MenuPreference{item.label, item.enabled});
			sprintf(buf, "MenuItem%d", i);
			oapiWriteItem_string(f, buf, const_cast<char *>(item.label.c_str()));
			sprintf(buf, "MenuItemEnabled%d", i);
			oapiWriteItem_bool(f, buf, item.enabled);
		}
		oapiCloseFile(f, FILE_OUT);
	}

	void SwapItems() {
		if(swapId != -1) {
			auto swappedItem = std::find_if(items.begin(), items.end(), [this](const MenuItem &a){return a.id == swapId;});
			auto draggedItem = std::find_if(items.begin(), items.end(), [](const MenuItem &a){return a.dragging;});
			// If the user moves the mouse quickly, or if there are disabled items,
			// there can be several buttons between the "hole" and the target.
			// We need to do a swap for each.
			if(draggedItem != items.end() && swappedItem != items.end()) {
				if(draggedItem < swappedItem) {
					for (auto it = draggedItem; it != swappedItem; ++it) {
						(it + 1)->animStateScroll = -1.0;
						std::iter_swap(it, it + 1);
					}
				} else {
					auto rdraggedItem = std::make_reverse_iterator(draggedItem + 1);
					auto rswappedItem = std::make_reverse_iterator(swappedItem + 1);
					for (auto it = rdraggedItem; it != rswappedItem; ++it) {
						(it + 1)->animStateScroll = 1.0;
						std::iter_swap(it, it + 1);
					}
				}
			}
			swapId = -1;
		}
	}
	void Display() {
		BeginMenuInfoBar();
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (!window->SkipItems) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ui::Col(ui::TEXT, 0.14f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::Col(ui::TEXT, 0.08f));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0,0));
			SwapItems();
			// MenuInfoBarItem uses isDragging so we cannot clear it here
			// oneDrag will be set in case one item is being dragged this frame
			oneDrag = false;
			for(auto &item: items) {
				if(item.enabled) {
					if(MenuInfoBarItem(item)) {
						item.func(item.context);
					}
				}
			}
			if(isDragging && !oneDrag) {
				SavePreferedOrder();
			}
			isDragging = oneDrag;
			ImGui::PopStyleVar();
			ImGui::PopStyleColor(3);
		}
		EndMenuInfoBar();
	}

	/// The menu bar's mode: as configured, but a menu bar always shown
	/// hides itself (until the pointer reaches it) over the head-up display
	/// of the 2D cockpit, whose heading tape it would cover
	int MenuMode() const {
		if (prm.MenuMode == 0 && !g_camera->IsExternal() && oapiCockpitMode() != COCKPIT_VIRTUAL && oapiGetHUDMode() != HUD_NONE)
			return 2;
		return prm.MenuMode;
	}

	void BeginMenuInfoBar() {
		g_menuBottom = 0.0f;
		ImGuiViewport *viewport = ImGui::GetMainViewport();
		// Force viewport, we don't want the menu bar to pop out
		// of the main window when we scroll it outside of view
		ImGui::SetNextWindowViewport(viewport->ID);

		// Calculate the menu position
		ImGuiStyle &style = ImGui::GetStyle();
		ImGuiContext &g = *GImGui;
		float yoffset = 0;
			
		switch(MenuMode()) {
		case 0: // show
			break;
		case 1: // hide
			yoffset -= style.WindowPadding.y;
			yoffset -= (prm.MenuButtonHoverSize * ui::Scale());
			yoffset -= g.FontSize;
			break;
		case 2: // auto-hide: the bar retracted out of view (the window's
		        // padding below it, still on screen, brings it back on hover)
			yoffset -= style.WindowPadding.y * 2.0f + (prm.MenuButtonHoverSize * ui::Scale()) + ui::Px(18);
			yoffset *= (1.0f - animState);
			break;
		}
		// The menu is always centered horizontally by using ImVec2(0.5, 0) as a pivot
		// and targeting the middle of the main window
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x / 2.0f, viewport->Pos.y + yoffset), ImGuiCond_Always, ImVec2(0.5, 0));
		
		if(ImGui::Begin("##MenuInfoBar", nullptr, MENUFLAGS)) {
			ImGuiStyle &style = ImGui::GetStyle();
			// the bar: a translucent panel behind the buttons and their labels
			{
				ImGuiWindow *w = ImGui::GetCurrentWindow();
				float h = style.WindowPadding.y * 2.0f + (prm.MenuButtonHoverSize * ui::Scale()) + ui::Px(18);
				ImVec2 b0 = w->Pos, b1 (w->Pos.x + w->Size.x, w->Pos.y + std::min (w->Size.y, h));
				w->DrawList->AddRectFilled(b0, b1, ui::Col32(ui::WINDOW), ui::Px(12)); // (opaque: the blend is linear, the HUD would show)
				g_menuBottom = b1.y;
				w->DrawList->AddRect(b0, b1, ui::Col32(ui::LINE, 0.7f), ui::Px(12), 0, std::max (1.0f, ui::Scale()));
			}
			ImVec2 bbmin = ImGui::GetCursorScreenPos() - style.WindowPadding;
			ImVec2 bbmax = bbmin + ImGui::GetContentRegionAvail() + style.WindowPadding * 2.0f;
			// Expand the bounding box a bit above the window so that the menubar
			// doesn't disappear when the user overshoots when the mouse cursor
			bbmin.y-=100;
			bool isHovering = ImGui::IsMouseHoveringRect(bbmin, bbmax, false);

			isDraggingOutside = !isHovering && isDragging;

			if(isHovering || isDragging) {
				if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
					g_pOrbiter->DlgMgr()->EnsureEntry<DlgMenuCfg> ();
				}

				// Lower the menu
				if (animState < 1.0f) {
					animState += prm.MenuScrollspeed * g.IO.DeltaTime;
					if(animState > 1.0f) animState = 1.0f;
				}
			} else {
				// Release focus so we can use the keyboard without having to click
				// on the screen after interacting with the menu bar
				if(ImGui::IsWindowFocused())
					ImGui::SetWindowFocus(NULL);

				// Retract the menu
				if (animState > 0.0f) {
					animState -= prm.MenuScrollspeed * g.IO.DeltaTime;
					if(animState < 0.0f) animState = 0.0f;
				}
			}

			// Remove the button if it's dropped outside the menubar
			if(ImGui::IsMouseReleased(ImGuiMouseButton_Left) && isDraggingOutside) {
				int itemsEnabled = std::count_if(items.begin(), items.end(), [](const auto &item){return item.enabled;});
				if(itemsEnabled > 1) { // Prevent removing the last button
					auto draggedItem = std::find_if(items.begin(), items.end(), [](const MenuItem &a){return a.dragging;});
					if(draggedItem != items.end()) {
						draggedItem->animStateScale = 1.0;
					}
				}
			}
		}
	}
	bool MenuInfoBarItem(MenuItem &item) {
		// Filter button spacing in time to prevent excessive jittering
		// when modifying MenuButtonSpacing
		if(animStateSpacing < (prm.MenuButtonSpacing * ui::Scale())) animStateSpacing+=0.1f;
		else if(animStateSpacing > (prm.MenuButtonSpacing * ui::Scale())) animStateSpacing-=0.1f;
		ImGui::SameLine(0.0f, animStateSpacing);
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		ImGuiContext& g = *GImGui;

		// Compute the item's position and size
		float buttonCenterPos = ImGui::GetCursorScreenPos().x + item.lastItemSize / 2.0f;
		float deltaPos = (ImGui::GetMousePos().x - buttonCenterPos) / item.lastItemSize;

		// Keep the button fully sized when hovered
		if(fabs(deltaPos) < 0.5f) {
			deltaPos = 0;
		}

		// Use a gaussian bell contour to compute the button size
		// given its distance from the cursor
		float scale = exp(-deltaPos * deltaPos * strength) * animState;
		float buttonSize = lrint(lerp((prm.MenuButtonSize * ui::Scale()), (prm.MenuButtonHoverSize * ui::Scale()), scale));
		item.lastItemSize = buttonSize;
		
		// Do a scale animation got the button
		bool disableText = false;
		if(item.animStateScale > 0.0f) {
			disableText = true;
			item.animStateScale -= prm.MenuScrollspeed * g.IO.DeltaTime;
			if(item.animStateScale <= 0.0f) {
				item.animStateScale = 0.0f;
				// Button has shrunk away, disable it
				item.enabled = false;
				SavePreferedOrder();
				return false;
			}
			buttonSize *= item.animStateScale;
		} else if(item.animStateScale < 0.0f) {
			disableText = true;
			item.animStateScale += prm.MenuScrollspeed * g.IO.DeltaTime;
			if(item.animStateScale >= 0.0f) {
				item.animStateScale = 0.0f;
			}
			buttonSize *= 1.0f + item.animStateScale;
		}

		ImRect bb(window->DC.CursorPos, window->DC.CursorPos + ImVec2(buttonSize, buttonSize));
		const char *label = item.label.c_str();
		bool ret = false;
		
		if(item.dragging) {
			// If this button is currently being dragged, we use an invisible
			// button to keep a "hole" in the menu bar and keep track of its
			// original position.
			// It will be drawn at its dragged position in EndMenuInfoBar()
			ImGui::InvisibleButton(label, ImVec2(buttonSize, buttonSize));
			holePos = bb.Min.x;
		} else {
			// If another button is being dragged, test if the current one needs to
			// be swapped with it
			if(isDragging) {
				// Different check if the button is to the right or to the left of the "hole"
				if(dragPos.x < holePos) {
					if(dragPos.x < bb.Min.x && (dragPos.x + (prm.MenuButtonHoverSize * ui::Scale()) > bb.Min.x)) {
						swapId = item.id;
					}
				} else {
					if((dragPos.x + (prm.MenuButtonHoverSize * ui::Scale()))> bb.Max.x && (dragPos.x < bb.Max.x)) {
						swapId = item.id;
					}
				}
			}
			ImVec2 textsize = ImGui::CalcTextSize(label, NULL, true);
			ImVec2 textpos = ImVec2(bb.Min.x + (bb.Max.x - bb.Min.x) / 2.0f - textsize.x / 2.0f, bb.Max.y);
			ui::PushFont(ui::GetFonts().medium, ui::SIZE_SMALL);
			textsize = ImGui::CalcTextSize(label, NULL, true);
			textpos = ImVec2(bb.Min.x + (bb.Max.x - bb.Min.x) / 2.0f - textsize.x / 2.0f, bb.Max.y + ui::Px(1));
			ImU32 textcol = ui::Col32(ui::TEXT_DIM);
			if (item.animStateScroll != 0.0f) {
				// If animState != 0, it means the current button has been swapped recently
				// We do the swapping animation by using an invisible button at its final position
				// and drawing it's animated position with the DrawList 
				if(item.animStateScroll < 0.0) {
					item.animStateScroll += prm.MenuScrollspeed * g.IO.DeltaTime;
					if(item.animStateScroll > 0.0) item.animStateScroll = 0.0;
				} else if(item.animStateScroll > 0.0) {
					item.animStateScroll -= prm.MenuScrollspeed * g.IO.DeltaTime;
					if(item.animStateScroll < 0.0) item.animStateScroll = 0.0;
				}
				ImGuiContext& g = *GImGui;
				ImGui::InvisibleButton(label, ImVec2(buttonSize, buttonSize));
				ImVec2 offset = ImVec2(item.animStateScroll * buttonSize, 0);
				if (item.glyph) DrawGlyph(window->DrawList, bb, item.glyph, false, false, offset);
				else window->DrawList->AddImage(item.texId, bb.Min - offset, bb.Max - offset);
				window->DrawList->AddText(textpos - offset, textcol, label);
			} else {
				if (item.glyph) {
					ret = ImGui::InvisibleButton(label, ImVec2(buttonSize, buttonSize));
					DrawGlyph(window->DrawList, bb, item.glyph, ImGui::IsItemHovered(), ImGui::IsItemActive());
				} else
					ret = ImGui::ImageButton(label, item.texId, ImVec2(buttonSize, buttonSize));
				if(!disableText)
					window->DrawList->AddText(textpos, ImGui::IsItemHovered() ? ui::Col32(ui::TEXT) : textcol, label);
			}
			ui::PopFont();
		}

		bool active = ImGui::IsItemActive();
		bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left);

		if(active && dragging) {
			// This item is being pressed and dragged.
			// If it's the first time we save the item's position and texture ID.
			// If not, we update the drag position according to the mouse movement.
			// dragPos will be used for drawing the button later and  for checking
			// if we need to swap items
			oneDrag = true;
			ImGuiIO& io = ImGui::GetIO();
			if(!item.dragging) {
				dragPos = ImGui::GetItemRectMin();
				item.dragging = true;
				dragTexId = item.texId;
				dragGlyph = item.glyph;
			} else {
				dragPos += io.MouseDelta;
			}
		} else if(!dragging) {
			item.dragging = false;
		}
		return ret;
	}

	void EndMenuInfoBar() {
		// If we're dragging a button, draw it last from here so it stays on top of the other items
		if(isDragging) {
			ImGuiWindow* window = ImGui::GetCurrentWindow();
			int itemsEnabled = std::count_if(items.begin(), items.end(), [](const auto &item){return item.enabled;});
			ImVec2 imageSize = ImVec2((prm.MenuButtonHoverSize * ui::Scale()), (prm.MenuButtonHoverSize * ui::Scale()));
			if(isDraggingOutside && itemsEnabled > 1) {
				// If it's outside the menu and it's not the only button remaining,
				// add a red background and show a removal message
				window->DrawList->AddRectFilled(dragPos, dragPos + imageSize, ui::Col32(ui::WARNING, 0.5f), ui::Px(8));
				const char *label = "Remove from menu";
				ImVec2 textSize = ImGui::CalcTextSize(label, NULL, true);
				window->DrawList->AddText(dragPos + ImVec2((prm.MenuButtonHoverSize * ui::Scale()) / 2.0f - textSize.x / 2.0f, (prm.MenuButtonHoverSize * ui::Scale())), ui::Col32(ui::TEXT), label);
			} else {
				window->DrawList->AddRectFilled(dragPos, dragPos + imageSize, ui::Col32(ui::RAISED_ACTIVE, 0.8f), ui::Px(8));
			}
			if (dragGlyph) DrawGlyph(window->DrawList, ImRect(dragPos, dragPos + imageSize), dragGlyph, true, false);
			else window->DrawList->AddImage(dragTexId, dragPos, dragPos + imageSize);
		}

		// Add some padding at the bottom so we can hover over the menu bar even when it's fully hidden
		if(MenuMode() != 0) {
			ImGui::NewLine();
			ImGui::NewLine();
		}
		ImGui::End();
	}
};


// The frame rate (Settings > Display: in the top-left or top-right info
// box): frames per second and the frame time, the 1% low (the frame rate of
// the slowest frames: stutter), and a graph of the recent frame times
void InfoRow(const char *label, const char *fmt, ...);

class InfoFrameRate {
	static const int N = 240;          // frame times kept
	float dt[N] = {};                  // [s]
	int head = 0, count = 0;
	double shownT = -1.0;              // (the numbers are updated a few times a second, to be readable)
	float fps = 0.0f, ms = 0.0f, low = 0.0f;
	uint64_t lastFrame = ~0ull;
public:
	void Update() {
		if (td.SysT0 < 0.0) return;
		ImGuiContext &g = *GImGui;
		if ((uint64_t)g.FrameCount == lastFrame) return; // (once per frame, wherever it is shown)
		lastFrame = (uint64_t)g.FrameCount;
		float d = (float)td.SysDT;
		if (d <= 0.0f || d > 5.0f) return;
		dt[head] = d;
		head = (head + 1) % N;
		count = std::min(count + 1, N);
		if (td.SysT1 - shownT >= 0.25 || shownT < 0.0) {
			shownT = td.SysT1;
			// the mean of the last half second
			float sum = 0.0f; int n = 0;
			for (int k = 1; k <= count && sum < 0.5f; k++) sum += dt[(head - k + N) % N], n++;
			ms = n ? 1e3f * sum / n : 0.0f;
			fps = sum > 0.0f ? n / sum : 0.0f;
			// the 1% low: the frame rate of the slowest 1% of the frames kept
			std::vector<float> v(dt, dt + N);
			v.resize(count);
			size_t k = std::max<size_t>(1, count / 100);
			std::nth_element(v.begin(), v.begin() + (k - 1), v.end(), std::greater<float>());
			float slow = 0.0f;
			for (size_t i = 0; i < k; i++) slow += v[i];
			low = slow > 0.0f ? k / slow : 0.0f;
		}
	}
	void Display() {
		Update();
		const ui::Fonts &f = ui::GetFonts();
		ui::PushFont(f.mono, ui::SIZE_SMALL + 0.5f);
		InfoRow("FPS", "%.0f  \xC2\xB7  %.1f ms", fps, ms);
		InfoRow("1% ", "%.0f low", low);
		ui::PopFont();
		ImGui::Dummy(ImVec2(0, ui::Px(3)));
		// the frame times: a line over the last frames (the scale: 33 ms or
		// the slowest frame; a reference at 60 per second)
		ImVec2 p = ImGui::GetCursorScreenPos();
		float w = std::max(ui::Px(150), ImGui::GetContentRegionAvail().x), h = ui::Px(26);
		ImGui::Dummy(ImVec2(w, h));
		if (count < 2) return;
		ImDrawList *dl = ImGui::GetWindowDrawList();
		float a = ImGui::GetStyle().Alpha;
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::Col32(ui::RAISED, 0.5f * a), ui::Px(3));
		float top = 1.0f / 30.0f;
		for (int k = 0; k < count; k++) top = std::max(top, dt[k]);
		float y60 = p.y + h - h * (1.0f / 60.0f) / top;
		dl->AddLine(ImVec2(p.x, y60), ImVec2(p.x + w, y60), ui::Col32(ui::TEXT_DIM, 0.35f * a), 1.0f);
		ImVec2 pts[N];
		int n = 0;
		for (int k = count; k >= 1; k--) {
			float d = dt[(head - k + N) % N];
			pts[n++] = ImVec2(p.x + w * (float)(count - k) / (N - 1), p.y + h - h * std::min(d / top, 1.0f));
		}
		dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
		dl->AddPolyline(pts, n, ui::Col32(ui::ACCENT, a), ImDrawFlags_None, std::max(1.0f, ui::Scale()));
		dl->PopClipRect();
	}
};

const float OPACITY_NONE = 0.001f;
void AnimateOpacity(float &animStateOpacity)
{
	CFG_UIPRM &prm = g_pOrbiter->Cfg()->CfgUIPrm;
	if(prm.InfoMode == 0) { // show
		animStateOpacity = 1.0f;
		return;
	}
	ImGuiIO& io = ImGui::GetIO();
	bool show = io.MousePos.y < (ImGui::GetCursorScreenPos().y + ImGui::GetContentRegionAvail().y);
	if(show) {
		if(animStateOpacity < 1.0f) {
			animStateOpacity += prm.MenuScrollspeed * io.DeltaTime / 3.0f;
			if(animStateOpacity > 1.0f) animStateOpacity = 1.0f;
		}
	} else {
		if(animStateOpacity > OPACITY_NONE) {
			animStateOpacity -= prm.MenuScrollspeed * io.DeltaTime / 3.0f;
			// Don't set it to 0 or ImGui will optimize the drawing away and return false on Begin
			if(animStateOpacity < OPACITY_NONE) animStateOpacity = OPACITY_NONE;
		}
	}
}

// the info bars' look: a small panel, labels dimmed beside the values
void InfoBegin(float opacity, const CFG_UIPRM &prm)
{
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, opacity);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Px(14), ui::Px(10)));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::Px(8));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Px(6), ui::Px(2)));
	ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::LINE, 0.6f));
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
	// (InfobarOpacity 0-10; the blending is linear: scaled down so that 2,
	// the default, is a light shade)
	ImGui::SetNextWindowBgAlpha(std::min(1.0f, 0.25f + prm.InfoOpacity/10.0f * 0.7f) * opacity);
}

void InfoEnd()
{
	ImGui::PopStyleColor(2);
	ImGui::PopStyleVar(4);
}

void InfoRow(const char *label, const char *fmt, ...)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::TEXT_DIM));
	ImGui::TextUnformatted(label);
	ImGui::PopStyleColor();
	ImGui::SameLine(0, ui::Px(8));
	va_list args;
	va_start(args, fmt);
	ImGui::TextV(fmt, args);
	va_end(args);
}

class InfoTime: public ImGuiDialog {
	InfoFrameRate *fps;
	float animStateOpacity;
public:
	InfoTime(InfoFrameRate *fps_):ImGuiDialog("InfoTime"),fps(fps_),animStateOpacity(OPACITY_NONE) {}
	void OnDraw() {}

	void Display() {
		CFG_UIPRM &prm = g_pOrbiter->Cfg()->CfgUIPrm;
		if(prm.InfoMode == 1)
			return;
		ImGuiViewport *viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowViewport(viewport->ID);
		float m = ui::Px(10);
		ImGui::SetNextWindowPos(viewport->Pos+ImVec2(viewport->Size.x - m, m),ImGuiCond_Always, ImVec2(1,0));

		InfoBegin(animStateOpacity, prm);
		if(ImGui::Begin("##InfoTime", nullptr, INFOFLAGS)) {
			AnimateOpacity(animStateOpacity);
			const ui::Fonts &f = ui::GetFonts();
			ui::PushFont(f.mono, ui::SIZE_SMALL + 0.5f);
			ImGui::TextUnformatted(DateStr (td.MJD1));
			InfoRow("MJD", "%0.4f", td.MJD1);
			if(td.SimT1 < 1e7) InfoRow("SIM", "%.0f s", td.SimT1);
			else InfoRow("SIM", "..%07.0f s", fmod (td.SimT1, 1e7));
			ui::PopFont();

			// state: paused, time acceleration, recording, playback
			bool rec = g_pOrbiter->RecorderStatus() == 1, play = g_pOrbiter->IsPlayback();
			bool paused = !g_pOrbiter->IsRunning();
			bool warp = td.Warp() != 1.0 || prm.bWarpAlways || g_pOrbiter->ShipWarp() > 0.0;
			if (paused || warp || rec || play) {
				ImGui::Dummy(ImVec2(0, ui::Px(2)));
				bool first = true;
				auto badge = [&](const char *t, ui::Colour c) {
					if (!first) ImGui::SameLine(0, ui::Px(6));
					first = false;
					ui::Badge(t, c);
				};
				if (paused) badge(ICON_LC_PAUSE " PAUSED", ui::CAUTION);
				else if (warp) {
					char buf[64];
					auto fmt = [](char *b, double w) {
						if (w >= 1e6) sprintf (b, ICON_LC_FAST_FORWARD " %0.3g\xC3\x97", w);
						else sprintf (b, ICON_LC_FAST_FORWARD " %0.*f\xC3\x97", w < 9.99 ? 1:0, w);
					};
					if (g_pOrbiter->ShipWarp() > 0.0) { // (ship-time warp: on the focus vessel's clock)
						fmt (buf, g_pOrbiter->ShipWarp());
						strcat (buf, " SHIP");
					} else fmt (buf, td.Warp());
					badge(buf, td.Warp() == 1.0 && g_pOrbiter->ShipWarp() <= 0.0 ? ui::TEXT_DIM : ui::CAUTION);
				}
				if (rec) badge(fmod (td.SysT1, 1.0) < 0.5 ? ICON_LC_CIRCLE_DOT " REC" : ICON_LC_CIRCLE " REC", ui::WARNING);
				if (play) badge(ICON_LC_PLAY " PLAYBACK", ui::ACCENT);
			}

			if(prm.FPS == 2)
				fps->Display();
		}
		if(ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
			ImGui::SetWindowFocus(NULL);
		ImGui::End();
		InfoEnd();
	}
};

class InfoTarget: public ImGuiDialog {
	InfoFrameRate *fps;
	float animStateOpacity;
public:
	InfoTarget(InfoFrameRate *fps_):ImGuiDialog("InfoTarget"),fps(fps_),animStateOpacity(OPACITY_NONE) {}
	void OnDraw() {}
	void Display() {
		CFG_UIPRM &prm = g_pOrbiter->Cfg()->CfgUIPrm;
		if(prm.InfoMode == 1) {
			animStateOpacity = OPACITY_NONE;
			return;
		}
		ImGuiViewport *viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowViewport(viewport->ID);
		float m = ui::Px(10);
		// (the glass cockpit's engine and propellant panel has the corner: beside it)
		float x0 = 0.0f;
		if (oapiCameraInternal() && oapiCockpitMode() == COCKPIT_GENERIC)
			x0 = 195.0f * std::max(0.625f, viewport->Size.y / 800.0f) + ui::Px(4);
		ImGui::SetNextWindowPos(viewport->Pos + ImVec2(m + x0, m));
		InfoBegin(animStateOpacity, prm);
		if(ImGui::Begin("##InfoTarget", nullptr, INFOFLAGS)) {
			AnimateOpacity(animStateOpacity);
			const ui::Fonts &f = ui::GetFonts();
			ui::PushFont(f.mono, ui::SIZE_SMALL + 0.5f);

			Body *tgt = g_camera->Target();
			InfoRow("TGT", "%s", tgt->Name());
			if (g_camera->IsExternal()) {
				switch (g_camera->GetExtMode()) {
					default: InfoRow("CAM", "%s", Camera::ExtModeName (g_camera->GetExtMode())); break;
					case CAMERA_GROUNDOBSERVER:   InfoRow("CAM", "Ground %s", g_camera->GroundObserver_TargetLock() ? "(target lock)" : "(free)"); break;
				}
			} else {
				InfoRow("CAM", "Cockpit");
			}
			if (g_camera->IsExternal())
				InfoRow("FOV", "%0.0f\xC2\xB0  \xC2\xB7  %s", 2.0*Deg(g_camera->Aperture()), DistStr (g_camera->Distance())+1);
			else
				InfoRow("FOV", "%0.0f\xC2\xB0", 2.0*Deg(g_camera->Aperture()));

			if(prm.FPS == 1)
				fps->Display();

			ui::PopFont();
		}
		if(ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
			ImGui::SetWindowFocus(NULL);
		ImGui::End();
		InfoEnd();
	}
};

// The camera's mode, for a moment after F2 changed it: under the menu bar
class CameraModeLabel: public ImGuiDialog {
public:
	CameraModeLabel(): ImGuiDialog("CameraModeLabel") {}
	void OnDraw() {}
	void Display() {
		const double SHOW = 1.6, FADEIN = 0.12, FADEOUT = 0.45;
		g_labelBottom = 0.0f;
		double t = g_camera->SinceModeChange();
		if (!g_camera->IsExternal() || t < 0.0 || t > SHOW) return;
		float a = (float)(t < FADEIN ? t / FADEIN : t > SHOW - FADEOUT ? (SHOW - t) / FADEOUT : 1.0);
		ImGuiViewport *viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowViewport(viewport->ID);
		float y = std::max(viewport->Pos.y + ui::Px(10), g_menuBottom + ui::Px(8));
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, y), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		CFG_UIPRM &prm = g_pOrbiter->Cfg()->CfgUIPrm;
		InfoBegin(a, prm);
		if (ImGui::Begin("##CameraMode", nullptr, INFOFLAGS | ImGuiWindowFlags_NoInputs)) {
			ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::ACCENT));
			ImGui::TextUnformatted(ICON_LC_VIDEO);
			ImGui::PopStyleColor();
			ImGui::SameLine(0, ui::Px(8));
			ui::TextDim("Camera");
			ImGui::SameLine(0, ui::Px(6));
			ui::TextDim("\xC2\xB7");
			ImGui::SameLine(0, ui::Px(6));
			const ui::Fonts &f = ui::GetFonts();
			ui::PushFont(f.strong, ui::SIZE_BODY);
			ImGui::TextUnformatted(Camera::ExtModeName(g_camera->GetExtMode()));
			ui::PopFont();
			g_labelBottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight();
		}
		ImGui::End();
		InfoEnd();
	}
};

// =======================================================================
// Flight events: banners under the menu bar

namespace {
	struct FlightEvent {
		flightevent::Level level;
		std::string title, detail, key, hint;
		double t0, duration; // [s] (steady clock)
		int count = 1;
	};
	std::vector<FlightEvent> g_events; // newest first

	double Now()
	{
		using namespace std::chrono;
		return duration<double>(steady_clock::now().time_since_epoch()).count();
	}
}

void flightevent::Post (Level level, const char *title, const char *detail, const char *key, const char *hint, double duration)
{
	static const double DURATION[4] = { 6.0, 5.0, 9.0, 10.0 }; // (a vessel lost: the flight menu follows)
	std::string t (title ? title : ""), d (detail ? detail : "");
	LOGOUT ("%s%s%s", t.c_str(), d.empty() ? "" : ": ", d.c_str());
	for (auto it = g_events.begin(); it != g_events.end(); ++it)
		if (it->title == t && it->detail == d) { // (again: counted, shown afresh)
			FlightEvent e = *it;
			e.count++;
			e.t0 = Now();
			g_events.erase(it);
			g_events.insert(g_events.begin(), e);
			return;
		}
	FlightEvent e;
	e.level = level;
	e.title = t, e.detail = d;
	e.key = key ? key : "", e.hint = hint ? hint : "";
	e.t0 = Now();
	e.duration = duration > 0.0 ? duration : DURATION[level];
	g_events.insert(g_events.begin(), e);
	const size_t MAXSHOWN = 3;
	if (g_events.size() > MAXSHOWN) g_events.resize(MAXSHOWN);
}

void flightevent::Clear ()
{
	g_events.clear();
}

class EventBanners: public ImGuiDialog {
public:
	EventBanners(): ImGuiDialog("EventBanners") {}
	void OnDraw() {}
	void Display() {
		const double FADEIN = 0.2, FADEOUT = 0.6;
		double now = Now();
		g_events.erase(std::remove_if(g_events.begin(), g_events.end(),
			[now](const FlightEvent &e) { return now - e.t0 > e.duration; }), g_events.end());
		if (g_events.empty()) return;

		static const ui::Colour colour[4] = { ui::ACCENT, ui::SUCCESS, ui::CAUTION, ui::WARNING };
		static const char *icon[4] = { ICON_LC_INFO, ICON_LC_CIRCLE_CHECK, ICON_LC_TRIANGLE_ALERT, ICON_LC_OCTAGON_ALERT };
		ImGuiViewport *viewport = ImGui::GetMainViewport();
		float y = std::max({viewport->Pos.y + ui::Px(12), g_menuBottom + ui::Px(10), g_labelBottom + ui::Px(8)});
		const ui::Fonts &f = ui::GetFonts();
		int i = 0;
		for (const FlightEvent &e : g_events) {
			double t = now - e.t0;
			float a = (float)std::clamp(std::min(t / FADEIN, (e.duration - t) / FADEOUT), 0.0, 1.0);
			ui::Colour c = colour[e.level];
			ImGui::SetNextWindowViewport(viewport->ID);
			ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, y), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
			ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Px(420), 0), ImVec2(ui::Px(640), FLT_MAX));
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::Px(10));
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ui::Px(18), ui::Px(12)));
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ui::Px(8), ui::Px(4)));
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::WINDOW, 0.94f));
			ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(c, 0.45f));
			char id[32];
			snprintf(id, sizeof(id), "##flightevent%d", i++);
			if (ImGui::Begin(id, nullptr, INFOFLAGS | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGuiWindow *w = ImGui::GetCurrentWindow();
				// a strip in the level's colour down the left edge
				w->DrawList->AddRectFilled(w->Pos, ImVec2(w->Pos.x + ui::Px(4), w->Pos.y + w->Size.y), ui::Col32(c, a), ui::Px(10), ImDrawFlags_RoundCornersLeft);
				ui::PushFont(f.body, ui::SIZE_BODY + 6.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(c));
				ImGui::TextUnformatted(icon[e.level]);
				ImGui::PopStyleColor();
				ui::PopFont();
				ImGui::SameLine(0, ui::Px(14));
				ImGui::BeginGroup();
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui::Px(540));
				ui::PushFont(f.strong, ui::SIZE_BODY + 2.0f);
				ImGui::TextUnformatted(e.title.c_str());
				ui::PopFont();
				if (e.count > 1) {
					ImGui::SameLine(0, ui::Px(8));
					ui::TextDim("\xC3\x97%d", e.count);
				}
				if (!e.detail.empty()) {
					ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::TEXT_DIM));
					ImGui::TextUnformatted(e.detail.c_str());
					ImGui::PopStyleColor();
				}
				if (!e.key.empty() || !e.hint.empty()) {
					ImGui::Dummy(ImVec2(0, ui::Px(2)));
					if (!e.key.empty()) {
						ui::KeyCap(e.key.c_str());
						ImGui::SameLine(0, ui::Px(8));
					}
					ui::TextDim("%s", e.hint.c_str());
				}
				ImGui::PopTextWrapPos();
				ImGui::EndGroup();
				y += ImGui::GetWindowHeight() + ui::Px(8);
			}
			ImGui::End();
			ImGui::PopStyleColor(2);
			ImGui::PopStyleVar(4);
		}
	}
};

// =======================================================================
// Screen annotations (oapiCreateAnnotation: scripts, tutorials, recorded
// flights' notes): panels at the annotation's place, its text wrapped to
// its box, a strip and the text in its colour

class AnnotationPanels: public ImGuiDialog {
public:
	AnnotationPanels(): ImGuiDialog("AnnotationPanels") {}
	void OnDraw() {}
	void Display() {
		int n = 0;
		for (DWORD i = 0; i < g_pOrbiter->nAnnotation(); i++)
			Panel(g_pOrbiter->Annotation(i), n++);
		if (g_pOrbiter->Cfg()->CfgRecPlayPrm.bShowNotes)
			Panel(g_pOrbiter->SNotePB(), n++);
	}
private:
	void Panel(const oapi::ScreenAnnotation *sn, int idx) {
		if (!sn || !*sn->Text()) return;
		int x1, y1, x2, y2;
		sn->Box(x1, y1, x2, y2);
		ImGuiViewport *viewport = ImGui::GetMainViewport();
		// (the box was laid out for the old text, a size a 35th of the
		// screen's height: the panel's text is smaller, its box narrower)
		float scale = (float)std::clamp(sn->Size(), 0.5, 2.0);
		float size = ui::SIZE_BODY * 1.15f * scale;
		float boxw = std::max(ui::Px(240), (float)(x2 - x1));
		COLORREF col = sn->Colour();
		ImVec4 c((col & 0xFF) / 255.0f, ((col >> 8) & 0xFF) / 255.0f, ((col >> 16) & 0xFF) / 255.0f, 1.0f);
		// (sRGB colours of the scripts, to the linear blend; lightened for reading on the panel)
		ImVec4 lc = ui::Srgb(((unsigned)(c.x*255) << 16) | ((unsigned)(c.y*255) << 8) | (unsigned)(c.z*255));
		ImVec4 tc(lc.x + (1.0f - lc.x) * 0.35f, lc.y + (1.0f - lc.y) * 0.35f, lc.z + (1.0f - lc.z) * 0.35f, 1.0f);
		float pad = ui::Px(16);
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + x1, viewport->Pos.y + y1), ImGuiCond_Always);
		ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(boxw, FLT_MAX));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, ui::Px(10));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad + ui::Px(4), ui::Px(12)));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::Col(ui::WINDOW, 0.86f));
		ImGui::PushStyleColor(ImGuiCol_Border, ui::Col(ui::LINE, 0.6f));
		char id[32];
		snprintf(id, sizeof(id), "##annotation%d", idx);
		if (ImGui::Begin(id, nullptr, INFOFLAGS | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing)) {
			ImGuiWindow *w = ImGui::GetCurrentWindow();
			w->DrawList->AddRectFilled(w->Pos, ImVec2(w->Pos.x + ui::Px(4), w->Pos.y + w->Size.y), ImGui::GetColorU32(lc), ui::Px(10), ImDrawFlags_RoundCornersLeft);
			ui::PushFont(ui::GetFonts().body, size);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + boxw - 2 * pad - ui::Px(4));
			ImGui::PushStyleColor(ImGuiCol_Text, tc);
			ImGui::TextUnformatted(sn->Text());
			ImGui::PopStyleColor();
			ImGui::PopTextWrapPos();
			ui::PopFont();
		}
		ImGui::End();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar(2);
	}
};

// =======================================================================
// class MenuInfoBar
// =======================================================================

MenuInfoBar::MenuInfoBar ()
{
	dynMenu  = std::make_unique<DynamicMenuBar>();
	infoFps  = std::make_unique<InfoFrameRate>();
	infoTime = std::make_unique<InfoTime>(infoFps.get());
	infoTgt  = std::make_unique<InfoTarget>(infoFps.get());
	oapiOpenDialog(dynMenu.get());
	oapiOpenDialog(infoTime.get());
	oapiOpenDialog(infoTgt.get());
	camLabel = std::make_unique<CameraModeLabel>();
	oapiOpenDialog(camLabel.get());
	notes = std::make_unique<AnnotationPanels>();
	oapiOpenDialog(notes.get());
	banners = std::make_unique<EventBanners>();
	oapiOpenDialog(banners.get());
}

MenuInfoBar::~MenuInfoBar ()
{
}
void MenuInfoBar::RegisterMenuItem(const char *label, const char *imagepath, int id, CustomFunc func, void *context)
{
	dynMenu->AddMenuItem(label, imagepath, id, func, context);
}

void MenuInfoBar::UnregisterMenuItem(int id)
{
	dynMenu->DelMenuItem(id);
}

std::vector<MenuInfoBar::MenuPreference> &MenuInfoBar::GetPreferences()
{
	return dynMenu->preferedOrder;
}

void MenuInfoBar::SyncPreferences()
{
	return dynMenu->SyncPreferedOrder();
}
