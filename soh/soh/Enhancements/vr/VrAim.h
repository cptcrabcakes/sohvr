#pragma once

// Aiming aids drawn in the world: where an arrow or seed will land, the path it will take, and the hookshot's laser
// and sight, green where it will catch.

extern "C" {
#include "global.h"
}

namespace SohVr {

// Draws the landing marker (and, if turned on, the flight path) of the bow or slingshot shot being drawn.
// An arrow from its tail to its tip (game units), as a shaft and a head, for one held in the hand.
void Aim_DrawArrow(PlayState* play, const Vec3f& tail, const Vec3f& tip, float width);
void Aim_DrawShotMarker(PlayState* play, Player* player);
// The Lens of Truth's window, in the depth buffer only: everything around a disc in the world (showInside) or the
// disc itself is set to the nearest depth, so what's drawn after it fails there. eyes is where it's looked from.
void Aim_DrawLensMask(PlayState* play, const Vec3f& center, const Vec3f& normal, float radius, const Vec3f& eyes,
                      bool showInside);
// The lens's thin tint, a see-through disc.
void Aim_DrawLensGlass(PlayState* play, const Vec3f& center, const Vec3f& normal, float radius, Color_RGBA8 color);
// A flat, see-through ring around a point, facing along normal (game units).
void Aim_DrawRing(PlayState* play, const Vec3f& center, const Vec3f& normal, float radius, float width,
                  Color_RGBA8 color);

} // namespace SohVr
