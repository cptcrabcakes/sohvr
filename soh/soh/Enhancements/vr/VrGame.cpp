#include "VrGame.h"
#include "VrAim.h"
#include "VrItems.h"
#include "VrOcarinaMic.h"
#include "VrRuntime.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ResourceManagerHelpers.h"
#include "soh/ShipInit.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "global.h"
#include "objects/object_link_boy/object_link_boy.h"
#include "objects/object_link_child/object_link_child.h"
#include "overlays/actors/ovl_Arms_Hook/z_arms_hook.h"
// Named in a log line when a hookshot pull doesn't aim (see CheckHookshotPull).
s32 func_8083501C(Player* thisx, PlayState* play);
s32 func_808351D4(Player* thisx, PlayState* play);
s32 func_808353D8(Player* thisx, PlayState* play);
// The game's own reactions to its blade hitting something hard: the hammer's quake, and wearing out the Deku stick
// and Biggoron's Sword.
void func_80842A28(PlayState* play, Player* thisx);
void func_80842CF0(PlayState* play, Player* thisx);
s32 Player_UpperAction_ChangeHeldItem(Player* thisx, PlayState* play);
void ArmsHook_Wait(ArmsHook* thisx, PlayState* play);
void ArmsHook_Shoot(ArmsHook* thisx, PlayState* play);
#include "soh/Enhancements/vr/Vr.h"
extern PlayState* gPlayState;
}

#define CVAR_VR_ENABLED CVAR_SETTING("VR.Enabled")
#define CVAR_VR_PLAYER_HEIGHT CVAR_SETTING("VR.PlayerHeight") // centimeters
// Whether ammo and magic drops are picked up by hand (1) or by walking into them (0).
#define CVAR_VR_GRAB_DROPS CVAR_SETTING("VR.GrabDrops")
// How big the world looks at each age, in percent: higher makes it bigger.
#define CVAR_VR_ADULT_WORLD_SCALE CVAR_SETTING("VR.AdultWorldScale")
#define CVAR_VR_CHILD_WORLD_SCALE CVAR_SETTING("VR.ChildWorldScale")
#define CVAR_VR_SNAP_TURN CVAR_SETTING("VR.SnapTurn")         // degrees; 0 turns smoothly
#define CVAR_VR_SEATED CVAR_SETTING("VR.Seated")
#define CVAR_VR_HAND_MOVEMENT CVAR_SETTING("VR.HandMovement") // the stick moves along where the left hand points
// Whether the ocarina listens for whistled, hummed or sung notes while it's up (see VrOcarinaMic.h).
#define CVAR_VR_MIC_OCARINA CVAR_SETTING("VR.MicOcarina")
// Whether ladders, climbable walls and ledges are climbed by pulling with the hands (see UpdateClimb).
#define CVAR_VR_CLIMB_BY_HAND CVAR_SETTING("VR.ClimbByHand")
// 0 keeps Link's hands on their own sides, so the sword is in the left hand like his; 1 swaps them.
#define CVAR_VR_SWORD_IN_RIGHT_HAND CVAR_SETTING("VR.SwordInRightHand")
// Extra rotation in degrees for each of Link's hand models, about the grip's X, Y and Z axes, for fine tuning.
#define CVAR_VR_HAND_TUNE(hand, axis) CVAR_SETTING("VR.Hand" hand ".Tune" axis)

