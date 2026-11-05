// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ======================================================================
// Time acceleration dialog
// ======================================================================
#define IMGUI_DEFINE_MATH_OPERATORS

#include "DlgTacc.h"
#include "Orbiter.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "IconsFontAwesome6.h"
#include "IconsLucide.h"
#include "UiTheme.h"
#include <algorithm>

extern TimeData td;
extern Orbiter *g_pOrbiter;

// Snap to 1..9,10 per decade (1,2,3,...,9,10,20,30,...)
static float SnapToDecimal(float v, float min_v, float max_v)
{
    v = std::clamp(v, min_v, max_v);

    float decade = floorf(log10f(v));
    float base   = powf(10.0f, decade);

    float norm = v / base;
    float snapped = roundf(norm);

    // Handle rounding up to next decade cleanly
    if (snapped >= 10.0f)
    {
        snapped = 10.0f;
    }

    float result = snapped * base;

    return std::clamp(result, min_v, max_v);
}

DlgTacc::DlgTacc() : ImGuiDialog(ICON_FA_CLOCK "  Time acceleration",{380,232}) {
	SetHelp("html/orbiter.chm", "/timeacc.htm");
}

void DlgTacc::OnDraw() {
    const ui::Fonts &f = ui::GetFonts();
    bool running = g_pOrbiter->IsRunning();

    // the time acceleration, large, and the state
    ui::PushFont(f.mono, ui::SIZE_DISPLAY - 4.0f);
    char big[32];
    double w0 = td.Warp();
    if (w0 >= 1e4) snprintf(big, sizeof(big), "%.0f\xC3\x97", w0);
    else snprintf(big, sizeof(big), "%.*f\xC3\x97", w0 < 9.99 ? 1 : 0, w0);
    ImGui::TextUnformatted(big);
    ui::PopFont();
    ImGui::SameLine(0, ui::Px(12));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ui::Px(10));
    if (!running) ui::Badge(ICON_LC_PAUSE " PAUSED", ui::CAUTION);
    else if (w0 == 1.0) ui::Badge("REAL TIME", ui::TEXT_DIM);
    else ui::Badge(ICON_LC_FAST_FORWARD " ACCELERATED", ui::CAUTION);
    ImGui::Dummy(ImVec2(0, ui::Px(4)));

    // presets: a segmented control, the current one filled
    static const struct { double w; const char *label; } presets[] = {
        {0.1, "0.1\xC3\x97"}, {1.0, "1\xC3\x97"}, {10.0, "10\xC3\x97"}, {100.0, "100\xC3\x97"}, {1000.0, "1k\xC3\x97"}, {10000.0, "10k\xC3\x97"}
    };
    int n = IM_ARRAYSIZE(presets);
    float gap = ui::Px(4);
    float bw = (ImGui::GetContentRegionAvail().x - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++) {
        bool cur = fabs(w0 - presets[i].w) < presets[i].w * 1e-3;
        if (i) ImGui::SameLine(0, gap);
        if (cur) {
            ImGui::PushStyleColor(ImGuiCol_Button, ui::Col(ui::ACCENT, 0.22f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ui::Col(ui::ACCENT, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_Text, ui::Col(ui::ACCENT));
        }
        ui::PushFont(f.mono, ui::SIZE_SMALL + 0.5f);
        if (ImGui::Button(presets[i].label, ImVec2(bw, 0))) g_pOrbiter->SetWarpFactor(presets[i].w);
        ui::PopFont();
        if (cur) ImGui::PopStyleColor(3);
    }
    ImGui::Dummy(ImVec2(0, ui::Px(2)));

    float warp = td.Warp();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGuiIO& io = ImGui::GetIO();

    // The time acceleration slider has a snapping behavior (disabled if SHIFT is pressed)
    float warp_min = 0.1f;
    float warp_max = 10000.0f;

    // If snapping, draw the grabber as a "ghost" (inactive color)
    // We'll draw the snapped version later with the active color
    if(!io.KeyShift)
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImGui::GetColorU32(ImGuiCol_SliderGrab));

    // Use a fake format so that the SliderFloat will show the snapped value
    char format[64];
    snprintf(format, 64, "%.1f", warp);
    format[63] = '\0';
    ImGui::SliderFloat("##slider warp", &warp,
	                        warp_min, warp_max,
	                        format,
	                        ImGuiSliderFlags_Logarithmic);

    if(!io.KeyShift)
        ImGui::PopStyleColor();

    bool active = ImGui::IsItemActive();
    bool dragging = active && ImGui::IsMouseDragging(ImGuiMouseButton_Left);

    if(active && !io.KeyShift)
        warp = SnapToDecimal(warp, warp_min, warp_max);

    g_pOrbiter->SetWarpFactor(warp);

    // Draw the snapped grabber while dragging
    if (dragging && !io.KeyShift)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        ImRect frame_bb(ImGui::GetItemRectMin(),
                        ImGui::GetItemRectMax());

        // Convert snapped value to parametric t (log scale)
        float t = (log10f(warp) - log10f(warp_min)) /
                  (log10f(warp_max) - log10f(warp_min));
        t = ImClamp(t, 0.0f, 1.0f);

        float grab_w = style.GrabMinSize;
        float x = ImLerp(frame_bb.Min.x + grab_w * 0.5f,
                          frame_bb.Max.x - grab_w * 0.5f,
                          t);

        ImRect snapped_grab(
            ImVec2(x - grab_w * 0.5f, frame_bb.Min.y + 2),
            ImVec2(x + grab_w * 0.5f, frame_bb.Max.y - 2));


        window->DrawList->AddRectFilled(
            snapped_grab.Min,
            snapped_grab.Max,
            ImGui::GetColorU32(ImGuiCol_SliderGrabActive),
            style.GrabRounding);

    }

    ImGui::Dummy(ImVec2(0, ui::Px(6)));
    if (running ? ui::SecondaryButton(ICON_LC_PAUSE "  Pause", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.2f))
                : ui::PrimaryButton(ICON_LC_PLAY "  Resume", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.2f)))
        g_pOrbiter->TogglePause();
}
