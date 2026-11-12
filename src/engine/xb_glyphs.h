#pragma once

// The Xbox button pictures (KG_<name>_XB) the pad-glyph switch in key_prompts.cpp shows when an Xbox-style pad
// is in use. The game has none, so at start this makes a copy of its menu/common.tpf.dcx with the 20 pictures
// appended (what tools/make_xb_tpf.py does) in <data>/bbhost/xb-assets and mounts that as a generated overlay.
// The pictures are in the exe (xb_glyphs.inc, made by tools/xb_pack_to_inc.py).
void xb_glyphs_install(const char* data);
// The pad at start is an Xbox one (or not); the made file goes in only for an Xbox one.
void xb_glyphs_pad(bool xbox);
