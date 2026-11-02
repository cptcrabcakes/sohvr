#pragma once

// Ties the headset to the game: anchors the play space to Link, scales it by age, turns it, and builds each
// eye's camera matrices. VrRender calls these; the decomp calls the C functions in Vr.h.

#include "VrRuntime.h"
#include "include/z64math.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

union Gfx;
struct Player;

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
// The controls editor: the actions that can be put on another button, and the controllers' buttons they can go on.
enum VrAction { VR_ACTION_A, VR_ACTION_B, VR_ACTION_WHEEL, VR_ACTION_PAUSE, VR_ACTION_RECENTER, VR_ACTION_NAVI,
                VR_ACTION_COUNT };
enum VrInput { VR_INPUT_NONE, VR_INPUT_A, VR_INPUT_B, VR_INPUT_X, VR_INPUT_Y, VR_INPUT_MENU, VR_INPUT_LEFT_STICK_CLICK,
               VR_INPUT_RIGHT_STICK_CLICK, VR_INPUT_RIGHT_STICK_UP, VR_INPUT_RIGHT_STICK_DOWN, VR_INPUT_COUNT };
// Each action's setting (it holds a VrInput), and the input it's on by default.
const char* Controls_ActionCvar(VrAction action);
VrInput Controls_DefaultInput(VrAction action);
// Whether the input an action is on is held.
bool Controls_ActionHeld(VrAction action);
// Waits for the next button pressed on the controllers, and puts the action on it.
void Controls_StartBinding(VrAction action);

// Whether the desktop window shows the game from its own camera (the streamer view) instead of the left eye.
bool Game_StreamerView();
// Adds the streamer view's camera to the frame's matrix replacements, for a window of this size. t is the
// interpolation step of the frame.
void Game_AddStreamerMatrices(float t, uint32_t width, uint32_t height, std::unordered_map<Mtx*, MtxF>& replacements);
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
// The scene being played, or -1 outside of one.
int Game_SceneNumber();
// Recenters the view on the next frame, measuring the seated height again in seated mode.
void Game_Recenter();
// Presses Start for the game's next update, opening the pause menu.
void Game_PressStart();
// A pose standing upright in front of where the player looks, the distance ahead and the drop below eye level
// (meters), facing back at them.
Pose Game_PanelInFront(float distance, float drop);
// Whether the bow or slingshot's string is being drawn.
bool Game_DrawingShot();
// Where a shot from the bow or slingshot in hand would leave, which way it would go, and how hard (the share of
// its full speed, from how far it's drawn), as Vr_AimProjectile launches it. False for anything else.
bool Game_ShotAim(Player* player, Vec3f* origin, Vec3f* direction, float* power);

} // namespace SohVr