namespace SohVr {

namespace {

// Link's height in game units, which is mapped onto the player's. The child's is Player_GetHeight's. The adult's is
// less than Player_GetHeight's 68, which made the world look too small: Sheik came up to the chest.
constexpr float kAdultHeight = 58.0f;
constexpr float kChildHeight = 44.0f;
// The game's own near plane is 10 units, which clips things held close to the face.
constexpr float kNearPlane = 4.0f;
// Extra culling field of view so head turns between game ticks don't reveal culled actors.
constexpr float kCullFovMargin = 10.0f;
// Anchor moves longer than this in one tick are warps, so they snap instead of sliding.
constexpr float kWarpDistance = 200.0f;
constexpr float kSmoothTurnSpeed = 2.5f; // radians per second at full stick
constexpr float kStickDeadzone = 0.15f;
constexpr float kStickRange = 80.0f;
// The over-the-shoulder sword gesture can't toggle again sooner than this, in seconds.
constexpr float kSwordGestureCooldown = 0.6f;
// How close a closed hand has to be to a drop to take it, in game units.
constexpr float kGrabRadius = 14.0f;
// A swing needs momentum: the blade tip faster than the start speed, the hand itself (not just the wrist) faster
// than the hand speed, and the tip carried that fast in one direction for the travel distance before the blade
// can hit (meters and meters per second). It ends when the tip slows under the stop speed or turns back, so
// waggling never gets going. It hits at most once, and the next swing can't start for the cooldown after it ends.
constexpr float kSwingStartSpeed = 2.5f;
constexpr float kSwingMinHandSpeed = 1.2f;
constexpr float kSwingMinTravel = 0.3f;
constexpr float kSwingStopSpeed = 1.0f;
// A swing this fast (meters a second at the tip) hits as hard as a jump attack.
constexpr float kHeavySwingSpeed = 7.0f;
// A swung blade striking a wall or the floor at least this fast sparks and stops (meters a second at the tip).
constexpr float kWallStrikeSpeed = 1.5f;
// Turning around this far (degrees) within the spin time (seconds), with the sword hand out this far from the head
// (meters), is a spin attack.
constexpr float kPhysicalSpinDegrees = 300.0f;
constexpr float kPhysicalSpinSeconds = 1.2f;
constexpr float kSpinReach = 0.3f;
constexpr int kSwingCooldownTicks = 8;
// Doors and chests open by hand. A hand closing on a door (within the depth of its plane, half its width across
// and its height, in game units) and then pushing or pulling it the push distance (meters) opens it; so does one
// closing on a chest (within its reach and height at full size) and lifting the lid the lift distance.
constexpr float kDoorReachDepth = 30.0f;
constexpr float kDoorHalfWidth = 50.0f;
constexpr float kDoorHeight = 120.0f;
constexpr float kDoorPushDistance = 0.08f;
constexpr float kChestReach = 45.0f;
constexpr float kChestHeight = 70.0f;
constexpr float kChestLiftDistance = 0.08f;
// A hand within this of the eyes is at the face, for masks and drinking (meters).
constexpr float kFaceReach = 0.18f;
// An empty bottle catches a creature it passes within the catch reach of (game units) at the catch speed (meters
// per second). A bottle is tipped when its opening points down this far (the y of its direction).
constexpr float kCatchReach = 25.0f;
constexpr float kCatchSpeed = 1.0f;
constexpr float kBottleTipDown = -0.3f;
// The fishing rod casts when its hand passes the cast speed (meters per second), at most once per cooldown (s).
constexpr float kCastSpeed = 2.0f;
constexpr float kCastCooldown = 1.0f;
// A two-handed grip is taken with the other hand within the reach of the first (meters), and let go of past the
// let-go distance.
constexpr float kTwoHandReach = 0.2f;
constexpr float kTwoHandLetGo = 0.35f;
// The player's own steps move Link by at most this much per tick (meters); more than the drift distance from him,
// which happens while he can't be moved, such as on a ladder, is let go of instead.
constexpr float kRoomScaleMaxStep = 0.15f;
constexpr float kRoomScaleMaxDrift = 0.4f;
constexpr float kTicksPerSecond = 20.0f;
// The string hand has to start drawing within this distance of the bow (meters).
constexpr float kNockReach = 0.3f;
// Bow aim falls back to where the bow hand points when the hands are closer than this (meters).
constexpr float kMinDrawLength = 0.08f;
// Drawn this far (meters), a shot flies at full speed; barely drawn, at the least share of it.
constexpr float kFullBowDraw = 0.55f;
constexpr float kFullSlingshotDraw = 0.4f;
constexpr float kLeastShotPower = 0.4f;
// The power the shot just loosed was given, for the game to give it once it sets its speed (below 0 for none).
float sShotPower = -1.0f;
// The longest draw, from the bow hand to the string hand (meters). Past it the string and arrow stay at full draw.
constexpr float kMaxBowDraw = 0.7f;
constexpr float kMaxSlingshotDraw = 0.5f;
// How far behind the arrow actor's origin its nock is, in game units: its model runs about 2400 units along the
// shaft, shifted back about 1000 by its skeleton, at the arrow's scale of 0.01.
constexpr float kArrowNockBehind = 14.0f;
// The sword is drawn by closing the sword hand behind the head, near head height (meters from the head).
constexpr float kShoulderBehind = 0.02f;
constexpr float kShoulderDrop = 0.35f;
constexpr float kShoulderReach = 0.45f;
// Crouching means the head is below this share of standing eye height (eyes sit at about 93% of height).
constexpr float kEyeHeightRatio = 0.93f;
constexpr float kCrouchRatio = 0.72f;
// Hanging from or climbing onto a ledge, and crawling, Link's head is nowhere near where it is when he stands, so the
// view follows it, easing this share of the way each tick so his animation's sway doesn't shake it.
constexpr float kHeadFollowEase = 0.5f;
// Thrown things leave the hand a little faster than it moved, so throws don't feel weak.
constexpr float kThrowBoost = 1.5f;
constexpr float kMinThrowSpeed = 1.5f; // meters per second; slower releases just drop
// The other hand takes hold of something carried too when it closes this close to the carrying hand (meters). Let go
// of a two-handed hold within the window (seconds) of each other, the hands throw it together; one let go alone,
// the other keeps carrying it.
constexpr float kTwoHandCarryReach = 0.5f;
constexpr float kTwoHandThrowWindow = 0.15f;
// The game's own throw of whatever Link carries (func_8084409C from the A button), the limit for a pitched one.
constexpr float kBombMaxForward = 8.0f;
constexpr float kBombMaxUp = 12.0f;
// Rooms drawn on a pre-rendered background image instead of geometry.
constexpr u8 kPrerenderedRoomShape = 1;

// HUD pieces on the N64's 320x240 screen; the HUD texture is that screen scaled up.
struct ScreenRect {
    int x, y, w, h;
};
constexpr ScreenRect kHeartsRect = { 16, 8, 134, 48 };   // hearts and the magic bar
constexpr ScreenRect kRupeesRect = { 16, 176, 74, 52 };  // rupees and small keys
// The A button, whose label says what A does right now, and the B button beside it (its 32-pixel icons start at
// x 186 and 160). The C buttons have no use in VR.
constexpr ScreenRect kActionRect = { 156, 2, 66, 50 };
// The bow's or slingshot's ammo left, in the bottom middle, which the HUD itself leaves empty (see DrawAmmoCounter).
constexpr ScreenRect kAmmoRect = { 116, 208, 88, 16 };
constexpr int kTextboxHeight = 78;
// How far below eye level text panels appear, a meter ahead (meters). At 0.25 they covered the speaker's face.
constexpr float kMessageDrop = 0.4f;


struct Vec3 {
    float x, y, z;
};

Vec3 Rotate(const float q[4], Vec3 v) {
    // v + 2w(u x v) + 2u x (u x v), with u the quaternion's vector part
    Vec3 u = { q[0], q[1], q[2] };
    float w = q[3];
    Vec3 c = { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
    Vec3 cc = { u.y * c.z - u.z * c.y, u.z * c.x - u.x * c.z, u.x * c.y - u.y * c.x };
    return { v.x + 2.0f * (w * c.x + cc.x), v.y + 2.0f * (w * c.y + cc.y), v.z + 2.0f * (w * c.z + cc.z) };
}

Vec3 RotateYaw(float yaw, Vec3 v) {
    float s = sinf(yaw);
    float c = cosf(yaw);
    return { v.x * c + v.z * s, v.y, -v.x * s + v.z * c };
}

float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(Vec3 a, Vec3 b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

Vec3 Scaled(Vec3 v, float s) {
    return { v.x * s, v.y * s, v.z * s };
}

Vec3 Normalized(Vec3 v) {
    float length = sqrtf(Dot(v, v));
    return length > 0.0001f ? Scaled(v, 1.0f / length) : Vec3{ 0.0f, 0.0f, 1.0f };
}

// The part of v at right angles to the unit vector axis.
Vec3 Perpendicular(Vec3 v, Vec3 axis) {
    Vec3 along = Scaled(axis, Dot(v, axis));
    return { v.x - along.x, v.y - along.y, v.z - along.z };
}

void Multiply(const float a[4], const float b[4], float out[4]) {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x;
    out[1] = y;
    out[2] = z;
    out[3] = w;
}

// A pose fixed to a parent pose, given in the parent's own axes.
Pose Attach(const Pose& parent, Vec3 offset, const float rotation[4]) {
    Pose out;
    Vec3 moved = Rotate(parent.orientation, offset);
    out.position[0] = parent.position[0] + moved.x;
    out.position[1] = parent.position[1] + moved.y;
    out.position[2] = parent.position[2] + moved.z;
    Multiply(parent.orientation, rotation, out.orientation);
    out.valid = parent.valid;
    return out;
}

constexpr float kNoRotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
// OpenXR's grip space: -Z runs through the fist from little finger to thumb, +Y points out over the knuckles,
// and +X points left on both hands (out of the back of the left hand, into the palm of the right).
// Watch faces on the back of each wrist, read along the forearm with the little finger side up.
constexpr float kLeftWatchRotation[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
constexpr float kRightWatchRotation[4] = { 0.5f, -0.5f, -0.5f, 0.5f };
constexpr Vec3 kLeftWatchOffset = { 0.035f, -0.07f, 0.0f };
constexpr Vec3 kRightWatchOffset = { -0.035f, -0.07f, 0.0f };

// Link's hands; the left one holds his sword, the right one his shield, bow, hookshot and thrown things.
enum LinkHand { LINK_LEFT, LINK_RIGHT };
// The controller each of Link's hands is on.
Hand ControllerFor(LinkHand hand);
void PutLinkHandOn(LinkHand linkHand, Hand hand);
// How something held in Link's throwing hand is turned, as a shape rotation (see its definition).
bool HeldRotation(Player* player, Vec3s* out);

// In the headset the grip's X and Y axes come out half a turn about Z from the frame described above (both the
// hands and the wrist panels showed it), so everything placed on a hand goes through this first.
constexpr float kGripFix[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
// Both hand models' axes onto the (fixed) grip, from the game's own data. The left hand's sword runs along its +X
// out of the thumb side (grip -Z), with the fist 400 units along +Y (grip +Y, toward the knuckles); the right
// hand's hookshot muzzle is along +Y too, and its shield lies on the back of the hand. That's a quarter turn
// about Y.
constexpr float kHandAlignment[4] = { 0.0f, 0.7071068f, 0.0f, 0.7071068f };
// Where the fist is in the hand model (model units), which goes on the controller's grip point.
constexpr Vec3 kHandFist = { 0.0f, 400.0f, 0.0f };
// Between the arms of the slingshot in Link's right hand: where its band is fixed (sBowStringData in
// z_player_lib.c). The band pulls back along the hand's -Y, so shots go along +Y.
constexpr Vec3f kSlingshotForks = { 606.0f, 236.0f, 0.0f };

// What one game tick contributes to rendering.
struct TickState {
    Mtx* projection = nullptr;
    Mtx* viewing = nullptr;
    float zFar = 12800.0f;
    Vec3f anchor = {};
    float unitsPerMeter = 40.0f;
    bool stereo = false;
    // A fade, transition or game over is drawing over the view.
    bool fade = false;
    bool gameOver = false;
    // On the title screen the view follows the game's camera instead of Link (see FollowCamera), and the play
    // space's turn changes each tick, so the frames between ticks ease it.
    bool fromCamera = false;
    float yaw = 0.0f;
    bool message = false;
    s16 textboxY = 0;
    Gfx* overlayBranch = nullptr;
    Gfx* overlay = nullptr;
    Gfx* overlayEnd = nullptr;
    // Where the overlay goes on from the fades and transitions to the HUD (see Vr_OnFadeDrawn).
    Gfx* fadeEnd = nullptr;
    // The skybox's matrices this tick. They center the box on the game camera; each eye re-centers it on itself
    // so the sky is infinitely far away.
    static constexpr int kMaxSkyboxes = 4;
    Mtx* skybox[kMaxSkyboxes] = {};
    MtxF skyboxMatrix[kMaxSkyboxes] = {};
    int skyboxCount = 0;
    // How far the play space's center moved this tick to follow the player's steps (x, z meters). Link moved the
    // same way, so frames between ticks slide the center along with him and the view doesn't jump back.
    float centerShift[2] = {};
};

TickState sBuild;  // the tick whose display list is being built
TickState sRender; // the tick whose frames are being rendered
Vec3f sRenderPrevAnchor = {};
float sRenderPrevYaw = 0.0f;
bool sAwaitingWorldView = false;
bool sWorldViewCaptured = false;

// The play space maps onto the world by turning it by sYaw and putting sCenter (x, z meters) on Link.
float sYaw = 0.0f;
float sCenter[2] = {};
bool sNeedsRecenter = true;
// Seated, how far (meters) the player's head and hands are lifted, so the head is where a standing player's eyes
// would be (see UpdateSeating), and whether that's been measured since seated mode came on.
float sSeatedLift = 0.0f;
bool sSeatMeasured = false;
PlayState* sLastPlay = nullptr;
s16 sLastScene = -1;
float sHeadDrop = 0.0f; // how far the play space is lowered to follow Link's head (see kHeadFollowEase), in game units
s16 sLoggedEyeScene = -1;

u16 sLatchedButtons = 0;
bool sTurnLatched = false;
bool sRecenterLatched = false;
bool sWasStereo = false;

bool sMessageShown = false;
Pose sMessagePose;

Gfx sOverlayBranchSaved;
Gfx* sOverlayCutAt = nullptr;
bool sOverlayCut = false;

bool SessionRunning() {
    return Runtime::Get().IsSessionRunning();
}

Vec3 HeadForward() {
    return Rotate(Runtime::Get().GetHead().orientation, { 0.0f, 0.0f, -1.0f });
}

Vec3 ToWorldAround(const float position[3], const Vec3f& anchor, float unitsPerMeter, float centerX,
                   float centerZ) {
    Vec3 local = { (position[0] - centerX) * unitsPerMeter, (position[1] + sSeatedLift) * unitsPerMeter,
                   (position[2] - centerZ) * unitsPerMeter };
    Vec3 turned = RotateYaw(sYaw, local);
    return { anchor.x + turned.x, anchor.y + turned.y, anchor.z + turned.z };
}

Vec3 ToWorld(const float position[3], const Vec3f& anchor, float unitsPerMeter) {
    return ToWorldAround(position, anchor, unitsPerMeter, sCenter[0], sCenter[1]);
}

// Faces the play space the way Link faces and puts the player's current spot on him.
void Recenter(Player* player) {
    const Pose& head = Runtime::Get().GetHead();
    if (!head.valid) {
        return;
    }
    Vec3 forward = HeadForward();
    float headYaw = atan2f(-forward.x, -forward.z);
    float linkYaw = BINANG_TO_RAD(player->actor.shape.rot.y);
    sYaw = linkYaw + static_cast<float>(M_PI) - headYaw;
    sCenter[0] = head.position[0];
    sCenter[1] = head.position[2];
    sBuild.centerShift[0] = sBuild.centerShift[1] = 0.0f;
    sNeedsRecenter = false;
}

// Turns the play space around the player's head rather than its center.
void Turn(float angle) {
    const Pose& head = Runtime::Get().GetHead();
    Vec3 offset = RotateYaw(sYaw, { head.position[0] - sCenter[0], 0.0f, head.position[2] - sCenter[1] });
    sYaw += angle;
    Vec3 back = RotateYaw(-sYaw, offset);
    sCenter[0] = head.position[0] - back.x;
    sCenter[1] = head.position[2] - back.z;
}

// Moves the play space's center (meters), remembering how far for the frames until the next tick.
void ShiftCenter(float dx, float dz) {
    sCenter[0] += dx;
    sCenter[1] += dz;
    sBuild.centerShift[0] += dx;
    sBuild.centerShift[1] += dz;
}

// Whether the player's steps can move Link: not while the game decides where he is, such as when he's climbing,
// hanging, riding, crawling, holding a block, talking or in a cutscene.
bool StepsMoveLink(Player* player) {
    constexpr u32 kHeldInPlace = PLAYER_STATE1_HOOKSHOT_FALLING | PLAYER_STATE1_TALKING | PLAYER_STATE1_GETTING_ITEM |
                                 PLAYER_STATE1_HANGING_OFF_LEDGE | PLAYER_STATE1_CLIMBING_LEDGE |
                                 PLAYER_STATE1_CLIMBING_LADDER | PLAYER_STATE1_ON_HORSE | PLAYER_STATE1_IN_ITEM_CS |
                                 PLAYER_STATE1_IN_CUTSCENE;
    return !(player->stateFlags1 & kHeldInPlace) &&
           !(player->stateFlags2 & (PLAYER_STATE2_CRAWLING | PLAYER_STATE2_GRABBING_DYNAPOLY)) && player->csAction == 0;
}

// Past the drift distance from Link, the play space follows the player's steps instead, so the view never strays
// far from him while his position is the game's to decide.
void LimitDrift() {
    const Pose& head = Runtime::Get().GetHead();
    if (!head.valid) {
        return;
    }
    float dx = head.position[0] - sCenter[0];
    float dz = head.position[2] - sCenter[1];
    float length = sqrtf(dx * dx + dz * dz);
    if (length > kRoomScaleMaxDrift) {
        float excess = (length - kRoomScaleMaxDrift) / length;
        ShiftCenter(dx * excess, dz * excess);
    }
}

// Places that only look right from the game's own fixed camera, so they're played on the floating screen: rooms
// drawn on a pre-rendered background, and the market, whose geometry is real but built to be seen only from its
// fixed camera angles.
bool UsesFixedCamera(PlayState* play) {
    if (play->sceneNum == SCENE_MARKET_DAY || play->sceneNum == SCENE_MARKET_NIGHT ||
        play->sceneNum == SCENE_MARKET_RUINS) {
        return true;
    }
    MeshHeader* mesh = play->roomCtx.curRoom.meshHeader;
    return mesh != nullptr && mesh->base.type == kPrerenderedRoomShape;
}

// Link is being played: not paused, in a cutscene or on another camera.
bool InGameplay(PlayState* play) {
    return play != nullptr && GET_PLAYER(play) != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL &&
           play->pauseCtx.state == 0 && play->csCtx.state == CS_STATE_IDLE && play->activeCamera == CAM_ID_MAIN;
}

// The world is shown in stereo whenever Link is in it, cutscenes included: the player stays in Link's eyes while a
// scene plays out or the game's camera goes off on its own (opening a chest or a door), instead of watching it on
// the floating screen. So is the title screen's cutscene, from its own camera (see FollowCamera). File select, the
// pause menu and the credits stay on the screen.
bool ShowsWorld(PlayState* play) {
    return play != nullptr && GET_PLAYER(play) != nullptr &&
           (gSaveContext.gameMode == GAMEMODE_NORMAL || gSaveContext.gameMode == GAMEMODE_TITLE_SCREEN) &&
           play->pauseCtx.state == 0 && !UsesFixedCamera(play);
}

// Link is being played on the floating screen, in a place with a fixed camera.
bool ScreenGameplay() {
    return SessionRunning() && InGameplay(gPlayState) && UsesFixedCamera(gPlayState);
}

// Link is being played in VR, in the world or on the screen, so items come from the VR item menu.
bool PlayingInVr() {
    return Vr_IsWorldStereo() || ScreenGameplay();
}

// Whether notes can be played right now: the ocarina is up, or a song is being taught, played back or played for
// someone. Link can go on holding the ocarina after he stops playing, since VR doesn't put items away.
bool PlayingOcarina() {
    if (gPlayState == nullptr) {
        return false;
    }
    u8 mode = gPlayState->msgCtx.msgMode;
    return mode >= MSGMODE_OCARINA_STARTING && mode <= MSGMODE_FROGS_WAITING;
}

bool Crouching() {
    const Pose& head = Runtime::Get().GetHead();
    float eyeHeight = CVarGetInteger(CVAR_VR_PLAYER_HEIGHT, 170) / 100.0f * kEyeHeightRatio;
    return head.valid && head.position[1] + sSeatedLift < eyeHeight * kCrouchRatio;
}

// Whether a hand is behind the head at about head height, where it would reach for a sword on the back.
bool HandOverShoulder(const Pose& hand) {
    const Pose& head = Runtime::Get().GetHead();
    if (!hand.valid || !head.valid) {
        return false;
    }
    Vec3 offset = { hand.position[0] - head.position[0], hand.position[1] - head.position[1],
                    hand.position[2] - head.position[2] };
    Vec3 forward = Rotate(head.orientation, { 0.0f, 0.0f, -1.0f });
    float flat = sqrtf(forward.x * forward.x + forward.z * forward.z);
    if (flat < 0.001f) {
        return false;
    }
    float ahead = (offset.x * forward.x + offset.z * forward.z) / flat;
    return ahead < -kShoulderBehind && offset.y > -kShoulderDrop &&
           offset.x * offset.x + offset.z * offset.z < kShoulderReach * kShoulderReach;
}

bool SwordDrawn(Player* player) {
    s32 weapon = Player_GetMeleeWeaponHeld(player);
    return (weapon >= 1 && weapon <= 3) &&
           (player->leftHandType == PLAYER_MODELTYPE_LH_SWORD || player->leftHandType == PLAYER_MODELTYPE_LH_BGS);
}

// A gesture's request waits for the player's next update. One the player can't take soon, such as while
// carrying something, is dropped rather than carried out late.
s32 sItemRequest = VR_REQUEST_NONE;
float sItemRequestAge = 0.0f;
constexpr float kItemRequestLifetime = 0.5f; // seconds
bool sSwordGripLatched[HAND_COUNT] = {};
float sSwordGestureCooldown = 0.0f;
bool sUseTriggerLatched = false;

void RequestItem(s32 request) {
    sItemRequest = request;
    sItemRequestAge = 0.0f;
}

enum class HeldTool { None, Bow, Hookshot, Boomerang };

// Items whose use isn't a plain button press: the bow is drawn with the string hand's trigger, the hookshot
// fired with its own hand's trigger, and the boomerang thrown by letting go of its hand's grip.
HeldTool CurrentTool() {
    if (!Vr_IsWorldStereo()) {
        return HeldTool::None;
    }
    s8 action = GET_PLAYER(gPlayState)->heldItemAction;
    if ((action >= PLAYER_IA_BOW && action <= PLAYER_IA_BOW_0E) || action == PLAYER_IA_SLINGSHOT) {
        return HeldTool::Bow;
    }
    if (action == PLAYER_IA_HOOKSHOT || action == PLAYER_IA_LONGSHOT) {
        return HeldTool::Hookshot;
    }
    if (action == PLAYER_IA_BOOMERANG) {
        return HeldTool::Boomerang;
    }
    return HeldTool::None;
}

// The boomerang's hand holds it while its grip is squeezed, including a squeeze still held from choosing it, and
// letting go throws it from that hand the way the hand was heading. VR throws it itself (see Vr_TakeBoomerangThrow) rather than through the game's aiming, which
// switches to first-person aim and only throws once its animation is ready. While it's out, the hand waits for it.
bool sBoomerangGripDown = false;
bool sBoomerangThrowRequested = false;
bool sBoomerangAimed = false;
Vec3 sBoomerangDirection = {};

Vec3 ThrowVelocity(Hand hand);
float MetersPerSecond(Vec3 perTick);
Vec3 AimDirection(Hand hand);

void UpdateBoomerang() {
    if (CurrentTool() != HeldTool::Boomerang ||
        (GET_PLAYER(gPlayState)->stateFlags1 & PLAYER_STATE1_BOOMERANG_THROWN)) {
        sBoomerangGripDown = false;
        sBoomerangThrowRequested = false;
        return;
    }
    Hand hand = ControllerFor(LINK_LEFT);
    bool grip = Runtime::Get().GetController(hand).gripHeld;
    if (grip) {
        sBoomerangGripDown = true;
    } else if (sBoomerangGripDown) {
        sBoomerangGripDown = false;
        Vec3 velocity = ThrowVelocity(hand);
        bool swung = MetersPerSecond(velocity) >= kMinThrowSpeed;
        sBoomerangDirection = swung ? velocity : AimDirection(hand);
        sBoomerangAimed = true;
        sBoomerangThrowRequested = true;
        SPDLOG_INFO("[VR] Threw the boomerang at {:.1f} m/s", MetersPerSecond(velocity));
    }
}

// Blocks (and Armos statues) are taken hold of by hand. With both grips closed and both hands out in front at least
// the block reach (meters, from the head), A is held for one as soon as the game offers to grab it (Link up against
// it and facing it), as the game wants A held for as long as Link holds on. The grips can close first and Link be
// walked into it after, since its collision can be smaller than it looks. Pushing and pulling is the stick's, as in
// the game, and relative to where the head looks, as walking is: forward while looking at the block pushes it, and
// forward while looking away pulls it. Letting go of either grip lets go of the block.
constexpr float kBlockReach = 0.15f;
bool sBlockHeld = false;

// Where Link faces, in the play space (only x and z are set).
Vec3 FacingInPlay(Player* player) {
    return RotateYaw(-sYaw, { Math_SinS(player->actor.shape.rot.y), 0.0f, Math_CosS(player->actor.shape.rot.y) });
}

void UpdateBlockGrip() {
    const Runtime& runtime = Runtime::Get();
    const ControllerState& left = runtime.GetController(HAND_LEFT);
    const ControllerState& right = runtime.GetController(HAND_RIGHT);
    Player* player = GET_PLAYER(gPlayState);
    bool gripping = left.gripHeld && right.gripHeld && left.gripPose.valid && right.gripPose.valid;
    bool offered = player->stateFlags2 & (PLAYER_STATE2_DO_ACTION_GRAB | PLAYER_STATE2_GRABBING_DYNAPOLY);
    if (sBlockHeld) {
        if (!gripping || !offered) {
            sBlockHeld = false;
        }
        return;
    }
    const Pose& head = runtime.GetHead();
    if (!gripping || !(player->stateFlags2 & PLAYER_STATE2_DO_ACTION_GRAB) || !head.valid) {
        return;
    }
    Vec3 facing = FacingInPlay(player);
    auto reach = [&](const ControllerState& hand) {
        return (hand.gripPose.position[0] - head.position[0]) * facing.x +
               (hand.gripPose.position[2] - head.position[2]) * facing.z;
    };
    if (reach(left) < kBlockReach || reach(right) < kBlockReach) {
        return;
    }
    sBlockHeld = true;
    Runtime::Get().Vibrate(HAND_LEFT, 0.4f, 0.05f);
    Runtime::Get().Vibrate(HAND_RIGHT, 0.4f, 0.05f);
    SPDLOG_INFO("[VR] Took hold of a block with both hands");
}

// The note the microphone heard last tick, and its onset (see Vr_OverridePads).
uint16_t sMicButton = 0;
uint32_t sMicOnset = 0;

// Climbing by hand. On a ladder or a climbable wall, a hand that closes its grip holds on, and pulling it down climbs
// up, pushing it up climbs down and, on a wall, pulling it sideways climbs the other way: the stick the game climbs
// by is set from how fast the holding hands move, so reaching up with one hand while the other pulls climbs hand
// over hand. Hanging from a ledge, pulling up with a hand that holds on climbs onto it, and letting go of every hand
// that held drops. Where the game offers to climb a low ledge, closing both hands and pulling climbs it. The stick
// still climbs as before.
constexpr float kClimbMinSpeed = 0.1f;  // meters a second a hand has to pull to climb
constexpr float kClimbFullSpeed = 0.6f; // and at which Link climbs as fast as he can
constexpr float kClimbStickMax = 67.0f; // the stick that gives the game's fastest climb
constexpr float kLedgePull = 0.12f;     // meters to pull, holding on, to climb onto a ledge
constexpr float kClimbSmoothing = 0.3f;
struct ClimbHand {
    bool holding = false;
    float last[3] = {};
    float pulled = 0.0f; // meters pulled down since taking hold
};
ClimbHand sClimbHands[HAND_COUNT];
float sClimbStick[2] = {}; // x and y, in the game's stick units, while a hand holds on
bool sClimbStickActive = false;
bool sClimbHeldLedge = false; // a hand held on while hanging, so letting go drops

bool Climbing(Player* player) {
    return player->stateFlags1 &
           (PLAYER_STATE1_CLIMBING_LADDER | PLAYER_STATE1_HANGING_OFF_LEDGE | PLAYER_STATE1_CLIMBING_LEDGE);
}

void UpdateClimb() {
    Runtime& runtime = Runtime::Get();
    Player* player = GET_PLAYER(gPlayState);
    bool onWall = player->stateFlags1 & PLAYER_STATE1_CLIMBING_LADDER;
    bool hanging = (player->stateFlags1 & PLAYER_STATE1_HANGING_OFF_LEDGE) &&
                   !(player->stateFlags1 & PLAYER_STATE1_CLIMBING_LEDGE);
    bool climbOffered = player->stateFlags2 & PLAYER_STATE2_DO_ACTION_CLIMB;
    uint32_t refresh = runtime.GetRefreshRate();
    float dt = refresh > 0 ? 1.0f / refresh : 0.0f;
    if (!CVarGetInteger(CVAR_VR_CLIMB_BY_HAND, 1) || dt == 0.0f || (!onWall && !hanging && !climbOffered)) {
        for (ClimbHand& hand : sClimbHands) {
            hand = {};
        }
        sClimbStickActive = false;
        sClimbStick[0] = sClimbStick[1] = 0.0f;
        sClimbHeldLedge = false;
        return;
    }

    // Link's right on the wall he faces, in the play space.
    Vec3 facing = FacingInPlay(player);
    Vec3 right = { -facing.z, 0.0f, facing.x };
    float up = 0.0f;
    float side = 0.0f;
    int holding = 0;
    int pulledFar = 0;
    for (int i = 0; i < HAND_COUNT; i++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(i));
        ClimbHand& hand = sClimbHands[i];
        if (!state.gripHeld || !state.gripPose.valid) {
            hand.holding = false;
            continue;
        }
        if (!hand.holding) {
            hand.holding = true;
            hand.pulled = 0.0f;
            memcpy(hand.last, state.gripPose.position, sizeof(hand.last));
            runtime.Vibrate(static_cast<Hand>(i), 0.3f, 0.03f);
            continue;
        }
        float dx = state.gripPose.position[0] - hand.last[0];
        float dy = state.gripPose.position[1] - hand.last[1];
        float dz = state.gripPose.position[2] - hand.last[2];
        memcpy(hand.last, state.gripPose.position, sizeof(hand.last));
        // The body goes the other way to the hand that holds on.
        up -= dy / dt;
        side -= (dx * right.x + dz * right.z) / dt;
        hand.pulled -= dy;
        holding++;
        if (hand.pulled >= kLedgePull) {
            pulledFar++;
        }
    }

    if (hanging) {
        sClimbHeldLedge = sClimbHeldLedge || holding > 0;
        if (pulledFar > 0) {
            // Pushing the stick any way climbs up from a ledge.
            sClimbStick[0] = 0.0f;
            sClimbStick[1] = kClimbStickMax;
            sClimbStickActive = true;
            SPDLOG_INFO("[VR] Pulled up onto a ledge by hand");
        } else if (sClimbHeldLedge && holding == 0 && !sClimbHands[HAND_LEFT].holding &&
                   !sClimbHands[HAND_RIGHT].holding) {
            sLatchedButtons |= BTN_A;
            sClimbHeldLedge = false;
            SPDLOG_INFO("[VR] Let go of a ledge");
        } else {
            sClimbStickActive = false;
        }
        return;
    }
    sClimbHeldLedge = false;

    if (climbOffered && !onWall) {
        sClimbStickActive = false;
        if (pulledFar == HAND_COUNT) {
            sLatchedButtons |= BTN_A;
            for (ClimbHand& hand : sClimbHands) {
                hand.pulled = 0.0f;
            }
            SPDLOG_INFO("[VR] Climbed a ledge by hand");
        }
        return;
    }

    if (holding == 0) {
        sClimbStickActive = false;
        sClimbStick[0] = sClimbStick[1] = 0.0f;
        return;
    }
    up /= holding;
    side /= holding;
    auto stickFor = [](float speed) {
        if (fabsf(speed) < kClimbMinSpeed) {
            return 0.0f;
        }
        return copysignf(std::min(fabsf(speed) / kClimbFullSpeed, 1.0f) * kClimbStickMax, speed);
    };
    float target[2] = { stickFor(side), stickFor(up) };
    for (int i = 0; i < 2; i++) {
        sClimbStick[i] += (target[i] - sClimbStick[i]) * kClimbSmoothing;
    }
    sClimbStickActive = true;
}

bool sMenuInputsHeld = false;

// While the ocarina is up, the right stick pushed up, down, left or right plays C-up, C-down, C-left or C-right.
u16 OcarinaStickNotes(const float stick[2]) {
    if (fabsf(stick[1]) > 0.7f && fabsf(stick[1]) > fabsf(stick[0])) {
        return stick[1] > 0.0f ? BTN_CUP : BTN_CDOWN;
    }
    if (fabsf(stick[0]) > 0.7f && fabsf(stick[0]) > fabsf(stick[1])) {
        return stick[0] > 0.0f ? BTN_CRIGHT : BTN_CLEFT;
    }
    return 0;
}

u16 CurrentButtons() {
    const ControllerState& left = Runtime::Get().GetController(HAND_LEFT);
    const ControllerState& right = Runtime::Get().GetController(HAND_RIGHT);
    bool triggerTaken[HAND_COUNT] = {};
    switch (CurrentTool()) {
        case HeldTool::Bow:
            triggerTaken[ControllerFor(LINK_LEFT)] = true;
            break;
        case HeldTool::Hookshot:
            triggerTaken[ControllerFor(LINK_RIGHT)] = true;
            break;
        default:
            break;
    }

    u16 buttons = 0;
    buttons |= right.primary ? BTN_A : 0;
    // On the floating screen (the pause menu, where B saves, file select, cutscenes, pre-rendered rooms) the game
    // is played flat, so the buttons are the game's own.
    if (!Vr_IsWorldStereo()) {
        // While Link is played on the screen, Y opens the item menu instead of pressing C-right. Whatever chooses
        // from the menu doesn't also press the game's buttons, until it's let go.
        bool anyHeld = left.triggerHeld || right.triggerHeld || left.gripHeld || right.gripHeld || left.primary ||
                       left.secondary;
        if (Items_MenuOpen()) {
            sMenuInputsHeld = true;
        } else if (!anyHeld) {
            sMenuInputsHeld = false;
        }
        buttons |= right.secondary ? BTN_B : 0;
        if (!sMenuInputsHeld) {
            buttons |= left.primary ? BTN_CLEFT : 0;
            buttons |= (left.secondary && !ScreenGameplay()) ? BTN_CRIGHT : 0;
            buttons |= right.triggerHeld ? BTN_CDOWN : 0;
            buttons |= left.triggerHeld ? BTN_Z : 0;
            buttons |= left.gripHeld ? BTN_R : 0;
        }
        if (PlayingOcarina()) {
            buttons |= OcarinaStickNotes(right.stick);
        } else if (fabsf(right.stick[1]) > 0.7f && fabsf(right.stick[1]) > fabsf(right.stick[0])) {
            buttons |= right.stick[1] > 0.0f ? BTN_CUP : BTN_L;
        }
        buttons |= (left.menu || left.stickClick) ? BTN_START : 0;
        return buttons;
    }
    // In the world the sword, shield and items are physical, so R and the C buttons aren't on any button. B is
    // there for text boxes and to put the ocarina away. While it's up, its notes are A, X, Y and the right stick
    // pushed up and down.
    bool ocarina = PlayingOcarina();
    if (gPlayState->msgCtx.msgMode != MSGMODE_NONE) {
        buttons |= right.secondary ? BTN_B : 0;
    }
    if (ocarina) {
        buttons |= left.primary ? BTN_CLEFT : 0;
        buttons |= left.secondary ? BTN_CRIGHT : 0;
    }
    buttons |= (left.triggerHeld && !triggerTaken[HAND_LEFT]) ? BTN_Z : 0;
    buttons |= sBlockHeld ? BTN_A : 0;
    // A spin attack charges for as long as the sword hand's grip stays squeezed, as B held does.
    if ((GET_PLAYER(gPlayState)->stateFlags1 & PLAYER_STATE1_CHARGING_SPIN_ATTACK) &&
        Runtime::Get().GetController(ControllerFor(LINK_LEFT)).gripHeld) {
        buttons |= BTN_B;
    }
    // Fishing, the trigger of the hand without the rod reels in, as A does.
    if (GET_PLAYER(gPlayState)->heldItemAction == PLAYER_IA_FISHING_POLE &&
        Runtime::Get().GetController(ControllerFor(LINK_RIGHT)).triggerHeld) {
        buttons |= BTN_A;
    }
    // As a child the Hylian Shield is too big to hold, so crouching behind it is the game's own shield stance.
    if (Crouching() && Vr_IsWorldStereo() && Player_IsChildWithHylianShield(GET_PLAYER(gPlayState))) {
        buttons |= BTN_R;
    }
    // The right stick turns sideways. Pushed down it's L; up does nothing, since C-up's first-person view has no place
    // in VR. With the ocarina up it plays the four C notes instead, and doesn't turn (see Game_OnFrame).
    if (ocarina) {
        buttons |= OcarinaStickNotes(right.stick);
    } else if (fabsf(right.stick[1]) > 0.7f && fabsf(right.stick[1]) > fabsf(right.stick[0]) &&
               right.stick[1] < 0.0f) {
        buttons |= BTN_L;
    }
    // SteamVR on a Quest keeps the menu button for its own dashboard, so the stick click pauses too.
    buttons |= (left.menu || left.stickClick) ? BTN_START : 0;
    return buttons;
}

// Where a controller points, in the world.
Vec3 AimDirection(Hand hand) {
    return RotateYaw(sYaw, Rotate(Runtime::Get().GetController(hand).aimPose.orientation, { 0.0f, 0.0f, -1.0f }));
}

void FaceAlong(Actor* actor, Vec3 direction) {
    s16 yaw = RADF_TO_BINANG(atan2f(direction.x, direction.z));
    s16 pitch = RADF_TO_BINANG(atan2f(-direction.y, sqrtf(direction.x * direction.x + direction.z * direction.z)));
    actor->world.rot.x = actor->shape.rot.x = pitch;
    actor->world.rot.y = actor->shape.rot.y = yaw;
}

// Whether the string hand is drawing the bow: its trigger went down near the bow and is still held.
bool sDrawing = false;
bool sDrawTriggerLatched = false;
bool sHookshotTriggerLatched = false;
// Whether the hook was in the hookshot last frame. A pull only aims while it is, since the game takes no button
// while the hook is out, and a pull kept for later fired some time after it was let go. The trigger still held as
// the hook comes back aims it then, so letting go fires.
bool sHookshotWasHome = false;

// With the hookshot in hand and no hook in it, the game takes no item, not even the sword, until the hook is back,
// which while it flies is a moment. If it stays that way past the missing time (seconds), this logs what the hook
// is doing, and if the hook is gone gives the hookshot a new one, as taking it out does.
constexpr float kHookMissingTime = 3.0f;
float sHookMissingFor = 0.0f;

void RepairHookshot() {
    Player* player = GET_PLAYER(gPlayState);
    uint32_t rate = Runtime::Get().GetRefreshRate();
    if (!Player_HoldsHookshot(player) || player->heldActor != nullptr || rate == 0) {
        sHookMissingFor = 0.0f;
        return;
    }
    sHookMissingFor += 1.0f / rate;
    if (sHookMissingFor < kHookMissingTime) {
        return;
    }
    sHookMissingFor = 0.0f;
    ArmsHook* hook = nullptr;
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; actor != nullptr;
         actor = actor->next) {
        if (actor->id == ACTOR_ARMS_HOOK) {
            hook = reinterpret_cast<ArmsHook*>(actor);
            break;
        }
    }
    if (hook != nullptr) {
        SPDLOG_WARN("[VR] The hook has been out of the hookshot for {} s: its parent {}, child {}, timer {}, "
                    "grabbed {}; Link's child {}, parent {}, state1 {:08X}, state3 {:02X}",
                    kHookMissingTime, static_cast<void*>(hook->actor.parent), static_cast<void*>(hook->actor.child),
                    hook->timer, static_cast<void*>(hook->grabbed), static_cast<void*>(player->actor.child),
                    static_cast<void*>(player->actor.parent), player->stateFlags1, player->stateFlags3);
        return;
    }
    SPDLOG_WARN("[VR] The hookshot's hook was gone; gave it a new one (Link's child {}, parent {}, state1 {:08X}, "
                "state3 {:02X})",
                static_cast<void*>(player->actor.child), static_cast<void*>(player->actor.parent), player->stateFlags1,
                player->stateFlags3);
    player->heldActor =
        Actor_SpawnAsChild(&gPlayState->actorCtx, &player->actor, gPlayState, ACTOR_ARMS_HOOK, player->actor.world.pos.x,
                           player->actor.world.pos.y, player->actor.world.pos.z, 0, player->actor.shape.rot.y, 0, 0);
}

// A trigger pull with the hook home should have the hookshot aimed (or fired) by the game's next tick. Past the check
// time (seconds) after a pull that didn't, this logs what the game was doing instead.
constexpr float kHookshotPullCheckTime = 0.3f;
float sHookshotPullCheckIn = -1.0f;

const char* UpperActionName(Player* player) {
    if (player->upperActionFunc == func_8083501C) {
        return "out (func_8083501C)";
    } else if (player->upperActionFunc == func_808351D4) {
        return "aiming (func_808351D4)";
    } else if (player->upperActionFunc == func_808353D8) {
        return "after a shot (func_808353D8)";
    } else if (player->upperActionFunc == Player_UpperAction_ChangeHeldItem) {
        return "changing item";
    }
    return "other";
}

void CheckHookshotPull() {
    uint32_t rate = Runtime::Get().GetRefreshRate();
    if (sHookshotPullCheckIn < 0.0f || rate == 0) {
        return;
    }
    sHookshotPullCheckIn -= 1.0f / rate;
    if (sHookshotPullCheckIn > 0.0f) {
        return;
    }
    sHookshotPullCheckIn = -1.0f;
    Player* player = GET_PLAYER(gPlayState);
    if (!Player_HoldsHookshot(player) || player->heldActor == nullptr ||
        (player->stateFlags1 & PLAYER_STATE1_READY_TO_FIRE) || (player->upperActionFunc == func_808351D4)) {
        return;
    }
    ArmsHook* hook = nullptr;
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ITEMACTION].head; actor != nullptr;
         actor = actor->next) {
        if (actor == player->heldActor) {
            hook = reinterpret_cast<ArmsHook*>(actor);
        }
    }
    const char* hookState = (hook == nullptr)                        ? "not a live hook"
                            : (hook->actionFunc == ArmsHook_Wait)    ? "waiting"
                            : (hook->actionFunc == ArmsHook_Shoot)   ? "shooting"
                                                                     : "other";
    SPDLOG_WARN("[VR] A hookshot pull didn't aim it: request {}, upper action {}, item {} action {} held action {}, "
                "state1 {:08X} state2 {:08X} state3 {:02X}, unk_860 {} unk_836 {} unk_834 {} unk_6AD {}, door {}, "
                "ground {}, hook {} (parent {}), Link's child {}",
                sItemRequest == VR_REQUEST_USE_HELD ? "never taken" : "taken", UpperActionName(player),
                player->heldItemId, player->itemAction, player->heldItemAction, player->stateFlags1,
                player->stateFlags2, player->stateFlags3, player->unk_860, player->unk_836, player->unk_834,
                player->unk_6AD, player->doorType, (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) != 0, hookState,
                static_cast<void*>(hook != nullptr ? hook->actor.parent : nullptr),
                static_cast<void*>(player->actor.child));
}

// Nocking the bow and pulling the hookshot's trigger each aim it afresh, as pressing its button again does, so
// it can be used over and over without going back to the belt.
void UpdateDraw() {
    const Runtime& runtime = Runtime::Get();
    const ControllerState& stringHand = runtime.GetController(ControllerFor(LINK_LEFT));
    const ControllerState& bowHand = runtime.GetController(ControllerFor(LINK_RIGHT));
    HeldTool tool = CurrentTool();
    bool held = stringHand.triggerHeld;
    if (tool != HeldTool::Bow || !held) {
        sDrawing = false;
    } else if (!sDrawTriggerLatched && stringHand.gripPose.valid && bowHand.gripPose.valid) {
        float dx = stringHand.gripPose.position[0] - bowHand.gripPose.position[0];
        float dy = stringHand.gripPose.position[1] - bowHand.gripPose.position[1];
        float dz = stringHand.gripPose.position[2] - bowHand.gripPose.position[2];
        sDrawing = dx * dx + dy * dy + dz * dz < kNockReach * kNockReach;
        if (sDrawing) {
            RequestItem(VR_REQUEST_USE_HELD);
        }
    }
    sDrawTriggerLatched = held;

    bool hookshotTrigger = bowHand.triggerHeld;
    bool hookHome = tool == HeldTool::Hookshot && GET_PLAYER(gPlayState)->heldActor != nullptr;
    if (hookHome && hookshotTrigger && (!sHookshotTriggerLatched || !sHookshotWasHome)) {
        RequestItem(VR_REQUEST_USE_HELD);
        sHookshotPullCheckIn = kHookshotPullCheckTime;
    }
    sHookshotTriggerLatched = hookshotTrigger;
    sHookshotWasHome = hookHome;
}

// Where the string is drawn to, in the play space: the string hand, but no farther from the bow hand than a full
// draw.
bool DrawPoint(Player* player, float out[3]) {
    const Runtime& runtime = Runtime::Get();
    const Pose& bow = runtime.GetController(ControllerFor(LINK_RIGHT)).gripPose;
    const Pose& string = runtime.GetController(ControllerFor(LINK_LEFT)).gripPose;
    if (!bow.valid || !string.valid) {
        return false;
    }
    float maxDraw = Player_HoldsSlingshot(player) ? kMaxSlingshotDraw : kMaxBowDraw;
    Vec3 draw = { string.position[0] - bow.position[0], string.position[1] - bow.position[1],
                  string.position[2] - bow.position[2] };
    float length = sqrtf(Dot(draw, draw));
    float scale = length > maxDraw ? maxDraw / length : 1.0f;
    out[0] = bow.position[0] + draw.x * scale;
    out[1] = bow.position[1] + draw.y * scale;
    out[2] = bow.position[2] + draw.z * scale;
    return true;
}

// Grip positions over the last few headset frames, for measuring how fast a hand moves when it lets go.
constexpr int kHandHistory = 5;
float sHandHistory[HAND_COUNT][kHandHistory][3] = {};
int sHandHistoryCount[HAND_COUNT] = {};

void RecordHands() {
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const Pose& grip = Runtime::Get().GetController(static_cast<Hand>(hand)).gripPose;
        if (!grip.valid) {
            sHandHistoryCount[hand] = 0;
            continue;
        }
        memmove(sHandHistory[hand][1], sHandHistory[hand][0], sizeof(float) * 3 * (kHandHistory - 1));
        memcpy(sHandHistory[hand][0], grip.position, sizeof(float) * 3);
        sHandHistoryCount[hand] = std::min(sHandHistoryCount[hand] + 1, kHandHistory);
    }
}

// How fast the hand is moving in the world, in game units per tick, including Link's own movement.
Vec3 ThrowVelocity(Hand hand) {
    uint32_t refresh = Runtime::Get().GetRefreshRate();
    int count = sHandHistoryCount[hand];
    Vec3 velocity = { 0.0f, 0.0f, 0.0f };
    if (refresh > 0 && count >= 2) {
        const float* newest = sHandHistory[hand][0];
        const float* oldest = sHandHistory[hand][count - 1];
        float seconds = static_cast<float>(count - 1) / refresh;
        float perTick = sRender.unitsPerMeter / (seconds * kTicksPerSecond);
        velocity = RotateYaw(sYaw, { (newest[0] - oldest[0]) * perTick, (newest[1] - oldest[1]) * perTick,
                                     (newest[2] - oldest[2]) * perTick });
    }
    velocity.x += sRender.anchor.x - sRenderPrevAnchor.x;
    velocity.y += sRender.anchor.y - sRenderPrevAnchor.y;
    velocity.z += sRender.anchor.z - sRenderPrevAnchor.z;
    return velocity;
}

float MetersPerSecond(Vec3 perTick) {
    return sqrtf(Dot(perTick, perTick)) * kTicksPerSecond / sBuild.unitsPerMeter;
}

// How fast a hand is moving in the play space (meters per second), leaving out Link's own movement.
float HandSpeed(Hand hand) {
    uint32_t refresh = Runtime::Get().GetRefreshRate();
    int count = sHandHistoryCount[hand];
    if (refresh == 0 || count < 2) {
        return 0.0f;
    }
    const float* newest = sHandHistory[hand][0];
    const float* oldest = sHandHistory[hand][count - 1];
    Vec3 moved = { newest[0] - oldest[0], newest[1] - oldest[1], newest[2] - oldest[2] };
    return sqrtf(Dot(moved, moved)) * refresh / static_cast<float>(count - 1);
}

// What can be lifted by hand (see UpdateLiftGrab) is within this reach (game units) of a point this far above its
// origin.
constexpr float kLiftReach = 22.0f;
constexpr float kLiftCenterHeight = 12.0f;

// A throw the grip let go of, waiting for the game to release the carried actor.
bool sThrowPending = false;
bool sThrowRequested = false;
Vec3 sThrowVelocity = {};

// A Deku nut held in the throwing hand, and one just thrown that the game hasn't launched yet.
Actor* sHeldNut = nullptr;
Actor* sThrownNut = nullptr;
Vec3 sNutVelocity = {};

// Whether an actor is still alive, since a held one can be removed without telling anyone. Only compares
// pointers, as a removed actor's memory can't be read.
bool ActorAlive(Actor* actor) {
    if (actor == nullptr || gPlayState == nullptr) {
        return false;
    }
    for (const ActorListEntry& list : gPlayState->actorCtx.actorLists) {
        for (Actor* other = list.head; other != nullptr; other = other->next) {
            if (other == actor) {
                return true;
            }
        }
    }
    return false;
}

void ReleaseNut(Vec3 velocity) {
    if (!ActorAlive(sHeldNut)) {
        sHeldNut = nullptr;
        return;
    }
    sHeldNut->parent = nullptr;
    sThrownNut = sHeldNut;
    sNutVelocity = velocity;
    sHeldNut = nullptr;
    SPDLOG_INFO("[VR] Threw a Deku nut at {:.1f} m/s", MetersPerSecond(velocity));
}

// Keeps a held nut in the throwing hand.
void HoldNut() {
    if (sHeldNut == nullptr) {
        return;
    }
    if (!ActorAlive(sHeldNut)) {
        sHeldNut = nullptr;
        return;
    }
    const Pose& grip = Runtime::Get().GetController(ControllerFor(LINK_RIGHT)).gripPose;
    if (grip.valid) {
        Vec3 hand = ToWorld(grip.position, sBuild.anchor, sBuild.unitsPerMeter);
        sHeldNut->world.pos = { hand.x, hand.y, hand.z };
        HeldRotation(GET_PLAYER(gPlayState), &sHeldNut->shape.rot);
    }
}

// Squeezing the throwing hand's grip, including a squeeze still held from choosing the item, takes hold of what it
// carries, and letting go throws it.
bool sCarryArmed = false;

// Carried things (not nuts) can be held in both hands: the other hand closes on it, or was already closed on it
// when it was lifted. In both, it sits between the hands. When one lets go, the release waits out the throw window
// with that hand's velocity, for the other to let go too.
bool sCarryTwoHanded = false;
bool sWasCarrying = false;
bool sSupportGripLatched = false;
float sReleaseWait = -1.0f; // seconds since one hand let go of a two-handed hold, or below 0
Hand sReleasedHand = HAND_LEFT;
Vec3 sReleasedVelocity = {};

Hand OtherHand(Hand hand) {
    return hand == HAND_LEFT ? HAND_RIGHT : HAND_LEFT;
}

bool HandsTogether() {
    const Runtime& runtime = Runtime::Get();
    const Pose& a = runtime.GetController(HAND_LEFT).gripPose;
    const Pose& b = runtime.GetController(HAND_RIGHT).gripPose;
    if (!a.valid || !b.valid) {
        return false;
    }
    Vec3 apart = { a.position[0] - b.position[0], a.position[1] - b.position[1], a.position[2] - b.position[2] };
    return Dot(apart, apart) < kTwoHandCarryReach * kTwoHandCarryReach;
}

void ThrowCarried(Player* player, Vec3 velocity, bool bothHands) {
    if (MetersPerSecond(velocity) < kMinThrowSpeed) {
        // A slow release sets it down where the hand is.
        velocity = { 0.0f, 0.0f, 0.0f };
    }
    // Bombs, bombchus, pots, rocks and the like leave the hand the way it was moving.
    sThrowPending = true;
    sThrowRequested = true;
    sThrowVelocity = velocity;
    SPDLOG_INFO("[VR] Threw a carried actor {}{} at {:.1f} m/s", player->heldActor->id,
                bothHands ? " with both hands" : "", MetersPerSecond(velocity));
}

void UpdateCarry() {
    Runtime& runtime = Runtime::Get();
    Player* player = Vr_IsWorldStereo() ? GET_PLAYER(gPlayState) : nullptr;
    bool carrying = player != nullptr && (player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR) &&
                    player->heldActor != nullptr;
    bool holdingNut = sHeldNut != nullptr;
    Hand hand = ControllerFor(LINK_RIGHT);
    Hand other = OtherHand(hand);
    bool gripHeld = runtime.GetController(hand).gripHeld;
    bool otherHeld = runtime.GetController(other).gripHeld;
    bool otherPressed = otherHeld && !sSupportGripLatched;
    sSupportGripLatched = otherHeld;
    bool liftedNow = carrying && !sWasCarrying;
    sWasCarrying = carrying;
    if (!carrying && !holdingNut) {
        sCarryArmed = false;
        sCarryTwoHanded = false;
        sReleaseWait = -1.0f;
        return;
    }

    if (carrying && sCarryTwoHanded) {
        if (sReleaseWait >= 0.0f) {
            Hand stillHolding = OtherHand(sReleasedHand);
            if (!runtime.GetController(stillHolding).gripHeld) {
                // Both let go together: thrown with both hands' swing.
                Vec3 a = sReleasedVelocity;
                Vec3 b = ThrowVelocity(stillHolding);
                sCarryTwoHanded = false;
                sReleaseWait = -1.0f;
                sCarryArmed = false;
                ThrowCarried(player, { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f }, true);
                return;
            }
            uint32_t refresh = runtime.GetRefreshRate();
            sReleaseWait += refresh > 0 ? 1.0f / refresh : kTwoHandThrowWindow;
            if (sReleaseWait > kTwoHandThrowWindow) {
                // Only one let go: the other carries it on its own, and throws it when it lets go.
                PutLinkHandOn(LINK_RIGHT, stillHolding);
                sCarryTwoHanded = false;
                sReleaseWait = -1.0f;
                sCarryArmed = true;
                SPDLOG_INFO("[VR] Carrying actor {} in one hand again", player->heldActor->id);
            }
            return;
        }
        if (!gripHeld || !otherHeld) {
            sReleasedHand = gripHeld ? other : hand;
            sReleasedVelocity = ThrowVelocity(sReleasedHand);
            sReleaseWait = 0.0f;
        }
        return;
    }

    // The other hand takes hold too.
    if (carrying && gripHeld && (otherPressed || (liftedNow && otherHeld)) && HandsTogether()) {
        sCarryTwoHanded = true;
        sCarryArmed = true;
        runtime.Vibrate(other, 0.4f, 0.05f);
        SPDLOG_INFO("[VR] Holding actor {} in both hands", player->heldActor->id);
        return;
    }

    if (gripHeld) {
        sCarryArmed = true;
        return;
    }
    if (gripHeld || !sCarryArmed) {
        return;
    }
    sCarryArmed = false;

    Vec3 velocity = ThrowVelocity(hand);
    if (holdingNut) {
        if (MetersPerSecond(velocity) < kMinThrowSpeed) {
            velocity = { 0.0f, 0.0f, 0.0f };
        }
        // Nuts are light, so they leave the hand a little faster than it moved.
        ReleaseNut({ velocity.x * kThrowBoost, velocity.y * kThrowBoost, velocity.z * kThrowBoost });
    } else {
        ThrowCarried(player, velocity, false);
    }
}

// A door or chest a hand has closed on, and where the hand was then (play space, meters). The game still decides
// when Link is close enough and facing one, as it does before offering A; the hand opens it as A would.
struct WorldGrab {
    Actor* actor = nullptr;
    float start[3] = {};
};
WorldGrab sWorldGrab[HAND_COUNT];
bool sWorldGripLatched[HAND_COUNT] = {};

bool HandOnDoor(Actor* door, Vec3 hand) {
    float s = Math_SinS(door->shape.rot.y);
    float c = Math_CosS(door->shape.rot.y);
    Vec3 d = { hand.x - door->world.pos.x, hand.y - door->world.pos.y, hand.z - door->world.pos.z };
    float depth = d.x * s + d.z * c; // along the way the door faces
    float across = d.x * c - d.z * s;
    return fabsf(depth) < kDoorReachDepth && fabsf(across) < kDoorHalfWidth && d.y > 0.0f && d.y < kDoorHeight;
}

bool HandOnChest(Actor* chest, Vec3 hand) {
    float size = chest->scale.x / 0.01f; // big chests are drawn at 0.01, small ones at 0.005
    Vec3 d = { hand.x - chest->world.pos.x, hand.y - chest->world.pos.y, hand.z - chest->world.pos.z };
    float reach = kChestReach * size;
    return d.x * d.x + d.z * d.z < reach * reach && d.y > 0.0f && d.y < kChestHeight * size;
}

void UpdateWorldGrabs() {
    Runtime& runtime = Runtime::Get();
    Player* player = GET_PLAYER(gPlayState);
    Actor* door = (player->doorType != PLAYER_DOORTYPE_NONE) ? player->doorActor : nullptr;
    Actor* chest = (player->interactRangeActor != nullptr && player->interactRangeActor->id == ACTOR_EN_BOX)
                       ? player->interactRangeActor
                       : nullptr;
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        bool pressed = state.gripHeld && !sWorldGripLatched[hand];
        sWorldGripLatched[hand] = state.gripHeld;
        WorldGrab& grab = sWorldGrab[hand];
        if (!state.gripHeld || !state.gripPose.valid) {
            grab.actor = nullptr;
            continue;
        }
        if (pressed) {
            Vec3 world = ToWorld(state.gripPose.position, sBuild.anchor, sBuild.unitsPerMeter);
            grab.actor = (door != nullptr && HandOnDoor(door, world))     ? door
                         : (chest != nullptr && HandOnChest(chest, world)) ? chest
                                                                            : nullptr;
            memcpy(grab.start, state.gripPose.position, sizeof(grab.start));
            if (grab.actor != nullptr) {
                runtime.Vibrate(static_cast<Hand>(hand), 0.3f, 0.03f);
            }
            continue;
        }
        // Out of reach now, so the game wouldn't open it either.
        if (grab.actor == nullptr || (grab.actor != door && grab.actor != chest)) {
            grab.actor = nullptr;
            continue;
        }
        Vec3 moved = RotateYaw(sYaw, { state.gripPose.position[0] - grab.start[0],
                                       state.gripPose.position[1] - grab.start[1],
                                       state.gripPose.position[2] - grab.start[2] });
        bool opens;
        if (grab.actor == door) {
            float along = moved.x * Math_SinS(door->shape.rot.y) + moved.z * Math_CosS(door->shape.rot.y);
            opens = fabsf(along) > kDoorPushDistance;
        } else {
            opens = moved.y > kChestLiftDistance;
        }
        if (opens) {
            sLatchedButtons |= BTN_A;
            runtime.Vibrate(static_cast<Hand>(hand), 0.7f, 0.08f);
            SPDLOG_INFO("[VR] Opened a {} by hand", grab.actor == door ? "door" : "chest");
            grab.actor = nullptr;
        }
    }
}

