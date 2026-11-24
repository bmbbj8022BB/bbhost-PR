#include "host/mods_page.h"

#include "core/config.h"
#include "host/launcher_ui.h"
#include "mods/fs_utf8.h"
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

// Read once, on the main thread: the jobs' threads use L() too, and the config's map is not theirs to read.
std::atomic<int> g_ko{-1};
bool korean() { return g_ko.load() == 1; }
// The window's two languages: English, and Korean when [ui] language says so.
const char* L(const char* en, const char* ko) { return korean() ? ko : en; }

std::string u8(const fs::path& p) { return mods::path_to_utf8(p); }
std::string generic(const std::string& p) { return mods::path_to_generic_utf8(mods::path_from_utf8(p)); }
std::string toml_str(const std::string& p) { return "\"" + generic(p) + "\""; }

std::string absolute_from_config(const std::string& p) {
    if (p.empty()) return p;
    fs::path x = mods::path_from_utf8(p);
    if (x.is_relative()) x = mods::path_from_utf8(config_user_file()).parent_path() / x;
    return u8(x.lexically_normal());
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
std::string state_dir() { return u8(mods::path_from_utf8(g.source) / ".bbmods"); }

void save_setting(const char* section, const char* key, const std::string& path) {
    if (config_set_values(config_user_file(), {{section, key, toml_str(path)}})) config_value_set(std::string(section) + "." + key, toml_str(path));
}

void load_settings(const std::string& data) {
    const std::string d = data_dir_of(data);
    std::string src = absolute_from_config(config_value("mods.source"));
    if (src.empty()) src = u8(mods::path_from_utf8(d) / "mod-sources");
    std::string ov = config().mods;
    if (ov.empty()) ov = absolute_from_config(config_value("paths.mods"));
    if (ov.empty()) ov = u8(mods::path_from_utf8(d) / "mods");
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
    say(L("Adding mods...", "모드를 추가하는 중..."));
    const auto added = mods::import_mods(paths, g.source, [](const std::string& l) { say(l); });
    refresh_list();
    if (!added.empty()) set_status(std::string(L("Added: ", "추가됨: ")) + std::to_string(added.size()));
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
        set_status(L("Reading the game folder...", "게임 폴더를 읽는 중..."));
        std::vector<std::string> roots{app0};
        const std::string up = mods::find_update_folder(app0);
        if (!up.empty()) roots.push_back(up);
        mods::GameIndex idx;
        idx.build(roots, {ov, src}, [](const std::string& l) { set_status(l); });
        set_status(L("Matching the mods' files...", "모드 파일을 게임 파일과 맞추는 중..."));
        auto plan = std::make_shared<mods::Plan>(mods::make_plan(on, idx, ov));
        auto env = std::make_shared<mods::MergeEnv>();
        env->paramdef = mods::load_paramdef(idx);
        if (env->paramdef.empty()) say(L("Note: the game's paramdef was not found, so gameparam files are not merged (the later mod wins whole).",
                                         "알림: 게임의 paramdef를 찾지 못해 gameparam은 병합하지 않고 뒤쪽 모드가 통째로 적용됩니다."));
        for (const auto& [mod, r] : plan->ambiguous) {
            std::string c;
            for (const auto& x : r.cands) c += (c.empty() ? "" : " | ") + x;
            say(std::string(L("Skipped (several candidates): ", "건너뜀(후보가 여러 개): ")) + mod + ": " + r.ref.rel() + "  -> " + c);
        }
        set_status(L("Checking where mods overlap...", "모드가 겹치는 곳을 확인하는 중..."));
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
            set_status(L("Applying...", "적용하는 중..."));
            std::error_code ec;
            fs::create_directories(mods::path_from_utf8(ov), ec);
            const mods::ApplyStats st = mods::apply_plan(*plan, u8(mods::path_from_utf8(src) / ".bbmods"), *env, [](const std::string& l) { say(l); },
                                                         [](int d, int t) { g.done = d; g.total = t; });
            char b[200];
            std::snprintf(b, sizeof(b), "%s: %d  |  %s: %d  |  %s: %d  |  %s: %d", L("written", "기록"), st.copied, L("merged", "병합"), st.merged,
                          L("unchanged", "변경 없음"), st.same, L("errors", "오류"), st.error);
            std::lock_guard<std::mutex> lk(g.mu);
            g.summary = b;
            g.status = L("Done.", "완료.");
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
        set_status(L("Applying...", "적용하는 중..."));
        std::error_code ec;
        fs::create_directories(mods::path_from_utf8(ov), ec);
        const mods::ApplyStats st = mods::apply_plan(*plan, u8(mods::path_from_utf8(src) / ".bbmods"), *env, [](const std::string& l) { say(l); },
                                                     [](int d, int t) { g.done = d; g.total = t; });
        char b[200];
        std::snprintf(b, sizeof(b), "%s: %d  |  %s: %d  |  %s: %d  |  %s: %d  |  %s: %d", L("written", "기록"), st.copied, L("merged", "병합"), st.merged,
                      L("conflicts", "충돌"), st.conflict, L("unchanged", "변경 없음"), st.same, L("errors", "오류"), st.error);
        std::lock_guard<std::mutex> lk(g.mu);
        g.summary = b;
        g.status = L("Done.", "완료.");
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
        set_status(L("Reverting...", "되돌리는 중..."));
        const int n = mods::revert_all(sd, [](const std::string& l) { say(l); });
        std::lock_guard<std::mutex> lk(g.mu);
        g.summary = std::string(L("Files restored or removed: ", "복원하거나 지운 파일: ")) + std::to_string(n);
        g.status = L("Done.", "완료.");
        g.phase = Page::Idle;
        g.running = false;
    });
}

