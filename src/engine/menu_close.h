#pragma once

// A menu window that closes answers at once, not after its fade-out.
//
// Every window of the game's menus that closes on a choice - the title's, the
// pause grid, the inventory and its lists, an item's command menu - shares
// one Close (sub_1ed6820, vtable slot 8 of each; addresses are Binary
// Ninja's). It plays the movie's "FadeOut" (or "Disabled_FadeOut") and queues
// two tasks on the window: wait while the root clip is still playing that
// label (sub_1ed7580, a functor nothing but Close makes - Scaleform's play
// state and current label), then write the answer at +0x1c0 that the
// window's owner is waiting for. The windows' common update (sub_1ed5b10)
// reads no input while a task is current (+0x10) or queued (+0x40). So a
// choice that closes a window takes effect only once the fade has played -
// 6 frames of a 60 fps movie, 100 ms, twice that in the 30 fps ones - and a
// press in that time is lost: Circle twice, 100 ms apart, closed the inventory
// and not the pause grid behind it.
//
// The wait is answered "not playing" from the start, so the answer lands on
// the frame of the press, the window it returns to takes input on the next
// one, and that second Circle closes the pause grid. The closing window goes
// when its owner lets it go, so its fade-out is not seen; the window behind
// it, or the next one, fades in as before and takes input while it does.
// Windows that end without Close never waited (the quantity and Yes/No box:
// 2 frames either way).
//
// BBHOST_MENU_FADE_WAIT=1 keeps the game's wait. BBHOST_MENU_LOG=1 logs each
// Close and the frame its answer lands.

struct ElfImage;

void menu_close_install(ElfImage* image);