bool HandOnDoorOrChest(Hand hand) {
    return sWorldGrab[hand].actor != nullptr;
}

// Pots, rocks, bomb flowers and the like, which the game offers Link to lift with A (offered with no item, unlike
// chests and catchable creatures), are lifted by closing a hand on them, within the lift reach (game units) of
// a point a little above their origin. The grabbing hand becomes the throwing hand, and the grip still held carries
// it until it's let go (see UpdateCarry). A while carrying still puts it down or throws it as the game does.
bool sLiftGripLatched[HAND_COUNT] = {};

void UpdateLiftGrab() {
    Runtime& runtime = Runtime::Get();
    Player* player = GET_PLAYER(gPlayState);
    Actor* liftable = player->interactRangeActor;
    bool canLift = liftable != nullptr && player->getItemId == GI_NONE && player->heldActor == nullptr &&
                   !(player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR);
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        bool pressed = state.gripHeld && !sLiftGripLatched[hand];
        sLiftGripLatched[hand] = state.gripHeld;
        if (!pressed || !canLift || !state.gripPose.valid) {
            continue;
        }
        Vec3 world = ToWorld(state.gripPose.position, sBuild.anchor, sBuild.unitsPerMeter);
        float dx = world.x - liftable->world.pos.x;
        float dy = world.y - (liftable->world.pos.y + kLiftCenterHeight);
        float dz = world.z - liftable->world.pos.z;
        if (dx * dx + dy * dy + dz * dz < kLiftReach * kLiftReach) {
            PutLinkHandOn(LINK_RIGHT, static_cast<Hand>(hand));
            sLatchedButtons |= BTN_A;
            runtime.Vibrate(static_cast<Hand>(hand), 0.5f, 0.06f);
            SPDLOG_INFO("[VR] Lifted actor {} by hand", liftable->id);
            return;
        }
    }
}

