// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// =======================================================================
// Dear ImGui's test engine hooks (IMGUI_ENABLE_TEST_ENGINE), forwarded to
// whoever installs a table in the context's TestEngine pointer: the Orbiter
// core lists the widgets drawn (labels and places) for remote control and
// tests (UiAnchor.h). Every module compiles ImGui (Orbitersdk) and shares
// the core's context, so plugins' widgets are reported too.
// =======================================================================

#ifndef __IMGUIITEMHOOKS_H
#define __IMGUIITEMHOOKS_H

#include "imgui.h"
#include "imgui_internal.h"

struct ImGuiItemHooks {
	void (*itemAdd) (ImGuiContext *ctx, ImGuiID id, const ImRect &bb, const ImGuiLastItemData *data);
	void (*itemInfo) (ImGuiContext *ctx, ImGuiID id, const char *label, ImGuiItemStatusFlags flags);
};

#endif // !__IMGUIITEMHOOKS_H
