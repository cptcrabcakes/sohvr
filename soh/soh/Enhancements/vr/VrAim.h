#pragma once

// Aiming aids drawn in the world: where an arrow or seed will land, the path it will take, and the hookshot's laser
// and sight, green where it will catch.

extern "C" {
#include "global.h"
}

namespace SohVr {

// Draws the landing marker (and, if turned on, the flight path) of the bow or slingshot shot being drawn.
void Aim_DrawShotMarker(PlayState* play, Player* player);

} // namespace SohVr