// A mask in a hand, put on by bringing it to the face (ITEM_NONE when there's none), and the controller holding it.
u8 sHeldMask = ITEM_NONE;
Hand sMaskHand = HAND_LEFT;
bool sMaskGripLatched[HAND_COUNT] = {};
// Whether the hand holding a mask has been away from the face since it took the mask off. Until it has, the mask
// doesn't go back on: the hand is still at the face right after, and it went straight back on the next frame.
bool sMaskLeftFace = true;

// A bottle taken from the menu goes on C-left (see Game_TakeItem); whether the request waiting for the player is
// one, and what a full one holds.
constexpr u8 kBottleButton = 1;
bool sBottleOnButton = false;
u8 sBottleContent = ITEM_NONE;
// A creature an empty bottle was just swung through, for the player's update to bottle.
Actor* sBottleCatch = nullptr;
bool sBottleTipped = false;

bool HandAtFace(const Pose& hand) {
    const Pose& head = Runtime::Get().GetHead();
    if (!hand.valid || !head.valid) {
        return false;
    }
    float dx = hand.position[0] - head.position[0];
    float dy = hand.position[1] - head.position[1];
    float dz = hand.position[2] - head.position[2];
    return dx * dx + dy * dy + dz * dz < kFaceReach * kFaceReach;
}

// Masks: one in a hand goes on when the hand reaches the face. With one on, a hand closing at the face takes it off
// into that hand. In VR Link's face isn't drawn, so wearing one shows only in how people react to it.
void UpdateMasks() {
    Runtime& runtime = Runtime::Get();
    Player* player = GET_PLAYER(gPlayState);
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
        bool pressed = state.gripHeld && !sMaskGripLatched[hand];
        sMaskGripLatched[hand] = state.gripHeld;
        if (sHeldMask != ITEM_NONE) {
            if (hand == sMaskHand && !HandAtFace(state.gripPose)) {
                sMaskLeftFace = true;
            } else if (hand == sMaskHand && sMaskLeftFace) {
                player->currentMask = sHeldMask - ITEM_MASK_KEATON + PLAYER_MASK_KEATON;
                gSaveContext.ship.maskMemory = player->currentMask;
                Audio_PlayActorSound2(&player->actor, NA_SE_PL_CHANGE_ARMS);
                runtime.Vibrate(static_cast<Hand>(hand), 0.6f, 0.06f);
                SPDLOG_INFO("[VR] Put on mask {}", player->currentMask);
                sHeldMask = ITEM_NONE;
            }
        } else if (player->currentMask != PLAYER_MASK_NONE && pressed && HandAtFace(state.gripPose)) {
            sHeldMask = player->currentMask - PLAYER_MASK_KEATON + ITEM_MASK_KEATON;
            sMaskHand = static_cast<Hand>(hand);
            sMaskLeftFace = false;
            player->currentMask = PLAYER_MASK_NONE;
            gSaveContext.ship.maskMemory = PLAYER_MASK_NONE;
            Audio_PlayActorSound2(&player->actor, NA_SE_PL_CHANGE_ARMS);
            runtime.Vibrate(static_cast<Hand>(hand), 0.6f, 0.06f);
            SPDLOG_INFO("[VR] Took off the mask");
        }
    }
}

bool IsCatchable(s16 actorId) {
    return actorId == ACTOR_EN_ELF || actorId == ACTOR_EN_FISH || actorId == ACTOR_EN_ICE_HONO ||
           actorId == ACTOR_EN_INSECT;
}

bool IsDrink(u8 item) {
    return item == ITEM_POTION_RED || item == ITEM_POTION_GREEN || item == ITEM_POTION_BLUE ||
           item == ITEM_MILK_BOTTLE || item == ITEM_MILK_HALF;
}

// Bottles, in Link's left hand. An empty one catches a bug, fish, fairy or blue fire it's swung through, when the
// game offers it to Link as catchable. One with something in it is used when it's tipped over, its opening (out of
// the thumb side of the fist) pointing down: at the mouth for something to drink, anywhere for the rest, which
// are let out or poured.
void UpdateBottle() {
    Runtime& runtime = Runtime::Get();
    Player* player = GET_PLAYER(gPlayState);
    u8 held = player->heldItemId;
    bool bottle = held >= ITEM_BOTTLE && held <= ITEM_POE && player->heldItemAction >= PLAYER_IA_BOTTLE &&
                  player->heldItemAction <= PLAYER_IA_BOTTLE_FAIRY;
    Hand hand = ControllerFor(LINK_LEFT);
    const ControllerState& state = runtime.GetController(hand);
    if (!bottle || !state.gripPose.valid) {
        sBottleTipped = false;
        return;
    }
    if (held == ITEM_BOTTLE) {
        Actor* creature = player->interactRangeActor;
        if (creature != nullptr && IsCatchable(creature->id) && HandSpeed(hand) > kCatchSpeed) {
            Vec3 mouth = ToWorld(state.gripPose.position, sBuild.anchor, sBuild.unitsPerMeter);
            float dx = mouth.x - creature->world.pos.x;
            float dy = mouth.y - creature->world.pos.y;
            float dz = mouth.z - creature->world.pos.z;
            if (dx * dx + dy * dy + dz * dz < kCatchReach * kCatchReach) {
                sBottleCatch = creature;
                runtime.Vibrate(hand, 0.8f, 0.1f);
            }
        }
        return;
    }
    Vec3 opening = RotateYaw(sYaw, Rotate(state.gripPose.orientation, { 0.0f, 0.0f, -1.0f }));
    bool tipped = opening.y < kBottleTipDown;
    if (tipped && !sBottleTipped && (!IsDrink(held) || HandAtFace(state.gripPose))) {
        RequestItem(VR_REQUEST_USE_HELD);
        runtime.Vibrate(hand, 0.4f, 0.08f);
        SPDLOG_INFO("[VR] Tipped bottle of item {}", held);
    }
    sBottleTipped = tipped;
}

// Fishing: the rod is in Link's left hand, and follows the controller. A quick swing of it casts, as B does, and
// the other hand's trigger reels in, as A does (see CurrentButtons).
float sCastCooldown = 0.0f;

void UpdateFishing() {
    Runtime& runtime = Runtime::Get();
    if (sCastCooldown > 0.0f && runtime.GetRefreshRate() > 0) {
        sCastCooldown -= 1.0f / runtime.GetRefreshRate();
    }
    if (GET_PLAYER(gPlayState)->heldItemAction != PLAYER_IA_FISHING_POLE) {
        return;
    }
    Hand rod = ControllerFor(LINK_LEFT);
    if (sCastCooldown <= 0.0f && HandSpeed(rod) > kCastSpeed) {
        sLatchedButtons |= BTN_B;
        sCastCooldown = kCastCooldown;
        runtime.Vibrate(rod, 0.5f, 0.06f);
        SPDLOG_INFO("[VR] Cast the fishing rod");
    }
}

// The Deku stick, hammer and Biggoron's Sword can be held in both hands: the other hand closing on the handle near
// the first one turns the weapon to run along the line between them, away from whichever hand is lower on it.
bool sTwoHanded = false;
bool sTwoHandGripLatched = false;

bool TwoHandable(Player* player) {
    return player->leftHandType == PLAYER_MODELTYPE_LH_BGS || player->leftHandType == PLAYER_MODELTYPE_LH_HAMMER ||
           player->itemAction == PLAYER_IA_DEKU_STICK;
}

void UpdateTwoHanded() {
    Runtime& runtime = Runtime::Get();
    Hand main = ControllerFor(LINK_LEFT);
    Hand other = ControllerFor(LINK_RIGHT);
    const ControllerState& mainState = runtime.GetController(main);
    const ControllerState& otherState = runtime.GetController(other);
    bool pressed = otherState.gripHeld && !sTwoHandGripLatched;
    sTwoHandGripLatched = otherState.gripHeld;
    if (!TwoHandable(GET_PLAYER(gPlayState)) || !otherState.gripHeld || !mainState.gripPose.valid ||
        !otherState.gripPose.valid) {
        sTwoHanded = false;
        return;
    }
    float dx = mainState.gripPose.position[0] - otherState.gripPose.position[0];
    float dy = mainState.gripPose.position[1] - otherState.gripPose.position[1];
    float dz = mainState.gripPose.position[2] - otherState.gripPose.position[2];
    float distance = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!sTwoHanded && pressed && distance < kTwoHandReach) {
        sTwoHanded = true;
        runtime.Vibrate(main, 0.3f, 0.04f);
        runtime.Vibrate(other, 0.3f, 0.04f);
    } else if (sTwoHanded && distance > kTwoHandLetGo) {
        sTwoHanded = false;
    }
}

// One swing at a time: see kSwingStartSpeed.
bool sSwingActive = false;
bool sSwingHit = false;
int sSwingCooldown = 0;
float sSwingTravel = 0.0f; // meters the tip has carried its speed so far, while a swing builds up

// Points the game camera along the head so culling, billboards and sound follow where the player looks.
void AimGameCamera(PlayState* play) {
    const Runtime& runtime = Runtime::Get();
    const Pose& head = runtime.GetHead();
    if (!head.valid) {
        return;
    }
    Vec3 eye = ToWorld(head.position, sBuild.anchor, sBuild.unitsPerMeter);
    Vec3 forward = RotateYaw(sYaw, HeadForward());
    // Billboards such as the sun and moon turn with the game camera, so it stays upright instead of rolling with
    // the head. Straight up or down, world up is undefined as a camera up, so the head's own is used there.
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    if (fabsf(forward.y) > 0.98f) {
        up = RotateYaw(sYaw, Rotate(head.orientation, { 0.0f, 1.0f, 0.0f }));
    }
    play->view.eye = { eye.x, eye.y, eye.z };
    play->view.lookAt = { eye.x + forward.x * 100.0f, eye.y + forward.y * 100.0f, eye.z + forward.z * 100.0f };
    play->view.up = { up.x, up.y, up.z };

    float halfFov = 0.0f;
    for (const EyeView& view : runtime.GetEyes()) {
        halfFov = std::max({ halfFov, fabsf(view.fov.up), fabsf(view.fov.down) });
    }
    float fovy = RADF_TO_DEGF(halfFov * 2.0f) + kCullFovMargin;
    play->view.fovy = std::clamp(fovy, 60.0f, 150.0f);
}

Vec3f RootMotionOffset(Player* player);

// On the title screen the player watches its cutscene from its camera, wherever it goes: the head is put where the
// camera is, as a standing player's eyes, and the play space turns as the camera does, so the way the player faced
// when it began is always the way the camera looks. Its logo is on a big panel ahead (see Game_GetHudQuads).
Pose PanelInFront(float distance, float drop);
constexpr float kTitlePanelDistance = 2.5f; // meters
constexpr float kTitlePanelWidth = 2.4f;
bool sFollowingCamera = false;
float sCameraHeadYaw = 0.0f;
Pose sTitlePanel;

void FollowCamera(PlayState* play, float heightMeters) {
    const Pose& head = Runtime::Get().GetHead();
    Camera* camera = GET_ACTIVE_CAM(play);
    if (!head.valid || camera == nullptr) {
        return;
    }
    if (!sFollowingCamera) {
        Vec3 forward = HeadForward();
        sCameraHeadYaw = atan2f(-forward.x, -forward.z);
        sTitlePanel = PanelInFront(kTitlePanelDistance, 0.0f);
        sFollowingCamera = true;
    }
    float lookX = camera->at.x - camera->eye.x;
    float lookZ = camera->at.z - camera->eye.z;
    if (lookX * lookX + lookZ * lookZ > 0.01f) {
        sYaw = atan2f(lookX, lookZ) + static_cast<float>(M_PI) - sCameraHeadYaw;
    }
    sCenter[0] = head.position[0];
    sCenter[1] = head.position[2];
    sBuild.centerShift[0] = sBuild.centerShift[1] = 0.0f;
    float eyeHeight = std::max(heightMeters, 1.0f) * kEyeHeightRatio * sBuild.unitsPerMeter;
    sBuild.anchor = { camera->eye.x, camera->eye.y - eyeHeight, camera->eye.z };
}

// Runs as the game starts drawing a tick, before the play view's matrices are built.
void CaptureTick() {
    sBuild.stereo = false;
    sAwaitingWorldView = false;
    gVrBillboard = nullptr;
    if (!SessionRunning() || !ShowsWorld(gPlayState)) {
        sNeedsRecenter = true;
        sFollowingCamera = false;
        return;
    }

    Player* player = GET_PLAYER(gPlayState);
    sBuild.fromCamera = gSaveContext.gameMode == GAMEMODE_TITLE_SCREEN;
    if (!sBuild.fromCamera && sFollowingCamera) {
        sFollowingCamera = false;
        sNeedsRecenter = true;
    }
    if (gPlayState != sLastPlay || gPlayState->sceneNum != sLastScene) {
        sNeedsRecenter = true;
        sLastPlay = gPlayState;
        sLastScene = gPlayState->sceneNum;
        sHeldNut = nullptr;
        sThrownNut = nullptr;
    }
    if (sNeedsRecenter) {
        Recenter(player);
    }
    LimitDrift();

    float heightMeters = CVarGetInteger(CVAR_VR_PLAYER_HEIGHT, 170) / 100.0f;
    int worldScale = CVarGetInteger(LINK_IS_ADULT ? CVAR_VR_ADULT_WORLD_SCALE : CVAR_VR_CHILD_WORLD_SCALE, 100);
    sBuild.unitsPerMeter = (LINK_IS_ADULT ? kAdultHeight : kChildHeight) / std::max(heightMeters, 1.0f) * 100.0f /
                           std::clamp(worldScale, 50, 200);
    // For checking the scale against the game: how high Link's eyes (his focus point) are while he stands.
    if (gPlayState->sceneNum != sLoggedEyeScene && player->linearVelocity == 0.0f &&
        (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) && player->csAction == 0) {
        sLoggedEyeScene = gPlayState->sceneNum;
        SPDLOG_INFO("[VR] {} Link's eyes are {:.1f} units up", LINK_IS_ADULT ? "Adult" : "Child",
                    player->actor.focus.pos.y - player->actor.world.pos.y);
    }
    // The view goes along when an animation carries Link across, but not up or down: going into a crawlspace his
    // hips drop, which took the view into the ground. Height is his head's to say, below. Not in the saddle, where
    // the horse carries him: his riding animations keep the game's root motion switched on while they sway his
    // root with every stride, so the view swayed with them, and spawning on the horse past a loading zone that
    // root was briefly nowhere near him. Mounting still follows his model up onto the horse.
    bool inSaddle = (player->stateFlags1 & PLAYER_STATE1_ON_HORSE) && player->av2.actionVar2 != 0;
    Vec3f carried = inSaddle ? Vec3f{ 0.0f, 0.0f, 0.0f } : RootMotionOffset(player);
    sBuild.anchor = { player->actor.world.pos.x + carried.x, player->actor.world.pos.y,
                      player->actor.world.pos.z + carried.z };
    if (sBuild.fromCamera) {
        FollowCamera(gPlayState, heightMeters);
    }

    // Hanging from a ledge his position stays up on it while he hangs below, climbing onto one his body goes up
    // before his position, crawling his head is a foot off the floor, and in the saddle his position is below the
    // saddle while he sits on it. Then the play space moves up or down until a standing player's eyes are at Link's
    // (his focus point, from his head as it's drawn). Moving the head still moves the view as usual.
    float headDrop = 0.0f;
    bool followHead = (player->stateFlags1 & (PLAYER_STATE1_HANGING_OFF_LEDGE | PLAYER_STATE1_CLIMBING_LEDGE)) ||
                      (player->stateFlags2 & PLAYER_STATE2_CRAWLING) || inSaddle;
    if (followHead) {
        float eyeHeight = std::max(heightMeters, 1.0f) * kEyeHeightRatio * sBuild.unitsPerMeter;
        headDrop = player->actor.world.pos.y + eyeHeight - player->actor.focus.pos.y;
    }
    sHeadDrop += (headDrop - sHeadDrop) * kHeadFollowEase;
    if (!sBuild.fromCamera) {
        sBuild.anchor.y -= sHeadDrop;
    }
    sBuild.yaw = sYaw;
    HoldNut();
    // The screen fill can stay switched on, fully transparent, long after a fade, so only a visible one counts.
    bool screenFilled = gPlayState->envCtx.fillScreen && gPlayState->envCtx.screenFillColor[3] > 0;
    sBuild.fade = gPlayState->transitionMode != TRANS_MODE_OFF || gPlayState->transitionTrigger != TRANS_TRIGGER_OFF ||
                  screenFilled || gPlayState->gameOverCtx.state != GAMEOVER_INACTIVE;
    sBuild.gameOver = gPlayState->gameOverCtx.state != GAMEOVER_INACTIVE;
    sBuild.message = gPlayState->msgCtx.msgMode != MSGMODE_NONE;
    sBuild.textboxY = R_TEXTBOX_Y;
    sBuild.stereo = true;
    sAwaitingWorldView = true;
    AimGameCamera(gPlayState);
}

void BuildProjection(const Fov& fov, float zNear, float zFar, uint32_t width, uint32_t height, MtxF& out) {
    float left = tanf(fov.left);
    float right = tanf(fov.right);
    float up = tanf(fov.up);
    float down = tanf(fov.down);
    // The renderer stretches clip-space x from 4:3 to the framebuffer's aspect, so undo that here.
    float aspectFix = (static_cast<float>(width) / height) / (4.0f / 3.0f);

    memset(&out, 0, sizeof(out));
    out.mf[0][0] = 2.0f / (right - left) * aspectFix;
    out.mf[2][0] = (right + left) / (right - left) * aspectFix;
    out.mf[1][1] = 2.0f / (up - down);
    out.mf[2][1] = (up + down) / (up - down);
    out.mf[2][2] = -(zFar + zNear) / (zFar - zNear);
    out.mf[2][3] = -1.0f;
    out.mf[3][2] = -2.0f * zFar * zNear / (zFar - zNear);
}

// World-to-eye matrix in the game's row-vector layout.
void BuildView(const Vec3& eye, const float orientation[4], MtxF& out) {
    Vec3 right = RotateYaw(sYaw, Rotate(orientation, { 1.0f, 0.0f, 0.0f }));
    Vec3 up = RotateYaw(sYaw, Rotate(orientation, { 0.0f, 1.0f, 0.0f }));
    Vec3 back = RotateYaw(sYaw, Rotate(orientation, { 0.0f, 0.0f, 1.0f }));

    out.mf[0][0] = right.x;
    out.mf[1][0] = right.y;
    out.mf[2][0] = right.z;
    out.mf[3][0] = -Dot(right, eye);
    out.mf[0][1] = up.x;
    out.mf[1][1] = up.y;
    out.mf[2][1] = up.z;
    out.mf[3][1] = -Dot(up, eye);
    out.mf[0][2] = back.x;
    out.mf[1][2] = back.y;
    out.mf[2][2] = back.z;
    out.mf[3][2] = -Dot(back, eye);
    out.mf[0][3] = 0.0f;
    out.mf[1][3] = 0.0f;
    out.mf[2][3] = 0.0f;
    out.mf[3][3] = 1.0f;
}

// A panel standing upright in front of where the player is looking, a little below eye level.
Pose PanelInFront(float distance, float drop) {
    const Pose& head = Runtime::Get().GetHead();
    Vec3 forward = HeadForward();
    float length = sqrtf(forward.x * forward.x + forward.z * forward.z);
    Pose out;
    if (!head.valid || length < 0.001f) {
        return out;
    }
    float yaw = atan2f(-forward.x, -forward.z);
    out.orientation[1] = sinf(yaw * 0.5f);
    out.orientation[3] = cosf(yaw * 0.5f);
    out.position[0] = head.position[0] + forward.x / length * distance;
    out.position[1] = head.position[1] - drop;
    out.position[2] = head.position[2] + forward.z / length * distance;
    out.valid = true;
    return out;
}


// Animations that carry Link, such as climbing onto a ledge, move his model ahead of his position, which only
// catches up when they end. Following the model keeps the view from lagging behind and then jumping.
Vec3f RootMotionOffset(Player* player) {
    if (player->skelAnime.movementFlags == 0) {
        return { 0.0f, 0.0f, 0.0f };
    }
    f32 dx = static_cast<f32>(player->skelAnime.jointTable[0].x - player->skelAnime.baseTransl.x);
    f32 dy = static_cast<f32>(player->skelAnime.jointTable[0].y - player->skelAnime.baseTransl.y);
    f32 dz = static_cast<f32>(player->skelAnime.jointTable[0].z - player->skelAnime.baseTransl.z);
    f32 s = Math_SinS(player->actor.shape.rot.y);
    f32 c = Math_CosS(player->actor.shape.rot.y);
    return { (dx * c + dz * s) * player->actor.scale.x, dy * player->actor.scale.y,
             (-dx * s + dz * c) * player->actor.scale.z };
}

// Whatever a hand takes, from the belt or over the shoulder, goes into that hand, so Link's hands trade
// controllers when the item belongs in his other one.
bool sHandsSwapped = false;

Hand ControllerFor(LinkHand hand) {
    bool swapped = (CVarGetInteger(CVAR_VR_SWORD_IN_RIGHT_HAND, 0) != 0) != sHandsSwapped;
    return ((hand == LINK_LEFT) != swapped) ? HAND_LEFT : HAND_RIGHT;
}

void PutLinkHandOn(LinkHand linkHand, Hand hand) {
    if (ControllerFor(linkHand) != hand) {
        sHandsSwapped = !sHandsSwapped;
    }
}

// Which of Link's hands an item goes in: the boomerang, Deku stick, hammer and bottles in the left like his
// sword, everything else in the right.
LinkHand ItemHand(u8 item) {
    bool left = item == ITEM_BOOMERANG || item == ITEM_STICK || item == ITEM_HAMMER ||
                (item >= ITEM_BOTTLE && item <= ITEM_POE);
    return left ? LINK_LEFT : LINK_RIGHT;
}

