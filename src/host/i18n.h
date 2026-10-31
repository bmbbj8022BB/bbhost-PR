// The interface language. English is the text in the source; Korean ("ko") is
// a table of English -> Korean pairs (lang/ko.po, embedded as i18n_ko.inc and
// overridden by <exe dir>/lang/ko.po when that file exists), looked up by the
// English text itself, so an untranslated string simply stays English.
#pragma once

#include <string>

namespace i18n {

// "en" (or anything unknown): English. "ko": the Korean table.
void set_language(const std::string& lang);
const std::string& language();
bool active();  // a table is in use (not English)

// Plain text (Text, TextWrapped, tooltips, hints, combo items): the
// translation, or `en` itself. The returned pointer stays valid.
const char* tr(const char* en);
// A widget label (Button, Checkbox, tab, InputText, Combo): the translation
// with "##<English>" after it, so the widget's ImGui id does not depend on
// the language and two labels that translate alike do not collide. A label
// that already has a "##suffix" keeps that suffix.
const char* trl(const char* en);
// For a string built at run time: its translation if the table has the whole
// string, else itself.
std::string trs(const std::string& en);
// tr(fmt) through vsnprintf: the Korean format must take the same arguments
// in the same order.
std::string tf(const char* fmt, ...);

// A font file that has Hangul: `configured` (ui.font) if it exists, else the
// system's usual ones (Malgun Gothic on Windows, Noto/Nanum on Linux); "" when
// none is found.
std::string find_font(const std::string& configured);

}  // namespace i18n
