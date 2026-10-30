#pragma once

// Network > Matching Regions starts at Worldwide.
//
// The option is a byte of the game's settings object (the global at
// 0x5962878, +0xd10; addresses are Binary Ninja's): 1 Worldwide (message
// 0x9d80), 0 the other choice (0x9d81) - the Network section's handler,
// sub_1f21e80, builds the row from it. The game writes 0 in two places: the
// object's constructor (0x1b642cc, `mov byte [r15+0xd10], 0`) and a reset in
// sub_2035b50 (0x2036285, `mov byte [rax+0xd10], 0`). Both write 1 here, so a
// new profile and every reset start at Worldwide; the player can still pick
// the other region in the menu. Only on the 1.09 eboot, and only where the
// bytes are the expected ones. BBHOST_MATCHING_REGION=game keeps the game's 0.

struct ElfImage;

void matching_region_install(ElfImage* image);