// Items held and used in one hand, which go into the hand that took them. The ocarina, masks, spells, the lens
// and trade items leave the hands as they are.
bool HeldInOneHand(u8 item) {
    switch (item) {
        case ITEM_STICK:
        case ITEM_NUT:
        case ITEM_BOMB:
        case ITEM_BOW:
        case ITEM_BOW_ARROW_FIRE:
        case ITEM_BOW_ARROW_ICE:
        case ITEM_BOW_ARROW_LIGHT:
        case ITEM_SLINGSHOT:
        case ITEM_BOMBCHU:
        case ITEM_HOOKSHOT:
        case ITEM_LONGSHOT:
        case ITEM_BOOMERANG:
        case ITEM_HAMMER:
            return true;
        default:
            return item >= ITEM_BOTTLE && item <= ITEM_POE;
    }
}

// Whether one of Link's hands is on the other side's controller, so it's drawn mirrored to look like that side's
// hand.
bool HandMirrored(LinkHand hand) {
    return (ControllerFor(hand) == HAND_LEFT) != (hand == LINK_LEFT);
}

void AxisAngle(float x, float y, float z, float degrees, float out[4]) {
    float half = DEG_TO_RAD(degrees) * 0.5f;
    float s = sinf(half);
    out[0] = x * s;
    out[1] = y * s;
    out[2] = z * s;
    out[3] = cosf(half);
}

// How a hand model sits on its controller: the alignment from the game's data, then any fine tuning, turned
// about the grip's X, then Y, then Z axis.
void HandAlignment(LinkHand hand, float out[4]) {
    int degrees[3];
    if (hand == LINK_LEFT) {
        degrees[0] = CVarGetInteger(CVAR_VR_HAND_TUNE("L", "X"), 0);
        degrees[1] = CVarGetInteger(CVAR_VR_HAND_TUNE("L", "Y"), 0);
        degrees[2] = CVarGetInteger(CVAR_VR_HAND_TUNE("L", "Z"), 0);
    } else {
        degrees[0] = CVarGetInteger(CVAR_VR_HAND_TUNE("R", "X"), 0);
        degrees[1] = CVarGetInteger(CVAR_VR_HAND_TUNE("R", "Y"), 0);
        degrees[2] = CVarGetInteger(CVAR_VR_HAND_TUNE("R", "Z"), 0);
    }
    float rx[4], ry[4], rz[4], zy[4], tune[4];
    AxisAngle(1.0f, 0.0f, 0.0f, static_cast<float>(degrees[0]), rx);
    AxisAngle(0.0f, 1.0f, 0.0f, static_cast<float>(degrees[1]), ry);
    AxisAngle(0.0f, 0.0f, 1.0f, static_cast<float>(degrees[2]), rz);
    Multiply(rz, ry, zy);
    Multiply(zy, rx, tune);
    float tuned[4];
    Multiply(kGripFix, tune, tuned);
    Multiply(tuned, kHandAlignment, out);
}

// The grip pose turned into the frame the hand and wrist placements are written for. See kGripFix.
Pose GripFrame(const Pose& grip) {
    return Attach(grip, { 0.0f, 0.0f, 0.0f }, kGripFix);
}

// Row-vector product a * b, the order the game's matrices compose in.
void MultiplyRows(const MtxF& a, const MtxF& b, MtxF& out) {
    MtxF result;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            result.mf[i][j] = a.mf[i][0] * b.mf[0][j] + a.mf[i][1] * b.mf[1][j] + a.mf[i][2] * b.mf[2][j] +
                              a.mf[i][3] * b.mf[3][j];
        }
    }
    out = result;
}

// An item-get model held in one of Link's hands in place of the game's own in-hand model. Those models lie in the
// plane of L = (-1, 0, 1)/sqrt2 and Y, so each item's points and directions are given in L, Y and
// N = (1, 0, 1)/sqrt2, as measured from the models' own vertices.
struct HandItem {
    s16 model;
    Vec3 grip;           // where the fist closes on it
    Vec3 up;             // the thumb's side of the fist
    Vec3 forward;        // where it points, or shoots
    float unitsPerMeter; // model units per meter, which sets its size in the hand
    Vec3 tip = {};       // where a hookshot's hook leaves from
};

// The hookshot and longshot are held by the bar at their back, and the boomerang by one wing with the other one
// ahead. The bow and slingshot keep Link's own models, whose strings can stretch to the drawing hand; their
// item-get models have the string drawn in at rest.
constexpr HandItem kHookshot = { GID_HOOKSHOT, { 23.6f, -20.5f, 0.0f }, { 0.71f, 0.71f, 0.0f },
                                 { -0.71f, 0.71f, 0.0f }, 212.0f, { -26.9f, 30.0f, 0.0f } };
constexpr HandItem kLongshot = { GID_LONGSHOT, { 36.3f, -21.8f, 0.0f }, { 0.57f, 0.82f, 0.0f },
                                 { -0.82f, 0.57f, 0.0f }, 212.0f, { -42.4f, 33.0f, 0.0f } };
constexpr HandItem kBoomerang = { GID_BOOMERANG, { 27.0f, -8.0f, 0.0f }, { -0.69f, 0.72f, 0.0f },
                                  { -0.72f, -0.69f, 0.0f }, 224.0f };

// The model in one of Link's hands, if it's one drawn from its item-get model.
const HandItem* HandItemFor(Player* player, LinkHand hand) {
    if (hand == LINK_RIGHT) {
        if (player->rightHandType == PLAYER_MODELTYPE_RH_HOOKSHOT) {
            return player->heldItemAction == PLAYER_IA_LONGSHOT ? &kLongshot : &kHookshot;
        }
    } else if (player->leftHandType == PLAYER_MODELTYPE_LH_BOOMERANG &&
               !(player->stateFlags1 & PLAYER_STATE1_BOOMERANG_THROWN)) {
        return &kBoomerang;
    }
    return nullptr;
}

Vec3 FromItemPlane(Vec3 v) {
    constexpr float k = 0.70710678f;
    return { (v.z - v.x) * k, v.y, (v.x + v.z) * k };
}

bool HandMatrix(Player* player, LinkHand hand, MtxF& out);

// Where an item in one of Link's hands goes: in line with his hand model, pointing out over the knuckles (its +Y,
// the way a punch goes) with the thumb's side (its +X) up, and its grip in the fist. Following the hand, rather
// than the controller's aim, keeps it in line with the hand the player sees, fine tuning included. forwardOut, if
// given, is where it points in the world.
bool HandItemMatrix(Player* player, LinkHand hand, const HandItem* item, MtxF& m, Vec3* forwardOut = nullptr) {
    const ControllerState& state = Runtime::Get().GetController(ControllerFor(hand));
    MtxF handMtx;
    if (item == nullptr || !state.gripPose.valid || !HandMatrix(player, hand, handMtx)) {
        return false;
    }
    Vec3 forward = Normalized({ handMtx.mf[1][0], handMtx.mf[1][1], handMtx.mf[1][2] });
    Vec3 thumb = { handMtx.mf[0][0], handMtx.mf[0][1], handMtx.mf[0][2] };
    Vec3 up = Normalized(Perpendicular(thumb, forward));
    Vec3 side = Cross(up, forward);
    if (forwardOut != nullptr) {
        *forwardOut = forward;
    }

    Vec3 modelForward = Normalized(FromItemPlane(item->forward));
    Vec3 modelUp = Normalized(Perpendicular(FromItemPlane(item->up), modelForward));
    Vec3 modelSide = Cross(modelUp, modelForward);

    // Row i is where the model's axis i ends up: the same mix of side, up and forward in the world as in the model.
    float scale = sBuild.unitsPerMeter / item->unitsPerMeter;
    const float sideAt[3] = { modelSide.x, modelSide.y, modelSide.z };
    const float upAt[3] = { modelUp.x, modelUp.y, modelUp.z };
    const float forwardAt[3] = { modelForward.x, modelForward.y, modelForward.z };
    for (int i = 0; i < 3; i++) {
        Vec3 row = { side.x * sideAt[i] + up.x * upAt[i] + forward.x * forwardAt[i],
                     side.y * sideAt[i] + up.y * upAt[i] + forward.y * forwardAt[i],
                     side.z * sideAt[i] + up.z * upAt[i] + forward.z * forwardAt[i] };
        m.mf[i][0] = row.x * scale;
        m.mf[i][1] = row.y * scale;
        m.mf[i][2] = row.z * scale;
        m.mf[i][3] = 0.0f;
    }
    Vec3 grip = FromItemPlane(item->grip);
    Vec3 gripOffset = { side.x * Dot(grip, modelSide) + up.x * Dot(grip, modelUp) + forward.x * Dot(grip, modelForward),
                        side.y * Dot(grip, modelSide) + up.y * Dot(grip, modelUp) + forward.y * Dot(grip, modelForward),
                        side.z * Dot(grip, modelSide) + up.z * Dot(grip, modelUp) +
                            forward.z * Dot(grip, modelForward) };
    Vec3 fist = ToWorld(state.gripPose.position, sBuild.anchor, sBuild.unitsPerMeter);
    m.mf[3][0] = fist.x - gripOffset.x * scale;
    m.mf[3][1] = fist.y - gripOffset.y * scale;
    m.mf[3][2] = fist.z - gripOffset.z * scale;
    m.mf[3][3] = 1.0f;
    return true;
}

// A point on an item placed by HandItemMatrix, given in its L, Y, N coordinates, in the world.
Vec3 HandItemPoint(const MtxF& m, Vec3 point) {
    Vec3 p = FromItemPlane(point);
    return { m.mf[3][0] + p.x * m.mf[0][0] + p.y * m.mf[1][0] + p.z * m.mf[2][0],
             m.mf[3][1] + p.x * m.mf[0][1] + p.y * m.mf[1][1] + p.z * m.mf[2][1],
             m.mf[3][2] + p.x * m.mf[0][2] + p.y * m.mf[1][2] + p.z * m.mf[2][2] };
}

void DrawHandItem(PlayState* play, Player* player, LinkHand hand) {
    const HandItem* item = HandItemFor(player, hand);
    MtxF m;
    if (!HandItemMatrix(player, hand, item, m)) {
        return;
    }
    Matrix_Push();
    Vr_PutWorldMatrix(&m);
    GetItem_Draw(play, item->model);
    Matrix_Pop();
}

// A mask in hand, held by its middle with its face out along where the hand points, about 25 cm across.
void DrawHeldMask(PlayState* play) {
    if (sHeldMask == ITEM_NONE) {
        return;
    }
    HandItem mask = { Items_ItemModel(sHeldMask), { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
                      240.0f };
    LinkHand hand = (ControllerFor(LINK_LEFT) == sMaskHand) ? LINK_LEFT : LINK_RIGHT;
    MtxF m;
    if (!HandItemMatrix(GET_PLAYER(play), hand, &mask, m)) {
        return;
    }
    Matrix_Push();
    Vr_PutWorldMatrix(&m);
    GetItem_Draw(play, mask.model);
    Matrix_Pop();
}

// Where one of Link's hand models goes in the world: its wrist at the controller, scaled like the rest of him.
bool HandMatrix(Player* player, LinkHand hand, MtxF& out) {
    const Pose& grip = Runtime::Get().GetController(ControllerFor(hand)).gripPose;
    if (!grip.valid) {
        return false;
    }
    float playSpaceYaw[4] = { 0.0f, sinf(sYaw * 0.5f), 0.0f, cosf(sYaw * 0.5f) };
    float gripWorld[4], alignment[4], handWorld[4];
    Multiply(playSpaceYaw, grip.orientation, gripWorld);
    HandAlignment(hand, alignment);
    Multiply(gripWorld, alignment, handWorld);

    // Held in both hands, the weapon (along the hand's X) turns by the shortest way onto the line between the two
    // hands, pointing away from whichever is lower on the handle.
    if (hand == LINK_LEFT && sTwoHanded) {
        const Pose& other = Runtime::Get().GetController(ControllerFor(LINK_RIGHT)).gripPose;
        Vec3 blade = Rotate(handWorld, { 1.0f, 0.0f, 0.0f });
        Vec3 between = Normalized(RotateYaw(sYaw, { grip.position[0] - other.position[0],
                                                    grip.position[1] - other.position[1],
                                                    grip.position[2] - other.position[2] }));
        if (Dot(between, blade) < 0.0f) {
            between = Scaled(between, -1.0f);
        }
        Vec3 axis = Cross(blade, between);
        float turn[4] = { axis.x, axis.y, axis.z, 1.0f + Dot(blade, between) };
        float length = sqrtf(turn[0] * turn[0] + turn[1] * turn[1] + turn[2] * turn[2] + turn[3] * turn[3]);
        if (length > 0.0001f) {
            for (float& component : turn) {
                component /= length;
            }
            float turned[4];
            Multiply(turn, handWorld, turned);
            memcpy(handWorld, turned, sizeof(handWorld));
        }
    }

    // The model's origin is the wrist; put its fist on the controller's grip point instead.
    float scale = player->actor.scale.x;
    Vec3 gripPoint = ToWorld(grip.position, sBuild.anchor, sBuild.unitsPerMeter);
    Vec3 fist = Rotate(handWorld, { kHandFist.x * scale, kHandFist.y * scale, kHandFist.z * scale });
    Vec3 origin = { gripPoint.x - fist.x, gripPoint.y - fist.y, gripPoint.z - fist.z };

    Vec3 axes[3] = { Rotate(handWorld, { 1.0f, 0.0f, 0.0f }), Rotate(handWorld, { 0.0f, 1.0f, 0.0f }),
                     Rotate(handWorld, { 0.0f, 0.0f, 1.0f }) };
    // A hand on the other side's controller is mirrored across its palm: the model's Z is the grip's X, the palm's
    // normal, which faces the other way in the other hand. The blade along X and the fist along Y stay put.
    if (HandMirrored(hand)) {
        axes[2] = { -axes[2].x, -axes[2].y, -axes[2].z };
    }
    for (int i = 0; i < 3; i++) {
        out.mf[i][0] = axes[i].x * scale;
        out.mf[i][1] = axes[i].y * scale;
        out.mf[i][2] = axes[i].z * scale;
        out.mf[i][3] = 0.0f;
    }
    out.mf[3][0] = origin.x;
    out.mf[3][1] = origin.y;
    out.mf[3][2] = origin.z;
    out.mf[3][3] = 1.0f;
    return true;
}

// How something held in Link's throwing hand is turned: its front (+Z) out over the knuckles and its top (+Y) toward
// the thumb, as a shape rotation (Y, X, Z), so it turns with the hand.
bool HeldRotation(Player* player, Vec3s* out) {
    MtxF hand;
    if (!HandMatrix(player, LINK_RIGHT, hand)) {
        return false;
    }
    Vec3 forward = Normalized({ hand.mf[1][0], hand.mf[1][1], hand.mf[1][2] });
    Vec3 up = Normalized(Perpendicular({ hand.mf[0][0], hand.mf[0][1], hand.mf[0][2] }, forward));
    Vec3 side = Cross(up, forward);
    MtxF turn;
    SkinMatrix_Clear(&turn);
    const Vec3 axes[3] = { side, up, forward };
    for (int i = 0; i < 3; i++) {
        turn.mf[i][0] = axes[i].x;
        turn.mf[i][1] = axes[i].y;
        turn.mf[i][2] = axes[i].z;
    }
    Matrix_MtxFToYXZRotS(&turn, out, 0);
    return true;
}

// Poses a forearm so that its hand, drawn next with its usual offset from the forearm, lands on the controller.
// Moving the forearm rather than the hand keeps the wrist vertices, which are skinned to the forearm, attached.
void PlaceForearm(Player* player, LinkHand hand, s32 handLimb, Vec3f* pos, Vec3s* rot) {
    MtxF handTarget;
    if (!HandMatrix(player, hand, handTarget)) {
        return;
    }
    LodLimb* limb = (LodLimb*)SEGMENTED_TO_VIRTUAL(player->skelAnime.skeleton[handLimb - 1]);
    Vec3f handPos = { (f32)limb->jointPos.x, (f32)limb->jointPos.y, (f32)limb->jointPos.z };
    Vec3s handRot = player->skelAnime.jointTable[handLimb];

    MtxF identity, local, localInverse, forearm;
    SkinMatrix_Clear(&identity);
    Matrix_Push();
    Matrix_Put(&identity);
    Matrix_TranslateRotateZYX(&handPos, &handRot);
    Matrix_Get(&local);
    Matrix_Pop();
    if (SkinMatrix_Invert(&local, &localInverse) != 0) {
        return;
    }
    // hand = local * forearm, so forearm = local^-1 * hand.
    MultiplyRows(localInverse, handTarget, forearm);
    Vr_PutWorldMatrix(&forearm);
    *pos = { 0.0f, 0.0f, 0.0f };
    *rot = { 0, 0, 0 };
}

// The damage of a normal slash with each melee weapon, as D_80854488 in z_player.c gives it. Indexed by
// Player_GetMeleeWeaponHeld: Master Sword, Kokiri Sword, Biggoron's Sword, Deku stick, hammer.
u32 SlashDamageFlags(Player* player) {
    static const u32 kSlashFlags[] = { DMG_SLASH_MASTER, DMG_SLASH_KOKIRI, DMG_SLASH_GIANT, DMG_DEKU_STICK,
                                       DMG_HAMMER_SWING };
    s32 weapon = Player_HoldsBrokenKnife(player) ? 2 : Player_GetMeleeWeaponHeld(player);
    return (weapon >= 1 && weapon <= 5) ? kSlashFlags[weapon - 1] : DMG_SLASH_KOKIRI;
}

// What a very hard swing does: a jump attack's damage. The Deku stick has none of its own.
u32 HeavyDamageFlags(Player* player) {
    static const u32 kHeavyFlags[] = { DMG_JUMP_MASTER, DMG_JUMP_KOKIRI, DMG_JUMP_GIANT, DMG_DEKU_STICK,
                                       DMG_HAMMER_JUMP };
    s32 weapon = Player_HoldsBrokenKnife(player) ? 2 : Player_GetMeleeWeaponHeld(player);
    return (weapon >= 1 && weapon <= 5) ? kHeavyFlags[weapon - 1] : DMG_JUMP_KOKIRI;
}

// A spin attack's damage, for a spin made by turning around for real.
u32 SpinDamageFlags(Player* player) {
    static const u32 kSpinFlags[] = { DMG_SPIN_MASTER, DMG_SPIN_KOKIRI, DMG_SPIN_GIANT };
    s32 weapon = Player_HoldsBrokenKnife(player) ? 2 : Player_GetMeleeWeaponHeld(player);
    return (weapon >= 1 && weapon <= 3) ? kSpinFlags[weapon - 1] : SlashDamageFlags(player);
}

// Spin attacks. Squeezing the sword hand's grip (away from the shoulder, which draws and sheathes the sword, and not
// on a door or chest) charges one, as holding B does, and letting go spins: the game's spin sweeps the sword around
// Link, which in VR means around the player. Turning around for real with the sword held out is a spin attack too:
// the sword swung around with the player hits with a spin's damage.
bool sSpinChargeRequested = false;
bool sSpinGripLatched = false;
float sSpinTurned = 0.0f;   // degrees the head has turned one way in the current turn
float sSpinTurnTime = 0.0f; // seconds the current turn has taken
float sLastHeadYaw = 0.0f;
bool sHasLastHeadYaw = false;
float sPhysicalSpinLeft = 0.0f; // seconds a spin by turning keeps a spin's damage
float sTipSpeed = 0.0f;         // meters a second, as of the last tick

bool HoldsSword(Player* player) {
    s32 weapon = Player_GetMeleeWeaponHeld(player);
    return weapon >= 1 && weapon <= 3;
}

bool HandOnDoorOrChest(Hand hand);

void UpdateSpinAttack() {
    Runtime& runtime = Runtime::Get();
    Player* player = GET_PLAYER(gPlayState);
    Hand swordHand = ControllerFor(LINK_LEFT);
    const ControllerState& state = runtime.GetController(swordHand);
    bool pressed = state.gripHeld && !sSpinGripLatched;
    sSpinGripLatched = state.gripHeld;
    bool charging = player->stateFlags1 & PLAYER_STATE1_CHARGING_SPIN_ATTACK;
    if (pressed && HoldsSword(player) && !charging && player->meleeWeaponState == 0 && !Items_MenuOpen() &&
        state.gripPose.valid && !HandOverShoulder(state.gripPose) && !HandOnDoorOrChest(swordHand) &&
        (player->actor.bgCheckFlags & 1)) {
        sSpinChargeRequested = true;
    } else if (!state.gripHeld) {
        sSpinChargeRequested = false;
    }

    // A spin by turning: how far the head has turned one way, and how long it took.
    uint32_t refresh = runtime.GetRefreshRate();
    float dt = refresh > 0 ? 1.0f / refresh : 0.0f;
    sPhysicalSpinLeft = std::max(sPhysicalSpinLeft - dt, 0.0f);
    const Pose& head = runtime.GetHead();
    if (!head.valid || dt == 0.0f) {
        sHasLastHeadYaw = false;
        return;
    }
    Vec3 forward = HeadForward();
    float yaw = atan2f(forward.x, forward.z) * (180.0f / static_cast<float>(M_PI));
    if (sHasLastHeadYaw) {
        float turned = remainderf(yaw - sLastHeadYaw, 360.0f);
        if (turned * sSpinTurned < 0.0f || fabsf(turned) < 0.5f) {
            sSpinTurned = 0.0f; // stopped, or turned back
            sSpinTurnTime = 0.0f;
        }
        sSpinTurned += turned;
        sSpinTurnTime += dt;
        if (sSpinTurnTime > kPhysicalSpinSeconds) {
            sSpinTurned = 0.0f;
            sSpinTurnTime = 0.0f;
        }
    }
    sLastHeadYaw = yaw;
    sHasLastHeadYaw = true;
    float reach = hypotf(state.gripPose.position[0] - head.position[0], state.gripPose.position[2] - head.position[2]);
    if (fabsf(sSpinTurned) >= kPhysicalSpinDegrees && HoldsSword(player) && state.gripPose.valid &&
        reach >= kSpinReach) {
        sSpinTurned = 0.0f;
        sSpinTurnTime = 0.0f;
        sPhysicalSpinLeft = 0.6f;
        Player_PlaySfx(&player->actor, NA_SE_IT_ROLLING_CUT);
        Player_PlaySfx(&player->actor, NA_SE_VO_LI_SWORD_L);
        runtime.Vibrate(swordHand, 0.8f, 0.2f);
        SPDLOG_INFO("[VR] Spin attack by turning around");
    }
}

HudQuad MakeQuad(const Pose& pose, float width, ScreenRect rect) {
    uint32_t scale = Runtime::Get().GetHudWidth() / SCREEN_WIDTH;
    HudQuad quad;
    quad.pose = pose;
    quad.width = width;
    quad.rect[0] = rect.x * scale;
    quad.rect[1] = rect.y * scale;
    quad.rect[2] = rect.w * scale;
    quad.rect[3] = rect.h * scale;
    return quad;
}

void RegisterVr() {
    bool enabled = CVarGetInteger(CVAR_VR_ENABLED, 0);
    COND_HOOK(OnPlayDrawBegin, enabled, CaptureTick);

    // The game puts away a held item, and takes off a mask, when no button holds it. In VR items come from the
    // item menu instead of buttons, so that would undo every pick at once.
    COND_VB_SHOULD(VB_PUTAWAY_BECAUSE_DISABLED_ITEM_BUTTONS, enabled, {
        if (PlayingInVr()) {
            *should = false;
        }
    });
    COND_VB_SHOULD(VB_PLAYER_UNEQUIP_MASK_WITHOUT_BUTTON, enabled, {
        if (PlayingInVr()) {
            *should = false;
        }
    });
    // In the world nothing is used just for being taken out, as a held button would: the sword comes out without a
    // slash, a bottle without a swing, and the bow, hookshot and boomerang wait to be nocked, pulled or squeezed.
    COND_VB_SHOULD(VB_USE_HELD_ITEM_AFTER_CHANGE, enabled, {
        if (Vr_IsWorldStereo()) {
            *should = false;
        }
    });
}

} // namespace

