#pragma once

// Ties the headset to Majora's Mask: anchors the play space to Link, scales it by his form, turns it, and builds each
// eye's camera matrices. The shared VR core (vr/core at the repo's root) calls these; the decomp calls the C
// functions in Vr.h.

#include "VrRuntime.h"
#include "z64math.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

union Gfx;

namespace SohVr {

// Called once per game tick, when its display list is handed over for rendering.
void Game_OnTickSubmitted();
// Called once per rendered frame, after the runtime has new head and controller poses.
void Game_OnFrame();
// Whether the tick being rendered shows the world in stereo, as opposed to on the virtual screen.
bool Game_RenderStereo();
// Adds this eye's projection and view to the frame's matrix replacements. t is the interpolation step of the frame.
void Game_AddEyeMatrices(int eye, float t, uint32_t width, uint32_t height,
                         std::unordered_map<Mtx*, MtxF>& replacements);
// Whether the HUD is drawn on its own and shown on a panel, as opposed to staying in view.
bool Game_SplitHud();
// Ends the frame's display list before the overlay, or restores it.
void Game_CutOverlay(bool cut);
// The overlay's display list, for drawing the HUD on its own.
Gfx* Game_Overlay();
// The pieces of the HUD to show in the room.
void Game_GetHudQuads(std::vector<HudQuad>& out);

} // namespace SohVr
