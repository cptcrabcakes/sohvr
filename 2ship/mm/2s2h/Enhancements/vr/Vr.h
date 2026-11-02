#ifndef MM_ENHANCEMENTS_VR_H
#define MM_ENHANCEMENTS_VR_H

// Entry points the decomp code calls into, for Majora's Mask. Everything returns early when no VR session is
// running. Callers include the game's headers first, so its types are already declared.

#ifdef __cplusplus
extern "C" {
#endif

// True while the world is rendered in stereo around the player.
bool Vr_IsWorldStereo(void);
// At the start of drawing a tick, before its view is set: aims the game's camera along the head. Called from
// Play_DrawMain.
void Vr_OnPlayDrawStart(PlayState* play);
// Records the main play view's matrices so each eye can replace them. Called from View_ApplyPerspective.
void Vr_OnViewApplied(View* view, Mtx* projection, Mtx* viewing);
// Records where the overlay (HUD, text, fades) is called from the master list, the overlay itself, and the master
// list's end, so VR can draw the overlay on its own. Called from Graph_ExecuteAndDraw.
void Vr_OnGraphBuilt(Gfx* overlayCall, Gfx* overlay, Gfx* masterEnd);
// A list that clears to white, then draws the overlay. Rendered alongside the normal clear to black, the two give the
// HUD's transparency.
Gfx* Vr_WhiteBackgroundList(Gfx* overlay);
// Records a skybox matrix and its value, so each eye can center the sky on itself. Called from Skybox_Draw.
void Vr_OnSkyboxDrawn(Mtx* mtx, MtxF* matrix);
// Merges headset controller input into pad 0. Called right after the pads are read.
void Vr_OverridePads(OSContPad* pads);
// In the world, the stick moves Link relative to where the head looks: the yaw to use. False outside VR.
bool Vr_GetInputDirYaw(s16* yaw);
// Whether Link is seen from inside, so what his draw drew is taken back out (see Player_Draw).
bool Vr_HidesLink(Player* player);

#ifdef __cplusplus
}
#endif

#endif