void Game_OnTickSubmitted() {
    sRenderPrevAnchor = sRender.anchor;
    sRenderPrevYaw = sRender.yaw;
    sRender = sBuild;
    sRender.stereo = sBuild.stereo && sWorldViewCaptured;
    sWorldViewCaptured = false;
    sBuild.overlayBranch = nullptr;
    sBuild.overlay = nullptr;
    sBuild.overlayEnd = nullptr;
    sBuild.fadeEnd = nullptr;
    sBuild.skyboxCount = 0;
    sBuild.centerShift[0] = sBuild.centerShift[1] = 0.0f;

    float dx = sRender.anchor.x - sRenderPrevAnchor.x;
    float dy = sRender.anchor.y - sRenderPrevAnchor.y;
    float dz = sRender.anchor.z - sRenderPrevAnchor.z;
    if (dx * dx + dy * dy + dz * dz > kWarpDistance * kWarpDistance) {
        sRenderPrevAnchor = sRender.anchor;
        sRenderPrevYaw = sRender.yaw;
        sRender.centerShift[0] = sRender.centerShift[1] = 0.0f;
    }
}

// Seated mode lifts the head and hands together, so the head is where a standing player's eyes would be and the
// world is the same height as when standing. The lift is measured when seated mode comes on and at every recenter.
void UpdateSeating(bool recenter) {
    if (!CVarGetInteger(CVAR_VR_SEATED, 0)) {
        sSeatedLift = 0.0f;
        sSeatMeasured = false;
        return;
    }
    const Pose& head = Runtime::Get().GetHead();
    if (!head.valid || (sSeatMeasured && !recenter)) {
        return;
    }
    float heightMeters = CVarGetInteger(CVAR_VR_PLAYER_HEIGHT, 170) / 100.0f;
    sSeatedLift = std::max(heightMeters, 1.0f) * kEyeHeightRatio - head.position[1];
    sSeatMeasured = true;
    SPDLOG_INFO("[VR] Seated: the view is lifted {:.2f} m", sSeatedLift);
}

int Game_SceneNumber() {
    return gPlayState != nullptr ? gPlayState->sceneNum : -1;
}

void Game_Recenter() {
    sNeedsRecenter = true;
    sSeatMeasured = false;
}

void Game_PressStart() {
    sLatchedButtons |= BTN_START;
}

Pose Game_PanelInFront(float distance, float drop) {
    return PanelInFront(distance, drop);
}

void Game_OnFrame() {
    Runtime& runtime = Runtime::Get();
    sLatchedButtons |= CurrentButtons();

    const ControllerState& left = runtime.GetController(HAND_LEFT);
    const ControllerState& right = runtime.GetController(HAND_RIGHT);
    bool stereo = Game_RenderStereo();
    bool recenterPressed = right.stickClick && !sRecenterLatched;
    sRecenterLatched = right.stickClick;
    UpdateSeating(recenterPressed);

    // The floating screen comes back in front of the player each time it's needed, in case they turned away.
    if (!stereo && (sWasStereo || recenterPressed)) {
        runtime.RecenterScreen();
    }
    sWasStereo = stereo;

    if (sItemRequest != VR_REQUEST_NONE && runtime.GetRefreshRate() > 0) {
        sItemRequestAge += 1.0f / runtime.GetRefreshRate();
        if (sItemRequestAge > kItemRequestLifetime) {
            sItemRequest = VR_REQUEST_NONE;
        }
    }

    if (!stereo) {
        sMessageShown = false;
        // In places with a fixed camera the item menu opens on the screen instead.
        if (ScreenGameplay()) {
            Items_OnFrame(true);
        } else {
            Items_Close();
        }
        return;
    }

    if (recenterPressed) {
        sNeedsRecenter = true;
    }

    RecordHands();
    // During a cutscene, or while the game's camera is off doing something, the player watches through Link's eyes
    // but their hands don't act on the game: no sword, grabs, items or masks, and no item menu or settings panel.
    if (InGameplay(gPlayState)) {
        UpdateDraw();
        RepairHookshot();
        CheckHookshotPull();
        UpdateCarry();
        UpdateBoomerang();
        UpdateWorldGrabs();
        UpdateLiftGrab();
        UpdateBlockGrip();
        UpdateClimb();
        UpdateSpinAttack();
        UpdateMasks();
        UpdateBottle();
        UpdateFishing();
        UpdateTwoHanded();

        // Closing either hand over the shoulder draws the sword into that hand, or puts it back.
        if (sSwordGestureCooldown > 0.0f && runtime.GetRefreshRate() > 0) {
            sSwordGestureCooldown -= 1.0f / runtime.GetRefreshRate();
        }
        for (int hand = 0; hand < HAND_COUNT; hand++) {
            const ControllerState& state = runtime.GetController(static_cast<Hand>(hand));
            bool pressedNow = state.gripHeld && !sSwordGripLatched[hand];
            sSwordGripLatched[hand] = state.gripHeld;
            // While climbing, a hand reaching up past the shoulder is reaching for the next hold.
            if (!pressedNow || sSwordGestureCooldown > 0.0f || !HandOverShoulder(state.gripPose) ||
                Climbing(GET_PLAYER(gPlayState))) {
                continue;
            }
            bool drawn = SwordDrawn(GET_PLAYER(gPlayState));
            if (!drawn) {
                PutLinkHandOn(LINK_LEFT, static_cast<Hand>(hand));
            }
            RequestItem(drawn ? VR_REQUEST_PUT_AWAY : VR_REQUEST_DRAW_SWORD);
            sSwordGestureCooldown = kSwordGestureCooldown;
            runtime.Vibrate(static_cast<Hand>(hand), 0.4f, 0.05f);
            SPDLOG_INFO("[VR] Sword {} with the {} hand", drawn ? "put away" : "drawn", hand == HAND_LEFT ? "left" : "right");
        }

        // The trigger on the hand holding an item uses it again, as pressing its button did: toggling the Lens of
        // Truth, showing a trade item. The bow, hookshot and boomerang have their own use, thrown things are used by
        // throwing them, bottles by swinging or tipping them, and the fishing rod by casting it.
        Player* player = GET_PLAYER(gPlayState);
        u8 held = player->heldItemId;
        bool useTrigger = runtime.GetController(ControllerFor(ItemHand(held))).triggerHeld;
        bool thrownItem = held == ITEM_BOMB || held == ITEM_BOMBCHU || held == ITEM_NUT || held == ITEM_STICK;
        bool physicalItem = (held >= ITEM_BOTTLE && held <= ITEM_POE) || held == ITEM_FISHING_POLE;
        if (useTrigger && !sUseTriggerLatched && CurrentTool() == HeldTool::None && held < ITEM_NONE_FE && !thrownItem &&
            !physicalItem &&
            Player_GetMeleeWeaponHeld(player) == 0 && !(player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR)) {
            RequestItem(VR_REQUEST_USE_HELD);
        }
        sUseTriggerLatched = useTrigger;

        Items_OnFrame(false);
    } else {
        Items_Close();
        sBlockHeld = false;
    }

    OcarinaMic_Listen(PlayingOcarina() && CVarGetInteger(CVAR_VR_MIC_OCARINA, 1));

    int snapDegrees = CVarGetInteger(CVAR_VR_SNAP_TURN, 30);
    // Up and down on the right stick are buttons, so only mostly-sideways pushes turn.
    float turn = fabsf(right.stick[0]) >= fabsf(right.stick[1]) ? right.stick[0] : 0.0f;
    if (sRender.fromCamera) {
        // Following the title's camera, it turns the view.
    } else if (PlayingOcarina()) {
        // The right stick plays the ocarina's notes instead. A push still held when the ocarina is put away doesn't
        // turn either, until the stick is let go.
        sTurnLatched = true;
    } else if (snapDegrees > 0) {
        if (!sTurnLatched && fabsf(turn) > 0.7f) {
            Turn(-copysignf(DEG_TO_RAD(static_cast<float>(snapDegrees)), turn));
            sTurnLatched = true;
        } else if (fabsf(turn) < 0.3f) {
            sTurnLatched = false;
        }
    } else if (sTurnLatched) {
        sTurnLatched = fabsf(turn) >= 0.3f;
    } else if (fabsf(turn) > kStickDeadzone && runtime.GetRefreshRate() > 0) {
        Turn(-turn * kSmoothTurnSpeed / runtime.GetRefreshRate());
    }

    // Text panels stay where they first appeared, so they don't swim around while reading. They sit well below eye
    // level, under the face of whoever's talking.
    if (sRender.message && !sMessageShown) {
        sMessagePose = PanelInFront(1.0f, kMessageDrop);
    }
    sMessageShown = sRender.message;
}

bool Game_RenderStereo() {
    return SessionRunning() && sRender.stereo && sRender.projection != nullptr && sRender.viewing != nullptr;
}

void Game_AddEyeMatrices(int eye, float t, uint32_t width, uint32_t height,
                         std::unordered_map<Mtx*, MtxF>& replacements) {
    const EyeView& view = Runtime::Get().GetEyes()[eye];
    // Following the title's camera, its turns are eased between ticks too, so its pans are smooth.
    float tickYaw = sYaw;
    if (sRender.fromCamera) {
        float turn = sRender.yaw - sRenderPrevYaw;
        turn = remainderf(turn, 2.0f * static_cast<float>(M_PI));
        sYaw = sRenderPrevYaw + turn * t;
    }
    Vec3f anchor = {
        sRenderPrevAnchor.x + (sRender.anchor.x - sRenderPrevAnchor.x) * t,
        sRenderPrevAnchor.y + (sRender.anchor.y - sRenderPrevAnchor.y) * t,
        sRenderPrevAnchor.z + (sRender.anchor.z - sRenderPrevAnchor.z) * t,
    };
    // The center as of this frame. Where the player's steps moved Link at the last tick, it moved with him, so
    // both slide together and the view follows the head exactly instead of lagging a tick behind.
    float back = 1.0f - t;
    Vec3 eyeWorld = ToWorldAround(view.pose.position, anchor, sRender.unitsPerMeter,
                                  sCenter[0] - sRender.centerShift[0] * back, sCenter[1] - sRender.centerShift[1] * back);

    BuildProjection(view.fov, kNearPlane, sRender.zFar, width, height, replacements[sRender.projection]);
    BuildView(eyeWorld, view.pose.orientation, replacements[sRender.viewing]);
    sYaw = tickYaw;

    for (int i = 0; i < sRender.skyboxCount; i++) {
        MtxF centered = sRender.skyboxMatrix[i];
        centered.mf[3][0] = eyeWorld.x;
        centered.mf[3][1] = eyeWorld.y;
        centered.mf[3][2] = eyeWorld.z;
        replacements[sRender.skybox[i]] = centered;
    }
}

// During a fade or transition the eyes get the overlay up to where the fade ends, so it still covers the world,
// and the HUD and text after it stay on the wrists. Putting the whole overlay in front of the eyes flashed the
// HUD and the place name up in your face at every loading zone. A game over still shows it whole, since what it
// says is meant to be read.
bool FadeOnlyInEyes() {
    return sRender.fade && !sRender.gameOver && sRender.fadeEnd != nullptr;
}

bool Game_SplitHud() {
    return Game_RenderStereo() && (!sRender.fade || FadeOnlyInEyes()) && sRender.overlayBranch != nullptr &&
           sRender.overlay != nullptr && sRender.overlayEnd != nullptr;
}

void Game_CutOverlay(bool cut) {
    if (cut && !sOverlayCut && Game_SplitHud()) {
        sOverlayCutAt = FadeOnlyInEyes() ? sRender.fadeEnd : sRender.overlayBranch;
        sOverlayBranchSaved = *sOverlayCutAt;
        *sOverlayCutAt = *sRender.overlayEnd;
        sOverlayCut = true;
    } else if (!cut && sOverlayCut) {
        *sOverlayCutAt = sOverlayBranchSaved;
        sOverlayCut = false;
    }
}

Gfx* Game_Overlay() {
    return sRender.overlay;
}

Vec3f Game_PlayToWorld(const float position[3]) {
    Vec3 world = ToWorld(position, sBuild.anchor, sBuild.unitsPerMeter);
    return { world.x, world.y, world.z };
}

float Game_UnitsPerMeter() {
    return sBuild.unitsPerMeter;
}

bool Game_PlayingOcarina() {
    return PlayingOcarina();
}

void Game_RequestItem(int request) {
    RequestItem(request);
}

void Game_TakeItem(uint8_t item, Hand hand, int8_t slot) {
    sHeldMask = ITEM_NONE;
    sBottleOnButton = false;
    sBottleContent = ITEM_NONE;
    // A mask goes into the hand, to be put on by bringing it to the face. On the floating screen there's no face
    // to bring it to, so choosing it puts it on, as its button did.
    bool physical = !ScreenGameplay();
    if (physical && item >= ITEM_MASK_KEATON && item <= ITEM_MASK_TRUTH) {
        sHeldMask = item;
        sMaskHand = hand;
        Runtime::Get().Vibrate(hand, 0.3f, 0.04f);
        return;
    }
    if (HeldInOneHand(item)) {
        PutLinkHandOn(ItemHand(item), hand);
    }
    if (slot >= 0) {
        // The game drinks from or fills the bottle on the button it was used from, so the chosen one goes on C-left.
        gSaveContext.equips.buttonItems[kBottleButton] = item;
        gSaveContext.equips.cButtonSlots[kBottleButton - 1] = static_cast<u8>(slot);
        Interface_LoadItemIcon1(gPlayState, kBottleButton);
        sBottleOnButton = true;
        if (physical && item != ITEM_BOTTLE) {
            sBottleContent = item;
            RequestItem(VR_REQUEST_HOLD_BOTTLE);
            return;
        }
    }
    RequestItem(item);
}

void Game_GetHudQuads(std::vector<HudQuad>& out) {
    if (!Game_SplitHud()) {
        return;
    }
    if (sRender.fromCamera) {
        if (sTitlePanel.valid) {
            out.push_back(MakeQuad(sTitlePanel, kTitlePanelWidth, { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT }));
        }
        return;
    }
    const Runtime& runtime = Runtime::Get();
    const ControllerState& left = runtime.GetController(HAND_LEFT);
    const ControllerState& right = runtime.GetController(HAND_RIGHT);

    if (left.gripPose.valid) {
        Pose watch = Attach(GripFrame(left.gripPose), kLeftWatchOffset, kLeftWatchRotation);
        out.push_back(MakeQuad(Attach(watch, { 0.0f, 0.026f, 0.0f }, kNoRotation), 0.12f, kHeartsRect));
        out.push_back(MakeQuad(Attach(watch, { -0.028f, -0.03f, 0.0f }, kNoRotation), 0.065f, kRupeesRect));
    }
    if (right.gripPose.valid) {
        Pose rightGrip = GripFrame(right.gripPose);
        // As wide as before per pixel: 5 cm for the A button's 46 pixels.
        out.push_back(MakeQuad(Attach(rightGrip, kRightWatchOffset, kRightWatchRotation),
                               0.05f * kActionRect.w / 46.0f, kActionRect));
    }
    // The ammo left (see DrawAmmoCounter) on the back of the hand holding the bow or slingshot, past that wrist's
    // own panel toward the knuckles.
    if (CurrentTool() == HeldTool::Bow) {
        Hand bowHand = ControllerFor(LINK_RIGHT);
        const Pose& grip = runtime.GetController(bowHand).gripPose;
        if (grip.valid) {
            Pose watch = (bowHand == HAND_RIGHT) ? Attach(GripFrame(grip), kRightWatchOffset, kRightWatchRotation)
                                                 : Attach(GripFrame(grip), kLeftWatchOffset, kLeftWatchRotation);
            float along = (bowHand == HAND_RIGHT) ? 0.045f : 0.075f;
            out.push_back(MakeQuad(Attach(watch, { 0.0f, along, 0.0f }, kNoRotation), 0.06f, kAmmoRect));
        }
    }
    if (sRender.message && sMessagePose.valid) {
        int y = std::clamp(static_cast<int>(sRender.textboxY) - 6, 0, SCREEN_HEIGHT - kTextboxHeight);
        out.push_back(MakeQuad(sMessagePose, 0.9f, { 0, y, SCREEN_WIDTH, kTextboxHeight }));
    }
    // The VR settings panel, drawn into the HUD (see Items_DrawPanel), on its own panel in front of the player.
    Pose panelPose;
    float panelWidth;
    int panelRect[4];
    if (Items_PanelShown(panelPose, panelWidth, panelRect)) {
        out.push_back(MakeQuad(panelPose, panelWidth, { panelRect[0], panelRect[1], panelRect[2], panelRect[3] }));
    }
}

static RegisterShipInitFunc initFunc(RegisterVr, { CVAR_VR_ENABLED });

} // namespace SohVr

extern "C" bool Vr_IsWorldStereo(void) {
    return SohVr::SessionRunning() && gPlayState != nullptr && SohVr::sBuild.stereo;
}

extern "C" void Vr_OnViewApplied(View* view, Mtx* projection, Mtx* viewing) {
    if (!SohVr::sAwaitingWorldView || gPlayState == nullptr || view != &gPlayState->view) {
        return;
    }
    SohVr::sAwaitingWorldView = false;
    SohVr::sWorldViewCaptured = true;
    SohVr::sBuild.projection = projection;
    SohVr::sBuild.viewing = viewing;
    SohVr::sBuild.zFar = view->zFar;
}

extern "C" void Vr_OnSkyboxDrawn(Mtx* mtx, MtxF* matrix) {
    SohVr::TickState& tick = SohVr::sBuild;
    if (tick.skyboxCount < SohVr::TickState::kMaxSkyboxes) {
        tick.skybox[tick.skyboxCount] = mtx;
        tick.skyboxMatrix[tick.skyboxCount] = *matrix;
        tick.skyboxCount++;
    }
}

extern "C" void Vr_OnGraphBuilt(Gfx* overlayBranch, Gfx* overlay, Gfx* overlayEnd) {
    SohVr::sBuild.overlayBranch = overlayBranch;
    SohVr::sBuild.overlay = overlay;
    SohVr::sBuild.overlayEnd = overlayEnd;
}

extern "C" void Vr_OnFadeDrawn(Gfx* fadeEnd) {
    SohVr::sBuild.fadeEnd = fadeEnd;
}

extern "C" void Vr_OverridePads(OSContPad* pads) {
    if (!SohVr::SessionRunning()) {
        return;
    }
    const SohVr::ControllerState& left = SohVr::Runtime::Get().GetController(SohVr::HAND_LEFT);
    float x = left.stick[0];
    float y = left.stick[1];
    // The game moves Link relative to where the camera looks, which in the world is where the head looks. With
    // hand-relative movement the stick is turned by how far the left hand points to the side of that, flat on the
    // floor, so pushing it forward goes where the hand points. A hand pointing straight up or down leaves it be, and
    // so does holding a block, which is pushed and pulled relative to where the head looks.
    const SohVr::Pose& head = SohVr::Runtime::Get().GetHead();
    bool holdingBlock = Vr_IsWorldStereo() && (GET_PLAYER(gPlayState)->stateFlags2 & PLAYER_STATE2_GRABBING_DYNAPOLY);
    if (CVarGetInteger(CVAR_VR_HAND_MOVEMENT, 0) && Vr_IsWorldStereo() && !holdingBlock && head.valid &&
        left.aimPose.valid) {
        const float* q = left.aimPose.orientation;
        float handX = -2.0f * (q[0] * q[2] + q[3] * q[1]);
        float handZ = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
        SohVr::Vec3 look = SohVr::HeadForward();
        if (handX * handX + handZ * handZ > 0.1f && look.x * look.x + look.z * look.z > 0.1f) {
            // Positive to the right of where the head looks (in the play space -z is ahead and +x is right).
            float side = handX * -look.z + handZ * look.x;
            float ahead = handX * look.x + handZ * look.z;
            float turn = atan2f(side, ahead);
            float c = cosf(turn);
            float s = sinf(turn);
            float turnedX = x * c + y * s;
            y = -x * s + y * c;
            x = turnedX;
        }
    }
    if (x * x + y * y > SohVr::kStickDeadzone * SohVr::kStickDeadzone) {
        pads[0].stick_x = static_cast<int8_t>(lroundf(std::clamp(x, -1.0f, 1.0f) * SohVr::kStickRange));
        pads[0].stick_y = static_cast<int8_t>(lroundf(std::clamp(y, -1.0f, 1.0f) * SohVr::kStickRange));
    }
    // A hand holding on while climbing climbs instead (see UpdateClimb).
    if (SohVr::sClimbStickActive && Vr_IsWorldStereo()) {
        pads[0].stick_x = static_cast<int8_t>(lroundf(SohVr::sClimbStick[0]));
        pads[0].stick_y = static_cast<int8_t>(lroundf(SohVr::sClimbStick[1]));
    }
    // Buttons pressed and released between two game ticks still count.
    pads[0].button |= SohVr::sLatchedButtons | SohVr::CurrentButtons();
    SohVr::sLatchedButtons = 0;

    // A whistled, hummed or sung note presses its button, and how far off it is bends the ocarina as the stick's up
    // and down do, two semitones at most each way. The game only plays a note on a new press, so the same note
    // started again is let go for a tick first.
    if (SohVr::PlayingOcarina()) {
        SohVr::MicNote heard = SohVr::OcarinaMic_Current();
        bool again = heard.onset != SohVr::sMicOnset && heard.button == SohVr::sMicButton;
        SohVr::sMicOnset = heard.onset;
        if (heard.button != 0 && !again) {
            pads[0].button |= heard.button;
            pads[0].stick_y = static_cast<int8_t>(std::clamp(lroundf(heard.bend * 32.0f), -64L, 64L));
        }
        SohVr::sMicButton = again ? 0 : heard.button;
    }

    if (Vr_IsWorldStereo()) {
        SohVr::Items_ApplyPending();
    }
}

