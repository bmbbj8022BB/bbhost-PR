// The setup window's Mods page: the mods folder's list (on/off, order), Apply,
// Revert all and the merge log, over mods/mod_loader.h. Mods are written to
// the overlay folder (paths.mods) before the game starts, so Play needs no
// restart. The settings it keeps in bbhost.toml: [mods] source (the folder the
// mods are in) and [paths] mods (the overlay).
#pragma once

#include <string>

struct SDL_Window;

// Draws the page inside the setup window's page child. `app0` is the game
// folder as the Game page has it, `data` the data folder field ("" for the
// default), `label_w` the width the pages give a row's label.
void mods_page_draw(SDL_Window* win, const std::string& app0, const std::string& data, float scale, float label_w);
// A file or folder dropped on the window while the page is open.
void mods_page_drop_file(const std::string& path);
// True while an apply or revert runs (or waits for a confirmation): the window
// must not start the game then.
bool mods_page_busy();
