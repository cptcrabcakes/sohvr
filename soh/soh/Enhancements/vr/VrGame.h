#pragma once

// Ties the headset to the game: anchors the play space to Link, scales it by age, turns it, and builds each
// eye's camera matrices. VrRender calls these; the decomp calls the C functions in Vr.h.

#include "VrRuntime.h"
#include "include/z64math.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

union Gfx;

namespace SohVr {

// Called once per game tick, when its display list is handed over for rendering.
void Game_OnTickSubmitted();
// Called once per rendered frame, after the runtime has new head and controller poses.
void Game_OnFrame();
// Whether the tick being rendered shows the world in stereo, as opposed to a menu on the virtual screen.
bool Game_RenderStereo();
// Adds this eye's projection and view to the frame's matrix replacements. t is the interpolation step of the frame.
void Game_AddEyeMatrices(int eye, float t, uint32_t width, uint32_t height,
                         std::unordered_map<Mtx*, MtxF>& replacements);

// Whether the HUD is drawn on its own and shown on the wrists, as opposed to staying in view. It stays in view
// during fades so they still cover the world.
bool Game_SplitHud();
// Ends the frame's display list before the overlay, or restores it.
void Game_CutOverlay(bool cut);
// The overlay's display list on its own.
Gfx* Game_Overlay();
// Where each piece of the HUD goes this frame.
void Game_GetHudQuads(std::vector<HudQuad>& out);

// Converts a point in the play space (meters) into the world, as of the tick being built.
Vec3f Game_PlayToWorld(const float position[3]);
// Game units per meter for Link's current age.
float Game_UnitsPerMeter();
// Whether the ocarina is out, when the face buttons play its notes.
bool Game_PlayingOcarina();
// Asks the player to use an item (see Vr_TakeItemRequest) on its next update.
void Game_RequestItem(int request);
// Takes an item out into the hand that picked it, swapping which controller each of Link's hands is on if the
// item belongs in his other one. slot is a bottle's inventory slot, or -1. A mask goes into the hand, to be put on
// by bringing it to the face.
void Game_TakeItem(uint8_t item, Hand hand, int8_t slot);
// Recenters the view on the next frame, measuring the seated height again in seated mode.
void Game_Recenter();
// Presses Start for the game's next update, opening the pause menu.
void Game_PressStart();
// A pose standing upright in front of where the player looks, the distance ahead and the drop below eye level
// (meters), facing back at them.
Pose Game_PanelInFront(float distance, float drop);

} // namespace SohVr