// Frame interpolation keeps each matrix the player's draw makes relative to Link's own (position and facing), unless
// it was built from scratch (MTXMODE_NEW), and blends the old tick's with the new. One already in the world, put with
// Matrix_Put, counted as relative: when Link turned half around in a tick, as tapping the stick back and letting go
// turns him, the old and new ones pointed opposite ways, and blending them squashed his hands and what they hold
// to nothing in the frames between, so they flickered. Starting from scratch and then applying it blends it as it
// is, which follows the controllers.
extern "C" void Vr_PutWorldMatrix(MtxF* matrix) {
    Matrix_Translate(0.0f, 0.0f, 0.0f, MTXMODE_NEW);
    Matrix_Mult(matrix, MTXMODE_APPLY);
}

// The shop's bomb, measured from its vertices (model units): a round body of radius 25 whose middle is 5 below the
// model's origin, and a fuse whose tip is 30 above it.
constexpr float kBombBodyRadius = 25.0f;
constexpr float kBombBodyMiddle = -5.0f;
constexpr float kBombFuseTip = 30.0f;

extern "C" void Vr_PlaceBombModel(Actor* actor, f32 scale, Vec3f* origin, Vec3f* fuseTip) {
    MtxF turn;
    SkinMatrix_SetTranslateRotateYXZScale(&turn, 1.0f, 1.0f, 1.0f, actor->shape.rot.x, actor->shape.rot.y,
                                          actor->shape.rot.z, 0.0f, 0.0f, 0.0f);
    Vec3f up = { 0.0f, 1.0f, 0.0f };
    Vec3f axis;
    SkinMatrix_Vec3fMtxFMultXYZ(&turn, &up, &axis);
    *origin = actor->world.pos;
    if (actor->parent == nullptr) {
        // The body's lowest point on the ground, wherever its middle is turned to.
        origin->y += (kBombBodyRadius - kBombBodyMiddle * axis.y) * scale;
    }
    fuseTip->x = origin->x + axis.x * kBombFuseTip * scale;
    fuseTip->y = origin->y + axis.y * kBombFuseTip * scale;
    fuseTip->z = origin->z + axis.z * kBombFuseTip * scale;
}

// While the bow or slingshot is out, the arrows or seeds left are shown on the back of the hand holding it: drawn into
// the HUD's ammo rect, and shown there by a panel on that hand (see Game_GetHudQuads). The count is the one the game
// shoots from: the horseback archery's or the shooting gallery's while one is on, otherwise the player's own.
static void DrawAmmoCounter(PlayState* play) {
    const SohVr::ScreenRect& kAmmoRect = SohVr::kAmmoRect;
    Player* player = GET_PLAYER(play);
    if (SohVr::CurrentTool() != SohVr::HeldTool::Bow) {
        return;
    }
    bool slingshot = Player_HoldsSlingshot(player);
    s32 ammo;
    if (gSaveContext.minigameState == 1) {
        ammo = play->interfaceCtx.hbaAmmo;
    } else if (play->shootingGalleryStatus != 0) {
        ammo = play->shootingGalleryStatus;
    } else {
        ammo = AMMO(slingshot ? ITEM_SLINGSHOT : ITEM_BOW);
    }
    Gfx*& overlay = play->state.gfxCtx->overlay.p;
    gDPPipeSync(overlay++);
    gDPSetCycleType(overlay++, G_CYC_1CYCLE);
    gDPSetRenderMode(overlay++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    gDPSetCombineMode(overlay++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(overlay++, 0, 0, 10, 15, 30, 200);
    gDPFillRectangle(overlay++, kAmmoRect.x, kAmmoRect.y, kAmmoRect.x + kAmmoRect.w, kAmmoRect.y + kAmmoRect.h);

    char text[16];
    snprintf(text, sizeof(text), "%s %d", slingshot ? "SEEDS" : "ARROWS", static_cast<int>(std::max(ammo, 0)));
    GfxPrint printer;
    GfxPrint_Init(&printer);
    GfxPrint_Open(&printer, overlay);
    GfxPrint_SetColor(&printer, 255, ammo > 0 ? 255 : 90, ammo > 0 ? 255 : 90, 255);
    GfxPrint_SetPosPx(&printer, kAmmoRect.x + (kAmmoRect.w - 8 * static_cast<int>(strlen(text))) / 2,
                      kAmmoRect.y + (kAmmoRect.h - 8) / 2);
    GfxPrint_Printf(&printer, "%s", text);
    overlay = GfxPrint_Close(&printer);
    GfxPrint_Destroy(&printer);
}

extern "C" bool Vr_ViewFromCamera(void) {
    return Vr_IsWorldStereo() && SohVr::sBuild.fromCamera;
}

extern "C" void Vr_DrawPlayerExtras(PlayState* play) {
    if (Vr_ViewFromCamera()) {
        return;
    }
    if (!Vr_IsWorldStereo()) {
        if (SohVr::ScreenGameplay()) {
            SohVr::Items_Draw(play);
        }
        return;
    }
    Player* player = GET_PLAYER(play);
    if (!(player->stateFlags2 & PLAYER_STATE2_DISABLE_DRAW)) {
        SohVr::DrawHandItem(play, player, SohVr::LINK_LEFT);
        SohVr::DrawHandItem(play, player, SohVr::LINK_RIGHT);
        SohVr::DrawHeldMask(play);
        DrawAmmoCounter(play);
        SohVr::Aim_DrawShotMarker(play, player);
    }
    SohVr::Items_Draw(play);
}

extern "C" s32 Vr_ItemRequestButton(void) {
    bool onButton = SohVr::sBottleOnButton;
    SohVr::sBottleOnButton = false;
    return onButton ? SohVr::kBottleButton : -1;
}

extern "C" u8 Vr_TakeBottleContent(void) {
    u8 content = SohVr::sBottleContent;
    SohVr::sBottleContent = ITEM_NONE;
    return content;
}

extern "C" Actor* Vr_TakeBottleCatch(void) {
    Actor* creature = SohVr::sBottleCatch;
    SohVr::sBottleCatch = nullptr;
    return Vr_IsWorldStereo() ? creature : nullptr;
}

extern "C" void Vr_PlaceNockedArrow(Player* player, Actor* arrow) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || arrow == nullptr) {
        return;
    }
    // The arrow's back end sits in the string hand, no farther back than a full draw, pointing through the bow.
    const SohVr::Pose& bow = SohVr::Runtime::Get().GetController(SohVr::ControllerFor(SohVr::LINK_RIGHT)).gripPose;
    float nock[3];
    if (!SohVr::DrawPoint(player, nock)) {
        return;
    }
    SohVr::Vec3 hand = SohVr::ToWorld(nock, SohVr::sBuild.anchor, SohVr::sBuild.unitsPerMeter);
    arrow->world.pos = { hand.x, hand.y, hand.z };
    SohVr::Vec3 draw = { bow.position[0] - nock[0], bow.position[1] - nock[1], bow.position[2] - nock[2] };
    if (SohVr::Dot(draw, draw) > SohVr::kMinDrawLength * SohVr::kMinDrawLength) {
        SohVr::Vec3 along = SohVr::Normalized(SohVr::RotateYaw(SohVr::sYaw, draw));
        SohVr::FaceAlong(arrow, along);
        // A seed is too small to show; an arrow's origin is partway up its shaft, ahead of the nock.
        if (!Player_HoldsSlingshot(player)) {
            arrow->world.pos.x += along.x * SohVr::kArrowNockBehind;
            arrow->world.pos.y += along.y * SohVr::kArrowNockBehind;
            arrow->world.pos.z += along.z * SohVr::kArrowNockBehind;
        }
    }
}

extern "C" bool Vr_UseVendorModels(void) {
    return Vr_IsWorldStereo();
}

extern "C" bool Vr_GetInputDir(Vec3s* dst) {
    if (!Vr_IsWorldStereo() || !SohVr::Runtime::Get().GetHead().valid) {
        return false;
    }
    SohVr::Vec3 forward = SohVr::RotateYaw(SohVr::sYaw, SohVr::HeadForward());
    dst->x = 0;
    dst->y = RADF_TO_BINANG(atan2f(forward.x, forward.z));
    dst->z = 0;
    return true;
}

extern "C" bool Vr_UseNewDrops(void) {
    return CVarGetInteger(CVAR_ENHANCEMENT("NewDrops"), 0) || CVarGetInteger(CVAR_VR_ENABLED, 0);
}

extern "C" bool Vr_DropNeedsGrab(s16 type) {
    if (!Vr_IsWorldStereo() || !CVarGetInteger(CVAR_VR_GRAB_DROPS, 0)) {
        return false;
    }
    switch (type) {
        case ITEM00_BOMBS_A:
        case ITEM00_BOMBS_B:
        case ITEM00_BOMBS_SPECIAL:
        case ITEM00_BOMBCHU:
        case ITEM00_ARROWS_SMALL:
        case ITEM00_ARROWS_MEDIUM:
        case ITEM00_ARROWS_LARGE:
        case ITEM00_SEEDS:
        case ITEM00_NUTS:
        case ITEM00_STICK:
        case ITEM00_MAGIC_SMALL:
        case ITEM00_MAGIC_LARGE:
            return true;
        default:
            return false;
    }
}

extern "C" void Vr_OverridePlayerLimb(Player* player, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot) {
    // During a spin attack the sword goes where the game's spin takes it, around the player, so it sweeps everything
    // around them; the hands come back to the controllers after.
    if (player->stateFlags2 & PLAYER_STATE2_SPIN_ATTACKING) {
        return;
    }
    switch (limbIndex) {
        case PLAYER_LIMB_L_FOREARM:
            SohVr::PlaceForearm(player, SohVr::LINK_LEFT, PLAYER_LIMB_L_HAND, pos, rot);
            *dList = nullptr;
            break;
        case PLAYER_LIMB_R_FOREARM:
            SohVr::PlaceForearm(player, SohVr::LINK_RIGHT, PLAYER_LIMB_R_HAND, pos, rot);
            *dList = nullptr;
            break;
        // A hand holding an item drawn from its item-get model is a plain fist; the item is drawn on its own.
        case PLAYER_LIMB_L_HAND:
            if (SohVr::HandItemFor(player, SohVr::LINK_LEFT) != nullptr) {
                *dList = ResourceMgr_LoadGfxByName(LINK_IS_ADULT ? gLinkAdultLeftHandClosedNearDL
                                                                 : gLinkChildLeftFistNearDL);
            }
            break;
        case PLAYER_LIMB_R_HAND:
            if (SohVr::HandItemFor(player, SohVr::LINK_RIGHT) != nullptr) {
                *dList = ResourceMgr_LoadGfxByName(LINK_IS_ADULT ? gLinkAdultRightHandClosedNearDL
                                                                 : gLinkChildRightHandClosedNearDL);
            }
            break;
        default:
            *dList = nullptr;
            break;
    }
}

extern "C" void Vr_ApplyRoomScale(Player* player) {
    if (!Vr_IsWorldStereo() || Vr_ViewFromCamera() || player != GET_PLAYER(gPlayState) ||
        !SohVr::StepsMoveLink(player)) {
        return;
    }
    const SohVr::Pose& head = SohVr::Runtime::Get().GetHead();
    if (!head.valid) {
        return;
    }
    // Link goes where the head went since the last tick, and the play space's center follows so the head is back
    // over him. If a wall stops him, the view stops with him instead of going through it.
    float dx = head.position[0] - SohVr::sCenter[0];
    float dz = head.position[2] - SohVr::sCenter[1];
    float length = sqrtf(dx * dx + dz * dz);
    if (length > SohVr::kRoomScaleMaxStep) {
        dx *= SohVr::kRoomScaleMaxStep / length;
        dz *= SohVr::kRoomScaleMaxStep / length;
    }
    float unitsPerMeter = SohVr::sBuild.unitsPerMeter;
    SohVr::Vec3 step = SohVr::RotateYaw(SohVr::sYaw, { dx * unitsPerMeter, 0.0f, dz * unitsPerMeter });
    player->actor.world.pos.x += step.x;
    player->actor.world.pos.z += step.z;
    SohVr::ShiftCenter(dx, dz);

    // Standing still, Link faces where the player looks, so the door, chest or person the player faces is in front
    // of him too, which the game checks before offering to open or talk. Not while he swings, aims or locks on,
    // and not while the stick is pushed: backing up, the game stops and turns him around, and turning him back
    // to the view each time it stopped him made the two fight and left him shaking in place. Not until he has
    // stopped sliding either: on hover boots or ice he keeps sliding (speedXZ, along world.rot.y) after the game
    // has stopped him (linearVelocity), and turning him then sent the rest of a slide backward forward instead.
    const float* stick = SohVr::Runtime::Get().GetController(SohVr::HAND_LEFT).stick;
    bool stickIdle = stick[0] * stick[0] + stick[1] * stick[1] < SohVr::kStickDeadzone * SohVr::kStickDeadzone;
    // Nor while the game holds his facing, as it does while he holds a block.
    if (stickIdle && player->linearVelocity == 0.0f && player->actor.speedXZ == 0.0f &&
        player->focusActor == nullptr && !(player->stateFlags2 & PLAYER_STATE2_DISABLE_ROTATION_ALWAYS) &&
        player->meleeWeaponState == 0 && SohVr::CurrentTool() == SohVr::HeldTool::None) {
        SohVr::Vec3 forward = SohVr::RotateYaw(SohVr::sYaw, SohVr::HeadForward());
        if (forward.x * forward.x + forward.z * forward.z > 0.1f) {
            s16 yaw = RADF_TO_BINANG(atan2f(forward.x, forward.z));
            player->actor.shape.rot.y = player->actor.world.rot.y = player->yaw = yaw;
        }
    }
}

// An item Link holds up after getting it is shown above the player's hands: the middle of their grip points, raised
// the get-item lift (meters), and about the get-item size across (meters) whatever his age. The game puts it above
// the middle of Link's wrists, which in VR are toward the chest from the controllers, at his own size.
constexpr float kGetItemLift = 0.1f;
constexpr float kGetItemSize = 0.15f;
constexpr float kItemModelUnits = 60.0f; // about how wide an item-get model is at scale 1

extern "C" bool Vr_GetItemPlace(Player* player, Vec3f* pos, f32* scale) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState)) {
        return false;
    }
    const SohVr::Runtime& runtime = SohVr::Runtime::Get();
    const SohVr::Pose& left = runtime.GetController(SohVr::HAND_LEFT).gripPose;
    const SohVr::Pose& right = runtime.GetController(SohVr::HAND_RIGHT).gripPose;
    if (!left.valid || !right.valid) {
        return false;
    }
    float middle[3] = { (left.position[0] + right.position[0]) * 0.5f,
                        (left.position[1] + right.position[1]) * 0.5f + kGetItemLift,
                        (left.position[2] + right.position[2]) * 0.5f };
    SohVr::Vec3 world = SohVr::ToWorld(middle, SohVr::sBuild.anchor, SohVr::sBuild.unitsPerMeter);
    *pos = { world.x, world.y, world.z };
    *scale = kGetItemSize * SohVr::sBuild.unitsPerMeter / kItemModelUnits;
    return true;
}

extern "C" bool Vr_LimbMirrored(Player* player, s32 limbIndex) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState)) {
        return false;
    }
    switch (limbIndex) {
        case PLAYER_LIMB_L_FOREARM:
        case PLAYER_LIMB_L_HAND:
            return SohVr::HandMirrored(SohVr::LINK_LEFT);
        case PLAYER_LIMB_R_FOREARM:
        case PLAYER_LIMB_R_HAND:
            return SohVr::HandMirrored(SohVr::LINK_RIGHT);
        default:
            return false;
    }
}

extern "C" bool Vr_HoldsBlade(Player* player) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || Player_GetMeleeWeaponHeld(player) == 0) {
        return false;
    }
    return player->leftHandType == PLAYER_MODELTYPE_LH_SWORD || player->leftHandType == PLAYER_MODELTYPE_LH_BGS ||
           player->leftHandType == PLAYER_MODELTYPE_LH_HAMMER || player->itemAction == PLAYER_IA_DEKU_STICK;
}

extern "C" bool Vr_SwordSwing(Player* player, Vec3f* tip) {
    static Vec3f sLastTip;
    static bool sHasLastTip = false;
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState)) {
        sHasLastTip = false;
        SohVr::sSwingActive = false;
        return false;
    }

    // The tip's movement relative to Link, so running or stepping doesn't count as swinging.
    static SohVr::Vec3 sLastMove = {};
    Vec3f tipFromLink = { tip->x - player->actor.world.pos.x, tip->y - player->actor.world.pos.y,
                          tip->z - player->actor.world.pos.z };
    SohVr::Vec3 move = {};
    if (sHasLastTip) {
        move = { tipFromLink.x - sLastTip.x, tipFromLink.y - sLastTip.y, tipFromLink.z - sLastTip.z };
    }
    float travel = sqrtf(SohVr::Dot(move, move)) / SohVr::sBuild.unitsPerMeter; // meters this tick
    float speed = travel * SohVr::kTicksPerSecond;                               // meters per second
    SohVr::sTipSpeed = speed;
    bool turnedBack = SohVr::Dot(move, sLastMove) < 0.0f;
    float handSpeed = SohVr::HandSpeed(SohVr::ControllerFor(SohVr::LINK_LEFT));
    sLastTip = tipFromLink;
    sLastMove = move;
    sHasLastTip = true;

    // A hit from last tick ends the damage for the rest of the swing.
    for (ColliderQuad& quad : player->meleeWeaponQuads) {
        if (SohVr::sSwingActive && (quad.base.atFlags & AT_HIT)) {
            SohVr::sSwingHit = true;
            quad.base.atFlags &= ~AT_HIT;
            SohVr::Runtime::Get().Vibrate(SohVr::ControllerFor(SohVr::LINK_LEFT), 0.9f, 0.08f);
        }
    }
    if (!SohVr::sSwingActive) {
        if (SohVr::sSwingCooldown > 0) {
            SohVr::sSwingCooldown--;
            SohVr::sSwingTravel = 0.0f;
        } else if (speed > SohVr::kSwingStartSpeed && handSpeed > SohVr::kSwingMinHandSpeed && !turnedBack) {
            // Building up: the blade only becomes dangerous once it has carried its speed far enough.
            SohVr::sSwingTravel += travel;
            if (SohVr::sSwingTravel >= SohVr::kSwingMinTravel) {
                SohVr::sSwingActive = true;
                SohVr::sSwingHit = false;
            }
        } else {
            SohVr::sSwingTravel = 0.0f;
        }
    } else if (speed < SohVr::kSwingStopSpeed || turnedBack) {
        SohVr::sSwingActive = false;
        SohVr::sSwingCooldown = SohVr::kSwingCooldownTicks;
        SohVr::sSwingTravel = 0.0f;
    }

    bool live = SohVr::sSwingActive && !SohVr::sSwingHit;
    // During the game's own attacks its damage stays as it set it. A spin made by turning hits as a spin, and a
    // very hard swing as hard as a jump attack.
    if (live && player->meleeWeaponState == 0) {
        u32 damage = SohVr::sPhysicalSpinLeft > 0.0f       ? SohVr::SpinDamageFlags(player)
                     : speed >= SohVr::kHeavySwingSpeed ? SohVr::HeavyDamageFlags(player)
                                                        : SohVr::SlashDamageFlags(player);
        for (ColliderQuad& quad : player->meleeWeaponQuads) {
            quad.info.toucher.dmgFlags = damage;
            quad.info.toucherFlags = TOUCH_ON | TOUCH_NEAREST;
        }
    }
    return live;
}

extern "C" bool Vr_TakeSpinCharge(Player* player) {
    bool requested = SohVr::sSpinChargeRequested;
    SohVr::sSpinChargeRequested = false;
    return requested && Vr_IsWorldStereo() && player == GET_PLAYER(gPlayState) && SohVr::HoldsSword(player);
}

