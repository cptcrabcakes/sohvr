#ifndef SOH_ENHANCEMENTS_VR_H
#define SOH_ENHANCEMENTS_VR_H

// Entry points the decomp code calls into. Everything returns early when no VR session is running.
// Callers include global.h first, so the game's types are already declared.

#ifdef __cplusplus
extern "C" {
#endif

// True while the world is rendered in stereo around the player (gameplay, not a menu on the virtual screen).
bool Vr_IsWorldStereo(void);
// Whether the world is shown from the game's camera rather than through Link's eyes, as on the title screen, so
// Link is drawn whole instead of as the player's hands.
bool Vr_ViewFromCamera(void);
// Makes a matrix already placed in the world, such as one on a controller, the current matrix. The frames drawn
// between game ticks then blend it as it is, instead of relative to the actor drawing it.
void Vr_PutWorldMatrix(MtxF* matrix);
// The shop's bomb model, which VR draws for bombs and bomb flowers, at a scale (world units per model unit): where
// its origin goes, centered on the actor while it's held and otherwise resting on the ground, turned as the actor's
// shape.rot turns it (Matrix_SetTranslateRotateYXZ), and where its fuse tip then is.
void Vr_PlaceBombModel(Actor* actor, f32 scale, Vec3f* origin, Vec3f* fuseTip);
// Draws the bomb flower's bomb at the current matrix: the shop's bomb model in the flower bomb's colors, with a ring of
// petals around its cap.
void Vr_DrawBombFlowerBomb(PlayState* play);
// Where the item Link holds up after getting it is shown in VR, and its scale; false outside VR.
bool Vr_GetItemPlace(Player* player, Vec3f* pos, f32* scale);

// Moves Link by however far the player has stepped in the real world since the last tick, and keeps the view on
// him. Called from the player's update after it moves him and before collision, which pushes him back out of walls.
void Vr_ApplyRoomScale(Player* player);

// Records the main play view's matrices so each eye can replace them. Called from View_ApplyPerspective.
void Vr_OnViewApplied(View* view, Mtx* projection, Mtx* viewing);

// Merges headset controller input into pad 0. Called right after the pads are read.
void Vr_OverridePads(OSContPad* pads);

// Makes stick movement relative to where the player is looking. Returns false when not in stereo.
bool Vr_GetInputDir(Vec3s* dst);

// Records a skybox matrix and its value, so each eye can center the sky on itself. Called from SkyboxDraw_Draw.
void Vr_OnSkyboxDrawn(Mtx* mtx, MtxF* matrix);

// Records where the overlay (HUD, text, fades) is chained after the world, so VR can draw it on its own.
// overlayEnd is where the overlay's end-of-list command is about to be written. Called from Graph_Update.
void Vr_OnGraphBuilt(Gfx* overlayBranch, Gfx* overlay, Gfx* overlayEnd);
// Records where the overlay goes on past the fades and transitions, which come first in it, to the HUD and text.
// Called from Play_Draw.
void Vr_OnFadeDrawn(Gfx* fadeEnd);
// A list that clears to white, then draws the overlay. Rendered alongside the normal clear to black, the two
// give the HUD's transparency.
Gfx* Vr_WhiteBackgroundList(Gfx* overlay);

// Whether collectibles draw as 3D models. Always on in VR, where the flat sprites look like cardboard.
bool Vr_UseNewDrops(void);
// In VR, ammo and magic drops are picked up by hand instead of by walking into them.
bool Vr_DropNeedsGrab(s16 type);
// True when a hand is closed around this point. Buzzes that hand.
bool Vr_HandGrabsAt(Vec3f* pos);

// Hides Link's body and moves his forearms so his hands land on the controllers. Called for each limb from the
// player's limb override, after the game's own override.
void Vr_OverridePlayerLimb(Player* player, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot);
// Whether a limb is drawn mirrored: a hand on the other side's controller, mirrored to look like that side's hand.
// Mirroring turns faces inside out, so the other side of each face has to be culled.
bool Vr_LimbMirrored(Player* player, s32 limbIndex);
// Whether Link holds a sword or hammer in hand, so its hitbox follows the controller even outside attacks.
bool Vr_HoldsBlade(Player* player);
// Whether a swing of the held blade is fast enough to hit. Called once per tick with the blade tip's position.
// Sets the blade's damage to a normal slash when it is.
bool Vr_SwordSwing(Player* player, Vec3f* tip);
// Whether the shield in Link's hand should block on its own, wherever the player holds it.
bool Vr_HoldsShield(Player* player);
// Passes the game's controller rumble on to the VR controllers.
void Vr_Rumble(bool on);
// Whether the sword hand's grip just asked to charge a spin attack (see UpdateSpinAttack). Called from the player's
// sword attack handler, which starts the charge.
bool Vr_TakeSpinCharge(Player* player);
// After the blade's position is updated each tick: a swung blade striking a wall or the floor sparks and stops,
// and the hammer shakes the ground, as the game's own attacks do.
void Vr_SwordAfterUpdate(PlayState* play, Player* player);

// What the player asked for with gestures, for the player's own update to carry out. Item ids below 0x100 ask
// to take that item out.
enum {
    VR_REQUEST_NONE = -1,
    VR_REQUEST_DRAW_SWORD = 0x100, // reached over the shoulder with the sword sheathed
    VR_REQUEST_PUT_AWAY,           // reached over the shoulder with it drawn
    VR_REQUEST_USE_HELD,           // used an item already in hand again, such as tipping a bottle
    VR_REQUEST_HOLD_BOTTLE,        // took out a bottle with something in it (see Vr_TakeBottleContent)
};

// The request made since the last call, or VR_REQUEST_NONE.
s32 Vr_TakeItemRequest(void);
// Whether items come from the VR menu rather than the B and C buttons, so the game's checks that an item is on a
// button (such as the Lens of Truth turning off when it isn't) shouldn't apply.
bool Vr_ItemsFromMenu(void);
// The C button the requested item was put on, which the game changes a bottle through when it's drunk from or
// fills, or -1 to leave the held item's button as it is.
s32 Vr_ItemRequestButton(void);
// What's in the bottle VR_REQUEST_HOLD_BOTTLE took out. The game takes it out as an empty bottle and is given its
// contents once it's in hand, as after a catch, so it can be drunk from or poured by tipping it.
u8 Vr_TakeBottleContent(void);
// A bug, fish, fairy or blue fire an empty bottle in hand was just swung through, which the game should bottle,
// or NULL.
Actor* Vr_TakeBottleCatch(void);
// Draws what VR adds around Link, such as the belt menu. Called at the end of Player_Draw.
void Vr_DrawPlayerExtras(PlayState* play);
// Whether items should draw their full item-get models instead of their flat or first-person ones.
bool Vr_UseVendorModels(void);

// Whether the held item's button counts as held: the bow while its string is drawn, the hookshot while its
// trigger is down. Letting go fires them, as releasing the button does.
bool Vr_ItemButtonHeld(Player* player);
// Points an arrow, seed or hookshot along the player's aim just as it's fired.
void Vr_AimProjectile(Player* player, Actor* projectile);
// Keeps a nocked arrow or seed in the string hand, pointing through the bow. Called after the game places it.
void Vr_PlaceNockedArrow(Player* player, Actor* arrow);
// Where the bow or slingshot string is pulled to (the drawing hand, in the world) while it's drawn. False otherwise.
bool Vr_StringPull(Player* player, Vec3f* hand);
// Puts the hookshot's chain start and its waiting hook (if any) on the tip of the hookshot model in the hand,
// pointing the way it points. Called after the game places them on Link's own hand.
void Vr_PlaceHookshot(Player* player, Vec3f* chainStart, Actor* hook);
// The tip of the hookshot or longshot in the hand and the way it points (a unit vector), in the world, for its
// sight and its shot. False when it isn't in hand or outside VR.
bool Vr_HookshotSight(Player* player, Vec3f* tip, Vec3f* direction);
// Points something thrown from one of Link's hands (0 left, 1 right) where that controller points.
void Vr_AimFromHand(Player* player, Actor* actor, s32 linkHand);
// A billboard rotation for a sprite at this position that faces the eye, for the sun and moon. The game's own
// billboard turns sprites with the camera's direction, which in VR is the head's, so they swing as it turns.
Mtx* Vr_FacingBillboard(PlayState* play, Vec3f* position);
// The same as a matrix: upright, facing the eye from (x, y, z). False when that's too close to the eye to tell.
bool Vr_FacingRotation(f32 x, f32 y, f32 z, MtxF* out);
// Whether a wall stands between the eye and a point: on the line toward it, no farther than maxDistance, stopping
// stopShort of it so the thing at the point (a torch) doesn't hide itself. For glows and the sun's flare, which the
// game hides by reading the depth buffer, which in VR is the HUD's rather than the eyes'.
bool Vr_SightBlocked(PlayState* play, Vec3f* target, f32 maxDistance, f32 stopShort);
// The game's billboard matrix while the world is in stereo, or NULL. The matrix helpers swap it for one facing
// the eye from wherever the billboard is drawn, so sprites, smoke and sparkles stop turning with the head.
extern MtxF* gVrBillboard;
// Song effects are drawn around the eyes, facing where the camera looks. In VR they face where the head looked
// when they started instead (Start stores it), so they don't swing around as it turns. Place sets up the matrix
// as their draw does, and is false outside VR.
void Vr_StartSongEffect(PlayState* play, Actor* effect);
bool Vr_PlaceSongEffect(PlayState* play, Actor* effect, f32 scale);
// The yaw a camera looking at this position would have, for things that turn to face the camera's direction.
// Outside VR it's the camera's own yaw.
s16 Vr_CameraYawToward(PlayState* play, Vec3f* position, s16 cameraYaw);
// Puts a carried actor in the throwing hand, level and facing where the hand points. The game carries things
// between both hands, which in VR are the two controllers, wherever they happen to be.
void Vr_PlaceHeldActor(Player* player, Actor* held);
// Whether the grip just let go of whatever Link carries (bombs, bombchus, pots, rocks), which the game should
// throw right away.
bool Vr_TakeThrowRequest(void);
// Whether the grip just let go of the boomerang, which the game should throw from that hand (from) right away.
bool Vr_TakeBoomerangThrow(Vec3f* from);
// Gives a thrown actor the speed and direction the hand had when the grip let go. Leaves button throws alone.
void Vr_ThrowWithHand(Player* player, Actor* thrown);
// Keeps a just-spawned Deku nut in the throwing hand until the grip lets go. False outside VR.
bool Vr_HoldNut(Player* player, Actor* nut);
// Gives a nut thrown by hand the hand's velocity and an arc, as the game launches it. False for other nuts.
bool Vr_LaunchThrownNut(Actor* nut);

#ifdef __cplusplus
}
#endif

#endif
