#include "host/controls_page.h"

#include "engine/debug_menu.h"
#include "host/bindings.h"
#include "host/i18n.h"
#include "host/launcher_ui.h"
#include "host/options.h"
#include "host/window.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ui = launcher_ui;

namespace {

// A choice the page began (and not the game's Key Bindings screen), which
// ends with the menu.
bool g_page_capture = false;
// The bindings as last saved: a change made on the page is written at once,
// as the game's screen writes its own.
std::uint64_t g_saved_serial = 0;

struct Section {
    const char* name;
    int first, last;  // BindAction, inclusive
};
constexpr Section kSections[] = {
    {"MOVEMENT", kBindMoveF, kBindRollTap},
    {"COMBAT", kBindAttack, kBindUseItem},
    {"ITEMS AND GESTURES", kBindInteract, kBindEffects},
    {"CAMERA AND MENUS", kBindLookU, kBindBack},
    {"MENU PAGES AND OPTIONS", kBindMenuPageL, kBindMenuStatus},
    {"OTHER", kBindLockOn, kBindDebugMenu},
};

// What the controller column says for an action without a button of its own
// to move: the stick it is, a menu's fixed button, or nothing.
const char* pad_fixed(int a) {
    if (a >= kBindMoveF && a <= kBindMoveR) return i18n::tr("Left stick");
    if (a >= kBindLookU && a <= kBindLookR) return i18n::tr("Right stick");
    if (const int m = host_pad_menu_input(a); m >= 0) return host_pad_input_name(m);
    return nullptr;
}

// One cell: the binding as a button, "Press ..." while it is being chosen.
bool cell(const char* id, const char* text, bool choosing, float w, bool enabled) {
    ImGui::PushID(id);
    if (!enabled) ImGui::BeginDisabled();
    if (choosing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool clicked = ImGui::Button(choosing ? (std::strcmp(id, "##pad") == 0 ? i18n::tr("Press a button") : i18n::tr("Press a key")) : text,
                                       ImVec2(w, 0.0f));
    if (choosing) ImGui::PopStyleColor();
    if (!enabled) ImGui::EndDisabled();
    ImGui::PopID();
    return clicked && enabled;
}

void save_if_changed() {
    if (host_bind_capturing() >= 0 || host_pad_capturing() >= 0) return;
    if (const std::uint64_t s = host_bindings_serial(); s != g_saved_serial) {
        g_saved_serial = s;
        host_options_save_now();
    }
}

// The controller's short / long press, on one line: an action, the action it
// gives held past a time, and the time. Chosen by what they do (as the rows
// above are), not by the buttons' names. Read on every frame, so a change
// applies at once; the bindings' serial makes the page save it with the rest.
void pad_short_long_card() {
    const float sc = ui::scale();
    int src_c[kBindCount + 1], long_c[kBindCount + 1];
    bool src_off[kBindCount + 2] = {}, long_off[kBindCount + 2] = {};
    const char* src_items[kBindCount + 2];
    const char* long_items[kBindCount + 2];
    int ns = 0, nl = 0;
    src_items[0] = long_items[0] = "None";
    for (int c = 0; c <= kPadLongBack; ++c) {
        // An action on a trigger is shown greyed: the triggers are not taken.
        const int in = c == kPadLongBack ? -1 : host_pad_input_of(c);
        const bool trigger = in == kPadL2 || in == kPadR2;
        if (host_pad_long_src_allowed(c)) {
            src_c[ns] = c;
            src_off[ns + 1] = trigger;
            src_items[++ns] = i18n::tr(host_pad_long_name(c));
        }
        if (host_pad_long_target_allowed(c)) {
            long_c[nl] = c;
            long_off[nl + 1] = trigger;
            long_items[++nl] = i18n::tr(host_pad_long_name(c));
        }
    }
    const auto index_of = [](const int* choices, int n, int choice) {
        for (int i = 0; i < n; ++i) {
            if (choices[i] == choice) return i + 1;
        }
        return 0;
    };
    ImGui::Spacing();
    ui::card_begin("##padlong", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), i18n::tr("SHORT / LONG PRESS (CONTROLLER)"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", i18n::tr("The button of the short action gives that action when pressed briefly; held longer than the "
                        "time, it gives the long action instead. Actions on L2 and R2 are not taken."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    int src = -1, lng = -1;
    host_pad_long_get(&src, &lng);
    int want_src = index_of(src_c, ns, src), want_lng = index_of(long_c, nl, lng);
    const int had_src = want_src, had_lng = want_lng;
    const ImGuiStyle& st = ImGui::GetStyle();
    const float t_short = ImGui::CalcTextSize(i18n::tr("Short")).x, t_long = ImGui::CalcTextSize(i18n::tr("Long")).x;
    const float t_held = ImGui::CalcTextSize(i18n::tr("Held longer than")).x;
    const float gap = st.ItemSpacing.x;
    const float spare = ImGui::GetContentRegionAvail().x - t_short - t_long - t_held - ui::reset_width() - gap * 8.0f;
    const float cw = std::clamp(spare * 0.3f, 90.0f * sc, 200.0f * sc);
    const float sw = std::max(spare - 2.0f * cw, 80.0f * sc);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr("Short"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(cw);
    ui::combo("##plsrc", &want_src, src_items, ns + 1, nullptr, src_off);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr("Long"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(cw);
    ui::combo("##pllong", &want_lng, long_items, nl + 1, nullptr, long_off);
    if (want_src != had_src || want_lng != had_lng)
        host_pad_long_set(want_src > 0 ? src_c[want_src - 1] : -1, want_lng > 0 ? long_c[want_lng - 1] : -1);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(i18n::tr("Held longer than"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sw);
    int ms = host_pad_long_ms();
    if (ImGui::SliderInt("##plms", &ms, 100, 1000, "%d ms", ImGuiSliderFlags_AlwaysClamp) && ms != host_pad_long_ms())
        host_pad_long_set_ms(ms);
    ImGui::SameLine();
    if (ui::reset_button("##plreset", host_pad_long_at_default())) host_pad_long_reset();
    ui::card_end();
}

}  // namespace

void controls_page_draw(bool setup_window) {
    if (!g_saved_serial) g_saved_serial = host_bindings_serial();
    const float sc = ui::scale();
    if (g_page_capture && host_bind_capturing() < 0 && host_pad_capturing() < 0) g_page_capture = false;
    save_if_changed();

    // The settings' Input page's rows first.
    for (int i = 0; i < host_options_page_count(); ++i) {
        if (std::strcmp(host_options_page_name(i), "Input") != 0) continue;
        if (setup_window) host_options_draw_setup_page(i);
        else host_options_draw_page(i);
    }
    ImGui::Spacing();

    // The bindings on a card of their own, as the settings above sit on
    // theirs: the menus' glass under them, not the painting.
    ui::card_begin("##bindings", ImVec2(ImGui::GetContentRegionAvail().x, 0.0f), i18n::tr("BINDINGS"));
    // Its bar: Reset all on the left, and beside it how to choose a binding -
    // or, while one is being chosen, which and how to back out.
    const int key_choosing = host_bind_capturing(), pad_choosing = host_pad_capturing();
    if (ui::ghost_button(i18n::tr("Reset all to defaults"), ImVec2(0.0f, 0.0f))) ImGui::OpenPopup("##reset_all");
    ui::tooltip_for_item("Reset all to defaults", "Every key, mouse button and controller button back to its default "
                                                  "(bbhost.toml's [keys] for the keys and the mouse).");
    if (ImGui::BeginPopup("##reset_all")) {
        ImGui::TextUnformatted(i18n::tr("Put every binding back to its default?"));
        if (ui::primary_button(i18n::tr("Reset all"), ImVec2(130.0f * sc, 0.0f))) {
            host_bindings_reset_all();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::ghost_button(i18n::tr("Cancel"), ImVec2(130.0f * sc, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 14.0f * sc);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(0.0f);
    if (key_choosing >= 0) {
        ImGui::TextUnformatted(i18n::tf("%s: press the new key or mouse button. Escape cancels.", i18n::tr(host_binding_info(key_choosing).label)).c_str());
    } else if (pad_choosing >= 0) {
        ImGui::TextUnformatted(i18n::tf("%s: press the new controller button. Escape cancels.", i18n::tr(host_binding_info(pad_choosing).label)).c_str());
    } else {
        ImGui::TextDisabled("%s", i18n::tr("Click a box, then press the new key, mouse button or controller button; Escape cancels. "
                            "Right-click clears a key or a mouse button, and the arrow at the end of a line puts that "
                            "action back to its default. The controller's buttons move in play only: in menus each is "
                            "itself, so Confirm and Back stay where they were."));
    }
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##controls", 5, flags)) {
        ui::card_end();
        return;
    }
    // The three binding columns share what the action's name and the reset
    // leave, up to 170 each: a narrow menu or a large text size keeps every
    // column, the reset's too, on the page.
    const float reset_w = std::max(ui::reset_width(), ImGui::CalcTextSize(i18n::tr("Reset")).x);
    const float spare = ImGui::GetContentRegionAvail().x - 150.0f * sc - reset_w - ImGui::GetStyle().CellPadding.x * 10.0f;
    const float cw = std::clamp(spare / 3.0f, 72.0f * sc, 170.0f * sc);
    ImGui::TableSetupColumn(i18n::tr("Action"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(i18n::tr("Keyboard"), ImGuiTableColumnFlags_WidthFixed, cw);
    ImGui::TableSetupColumn(i18n::tr("Mouse"), ImGuiTableColumnFlags_WidthFixed, cw);
    ImGui::TableSetupColumn(i18n::tr("Controller"), ImGuiTableColumnFlags_WidthFixed, cw);
    ImGui::TableSetupColumn(i18n::tr("Reset"), ImGuiTableColumnFlags_WidthFixed, reset_w);
    ImGui::TableHeadersRow();
    for (const Section& sec : kSections) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Spacing();
        ui::caption(i18n::tr(sec.name));
        for (int a = sec.first; a <= sec.last; ++a) {
            if (a == kBindDebugMenu && !debug_menu_active()) continue;  // as the game's screen lists it
            const BindingInfo& info = host_binding_info(a);
            ImGui::PushID(a);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(i18n::tr(info.label));
            ui::tooltip_for_item(info.label, info.help);

            // The key and the mouse button: one choice takes either, as the
            // game's screen has it.
            const int k = host_binding_key(a), m = host_binding_mouse(a);
            const char* key = k ? host_key_name(k) : nullptr;
            const char* mouse = m ? host_mouse_name(m) : nullptr;
            ImGui::TableSetColumnIndex(1);
            if (cell("##key", key ? key : "-", key_choosing == a, cw, true)) {
                host_bind_capture_begin(a);
                g_page_capture = true;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && k) host_binding_clear_key(a);
            ImGui::TableSetColumnIndex(2);
            if (cell("##mouse", mouse ? mouse : "-", key_choosing == a, cw, true)) {
                host_bind_capture_begin(a);
                g_page_capture = true;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && m) host_binding_clear_mouse(a);

            // The controller: its button, or what it is fixed to.
            ImGui::TableSetColumnIndex(3);
            const int pad = host_pad_input_of(a);
            if (pad >= 0) {
                if (cell("##pad", host_pad_input_name(pad), pad_choosing == a, cw, true)) {
                    host_pad_capture_begin(a);
                    g_page_capture = true;
                }
                if (a == kBindGestures || a == kBindEffects)
                    ui::tooltip_for_item(info.label, "Gestures and Personal Effects are the touchpad's two sides: they move together.");
            } else {
                const char* fixed = pad_fixed(a);
                cell("##pad", fixed ? fixed : "-", false, cw, false);
                if (a == kBindConfirm || a == kBindBack)
                    ui::tooltip_for_item(info.label, "Follows the button the game's own OK or Back is on in play "
                                                     "(Interact or Dodge): move that action to move this.");
                else if (host_pad_menu_input(a) >= 0)
                    ui::tooltip_for_item(info.label, "A menu's own button: it moves only to make way for Menu Confirm "
                                                     "and Menu Back.");
            }

            ImGui::TableSetColumnIndex(4);
            const bool at_default = host_binding_at_default(a);
            ImGui::BeginDisabled(at_default);
            if (ui::icon_button("##reset", ui::Icon::Refresh, 0.0f)) host_binding_reset(a);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
                ui::tooltip(info.label, at_default ? "Its default key, mouse button and controller button."
                                                   : "Back to its default key, mouse button and controller button.");
            ImGui::PopID();
            // Dodge (tap only)'s press length, on a line of its own right under it.
            if (a == kBindRollTap) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("    %s", i18n::tr("Dodge only press"));
                ui::tooltip_for_item("Dodge only press", "How long the Dodge (tap only) key holds the dodge button, 5 to 150 ms. "
                                                         "Raise it if the dodge does not come out, lower it if it turns into a sprint.");
                ImGui::TableSetColumnIndex(1);
                host_options_dodge_slider(cw);
                ImGui::TableSetColumnIndex(4);
                host_options_dodge_reset();
            }
        }
    }
    ImGui::EndTable();
    ui::card_end();
    pad_short_long_card();
}

bool controls_capture_event(const SDL_Event& e) {
    if (host_pad_capturing() >= 0) {
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) {
            host_pad_capture_cancel();
            return true;
        }
        if (const int in = host_pad_input_of_event(e); in >= 0) {
            host_pad_capture_input(in);
            return true;
        }
        // Nothing of the controller reaches the menu while one is chosen.
        return e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    if (host_bind_capturing() < 0) return false;
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat) host_bind_capture_key(e.key.scancode);
            return true;
        case SDL_EVENT_KEY_UP:
            return true;
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            const std::uint8_t b = e.button.button;
            const int n = b == SDL_BUTTON_LEFT ? 1 : b == SDL_BUTTON_RIGHT ? 2 : b == SDL_BUTTON_MIDDLE ? 3 : b == SDL_BUTTON_X1 ? 4
                        : b == SDL_BUTTON_X2 ? 5 : 0;
            if (n) host_bind_capture_mouse(n);
            return true;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            host_bind_capture_mouse(e.wheel.y > 0.0f ? 6 : 7);
            return true;
        default:
            return false;
    }
}

void controls_menu_closed() {
    if (!g_page_capture) return;
    g_page_capture = false;
    if (host_pad_capturing() >= 0) host_pad_capture_cancel();
    if (host_bind_capturing() >= 0) host_bind_capture_key(SDL_SCANCODE_ESCAPE);
}