const char* folder_problem_text(mods::FolderProblem p) {
    switch (p) {
        case mods::FolderProblem::OverlayEmpty: return L("Choose the overlay folder.", "오버레이 폴더를 지정하세요.");
        case mods::FolderProblem::ModsEmpty: return L("Choose the mods folder.", "모드 폴더를 지정하세요.");
        case mods::FolderProblem::OverlayInsideOrContainsMods:
            return L("The overlay folder must not be the mods folder, or inside it or around it.", "오버레이 폴더는 모드 폴더와 같거나 그 안/밖에 있으면 안 됩니다.");
        case mods::FolderProblem::OverlayInsideOrContainsGame:
            return L("The overlay folder must not be the game folder, or inside it or around it.", "오버레이 폴더는 게임 폴더와 같거나 그 안/밖에 있으면 안 됩니다.");
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
    if (g_ko.load() < 0) g_ko = config_value("ui.language") == "ko" ? 1 : 0;
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
    ui::card_begin("##mods_folders", ImVec2(w, 0), L("FOLDERS", "폴더"));
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
        if (ui::icon_button("##pick", ui::Icon::Folder, ImGui::GetFrameHeight(), L("Choose a folder", "폴더 선택"))) {
            const std::string at = buf;
            SDL_ShowOpenFolderDialog(on_pick, reinterpret_cast<void*>(static_cast<std::intptr_t>(pick_id)), win, at.empty() ? nullptr : at.c_str(), false);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    };
    folder_row(L("Mods folder", "모드 폴더"), g.source, sizeof(g.source), 1, "", 
               L("Each sub-folder or .zip in here is one mod.", "이 안의 하위 폴더 또는 .zip 하나가 모드 하나입니다."), "mods", "source");
    folder_row(L("Overlay folder", "오버레이 폴더"), g.overlay, sizeof(g.overlay), 2, "",
               L("Where the mods' files are written: bbhost's paths.mods. The game folder is never touched.",
                 "모드 파일이 복사되는 곳(bbhost의 paths.mods)입니다. 게임 폴더는 건드리지 않습니다."), "paths", "mods");
    if (!game_ok) ui::banner(ui::Icon::Warning, ui::kWarn, L("Set the game folder on the Game page first.", "먼저 Game 페이지에서 게임 폴더를 지정하세요."));
    else if (problem != mods::FolderProblem::None) ui::banner(ui::Icon::Warning, ui::kWarn, folder_problem_text(problem));
    ui::card_end();
    ImGui::Dummy(ImVec2(0, 8 * scale));

    // ---- the mods
    ui::card_begin("##mods_list", ImVec2(w, 0), L("MODS", "모드"));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(ui::kTextDim));
    ImGui::TextWrapped("%s", L("Mods that are on are applied from the top down; a mod lower in the list wins where two change the same thing. Drop a .zip or a mod folder on this window to add it.",
                               "켜진 모드는 위에서 아래로 적용되며, 같은 곳을 바꾸면 아래쪽 모드가 이깁니다. .zip이나 모드 폴더를 이 창에 끌어다 놓으면 추가됩니다."));
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4 * scale));

    bool changed = false;
    int move_from = -1, move_to = -1;
    if (g.mods.empty()) {
        ui::banner(ui::Icon::Info, ui::kIce, L("No mods yet. Add a .zip or a folder.", "아직 모드가 없습니다. .zip이나 폴더를 추가하세요."));
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
        ui::pill(m.is_zip ? "zip" : L("folder", "폴더"), ui::kTextDim, ui::alpha(ui::kViolet, 40));
        if (g.applied.count(m.name)) {
            ImGui::SameLine();
            ui::pill(L("applied", "적용됨"), ui::kGood, ui::alpha(ui::kGood, 36));
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
    if (ui::ghost_button(L("Add zip...", "zip 추가..."), ui::Icon::Download)) {
        static const SDL_DialogFileFilter kZip[] = {{"Zip files", "zip"}};
        SDL_ShowOpenFileDialog(on_pick, reinterpret_cast<void*>(static_cast<std::intptr_t>(4)), win, kZip, 1, nullptr, true);
    }
    ImGui::SameLine();
    if (ui::ghost_button(L("Add folder...", "폴더 추가..."), ui::Icon::Folder))
        SDL_ShowOpenFolderDialog(on_pick, reinterpret_cast<void*>(static_cast<std::intptr_t>(3)), win, nullptr, true);
    ImGui::SameLine();
    if (ui::ghost_button(L("Refresh", "새로고침"), ui::Icon::Refresh)) refresh_list();
    ImGui::SameLine();
    if (ui::ghost_button(L("Open mods folder", "모드 폴더 열기"), ui::Icon::Folder)) {
        std::error_code ec;
        fs::create_directories(mods::path_from_utf8(g.source), ec);
        SDL_OpenURL(url_of(g.source).c_str());
    }
    ImGui::EndDisabled();
    ui::card_end();
    ImGui::Dummy(ImVec2(0, 8 * scale));

    // ---- actions
    ui::card_begin("##mods_actions", ImVec2(w, 0), L("APPLY", "적용"));
    const bool can = game_ok && problem == mods::FolderProblem::None && !busy;
    if (ui::primary_button(L("Apply", "적용"), ImVec2(130 * scale, 0), can)) start_apply(app0, data);
    ImGui::SameLine();
    ImGui::BeginDisabled(!(game_ok && !busy && g.source[0]));
    if (ui::ghost_button(L("Revert all", "모두 원래대로"))) g.confirm_revert = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::ghost_button(L("View merge log", "병합 로그 보기"))) g.show_log = true;
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
        ImGui::TextUnformatted(L("These mods overlap", "겹치는 부분이 있습니다"));
        ImGui::TextWrapped("%s", L("Nothing has been written yet. Where two mods change the same thing, the one lower in the list wins.",
                                   "아직 아무것도 기록하지 않았습니다. 같은 곳을 바꾸면 목록에서 아래쪽 모드가 이깁니다."));
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
        if (ui::primary_button(L("Apply as is", "그대로 적용"), ImVec2(160 * scale, 0), true)) {
            ImGui::CloseCurrentPopup();
            continue_apply();
        }
        ImGui::SameLine();
        if (ui::ghost_button(L("Cancel", "취소"))) {
            ImGui::CloseCurrentPopup();
            g.phase = Page::Idle;
            set_status(L("Cancelled; nothing was written.", "취소했습니다. 아무것도 기록하지 않았습니다."));
        }
        ImGui::EndPopup();
    }

    // ---- revert
    if (g.confirm_revert) {
        ImGui::OpenPopup("##revert");
        g.confirm_revert = false;
    }
    if (ImGui::BeginPopupModal("##revert", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextWrapped("%s", L("Remove every file the loader wrote to the overlay folder, and put back the ones it replaced?",
                                   "로더가 오버레이 폴더에 기록한 파일을 모두 지우고, 덮어썼던 파일은 되돌릴까요?"));
        if (ui::primary_button(L("Revert all", "모두 원래대로"), ImVec2(160 * scale, 0), true)) {
            ImGui::CloseCurrentPopup();
            start_revert();
        }
        ImGui::SameLine();
        if (ui::ghost_button(L("Cancel", "취소"))) ImGui::CloseCurrentPopup();
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
        if (text.empty()) ImGui::TextUnformatted(L("The log is empty: no merge or conflict has happened yet.", "로그가 비어 있습니다. 아직 병합이나 충돌이 없었습니다."));
        else ImGui::TextUnformatted(text.c_str());
        ImGui::EndChild();
        if (ui::ghost_button(L("Close", "닫기"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
