#include "host/mods_page.h"

#include "core/config.h"
#include "host/launcher_ui.h"
#include "mods/mod_loader.h"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace ui = launcher_ui;

namespace {


std::string generic(const std::string& p) { return fs::u8path(p).generic_u8string(); }
std::string toml_str(const std::string& p) { return "\"" + generic(p) + "\""; }

std::string absolute_from_config(const std::string& p) {
    if (p.empty()) return p;
    fs::path x = fs::u8path(p);
    if (x.is_relative()) x = fs::u8path(config_user_file()).parent_path() / x;
    return x.lexically_normal().u8string();
}

struct Page {
    ~Page() {
        if (worker.joinable()) worker.join();
    }
    // ---- settings
    bool inited = false;
    char source[1024] = {}, overlay[1024] = {};
    std::string last_app0;
    // ---- the list
    std::vector<mods::ModSource> mods;
    std::set<std::string> applied;
    std::string folder_note;
    // ---- the job
    enum Phase { Idle, Planning, Confirm, Applying, Reverting };
    std::atomic<Phase> phase{Idle};
    std::atomic<bool> running{false};
    std::thread worker;
    std::mutex mu;
    std::vector<std::string> log;
    std::string status;
    std::atomic<int> done{0}, total{0};
    std::shared_ptr<mods::Plan> plan;
    std::shared_ptr<mods::MergeEnv> env;
    std::vector<std::string> conflict_lines;
    std::string summary;
    // ---- pickers and drops (they answer on other threads)
    int pick_for = 0;   // 1 mods folder, 2 overlay, 3 add folder, 4 add zips
    std::vector<std::string> picked;
    std::vector<std::string> dropped;
    bool picked_ready = false;
    bool show_log = false;
    bool confirm_revert = false;
    bool open_confirm = false;
};
Page g;

void SDLCALL on_pick(void* ud, const char* const* files, int) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.pick_for = static_cast<int>(reinterpret_cast<std::intptr_t>(ud));
    g.picked.clear();
    for (int i = 0; files && files[i]; ++i) g.picked.push_back(files[i]);
    g.picked_ready = true;
}

void say(const std::string& line) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.log.push_back(line);
    if (g.log.size() > 2000) g.log.erase(g.log.begin(), g.log.begin() + 500);
}
void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.status = s;
}

std::string data_dir_of(const std::string& data) { return data.empty() ? config_default_data_dir() : data; }
std::string state_dir() { return (fs::u8path(g.source) / ".bbmods").u8string(); }

void save_setting(const char* section, const char* key, const std::string& path) {
    if (config_set_values(config_user_file(), {{section, key, toml_str(path)}})) config_value_set(std::string(section) + "." + key, toml_str(path));
}

void load_settings(const std::string& data) {
    const std::string d = data_dir_of(data);
    std::string src = absolute_from_config(config_value("mods.source"));
    if (src.empty()) src = (fs::u8path(d) / "mod-sources").u8string();
    std::string ov = config().mods;
    if (ov.empty()) ov = absolute_from_config(config_value("paths.mods"));
    if (ov.empty()) ov = (fs::u8path(d) / "mods").u8string();
    std::snprintf(g.source, sizeof(g.source), "%s", generic(src).c_str());
    std::snprintf(g.overlay, sizeof(g.overlay), "%s", generic(ov).c_str());
    g.inited = true;
}

void refresh_list() {
    g.mods = mods::list_mods(g.source);
    g.applied.clear();
    const auto info = mods::applied_info(state_dir());
    for (const std::string& n : info.first) g.applied.insert(n);
}

void save_list() {
    std::string why;
    if (!mods::save_modlist(g.source, g.mods, &why)) say("Could not save the mod list: " + why);
}

void join_worker() {
    if (!g.running && g.worker.joinable()) g.worker.join();
}

std::vector<std::string> take_picked(int* for_what) {
    std::lock_guard<std::mutex> lk(g.mu);
    std::vector<std::string> v;
    *for_what = 0;
    if (!g.picked_ready) return v;
    g.picked_ready = false;
    *for_what = g.pick_for;
    v.swap(g.picked);
    return v;
}

