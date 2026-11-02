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
// Whether none of Link is drawn, as when a Goron curls up around the eyes (see Player_Draw).
bool Vr_HidesLink(Player* player);
// Whether only Link's hands are drawn, on the controllers, as they are while the player is inside him. The rest of
// his skeleton is still posed, since his weapons' and shield's hitboxes are placed from it.
bool Vr_DrawsOnlyHands(Player* player);
// Whether a limb is one of the forearms or hands, which stay drawn.
bool Vr_HandLimb(s32 limbIndex);
// After the game's own limb override: puts the forearms where the hands land on the controllers and hides every
// other limb.
void Vr_OverridePlayerLimb(Player* player, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot);
// Whether a limb is a hand drawn on the other side's controller, mirrored, so its culling is inverted.
bool Vr_LimbMirrored(Player* player, s32 limbIndex);
// Puts a matrix already in world space on the matrix stack, in a way frame interpolation blends correctly.
void Vr_PutWorldMatrix(MtxF* matrix);
// Whether Link holds a blade (any sword, or a Deku stick) whose hitbox follows the hand.
bool Vr_HoldsBlade(Player* player);
// In VR, the blade in Link's hand as hit points in the hand model's space, sized to the blade as it's drawn instead of
// the game's longer, wider reach: its three tips and three bases, either of which may be NULL. gameLength is the
// game's own length for it, which a Deku stick keeps. False outside VR, or without a blade.
bool Vr_BladePoints(Player* player, f32 gameLength, Vec3f* tips, Vec3f* bases);
// Whether the blade, whose tip is at tip this tick, is swung by hand fast enough to hit. Sets its damage if so.
bool Vr_SwordSwing(Player* player, Vec3f* tip);
// A Goron's or Zora's punch thrown by hand: places that fist's hitbox (hand 0 is the left) while it's moving fast.
// Called from the hand's limb, with its matrix current.
void Vr_FistSwing(PlayState* play, Player* player, s32 hand);
// Whether the shield in Link's hand blocks, wherever it's held.
bool Vr_HoldsShield(Player* player);

// Draws what VR adds around the player, such as the item ring. Called from Player_Draw.
void Vr_DrawPlayerExtras(PlayState* play);

// In z_player.c: sets the sword's damage for a swing by hand, as a slash, or a jump attack when strong.
void Player_SetVrSwingDamage(Player* player, s32 strong);
// In z_player.c: the item-get model (GetItem_Draw's id) for an item, or GID_NONE.
s16 Player_GetItemModelVr(u8 item);

#ifdef __cplusplus
}
#endif

#endif
