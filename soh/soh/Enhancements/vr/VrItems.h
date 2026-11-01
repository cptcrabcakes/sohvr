#pragma once

// The item menu that replaces the item and equipment screens: Y opens an upright ring of item models in front of
// the player, with rings inside rings. Gear swaps on the spot; usable items go into the hand that chose them,
// through the game's own item code.

struct PlayState;

namespace SohVr {

// Per rendered frame during gameplay, after the runtime has new poses: opens, browses and closes the menu. On the
// screen (places with a fixed camera) the ring is drawn into the game's view and chosen from by pointing at it.
void Items_OnFrame(bool onScreen);
void Items_Close();
// Per game tick, while the world is drawn: the menu's item models.
void Items_Draw(PlayState* play);
// Per game tick, before the game reads input: applies a chosen piece of gear.
void Items_ApplyPending();
// Whether the menu is open, so that hand's grip isn't also a grab or a throw.
bool Items_MenuOpen();
// The item-get model the menu shows for an item.
short Items_ItemModel(unsigned char item);

} // namespace SohVr