std::string url_of(const std::string& path) {
    std::string out = "file:///";
    for (unsigned char c : generic(path)) {
        if (std::isalnum(c) || c == '/' || c == ':' || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else {
            char b[8];
            std::snprintf(b, sizeof(b), "%%%02X", c);
            out += b;
        }
    }
    return out;
}

void import_paths(const std::vector<std::string>& paths) {
    if (paths.empty() || g.running) return;
    say("Adding mods...");
    const auto added = mods::import_mods(paths, g.source, [](const std::string& l) { say(l); });
    refresh_list();
    if (!added.empty()) set_status(std::string("Added: ") + std::to_string(added.size()));
}

// ---- the jobs
void start_apply(const std::string& app0, const std::string& data) {
    (void)data;
    g.running = true;
    g.phase = Page::Planning;
    g.done = g.total = 0;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.log.clear();
        g.conflict_lines.clear();
        g.summary.clear();
    }
    const std::vector<mods::ModSource> list = g.mods;
    const std::string src = g.source, ov = g.overlay;
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread([=]() {
        std::vector<mods::ModSource> on;
        for (const auto& m : list)
            if (m.enabled) on.push_back(m);
        set_status("Reading the game folder...");
        std::vector<std::string> roots{app0};
        const std::string up = mods::find_update_folder(app0);
        if (!up.empty()) roots.push_back(up);
        mods::GameIndex idx;
        idx.build(roots, {ov, src}, [](const std::string& l) { set_status(l); });
        set_status("Matching the mods' files...");
        auto plan = std::make_shared<mods::Plan>(mods::make_plan(on, idx, ov));
        auto env = std::make_shared<mods::MergeEnv>();
        env->paramdef = mods::load_paramdef(idx);
        if (env->paramdef.empty()) say("Note: the game's paramdef was not found, so gameparam files are not merged (the later mod wins whole).");
        for (const auto& [mod, r] : plan->ambiguous) {
            std::string c;
            for (const auto& x : r.cands) c += (c.empty() ? "" : " | ") + x;
            say(std::string("Skipped (several candidates): ") + mod + ": " + r.ref.rel() + "  -> " + c);
        }
        set_status("Checking where mods overlap...");
        mods::Preview pv = mods::preview_merges(*plan, *env, nullptr);
        for (const std::string& l : pv.diagnostics()) say(l);
        {
            std::lock_guard<std::mutex> lk(g.mu);
            g.plan = plan;
            g.env = env;
            g.conflict_lines = pv.conflict_lines();
        }
        g.phase = pv.has_conflicts() ? Page::Confirm : Page::Applying;
        if (!pv.has_conflicts()) {
            // Nothing to ask: on to the apply, in this same thread.
            set_status("Applying...");
            std::error_code ec;
            fs::create_directories(fs::u8path(ov), ec);
            const mods::ApplyStats st = mods::apply_plan(*plan, (fs::u8path(src) / ".bbmods").u8string(), *env, [](const std::string& l) { say(l); },
                                                         [](int d, int t) { g.done = d; g.total = t; });
            char b[200];
            std::snprintf(b, sizeof(b), "%s: %d  |  %s: %d  |  %s: %d  |  %s: %d", "written", st.copied, "merged", st.merged,
                          "unchanged", st.same, "errors", st.error);
            std::lock_guard<std::mutex> lk(g.mu);
            g.summary = b;
            g.status = "Done.";
            g.phase = Page::Idle;
        }
        g.running = false;
    });
}

void continue_apply() {
    std::shared_ptr<mods::Plan> plan;
    std::shared_ptr<mods::MergeEnv> env;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        plan = g.plan;
        env = g.env;
    }
    if (!plan || !env) {
        g.phase = Page::Idle;
        return;
    }
    g.running = true;
    g.phase = Page::Applying;
    const std::string src = g.source, ov = g.overlay;
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread([=]() {
        set_status("Applying...");
        std::error_code ec;
        fs::create_directories(fs::u8path(ov), ec);
        const mods::ApplyStats st = mods::apply_plan(*plan, (fs::u8path(src) / ".bbmods").u8string(), *env, [](const std::string& l) { say(l); },
                                                     [](int d, int t) { g.done = d; g.total = t; });
        char b[200];
        std::snprintf(b, sizeof(b), "%s: %d  |  %s: %d  |  %s: %d  |  %s: %d  |  %s: %d", "written", st.copied, "merged", st.merged,
                      "conflicts", st.conflict, "unchanged", st.same, "errors", st.error);
        std::lock_guard<std::mutex> lk(g.mu);
        g.summary = b;
        g.status = "Done.";
        g.phase = Page::Idle;
        g.running = false;
    });
}

