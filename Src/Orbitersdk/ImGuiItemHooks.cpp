// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// Dear ImGui's test engine hooks: forwarded (ImGuiItemHooks.h)

#include "ImGuiItemHooks.h"

void ImGuiTestEngineHook_ItemAdd (ImGuiContext *ctx, ImGuiID id, const ImRect &bb, const ImGuiLastItemData *data)
{
	const ImGuiItemHooks *h = (const ImGuiItemHooks*)ctx->TestEngine;
	if (h && h->itemAdd) h->itemAdd (ctx, id, bb, data);
}

void ImGuiTestEngineHook_ItemInfo (ImGuiContext *ctx, ImGuiID id, const char *label, ImGuiItemStatusFlags flags)
{
	const ImGuiItemHooks *h = (const ImGuiItemHooks*)ctx->TestEngine;
	if (h && h->itemInfo) h->itemInfo (ctx, id, label, flags);
}

void ImGuiTestEngineHook_Log (ImGuiContext *, const char *, ...)
{
}

const char *ImGuiTestEngine_FindItemDebugLabel (ImGuiContext *, ImGuiID)
{
	return nullptr;
}
