#pragma once

// The item ring for Majora's Mask: Y opens an upright ring of item models in front of the player, with rings inside
// rings, and either hand chooses from it by touching or pointing and squeezing. Majora's Mask uses items from its C
// buttons, and takes a mask off once it's on none of them, so a chosen item goes on the C button of the hand that
// chose it (C-left for the left hand, C-right for the right), where that hand's trigger uses it. A chosen mask goes
// on C-down and stays in the hand until it's brought to the face, which puts it on; squeezing a grip at the face takes
// the mask (or a transformation) off again, into that hand.

#include "VrRuntime.h"

struct PlayState;

namespace SohVr {

// Per rendered frame while the world is shown: opens, browses and closes the ring.
void Items_OnFrame();
void Items_Close();
// Per game tick, while the world is drawn: the ring's item models.
void Items_Draw(PlayState* play);
// Per game tick, before the game reads input: puts a chosen item on its C button, and the buttons it presses.
uint16_t Items_TakeButtons();
// Whether the ring is open, so the hands' grips and triggers choose from it instead of doing anything else.
bool Items_MenuOpen();
// Whether a hand is at the face, where its grip takes off or puts on a mask instead of doing anything else.
bool Items_HandAtFace(Hand hand);

} // namespace SohVr