void start_revert() {
    g.running = true;
    g.phase = Page::Reverting;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.log.clear();
        g.summary.clear();
    }
    const std::string sd = state_dir();
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread([=]() {
        set_status("Reverting...");
        const int n = mods::revert_all(sd, [](const std::string& l) { say(l); });
        std::lock_guard<std::mutex> lk(g.mu);
        g.summary = std::string("Files restored or removed: ") + std::to_string(n);
        g.status = "Done.";
        g.phase = Page::Idle;
        g.running = false;
    });
}

const char* folder_problem_text(mods::FolderProblem p) {
    switch (p) {
        case mods::FolderProblem::OverlayEmpty: return "Choose the overlay folder.";
        case mods::FolderProblem::ModsEmpty: return "Choose the mods folder.";
        case mods::FolderProblem::OverlayInsideOrContainsMods:
            return "The overlay folder must not be the mods folder, or inside it or around it.";
        case mods::FolderProblem::OverlayInsideOrContainsGame:
            return "The overlay folder must not be the game folder, or inside it or around it.";
        default: return "";
    }
}

}  // namespace

void mods_page_drop_file(const std::string& path) {
    std::lock_guard<std::mutex> lk(g.mu);
    g.dropped.push_back(path);
}

bool mods_page_busy() { return g.running || g.phase == Page::Confirm; }