extern "C" void Vr_SwordAfterUpdate(PlayState* play, Player* player) {
    static bool sWasInWall = false;
    static int sCooldown = 0;
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || Player_GetMeleeWeaponHeld(player) == 0) {
        sWasInWall = false;
        return;
    }
    if (sCooldown > 0) {
        sCooldown--;
    }
    WeaponInfo& blade = player->meleeWeaponInfo[0];
    Vec3f hit;
    CollisionPoly* poly = nullptr;
    s32 bgId = 0;
    bool inWall = BgCheck_EntityLineTest1(&play->colCtx, &blade.base, &blade.tip, &hit, &poly, true, true, true, true,
                                          &bgId) &&
                  !SurfaceType_IsIgnoredByEntities(&play->colCtx, poly, bgId) &&
                  SurfaceType_GetFloorType(&play->colCtx, poly, bgId) != 6 &&
                  func_8002F9EC(play, &player->actor, poly, bgId, &hit) == 0;
    // Only on striking it: a blade resting against a wall is quiet.
    bool strike = inWall && !sWasInWall && sCooldown == 0 && SohVr::sTipSpeed >= SohVr::kWallStrikeSpeed;
    sWasInWall = inWall;
    if (!strike) {
        return;
    }
    sCooldown = 4;
    SohVr::Hand hand = SohVr::ControllerFor(SohVr::LINK_LEFT);
    if (player->heldItemAction == PLAYER_IA_HAMMER) {
        func_80842A28(play, player); // the ground shakes, and switches the hammer works on feel it
        SohVr::Runtime::Get().Vibrate(hand, 1.0f, 0.25f);
    } else {
        if (func_80041F10(&play->colCtx, poly, bgId) == 0xA) {
            CollisionCheck_SpawnShieldParticlesWood(play, &hit, &player->actor.projectedPos);
            Player_PlaySfx(&player->actor, NA_SE_IT_WALL_HIT_SOFT);
        } else {
            CollisionCheck_SpawnShieldParticles(play, &hit);
            Player_PlaySfx(&player->actor, NA_SE_IT_WALL_HIT_HARD);
        }
        SohVr::Runtime::Get().Vibrate(hand, 0.8f, 0.1f);
    }
    func_80842CF0(play, player); // the Deku stick breaks, and Biggoron's Sword wears, as on the game's own hits
    // The swing ends against the wall.
    SohVr::sSwingActive = false;
    SohVr::sSwingCooldown = SohVr::kSwingCooldownTicks;
    SohVr::sSwingTravel = 0.0f;
}

extern "C" bool Vr_ItemsFromMenu(void) {
    return SohVr::PlayingInVr();
}

extern "C" s32 Vr_TakeItemRequest(void) {
    s32 request = SohVr::PlayingInVr() ? SohVr::sItemRequest : VR_REQUEST_NONE;
    SohVr::sItemRequest = VR_REQUEST_NONE;
    return request;
}

extern "C" bool Vr_ItemButtonHeld(Player* player) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState)) {
        return false;
    }
    switch (SohVr::CurrentTool()) {
        case SohVr::HeldTool::Bow:
            return SohVr::sDrawing;
        case SohVr::HeldTool::Hookshot:
            // The trigger itself. In VR the hookshot is aimed on the tick a pull reaches the game (see
            // func_8083501C and func_808351D4), so letting go fires it on the next, however soon.
            return SohVr::Runtime::Get().GetController(SohVr::ControllerFor(SohVr::LINK_RIGHT)).triggerHeld;
        case SohVr::HeldTool::Boomerang:
            return false;
        default:
            return false;
    }
}

extern "C" bool Vr_HookshotSight(Player* player, Vec3f* tip, Vec3f* direction) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState)) {
        return false;
    }
    const SohVr::HandItem* item = SohVr::HandItemFor(player, SohVr::LINK_RIGHT);
    MtxF m;
    SohVr::Vec3 forward;
    if (item == nullptr || (item->model != GID_HOOKSHOT && item->model != GID_LONGSHOT) ||
        !SohVr::HandItemMatrix(player, SohVr::LINK_RIGHT, item, m, &forward)) {
        return false;
    }
    SohVr::Vec3 point = SohVr::HandItemPoint(m, item->tip);
    *tip = { point.x, point.y, point.z };
    *direction = { forward.x, forward.y, forward.z };
    return true;
}

extern "C" void Vr_PlaceHookshot(Player* player, Vec3f* chainStart, Actor* hook) {
    // The chain and the hook leave from the tip of the model in the hand, the way it points.
    Vec3f tip;
    Vec3f direction;
    if (!Vr_HookshotSight(player, &tip, &direction)) {
        return;
    }
    *chainStart = tip;
    if (hook != nullptr) {
        hook->world.pos = tip;
        SohVr::FaceAlong(hook, { direction.x, direction.y, direction.z });
    }
}

extern "C" bool Vr_StringPull(Player* player, Vec3f* hand) {
    float pull[3];
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || !SohVr::sDrawing ||
        !SohVr::DrawPoint(player, pull)) {
        return false;
    }
    SohVr::Vec3 world = SohVr::ToWorld(pull, SohVr::sBuild.anchor, SohVr::sBuild.unitsPerMeter);
    *hand = { world.x, world.y, world.z };
    return true;
}

bool SohVr::Game_DrawingShot() {
    return Vr_IsWorldStereo() && sDrawing && CurrentTool() == HeldTool::Bow;
}

bool SohVr::Game_ShotAim(Player* player, Vec3f* origin, Vec3f* direction, float* power) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || Player_HoldsHookshot(player)) {
        return false;
    }
    Runtime& runtime = Runtime::Get();
    Hand bowHand = ControllerFor(LINK_RIGHT);
    *power = 1.0f;
    // A seed leaves from between the slingshot's arms and flies along the band: from where the drawing hand pulls it
    // back to, through the arms, so pulling lower aims higher, as with a real slingshot. Barely drawn, it flies the way
    // the slingshot points (the hand model's +Y).
    MtxF hand;
    if (Player_HoldsSlingshot(player)) {
        if (!HandMatrix(player, LINK_RIGHT, hand)) {
            return false;
        }
        const Vec3f& forks = kSlingshotForks;
        Vec3 along = { hand.mf[1][0], hand.mf[1][1], hand.mf[1][2] };
        *origin = { hand.mf[3][0] + forks.x * hand.mf[0][0] + forks.y * hand.mf[1][0] + forks.z * hand.mf[2][0],
                    hand.mf[3][1] + forks.x * hand.mf[0][1] + forks.y * hand.mf[1][1] + forks.z * hand.mf[2][1],
                    hand.mf[3][2] + forks.x * hand.mf[0][2] + forks.y * hand.mf[1][2] + forks.z * hand.mf[2][2] };
        float pouch[3];
        const Pose& slingshot = runtime.GetController(bowHand).gripPose;
        float drawn = 0.0f;
        if (DrawPoint(player, pouch) && slingshot.valid) {
            float dx = pouch[0] - slingshot.position[0];
            float dy = pouch[1] - slingshot.position[1];
            float dz = pouch[2] - slingshot.position[2];
            drawn = sqrtf(dx * dx + dy * dy + dz * dz);
            if (drawn > kMinDrawLength) {
                Vec3 from = ToWorld(pouch, sBuild.anchor, sBuild.unitsPerMeter);
                along = { origin->x - from.x, origin->y - from.y, origin->z - from.z };
            }
        }
        Vec3 unit = Normalized(along);
        *direction = { unit.x, unit.y, unit.z };
        *power = kLeastShotPower + (1.0f - kLeastShotPower) * std::min(drawn / kFullSlingshotDraw, 1.0f);
        return true;
    }
    // Arrows fly from the string hand through the bow, from where the arrow is nocked.
    const Pose& bow = runtime.GetController(bowHand).gripPose;
    const Pose& string = runtime.GetController(ControllerFor(LINK_LEFT)).gripPose;
    Vec3 aim = AimDirection(bowHand);
    Vec3 draw = { bow.position[0] - string.position[0], bow.position[1] - string.position[1],
                  bow.position[2] - string.position[2] };
    float drawn = sqrtf(Dot(draw, draw));
    if (bow.valid && string.valid && drawn > kMinDrawLength) {
        aim = Normalized(RotateYaw(sYaw, draw));
    }
    if (player->heldActor != nullptr) {
        *origin = player->heldActor->world.pos;
    } else if (bow.valid) {
        Vec3 at = ToWorld(bow.position, sBuild.anchor, sBuild.unitsPerMeter);
        *origin = { at.x, at.y, at.z };
    } else {
        return false;
    }
    *direction = { aim.x, aim.y, aim.z };
    *power = kLeastShotPower + (1.0f - kLeastShotPower) * std::min(drawn / kFullBowDraw, 1.0f);
    return true;
}

extern "C" void Vr_AimProjectile(Player* player, Actor* projectile) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || projectile == nullptr) {
        return;
    }
    // The hook flies the way the hookshot in the hand points, along its sight.
    Vec3f hookTip;
    Vec3f hookDirection;
    if (Player_HoldsHookshot(player) && Vr_HookshotSight(player, &hookTip, &hookDirection)) {
        projectile->world.pos = projectile->prevPos = hookTip;
        SohVr::FaceAlong(projectile, { hookDirection.x, hookDirection.y, hookDirection.z });
        return;
    }
    Vec3f origin;
    Vec3f direction;
    float power = 1.0f;
    if (SohVr::Game_ShotAim(player, &origin, &direction, &power)) {
        if (Player_HoldsSlingshot(player)) {
            projectile->world.pos = projectile->prevPos = origin;
        }
        SohVr::FaceAlong(projectile, { direction.x, direction.y, direction.z });
        // The harder it was drawn, the faster it flies (see Vr_ScaleShotSpeed).
        SohVr::sShotPower = power;
        return;
    }
    SohVr::FaceAlong(projectile, SohVr::AimDirection(SohVr::ControllerFor(SohVr::LINK_RIGHT)));
}

extern "C" void Vr_ScaleShotSpeed(Actor* shot) {
    if (SohVr::sShotPower < 0.0f || shot == nullptr) {
        return;
    }
    shot->speedXZ *= SohVr::sShotPower;
    shot->velocity.y *= SohVr::sShotPower;
    SohVr::sShotPower = -1.0f;
}

extern "C" void Vr_AimFromHand(Player* player, Actor* actor, s32 linkHand) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || actor == nullptr) {
        return;
    }
    // The boomerang flies the way the hand was swinging when its grip let go.
    if (actor->id == ACTOR_EN_BOOM && SohVr::sBoomerangAimed) {
        SohVr::sBoomerangAimed = false;
        SohVr::FaceAlong(actor, SohVr::sBoomerangDirection);
        return;
    }
    SohVr::Hand hand = SohVr::ControllerFor(linkHand == 0 ? SohVr::LINK_LEFT : SohVr::LINK_RIGHT);
    SohVr::FaceAlong(actor, SohVr::AimDirection(hand));
}

extern "C" MtxF* gVrBillboard = nullptr;

extern "C" bool Vr_FacingRotation(f32 x, f32 y, f32 z, MtxF* out) {
    // Rows are the sprite's right, up and facing directions in the world, as in play->billboardMtxF, but facing
    // the eye's position instead of along the camera's direction, and kept upright.
    if (gPlayState == nullptr) {
        return false;
    }
    const View& view = gPlayState->view;
    SohVr::Vec3 back = { view.eye.x - x, view.eye.y - y, view.eye.z - z };
    float length = sqrtf(SohVr::Dot(back, back));
    if (length < 0.001f) {
        return false;
    }
    back = { back.x / length, back.y / length, back.z / length };
    SohVr::Vec3 right = { back.z, 0.0f, -back.x }; // world up crossed with the facing direction
    float rightLength = sqrtf(SohVr::Dot(right, right));
    right = rightLength > 0.001f ? SohVr::Vec3{ right.x / rightLength, 0.0f, right.z / rightLength }
                                 : SohVr::Vec3{ 1.0f, 0.0f, 0.0f };
    SohVr::Vec3 up = SohVr::Cross(back, right);

    SkinMatrix_Clear(out);
    out->mf[0][0] = right.x;
    out->mf[0][1] = right.y;
    out->mf[0][2] = right.z;
    out->mf[1][0] = up.x;
    out->mf[1][1] = up.y;
    out->mf[1][2] = up.z;
    out->mf[2][0] = back.x;
    out->mf[2][1] = back.y;
    out->mf[2][2] = back.z;
    return true;
}

extern "C" bool Vr_SightBlocked(PlayState* play, Vec3f* target, f32 maxDistance, f32 stopShort) {
    Vec3f eye = play->view.eye;
    SohVr::Vec3 toward = { target->x - eye.x, target->y - eye.y, target->z - eye.z };
    float distance = sqrtf(SohVr::Dot(toward, toward));
    float reach = std::min(distance, maxDistance) - stopShort;
    if (reach <= 0.0f) {
        return false;
    }
    SohVr::Vec3 along = SohVr::Scaled(toward, reach / distance);
    Vec3f end = { eye.x + along.x, eye.y + along.y, eye.z + along.z };
    Vec3f hit;
    CollisionPoly* poly;
    s32 bgId;
    return BgCheck_AnyLineTest3(&play->colCtx, &eye, &end, &hit, &poly, 1, 1, 1, 1, &bgId);
}

extern "C" Mtx* Vr_FacingBillboard(PlayState* play, Vec3f* position) {
    MtxF facing;
    if (!Vr_FacingRotation(position->x, position->y, position->z, &facing)) {
        facing = play->billboardMtxF;
    }
    return Matrix_MtxFToMtx(&facing, static_cast<Mtx*>(Graph_Alloc(play->state.gfxCtx, sizeof(Mtx))));
}

extern "C" void Vr_StartSongEffect(PlayState* play, Actor* effect) {
    // Where the head looks as the song starts, as the yaw and pitch that turn the effect's back (+Z) toward it.
    SohVr::Vec3 look = { play->view.lookAt.x - play->view.eye.x, play->view.lookAt.y - play->view.eye.y,
                         play->view.lookAt.z - play->view.eye.z };
    effect->home.rot.y = RADF_TO_BINANG(atan2f(-look.x, -look.z));
    effect->home.rot.x = RADF_TO_BINANG(atan2f(look.y, sqrtf(look.x * look.x + look.z * look.z)));
}

extern "C" bool Vr_PlaceSongEffect(PlayState* play, Actor* effect, f32 scale) {
    if (!Vr_IsWorldStereo()) {
        return false;
    }
    Matrix_Translate(play->view.eye.x, play->view.eye.y, play->view.eye.z, MTXMODE_NEW);
    Matrix_RotateY(BINANG_TO_RAD(effect->home.rot.y), MTXMODE_APPLY);
    Matrix_RotateX(BINANG_TO_RAD(effect->home.rot.x), MTXMODE_APPLY);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    return true;
}

extern "C" s16 Vr_CameraYawToward(PlayState* play, Vec3f* position, s16 cameraYaw) {
    if (!Vr_IsWorldStereo()) {
        return cameraYaw;
    }
    return RADF_TO_BINANG(atan2f(position->x - play->view.eye.x, position->z - play->view.eye.z));
}

extern "C" void Vr_PlaceHeldActor(Player* player, Actor* held) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || held == nullptr ||
        !(player->stateFlags1 & PLAYER_STATE1_CARRYING_ACTOR)) {
        return;
    }
    const SohVr::Runtime& runtime = SohVr::Runtime::Get();
    const SohVr::Pose& grip = runtime.GetController(SohVr::ControllerFor(SohVr::LINK_RIGHT)).gripPose;
    if (SohVr::sCarryTwoHanded) {
        // Between both hands, its middle (a little above its origin, as when it was lifted) halfway between them,
        // upright and facing the way the player looks.
        const SohVr::Pose& left = runtime.GetController(SohVr::HAND_LEFT).gripPose;
        const SohVr::Pose& right = runtime.GetController(SohVr::HAND_RIGHT).gripPose;
        if (left.valid && right.valid) {
            float middle[3];
            for (int i = 0; i < 3; i++) {
                middle[i] = (left.position[i] + right.position[i]) * 0.5f;
            }
            SohVr::Vec3 between = SohVr::ToWorld(middle, SohVr::sBuild.anchor, SohVr::sBuild.unitsPerMeter);
            held->world.pos = { between.x, between.y - SohVr::kLiftCenterHeight, between.z };
        }
        SohVr::Vec3 look = SohVr::RotateYaw(SohVr::sYaw, SohVr::HeadForward());
        s16 yaw = RADF_TO_BINANG(atan2f(look.x, look.z));
        held->shape.rot = { 0, yaw, 0 };
        held->world.rot.y = yaw;
        held->world.rot.x = 0;
        return;
    }
    if (grip.valid) {
        SohVr::Vec3 hand = SohVr::ToWorld(grip.position, SohVr::sBuild.anchor, SohVr::sBuild.unitsPerMeter);
        held->world.pos = { hand.x, hand.y, hand.z };
    }
    // What's carried turns with the hand, front out over the knuckles and top toward the thumb, rather than the way
    // Link faces. It's thrown level whichever way it was turned (see Vr_ThrowWithHand).
    Vec3s rot;
    if (SohVr::HeldRotation(player, &rot)) {
        held->shape.rot = rot;
        held->world.rot.y = rot.y;
        held->world.rot.x = 0;
    }
}

extern "C" bool Vr_TakeBoomerangThrow(Vec3f* from) {
    bool requested = SohVr::sBoomerangThrowRequested;
    SohVr::sBoomerangThrowRequested = false;
    if (!requested || !Vr_IsWorldStereo()) {
        return false;
    }
    const SohVr::Pose& grip = SohVr::Runtime::Get().GetController(SohVr::ControllerFor(SohVr::LINK_LEFT)).gripPose;
    if (!grip.valid) {
        return false;
    }
    SohVr::Vec3 hand = SohVr::ToWorld(grip.position, SohVr::sBuild.anchor, SohVr::sBuild.unitsPerMeter);
    *from = { hand.x, hand.y, hand.z };
    return true;
}

extern "C" bool Vr_TakeThrowRequest(void) {
    bool requested = SohVr::sThrowRequested;
    SohVr::sThrowRequested = false;
    return requested && Vr_IsWorldStereo();
}

extern "C" void Vr_ThrowWithHand(Player* player, Actor* thrown) {
    // Only throws the grip let go of; the game's own button throws stay as they are.
    if (!SohVr::sThrowPending || !Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || thrown == nullptr) {
        return;
    }
    SohVr::sThrowPending = false;
    const SohVr::Vec3& v = SohVr::sThrowVelocity;
    if (thrown->id == ACTOR_EN_BOM_CHU) {
        // Bombchus run along the floor, so only the direction comes from the hand, and they're set down flat.
        SohVr::Vec3 aim = SohVr::AimDirection(SohVr::ControllerFor(SohVr::LINK_RIGHT));
        thrown->world.rot.y = thrown->shape.rot.y = RADF_TO_BINANG(atan2f(aim.x, aim.z));
        thrown->world.rot.x = thrown->shape.rot.x = 0;
        thrown->world.rot.z = thrown->shape.rot.z = 0;
        return;
    }
    // Bombs, pots and rocks are heavy: however hard the pitch, they go no farther than the game's own throw, which
    // leaves at Link's running speed plus 8 forward and 12 up.
    float speedXZ = std::min(sqrtf(v.x * v.x + v.z * v.z), player->linearVelocity + SohVr::kBombMaxForward);
    float velocityY = std::min(v.y, SohVr::kBombMaxUp);
    // It flies level the way the hand moved; it keeps looking turned however the hand had it.
    thrown->world.rot.y = RADF_TO_BINANG(atan2f(v.x, v.z));
    thrown->world.rot.x = 0;
    thrown->speedXZ = speedXZ;
    thrown->velocity.y = velocityY;
}

extern "C" bool Vr_HoldNut(Player* player, Actor* nut) {
    if (!Vr_IsWorldStereo() || player != GET_PLAYER(gPlayState) || nut == nullptr) {
        return false;
    }
    // A nut already in hand drops when the next one arrives.
    if (SohVr::sHeldNut != nullptr) {
        SohVr::ReleaseNut({ 0.0f, 0.0f, 0.0f });
    }
    // While it has a parent the nut waits instead of flying off.
    nut->parent = &player->actor;
    SohVr::sHeldNut = nut;
    // The game throws nuts straight from whatever Link holds, so it leaves that in his hand. Here the nut is held
    // instead, so an item he held (the slingshot, say) is put away; a sword or stick stays drawn.
    if ((player->heldItemAction != PLAYER_IA_NONE) && (Player_GetMeleeWeaponHeld(player) == 0)) {
        SohVr::RequestItem(VR_REQUEST_PUT_AWAY);
    }
    SPDLOG_INFO("[VR] Deku nut in hand; squeeze the grip and let go to throw it");
    return true;
}

extern "C" bool Vr_LaunchThrownNut(Actor* nut) {
    if (nut == nullptr || nut != SohVr::sThrownNut) {
        return false;
    }
    SohVr::sThrownNut = nullptr;
    const SohVr::Vec3& v = SohVr::sNutVelocity;
    nut->world.rot.y = RADF_TO_BINANG(atan2f(v.x, v.z));
    nut->speedXZ = sqrtf(v.x * v.x + v.z * v.z);
    nut->velocity.y = v.y;
    nut->gravity = -1.2f;
    return true;
}

extern "C" bool Vr_HoldsShield(Player* player) {
    return Vr_IsWorldStereo() && player == GET_PLAYER(gPlayState) &&
           player->rightHandType == PLAYER_MODELTYPE_RH_SHIELD;
}

extern "C" void Vr_Rumble(bool on) {
    if (on && SohVr::SessionRunning()) {
        SohVr::Runtime::Get().Vibrate(SohVr::HAND_LEFT, 0.5f, 0.06f);
        SohVr::Runtime::Get().Vibrate(SohVr::HAND_RIGHT, 0.5f, 0.06f);
    }
}

extern "C" bool Vr_HandGrabsAt(Vec3f* pos) {
    if (!Vr_IsWorldStereo()) {
        return false;
    }
    SohVr::Runtime& runtime = SohVr::Runtime::Get();
    for (int hand = 0; hand < SohVr::HAND_COUNT; hand++) {
        const SohVr::ControllerState& state = runtime.GetController(static_cast<SohVr::Hand>(hand));
        if (!state.gripHeld || !state.gripPose.valid) {
            continue;
        }
        SohVr::Vec3 palm =
            SohVr::ToWorld(state.gripPose.position, SohVr::sBuild.anchor, SohVr::sBuild.unitsPerMeter);
        // Drops sit on the floor; aim a little above their origin, at the model.
        float dx = palm.x - pos->x;
        float dy = palm.y - (pos->y + 6.0f);
        float dz = palm.z - pos->z;
        if (dx * dx + dy * dy + dz * dz < SohVr::kGrabRadius * SohVr::kGrabRadius) {
            runtime.Vibrate(static_cast<SohVr::Hand>(hand), 0.6f, 0.06f);
            return true;
        }
    }
    return false;
}
