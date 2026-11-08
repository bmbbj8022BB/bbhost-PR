#include "host/i18n.h"

#include "core/config.h"
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace i18n {
namespace {

struct Pair {
    const char* en;
    const char* ko;
};
const Pair kKo[] = {
#include "host/i18n_ko.inc"
    {nullptr, nullptr}};

std::mutex g_mu;
std::string g_lang = "en";
bool g_on = false;
std::unordered_map<std::string, std::string> g_table;   // English -> translation
std::unordered_map<std::string, std::string> g_labels;  // English label -> "translation##id"

std::string unescape(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            out += s[i];
        }
    }
    return out;
}

// One quoted PO string after `key`: msgid "..." -> the text, or false.
bool po_value(const std::string& line, const char* key, std::string* out) {
    const std::size_t k = std::char_traits<char>::length(key);
    if (line.compare(0, k, key) != 0) return false;
    const std::size_t a = line.find('"', k), b = line.rfind('"');
    if (a == std::string::npos || b == std::string::npos || b <= a) return false;
    *out = unescape(line.substr(a + 1, b - a - 1));
    return true;
}

void load_po(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::string line, id;
    std::size_t n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string v;
        if (po_value(line, "msgid", &v)) {
            id = v;
        } else if (po_value(line, "msgstr", &v)) {
            if (!id.empty() && !v.empty()) {
                g_table[id] = v;
                ++n;
            }
            id.clear();
        }
    }
    host_log("i18n: %zu strings from %s", n, path.c_str());
}

bool exists(const std::string& p) {
    std::error_code ec;
    return !p.empty() && std::filesystem::is_regular_file(p, ec);
}

}  // namespace

void set_language(const std::string& lang) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_lang = lang == "ko" ? "ko" : "en";
    g_on = g_lang == "ko";
    g_table.clear();
    g_labels.clear();
    if (!g_on) return;
    for (const Pair* p = kKo; p->en; ++p) g_table[p->en] = p->ko;
    load_po(config_exe_dir() + "/lang/ko.po");
}

const std::string& language() { return g_lang; }
bool active() { return g_on; }

const char* tr(const char* en) {
    if (!g_on || !en || !*en) return en;
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_table.find(en);
    return it == g_table.end() ? en : it->second.c_str();
}

const char* trl(const char* en) {
    if (!g_on || !en || !*en) return en;
    std::lock_guard<std::mutex> lk(g_mu);
    const auto cached = g_labels.find(en);
    if (cached != g_labels.end()) return cached->second.c_str();
    const std::string label = en;
    const std::size_t hash = label.find("##");
    const std::string base = label.substr(0, hash);
    const auto it = g_table.find(base);
    std::string made = label;
    if (it != g_table.end()) {
        made = it->second + (hash == std::string::npos ? "##" + base : label.substr(hash));
    }
    return g_labels.emplace(label, made).first->second.c_str();
}

std::string trs(const std::string& en) {
    if (!g_on) return en;
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_table.find(en);
    return it == g_table.end() ? en : it->second;
}

std::string tf(const char* fmt, ...) {
    const char* f = tr(fmt);
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, f, ap);
    va_end(ap);
    std::string out(n > 0 ? static_cast<std::size_t>(n) : 0, '\0');
    if (n > 0) std::vsnprintf(out.data(), out.size() + 1, f, ap2);
    va_end(ap2);
    return out;
}

std::string find_font(const std::string& configured) {
    std::vector<std::string> cands;
    if (!configured.empty()) cands.push_back(configured);
#if defined(_WIN32)
    const char* win = std::getenv("WINDIR");
    const std::string dir = std::string(win ? win : "C:/Windows") + "/Fonts/";
    for (const char* f : {"malgun.ttf", "malgunsl.ttf", "gulim.ttc", "batang.ttc"}) cands.push_back(dir + f);
#else
    for (const char* f : {"/usr/share/fonts/truetype/nanum/NanumGothic.ttf", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
                          "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc"})
        cands.push_back(f);
#endif
    for (const std::string& c : cands) {
        if (exists(c)) return c;
    }
    return "";
}

}  // namespace i18n