void mods_page_draw(SDL_Window* win, const std::string& app0_in, const std::string& data, float scale, float label_w) {
    if (!g.inited) {
        load_settings(data);
        refresh_list();
    }
    join_worker();
    const std::string app0 = app0_in;
    const ImGuiStyle& st = ImGui::GetStyle();
    const bool busy = g.running || g.phase == Page::Confirm;
    const float w = ImGui::GetContentRegionAvail().x;

    // Answers from the pickers and files dropped on the window.
    {
        int what = 0;
        std::vector<std::string> got = take_picked(&what);
        if (what == 1 && !got.empty() && !busy) {
            std::snprintf(g.source, sizeof(g.source), "%s", generic(got[0]).c_str());
            save_setting("mods", "source", g.source);
            refresh_list();
        } else if (what == 2 && !got.empty() && !busy) {
            std::snprintf(g.overlay, sizeof(g.overlay), "%s", generic(got[0]).c_str());
            save_setting("paths", "mods", g.overlay);
        } else if ((what == 3 || what == 4) && !got.empty()) {
            import_paths(got);
        }
        std::vector<std::string> drops;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            drops.swap(g.dropped);
        }
        if (!drops.empty()) import_paths(drops);
    }

    std::string bb_msg;
    const bool game_ok = mods::check_bloodborne(app0, &bb_msg);
    const mods::FolderProblem problem = mods::check_folders(app0, g.overlay, g.source);

    // ---- folders
    ui::card_begin("##mods_folders", ImVec2(w, 0), "FOLDERS");
    auto folder_row = [&](const char* label, char* buf, size_t cap, int pick_id, const char* hint, const char* tip, const char* key_section, const char* key) {
        ImGui::PushID(pick_id);
        ui::row_label(label, label_w);
        ui::tooltip_for_item(nullptr, tip);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - st.ItemSpacing.x);
        ImGui::BeginDisabled(busy);
        ImGui::InputTextWithHint("##p", hint, buf, cap);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            save_setting(key_section, key, buf);
            if (pick_id == 1) refresh_list();
        }
        ImGui::SameLine();
        if (ui::icon_button("##pick", ui::Icon::Folder, ImGui::GetFrameHeight(), "Choose a folder")) {
            const std::string at = buf;
            SDL_ShowOpenFolderDialog(on_pick, reinterpret_cast<void*>(static_cast<std::intptr_t>(pick_id)), win, at.empty() ? nullptr : at.c_str(), false);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    };
    folder_row("Mods folder", g.source, sizeof(g.source), 1, "",
               "Each sub-folder or .zip in here is one mod.", "mods", "source");
    folder_row("Overlay folder", g.overlay, sizeof(g.overlay), 2, "",
               "Where the mods' files are written: bbhost's paths.mods. The game folder is never touched.", "paths", "mods");
    if (!game_ok) ui::banner(ui::Icon::Warning, ui::kWarn, "Set the game folder on the Game page first.");
    else if (problem != mods::FolderProblem::None) ui::banner(ui::Icon::Warning, ui::kWarn, folder_problem_text(problem));
    ui::card_end();
    ImGui::Dummy(ImVec2(0, 8 * scale));

    // ---- the mods
    ui::card_begin("##mods_list", ImVec2(w, 0), "MODS");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::kTextDim));
    ImGui::TextWrapped("%s", "Mods that are on are applied from the top down; a mod lower in the list wins where two change the same thing. Drop a .zip or a mod folder on this window to add it.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4 * scale));

    bool changed = false;
    int move_from = -1, move_to = -1;
    if (g.mods.empty()) {
        ui::banner(ui::Icon::Info, ui::kIce, "No mods yet. Add a .zip or a folder.");
    }
    ImGui::BeginDisabled(busy);
    for (std::size_t i = 0; i < g.mods.size(); ++i) {
        mods::ModSource& m = g.mods[i];
        ImGui::PushID(static_cast<int>(i));
        bool on = m.enabled;
        if (ui::toggle("##on", &on)) {
            m.enabled = on;
            changed = true;
        }
        ImGui::SameLine(0, 10 * scale);
        ImGui::AlignTextToFramePadding();
        const float arrows = ImGui::GetFrameHeight() * 2 + st.ItemSpacing.x * 2;
        const float pills = 150 * scale;
        ImGui::TextUnformatted(ui::fit_text(m.name, ImGui::GetContentRegionAvail().x - arrows - pills).c_str());
        ui::tooltip_for_item(nullptr, m.path);
        ImGui::SameLine(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - arrows - pills));
        ui::pill(m.is_zip ? "zip" : "folder", ui::kTextDim, ui::alpha(ui::kViolet, 40));
        if (g.applied.count(m.name)) {
            ImGui::SameLine();
            ui::pill("applied", ui::kGood, ui::alpha(ui::kGood, 36));
        }
        ImGui::SameLine(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - arrows));
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { move_from = static_cast<int>(i); move_to = static_cast<int>(i) - 1; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 == g.mods.size());
        if (ImGui::ArrowButton("##dn", ImGuiDir_Down)) { move_from = static_cast<int>(i); move_to = static_cast<int>(i) + 1; }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (move_from >= 0) {
        std::swap(g.mods[static_cast<std::size_t>(move_from)], g.mods[static_cast<std::size_t>(move_to)]);
        changed = true;
    }
    if (changed) save_list();
    ImGui::Dummy(ImVec2(0, 4 * scale));
    if (ui::ghost_button("Add zip...", ui::Icon::Download)) {
        static const SDL_DialogFileFilter kZip[] = {{"Zip files", "zip"}};
        SDL_ShowOpenFileDialog(on_pick, reinterpret_cast<void*>(static_cast<std::intptr_t>(4)), win, kZip, 1, nullptr, true);
    }
    ImGui::SameLine();
    if (ui::ghost_button("Add folder...", ui::Icon::Folder))
        SDL_ShowOpenFolderDialog(on_pick, reinterpret_cast<void*>(static_cast<std::intptr_t>(3)), win, nullptr, true);
    ImGui::SameLine();
    if (ui::ghost_button("Refresh", ui::Icon::Refresh)) refresh_list();
    ImGui::SameLine();
    if (ui::ghost_button("Open mods folder", ui::Icon::Folder)) {
        std::error_code ec;
        fs::create_directories(fs::u8path(g.source), ec);
        SDL_OpenURL(url_of(g.source).c_str());
    }
    ImGui::EndDisabled();
    ui::card_end();
    ImGui::Dummy(ImVec2(0, 8 * scale));

    // ---- actions
    ui::card_begin("##mods_actions", ImVec2(w, 0), "APPLY");
    const bool can = game_ok && problem == mods::FolderProblem::None && !busy;
    if (ui::primary_button("Apply", ImVec2(130 * scale, 0), can)) start_apply(app0, data);
    ImGui::SameLine();
    ImGui::BeginDisabled(!(game_ok && !busy && g.source[0]));
    if (ui::ghost_button("Revert all")) g.confirm_revert = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::ghost_button("View merge log")) g.show_log = true;
    ImGui::Dummy(ImVec2(0, 4 * scale));
    {
        std::string status, summary;
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            status = g.status;
            summary = g.summary;
            lines = g.log;
        }
        if (g.running) {
            const int t = g.total, d = g.done;
            char b[64];
            std::snprintf(b, sizeof(b), "%d / %d", d, t);
            ui::progress(t > 0 ? static_cast<float>(d) / static_cast<float>(t) : 0.0f, t > 0 ? b : status.c_str());
            if (t > 0) ImGui::TextUnformatted(status.c_str());
        } else if (!summary.empty()) {
            ui::banner(ui::Icon::Check, ui::kGood, summary);
        }
        if (!lines.empty()) {
            ImGui::BeginChild("##modlog", ImVec2(0, 150 * scale), ImGuiChildFlags_Borders);
            for (const std::string& l : lines) ImGui::TextWrapped("%s", l.c_str());
            if (g.running) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
    }
    ui::card_end();

    // ---- the overlap confirmation (nothing is written until "Apply as is")
    if (g.phase == Page::Confirm && !ImGui::IsPopupOpen("##conflicts")) ImGui::OpenPopup("##conflicts");
    if (ImGui::BeginPopupModal("##conflicts", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted("These mods overlap");
        ImGui::TextWrapped("%s", "Nothing has been written yet. Where two mods change the same thing, the one lower in the list wins.");
        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> lk(g.mu);
            lines = g.conflict_lines;
        }
        ImGui::BeginChild("##conflict_lines", ImVec2(std::min(w, 760.0f * scale), 300 * scale), ImGuiChildFlags_Borders);
        const std::size_t kMax = 400;
        for (std::size_t i = 0; i < lines.size() && i < kMax; ++i) ImGui::TextWrapped("%s", lines[i].c_str());
        if (lines.size() > kMax) ImGui::Text("... +%zu", lines.size() - kMax);
        ImGui::EndChild();
        if (ui::primary_button("Apply as is", ImVec2(160 * scale, 0), true)) {
            ImGui::CloseCurrentPopup();
            continue_apply();
        }
        ImGui::SameLine();
        if (ui::ghost_button("Cancel")) {
            ImGui::CloseCurrentPopup();
            g.phase = Page::Idle;
            set_status("Cancelled; nothing was written.");
        }
        ImGui::EndPopup();
    }

    // ---- revert
    if (g.confirm_revert) {
        ImGui::OpenPopup("##revert");
        g.confirm_revert = false;
    }
    if (ImGui::BeginPopupModal("##revert", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", "Remove every file the loader wrote to the overlay folder, and put back the ones it replaced?");
        if (ui::primary_button("Revert all", ImVec2(160 * scale, 0), true)) {
            ImGui::CloseCurrentPopup();
            start_revert();
        }
        ImGui::SameLine();
        if (ui::ghost_button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- the merge log
    if (g.show_log) {
        ImGui::OpenPopup("##mergelog");
        g.show_log = false;
    }
    if (ImGui::BeginPopupModal("##mergelog", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        const std::string text = mods::read_merge_log(state_dir());
        ImGui::BeginChild("##mergelog_text", ImVec2(std::min(w, 760.0f * scale), 360 * scale), ImGuiChildFlags_Borders);
        if (text.empty()) ImGui::TextUnformatted("The log is empty: no merge or conflict has happened yet.");
        else ImGui::TextUnformatted(text.c_str());
        ImGui::EndChild();
        if (ui::ghost_button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
