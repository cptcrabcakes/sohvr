#include "VrGame.h"
#include "VrCvars.h"
#include "VrItems.h"
#include "VrOcarinaMic.h"
#include "VrRuntime.h"

#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "global.h"
#include "2s2h/Enhancements/vr/Vr.h"
extern PlayState* gPlayState;
}

#define CVAR_VR_ENABLED CVAR_SETTING("VR.Enabled")
#define CVAR_VR_PLAYER_HEIGHT CVAR_SETTING("VR.PlayerHeight") // centimeters
#define CVAR_VR_WORLD_SCALE CVAR_SETTING("VR.WorldScale")     // percent: higher makes the world look bigger
#define CVAR_VR_SNAP_TURN CVAR_SETTING("VR.SnapTurn")         // degrees; 0 turns smoothly
#define CVAR_VR_SEATED CVAR_SETTING("VR.Seated")
// Link holds his sword in his left hand and his shield in his right; this puts the sword on the right controller.
#define CVAR_VR_SWORD_IN_RIGHT_HAND CVAR_SETTING("VR.SwordInRightHand")
// Whistling, humming or singing plays the ocarina's notes.
#define CVAR_VR_MIC_OCARINA CVAR_SETTING("VR.MicOcarina")
// Blowing into the microphone blows a Deku scrub's bubble.
#define CVAR_VR_DEKU_BLOW CVAR_SETTING("VR.DekuBlow")
// Fine turns (degrees) of each hand model on its controller, about the grip's X, Y and Z.
#define CVAR_VR_HAND_TUNE(hand, axis) CVAR_SETTING("VR.Hand" hand ".Tune" axis)

namespace SohVr {

namespace {

// Each form's height in game units (Player_GetHeight's, standing and off the horse), which is mapped onto the
// player's own, so the world is as much bigger or smaller around you as it is around that form: towering over a Deku
// scrub, small around a Goron, and far below the Fierce Deity.
float FormHeight(Player* player) {
    switch (player->transformation) {
        case PLAYER_FORM_FIERCE_DEITY:
            return 124.0f;
        case PLAYER_FORM_GORON:
            return 80.0f;
        case PLAYER_FORM_ZORA:
            return 68.0f;
        case PLAYER_FORM_DEKU:
            return 36.0f;
        default:
            return 44.0f;
    }
}

constexpr float kEyeHeightRatio = 0.93f; // eyes against height, for a standing player
constexpr float kNearPlane = 4.0f;
constexpr float kStickDeadzone = 0.15f;
constexpr float kStickRange = 80.0f;
constexpr float kSmoothTurnSpeed = 2.5f;   // radians a second at full stick
constexpr float kWarpDistance = 200.0f;     // game units in one tick: a teleport, not a step
constexpr float kCullFovMargin = 10.0f;     // degrees added to the game camera's view, for culling
// The HUD's panel: how far ahead and below the eyes (meters), how wide, and how far the head can turn from it
// (radians) before it comes around to where the player looks.
constexpr float kHudDistance = 1.1f;
constexpr float kHudDrop = 0.35f;
constexpr float kHudWidth = 1.0f;
constexpr float kHudFollowAngle = 0.9f;
constexpr float kTicksPerSecond = 20.0f;
// A blade is swung once its tip passes kSwingStartSpeed (meters a second) with the hand behind it passing
// kSwingMinHandSpeed, and has kept that up for kSwingMinTravel meters. It stops below kSwingStopSpeed or when it turns
// back, and then rests kSwingCooldownTicks. Swung at kHeavySwingSpeed or more, it hits like a jump attack.
constexpr float kSwingStartSpeed = 2.5f;
constexpr float kSwingMinHandSpeed = 1.2f;
constexpr float kSwingMinTravel = 0.3f;
constexpr float kSwingStopSpeed = 1.0f;
constexpr float kHeavySwingSpeed = 7.0f;
constexpr int kSwingCooldownTicks = 8;
// A Goron's or Zora's punch lands once the fist moves at kPunchSpeed (meters a second), and hits hard at
// kHeavyPunchSpeed.
constexpr float kPunchSpeed = 2.5f;
constexpr float kHeavyPunchSpeed = 5.0f;

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

void AxisAngle(float x, float y, float z, float degrees, float out[4]) {
    float half = DEG_TO_RAD(degrees) * 0.5f;
    float s = sinf(half);
    out[0] = x * s;
    out[1] = y * s;
    out[2] = z * s;
    out[3] = cosf(half);
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

// Link's hands; the left one holds his sword, the right one his shield, bow and hookshot, in every form.
enum LinkHand { LINK_LEFT, LINK_RIGHT };

// In the headset the grip's X and Y axes come out half a turn about Z from the frame the hands are written for, so
// everything placed on a hand goes through this first. The same as in Ocarina of Time.
constexpr float kGripFix[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
// Every form's hand models share Ocarina of Time's child Link's axes: the left hand's sword runs along +X out of the
// thumb side, with the fist along +Y toward the knuckles, and the shield lies on the back of the right hand. That's
// a quarter turn about Y onto the grip.
constexpr float kHandAlignment[4] = { 0.0f, 0.7071068f, 0.0f, 0.7071068f };

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

// The grip pose turned into the frame the hand and wrist placements are written for. See kGripFix.
Pose GripFrame(const Pose& grip) {
    return Attach(grip, { 0.0f, 0.0f, 0.0f }, kGripFix);
}

constexpr float kNoRotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
// OpenXR's grip space: -Z runs through the fist from little finger to thumb, +Y points out over the knuckles,
// and +X points left on both hands (out of the back of the left hand, into the palm of the right). Watch faces on the
// back of each wrist, read along the forearm with the little finger side up, as in Ocarina of Time.
constexpr float kLeftWatchRotation[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
constexpr float kRightWatchRotation[4] = { 0.5f, -0.5f, -0.5f, 0.5f };
constexpr Vec3 kLeftWatchOffset = { 0.035f, -0.07f, 0.0f };
constexpr Vec3 kRightWatchOffset = { -0.035f, -0.07f, 0.0f };

// HUD pieces on the N64's 320x240 screen; the HUD texture is that screen scaled up.
struct ScreenRect {
    int x, y, w, h;
};
// The three-day clock across the bottom middle: the sun and moon on their arc, the hour, and the day (or the final
// hours' countdown), which is the left wrist's watch face.
constexpr ScreenRect kClockRect = { 96, 166, 128, 74 };
constexpr ScreenRect kHeartsRect = { 16, 8, 134, 48 };  // hearts and the magic bar
constexpr ScreenRect kRupeesRect = { 16, 176, 74, 52 }; // rupees and small keys
// The A, B and C buttons in the top right, whose icons say what A does right now and which item each trigger holds
// (C-left on the left, C-right on the right) and which mask is on X (C-down).
constexpr ScreenRect kButtonsRect = { 150, 0, 160, 64 };

// Where the fist is in each form's hand model (model units), which goes on the controller's grip point. A Goron's
// fists are much bigger and a Deku scrub's hands much smaller than Link's own. Estimates, not yet checked in the
// headset.
float HandFistReach(Player* player) {
    switch (player->transformation) {
        case PLAYER_FORM_GORON:
            return 650.0f;
        case PLAYER_FORM_DEKU:
            return 300.0f;
        case PLAYER_FORM_ZORA:
            return 450.0f;
        default:
            return 400.0f;
    }
}

// What one game tick contributes to rendering.
struct TickState {
    Mtx* projection = nullptr;
    Mtx* viewing = nullptr;
    float zFar = 12800.0f;
    Vec3f anchor = {};
    float unitsPerMeter = 40.0f;
    bool stereo = false;
    bool fade = false; // a fade, transition or game over is drawing over the view
    float yaw = 0.0f;
    Gfx* overlayCall = nullptr;
    Gfx* overlay = nullptr;
    Gfx* masterEnd = nullptr;
    // The skybox's matrices this tick. They center the box on the game camera; each eye re-centers it on itself so
    // the sky is infinitely far away.
    static constexpr int kMaxSkyboxes = 4;
    Mtx* skybox[kMaxSkyboxes] = {};
    MtxF skyboxMatrix[kMaxSkyboxes] = {};
    int skyboxCount = 0;
};

TickState sBuild;  // the tick whose display list is being built
TickState sRender; // the tick whose frames are being rendered
Vec3f sRenderPrevAnchor = {};
bool sAwaitingWorldView = false;
bool sWorldViewCaptured = false;

// The play space maps onto the world by turning it by sYaw and putting sCenter (x, z meters) on Link.
float sYaw = 0.0f;
float sCenter[2] = {};
bool sNeedsRecenter = true;
float sSeatedLift = 0.0f;
bool sSeatMeasured = false;
PlayState* sLastPlay = nullptr;
s16 sLastScene = -1;
u8 sLastForm = 0xFF;

u16 sLatchedButtons = 0;
bool sTurnLatched = false;
bool sRecenterLatched = false;
// A trigger that chose from the item ring doesn't also use the item until it's let go.
bool sTriggerBlocked[HAND_COUNT] = {};

// The note the microphone heard last tick, and its onset (see Vr_OverridePads).
uint32_t sMicOnset = 0;
u16 sMicButton = 0;

// What each form's gestures hold down this frame (see UpdateFormGestures).
u16 sFormButtons = 0;
float sCrossedFor = 0.0f; // seconds the arms have been crossed
// A spin by turning around: how far the head has turned one way (degrees), and how long that took (seconds).
float sLastHeadYaw = 0.0f;
bool sHasLastHeadYaw = false;
float sSpinTurned = 0.0f;
float sSpinTurnTime = 0.0f;
// Turning this far one way within this long is a spin. Slower than half a degree a frame counts as stopping.
constexpr float kPhysicalSpinDegrees = 300.0f;
constexpr float kPhysicalSpinSeconds = 1.2f;
// Crossed arms count once they've been held this long (seconds), so a punch passing across doesn't.
constexpr float kCrossedHold = 0.15f;

Gfx sOverlayCallSaved;
Gfx* sOverlayCutAt = nullptr;
bool sOverlayCut = false;

Pose sHudPose;

// Each hand's grip position over the last few frames, for its speed.
constexpr int kHandHistory = 5;
float sHandHistory[HAND_COUNT][kHandHistory][3] = {};
int sHandHistoryCount[HAND_COUNT] = {};

// A blade swung by hand (see Vr_SwordSwing).
bool sSwingActive = false;
bool sSwingHit = false;
int sSwingCooldown = 0;
float sSwingTravel = 0.0f; // meters the tip has carried its speed so far, while a swing builds up

// Each fist's punch (see Vr_FistSwing): the swept edge of its hitbox last tick, and whether it has landed.
WeaponInfo sFist[2] = {};
bool sFistHit[2] = {};

bool SessionRunning() {
    return Runtime::Get().IsSessionRunning();
}

Vec3 HeadForward() {
    return Rotate(Runtime::Get().GetHead().orientation, { 0.0f, 0.0f, -1.0f });
}

Vec3 ToWorld(const float position[3], const Vec3f& anchor, float unitsPerMeter) {
    Vec3 local = { (position[0] - sCenter[0]) * unitsPerMeter, (position[1] + sSeatedLift) * unitsPerMeter,
                   (position[2] - sCenter[1]) * unitsPerMeter };
    Vec3 turned = RotateYaw(sYaw, local);
    return { anchor.x + turned.x, anchor.y + turned.y, anchor.z + turned.z };
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
    sYaw = linkYaw + M_PIf - headYaw;
    sCenter[0] = head.position[0];
    sCenter[1] = head.position[2];
    sNeedsRecenter = false;
    sHudPose.valid = false;
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

// Seated, the head and hands are lifted so the head is where a standing player's eyes would be. Measured when seated
// mode comes on and at every recenter.
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
}

// The world is shown in stereo whenever Link is in it, cutscenes included. The title screen, file select and the
// pause menu stay on the virtual screen.
bool ShowsWorld(PlayState* play) {
    return play != nullptr && GET_PLAYER(play) != nullptr && gSaveContext.gameMode == GAMEMODE_NORMAL &&
           play->pauseCtx.state == 0;
}

// Points the game camera along the head so culling, billboards and sound follow where the player looks.
void AimGameCamera(PlayState* play) {
    const Runtime& runtime = Runtime::Get();
    const Pose& head = runtime.GetHead();
    if (!head.valid) {
        return;
    }
    Vec3 eye = ToWorld(head.position, sBuild.anchor, sBuild.unitsPerMeter);
    Vec3 forward = RotateYaw(sYaw, HeadForward());
    // The sky and billboards turn with the game camera, so it stays upright instead of rolling with the head.
    // Straight up or down, world up is undefined as a camera up, so the head's own is used there.
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    if (fabsf(forward.y) > 0.98f) {
        up = RotateYaw(sYaw, Rotate(head.orientation, { 0.0f, 1.0f, 0.0f }));
    }
    play->view.eye = { eye.x, eye.y, eye.z };
    play->view.at = { eye.x + forward.x * 100.0f, eye.y + forward.y * 100.0f, eye.z + forward.z * 100.0f };
    play->view.up = { up.x, up.y, up.z };

    float halfFov = 0.0f;
    for (const EyeView& view : runtime.GetEyes()) {
        halfFov = std::max({ halfFov, fabsf(view.fov.up), fabsf(view.fov.down) });
    }
    play->view.fovy = std::clamp(RAD_TO_DEG(halfFov * 2.0f) + kCullFovMargin, 60.0f, 150.0f);
}

void CaptureTick(PlayState* play) {
    sBuild.stereo = false;
    sAwaitingWorldView = false;
    if (!SessionRunning() || !ShowsWorld(play)) {
        sNeedsRecenter = true;
        return;
    }
    Player* player = GET_PLAYER(play);
    if (play != sLastPlay || play->sceneId != sLastScene) {
        sNeedsRecenter = true;
        sLastPlay = play;
        sLastScene = play->sceneId;
    }
    if (player->transformation != sLastForm) {
        sLastForm = player->transformation;
        SPDLOG_INFO("[VR] Form {}: {:.0f} units tall", sLastForm, FormHeight(player));
    }
    if (sNeedsRecenter) {
        Recenter(player);
    }
    float heightMeters = std::max(CVarGetInteger(CVAR_VR_PLAYER_HEIGHT, 170) / 100.0f, 1.0f);
    int worldScale = std::clamp(CVarGetInteger(CVAR_VR_WORLD_SCALE, 100), 50, 200);
    sBuild.unitsPerMeter = FormHeight(player) / heightMeters * 100.0f / worldScale;
    sBuild.anchor = player->actor.world.pos;
    sBuild.yaw = sYaw;
    sBuild.fade = play->transitionMode != TRANS_MODE_OFF || play->transitionTrigger != TRANS_TRIGGER_OFF ||
                  play->gameOverCtx.state != GAMEOVER_INACTIVE;
    sBuild.stereo = true;
    sAwaitingWorldView = true;
    AimGameCamera(play);
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
    memset(&out, 0, sizeof(out));
    const Vec3 axes[3] = { right, up, back };
    for (int i = 0; i < 3; i++) {
        out.mf[0][i] = axes[i].x;
        out.mf[1][i] = axes[i].y;
        out.mf[2][i] = axes[i].z;
        out.mf[3][i] = -Dot(axes[i], eye);
    }
    out.mf[3][3] = 1.0f;
}

// The eye's two matrices. The renderer rounds every matrix it's given to 1/65536 (see Interpreter::GfxSpMatrix), and
// a view matrix multiplies world coordinates, in the thousands, by its rotation, so that rounding alone moved the eyes
// by millimeters from frame to frame, more the farther Link was from the world's middle. Distant scenery hid it, but
// anything held close shook. So the rotation goes in with the projection, where it only ever multiplies what's near
// the eye, and the view is left a plain translation, which rounding can't hurt. The renderer multiplies them back
// together into the same thing.
void BuildEyeMatrices(const Fov& fov, float zNear, float zFar, uint32_t width, uint32_t height, const Vec3& eye,
                      const float orientation[4], MtxF& projection, MtxF& viewing) {
    MtxF lens;
    MtxF turn;
    BuildProjection(fov, zNear, zFar, width, height, lens);
    BuildView({ 0.0f, 0.0f, 0.0f }, orientation, turn);
    MultiplyRows(turn, lens, projection);
    memset(&viewing, 0, sizeof(viewing));
    viewing.mf[0][0] = viewing.mf[1][1] = viewing.mf[2][2] = viewing.mf[3][3] = 1.0f;
    viewing.mf[3][0] = -eye.x;
    viewing.mf[3][1] = -eye.y;
    viewing.mf[3][2] = -eye.z;
}

// A panel standing upright ahead of where the player looks, below eye level, facing them.
Pose PanelInFront(float distance, float drop) {
    const Pose& head = Runtime::Get().GetHead();
    Vec3 forward = HeadForward();
    float length = sqrtf(forward.x * forward.x + forward.z * forward.z);
    Pose out;
    if (!head.valid || length < 0.001f) {
        return out;
    }
    float x = forward.x / length;
    float z = forward.z / length;
    float yaw = atan2f(-x, -z);
    out.orientation[1] = sinf(yaw * 0.5f);
    out.orientation[3] = cosf(yaw * 0.5f);
    out.position[0] = head.position[0] + x * distance;
    out.position[1] = head.position[1] - drop;
    out.position[2] = head.position[2] + z * distance;
    out.valid = true;
    return out;
}

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

Hand ControllerFor(LinkHand hand) {
    bool swapped = CVarGetInteger(CVAR_VR_SWORD_IN_RIGHT_HAND, 0) != 0;
    return ((hand == LINK_LEFT) != swapped) ? HAND_LEFT : HAND_RIGHT;
}

// Whether one of Link's hands is on the other side's controller, so it's drawn mirrored to look like that side's
// hand.
bool HandMirrored(LinkHand hand) {
    return (ControllerFor(hand) == HAND_LEFT) != (hand == LINK_LEFT);
}

// How a hand model sits on its controller: the alignment, then any fine tuning, turned about the grip's X, then Y,
// then Z axis.
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

// Where one of Link's hand models goes in the world: its fist on the controller, scaled like the rest of him.
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

    // The model's origin is the wrist; put its fist on the controller's grip point instead.
    float scale = player->actor.scale.x;
    Vec3 gripPoint = ToWorld(grip.position, sBuild.anchor, sBuild.unitsPerMeter);
    Vec3 fist = Rotate(handWorld, { 0.0f, HandFistReach(player) * scale, 0.0f });
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

// Poses a forearm so that its hand, drawn next with its usual offset from the forearm, lands on the controller.
// Moving the forearm rather than the hand keeps the wrist vertices, which are skinned to the forearm, attached.
void PlaceForearm(Player* player, LinkHand hand, s32 handLimb, Vec3f* pos, Vec3s* rot) {
    MtxF handTarget;
    if (!HandMatrix(player, hand, handTarget)) {
        return;
    }
    LodLimb* limb = (LodLimb*)Lib_SegmentedToVirtual(((void**)player->skelAnime.skeleton)[handLimb - 1]);
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

// In VR a blade's hitbox is the blade as it's drawn. The game reaches well past the tip (another 1200 units on top of
// a length that's already longer than the blade) and 1000 units to each side, so that with a camera behind Link a
// swing feels like it connects; in VR that hit things a meter beyond the blade. Lengths are where each
// hand-holding-it model's vertices end along the blade (+X in the hand model), measured from the game's own models,
// with 5% to spare; the band from low to high (Y) and halfWidth (Z) are as thick as the fist that holds it.
struct BladeShape {
    float length;
    float low;
    float high;
    float halfWidth;
};

void FillBladePoints(const BladeShape& shape, Vec3f* tips, Vec3f* bases) {
    float length = shape.length * 1.05f;
    float middle = (shape.low + shape.high) * 0.5f;
    // The game's pattern: a point down the middle, for the trail, then two quads that cross along the blade.
    if (tips != nullptr) {
        tips[0] = { length, middle, 0.0f };
        tips[1] = { length, shape.low, shape.halfWidth };
        tips[2] = { length, shape.high, -shape.halfWidth };
    }
    if (bases != nullptr) {
        bases[0] = { 0.0f, middle, 0.0f };
        bases[1] = { 0.0f, shape.high, -shape.halfWidth };
        bases[2] = { 0.0f, shape.low, shape.halfWidth };
    }
}

// Majora's Mask's blades: the Kokiri, Razor and Gilded Swords, the Great Fairy's Sword, a Deku stick (whose length is
// the game's), and the Fierce Deity's sword.
bool BladeShapeFor(Player* player, float gameLength, BladeShape& out) {
    if (player->transformation == PLAYER_FORM_FIERCE_DEITY) {
        out = { 5400.0f, 0.0f, 800.0f, 280.0f };
        return true;
    }
    switch (Player_GetMeleeWeaponHeld(player)) {
        case PLAYER_MELEEWEAPON_SWORD_KOKIRI:
            out = { 1727.0f, 0.0f, 500.0f, 250.0f };
            return true;
        case PLAYER_MELEEWEAPON_SWORD_RAZOR:
            out = { 1902.0f, 0.0f, 500.0f, 250.0f };
            return true;
        case PLAYER_MELEEWEAPON_SWORD_GILDED:
            out = { 3348.0f, 0.0f, 560.0f, 250.0f };
            return true;
        case PLAYER_MELEEWEAPON_SWORD_TWO_HANDED:
            out = { 5498.0f, 0.0f, 700.0f, 250.0f };
            return true;
        case PLAYER_MELEEWEAPON_DEKU_STICK:
            out = { gameLength, 0.0f, 500.0f, 250.0f };
            return true;
        default:
            return false;
    }
}

// Whether the game is swinging Link's weapon itself, in a spin attack, which carries it around him.
bool GameSpinning(Player* player) {
    return player->meleeWeaponState != PLAYER_MELEE_WEAPON_STATE_0 &&
           player->meleeWeaponAnimation >= PLAYER_MWA_SPIN_ATTACK_1H;
}

// Whether the arms are crossed over the chest: each hand on the other side of the head's middle, below the head
// and in front of the body. The guard gesture for the Deku scrub, Goron and Zora.
bool ArmsCrossed() {
    const Runtime& runtime = Runtime::Get();
    const Pose& head = runtime.GetHead();
    const Pose& left = runtime.GetController(HAND_LEFT).gripPose;
    const Pose& right = runtime.GetController(HAND_RIGHT).gripPose;
    if (!head.valid || !left.valid || !right.valid) {
        return false;
    }
    Vec3 forward = HeadForward();
    float flat = sqrtf(forward.x * forward.x + forward.z * forward.z);
    if (flat < 0.001f) {
        return false;
    }
    float fx = forward.x / flat;
    float fz = forward.z / flat;
    // To the right of where the head looks, flat on the floor: -z is ahead and +x is right in the play space.
    float rx = -fz;
    float rz = fx;
    auto placed = [&](const Pose& hand, float& side, float& drop, float& ahead) {
        float dx = hand.position[0] - head.position[0];
        float dz = hand.position[2] - head.position[2];
        side = dx * rx + dz * rz;
        ahead = dx * fx + dz * fz;
        drop = head.position[1] - hand.position[1];
    };
    float leftSide, leftDrop, leftAhead, rightSide, rightDrop, rightAhead;
    placed(left, leftSide, leftDrop, leftAhead);
    placed(right, rightSide, rightDrop, rightAhead);
    auto onChest = [](float drop, float ahead) { return drop > 0.1f && drop < 0.6f && ahead > -0.1f && ahead < 0.4f; };
    return leftSide > 0.03f && rightSide < -0.03f && onChest(leftDrop, leftAhead) && onChest(rightDrop, rightAhead);
}

// Each form's own gestures, as buttons: crossed arms guard as the Deku scrub, Goron and Zora (R), raising the Zora's
// electric barrier on land (R and B; in water R alone does); blowing into the microphone blows a Deku scrub's bubble
// (B held, and let go to shoot it, aimed by the head); and turning around for real does the Deku scrub's spin (A).
void UpdateFormGestures() {
    sFormButtons = 0;
    Runtime& runtime = Runtime::Get();
    float dt = runtime.GetRefreshRate() > 0 ? 1.0f / runtime.GetRefreshRate() : 0.0f;
    if (!Vr_IsWorldStereo() || Items_MenuOpen()) {
        sCrossedFor = 0.0f;
        sHasLastHeadYaw = false;
        return;
    }
    Player* player = GET_PLAYER(gPlayState);
    u8 form = player->transformation;
    bool swimming = (player->stateFlags1 & PLAYER_STATE1_8000000) != 0;

    bool guardingForm = form == PLAYER_FORM_DEKU || form == PLAYER_FORM_GORON || form == PLAYER_FORM_ZORA;
    sCrossedFor = (guardingForm && ArmsCrossed()) ? sCrossedFor + dt : 0.0f;
    if (sCrossedFor >= kCrossedHold) {
        sFormButtons |= BTN_R;
        if (form == PLAYER_FORM_ZORA && !swimming) {
            sFormButtons |= BTN_B;
        }
    }

    if (form == PLAYER_FORM_DEKU && CVarGetInteger(CVAR_VR_DEKU_BLOW, 1) && !Game_PlayingOcarina() &&
        OcarinaMic_Current().blowing) {
        sFormButtons |= BTN_B;
    }

    // A spin by turning: how far the head has turned one way, and how long it took.
    Vec3 forward = RotateYaw(sYaw, HeadForward());
    float yaw = atan2f(forward.x, forward.z) * (180.0f / M_PIf);
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
    if (fabsf(sSpinTurned) >= kPhysicalSpinDegrees && form == PLAYER_FORM_DEKU && !swimming &&
        (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND)) {
        sSpinTurned = 0.0f;
        sSpinTurnTime = 0.0f;
        sLatchedButtons |= BTN_A;
        runtime.Vibrate(HAND_LEFT, 0.5f, 0.1f);
        runtime.Vibrate(HAND_RIGHT, 0.5f, 0.1f);
        SPDLOG_INFO("[VR] Deku spin by turning around");
    }
}

void RegisterVr() {
    // Nothing to register yet: the decomp calls in directly (see Vr.h).
}

static RegisterShipInitFunc initFunc(RegisterVr, { CVAR_VR_ENABLED });

} // namespace

void Game_OnTickSubmitted() {
    sRenderPrevAnchor = sRender.anchor;
    sRender = sBuild;
    sRender.stereo = sBuild.stereo && sWorldViewCaptured;
    sWorldViewCaptured = false;
    sBuild.overlayCall = nullptr;
    sBuild.overlay = nullptr;
    sBuild.masterEnd = nullptr;
    sBuild.skyboxCount = 0;

    float dx = sRender.anchor.x - sRenderPrevAnchor.x;
    float dy = sRender.anchor.y - sRenderPrevAnchor.y;
    float dz = sRender.anchor.z - sRenderPrevAnchor.z;
    if (dx * dx + dy * dy + dz * dz > kWarpDistance * kWarpDistance) {
        sRenderPrevAnchor = sRender.anchor;
    }
}

void Game_OnFrame() {
    Runtime& runtime = Runtime::Get();
    const ControllerState& right = runtime.GetController(HAND_RIGHT);
    RecordHands();

    // Clicking the right stick recenters.
    if (right.stickClick && !sRecenterLatched) {
        sNeedsRecenter = true;
        UpdateSeating(true);
        runtime.RecenterScreen();
    }
    sRecenterLatched = right.stickClick;
    UpdateSeating(false);

    // The right stick pushed sideways turns, by the snap angle or smoothly; up and down are C-up and C-down.
    int snapDegrees = CVarGetInteger(CVAR_VR_SNAP_TURN, 30);
    float turn = fabsf(right.stick[0]) >= fabsf(right.stick[1]) ? right.stick[0] : 0.0f;
    if (snapDegrees > 0) {
        if (!sTurnLatched && fabsf(turn) > 0.7f) {
            Turn(-copysignf(DEG_TO_RAD(static_cast<float>(snapDegrees)), turn));
            sTurnLatched = true;
        } else if (fabsf(turn) < 0.3f) {
            sTurnLatched = false;
        }
    } else if (fabsf(turn) > kStickDeadzone && runtime.GetRefreshRate() > 0) {
        Turn(-turn * kSmoothTurnSpeed / runtime.GetRefreshRate());
    }

    // The item ring, while the world is shown.
    if (Vr_IsWorldStereo()) {
        Items_OnFrame();
    } else {
        Items_Close();
    }
    UpdateFormGestures();

    // The microphone listens while the ocarina is out, and while Link is a Deku scrub, who blows bubbles.
    bool deku = Vr_IsWorldStereo() && GET_PLAYER(gPlayState)->transformation == PLAYER_FORM_DEKU &&
                CVarGetInteger(CVAR_VR_DEKU_BLOW, 1);
    OcarinaMic_Listen((Game_PlayingOcarina() && CVarGetInteger(CVAR_VR_MIC_OCARINA, 1)) || deku);

    // The panel ahead, for the whole HUD while text or a timer is up, stays put while the player looks around, and
    // comes around in front of them when they've turned well away from it.
    Vec3 forward = HeadForward();
    if (sHudPose.valid) {
        float panelYaw = 2.0f * atan2f(sHudPose.orientation[1], sHudPose.orientation[3]);
        float lookYaw = atan2f(-forward.x, -forward.z);
        if (fabsf(remainderf(lookYaw - panelYaw, 2.0f * M_PIf)) > kHudFollowAngle) {
            sHudPose.valid = false;
        }
    }
    if (!sHudPose.valid) {
        sHudPose = PanelInFront(kHudDistance, kHudDrop);
    }
}

bool Game_RenderStereo() {
    return SessionRunning() && sRender.stereo && sRender.projection != nullptr && sRender.viewing != nullptr;
}

void Game_AddEyeMatrices(int eye, float t, uint32_t width, uint32_t height,
                         std::unordered_map<Mtx*, MtxF>& replacements) {
    const EyeView& view = Runtime::Get().GetEyes()[eye];
    Vec3f anchor = {
        sRenderPrevAnchor.x + (sRender.anchor.x - sRenderPrevAnchor.x) * t,
        sRenderPrevAnchor.y + (sRender.anchor.y - sRenderPrevAnchor.y) * t,
        sRenderPrevAnchor.z + (sRender.anchor.z - sRenderPrevAnchor.z) * t,
    };
    Vec3 eyeWorld = ToWorld(view.pose.position, anchor, sRender.unitsPerMeter);
    BuildEyeMatrices(view.fov, kNearPlane, sRender.zFar, width, height, eyeWorld, view.pose.orientation,
                     replacements[sRender.projection], replacements[sRender.viewing]);
    for (int i = 0; i < sRender.skyboxCount; i++) {
        MtxF centered = sRender.skyboxMatrix[i];
        centered.mf[3][0] = eyeWorld.x;
        centered.mf[3][1] = eyeWorld.y;
        centered.mf[3][2] = eyeWorld.z;
        replacements[sRender.skybox[i]] = centered;
    }
}

bool Game_SplitHud() {
    return Game_RenderStereo() && !sRender.fade && sRender.overlayCall != nullptr && sRender.overlay != nullptr &&
           sRender.masterEnd != nullptr;
}

void Game_CutOverlay(bool cut) {
    if (cut && !sOverlayCut && Game_SplitHud()) {
        // The master list ends where it would call the overlay, so the eyes get only the world.
        sOverlayCutAt = sRender.overlayCall;
        sOverlayCallSaved = *sOverlayCutAt;
        *sOverlayCutAt = *sRender.masterEnd;
        sOverlayCut = true;
    } else if (!cut && sOverlayCut) {
        *sOverlayCutAt = sOverlayCallSaved;
        sOverlayCut = false;
    }
}

std::string Game_Where() {
    return gPlayState != nullptr ? fmt::format("scene {}", static_cast<int>(gPlayState->sceneId)) : "not playing";
}

bool Game_StreamerView() {
    return false;
}

void Game_AddStreamerMatrices(float, uint32_t, uint32_t, std::unordered_map<Mtx*, MtxF>&) {
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
    if (gPlayState == nullptr) {
        return false;
    }
    u8 mode = gPlayState->msgCtx.msgMode;
    return mode >= MSGMODE_OCARINA_STARTING && mode <= MSGMODE_SCARECROW_SPAWN_DEMONSTRATION;
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

// Whether the whole HUD goes on the panel ahead: while text, a title card or the ocarina's staff is up, or a
// minigame's timer is running, whose pieces aren't on the wrists.
bool WholeHudAhead() {
    if (gPlayState == nullptr || gPlayState->msgCtx.msgMode != MSGMODE_NONE) {
        return true;
    }
    for (int timer = 0; timer < TIMER_ID_MAX; timer++) {
        if (gSaveContext.timerStates[timer] != TIMER_STATE_OFF) {
            return true;
        }
    }
    return false;
}

void Game_GetHudQuads(std::vector<HudQuad>& out) {
    if (!Game_SplitHud()) {
        return;
    }
    if (WholeHudAhead()) {
        if (sHudPose.valid) {
            out.push_back(MakeQuad(sHudPose, kHudWidth, { 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT }));
        }
        return;
    }
    const Runtime& runtime = Runtime::Get();
    const ControllerState& left = runtime.GetController(HAND_LEFT);
    const ControllerState& right = runtime.GetController(HAND_RIGHT);

    // The three-day clock is a watch on the left wrist, with the hearts and magic above it along the forearm and
    // the rupees and keys beside it.
    if (left.gripPose.valid) {
        Pose watch = Attach(GripFrame(left.gripPose), kLeftWatchOffset, kLeftWatchRotation);
        out.push_back(MakeQuad(watch, 0.09f, kClockRect));
        out.push_back(MakeQuad(Attach(watch, { 0.0f, 0.06f, 0.0f }, kNoRotation), 0.12f, kHeartsRect));
        out.push_back(MakeQuad(Attach(watch, { -0.075f, -0.005f, 0.0f }, kNoRotation), 0.055f, kRupeesRect));
    }
    // The buttons on the right wrist.
    if (right.gripPose.valid) {
        Pose watch = Attach(GripFrame(right.gripPose), kRightWatchOffset, kRightWatchRotation);
        out.push_back(MakeQuad(watch, 0.11f, kButtonsRect));
    }
}

} // namespace SohVr

extern "C" bool Vr_IsWorldStereo(void) {
    return SohVr::SessionRunning() && gPlayState != nullptr && SohVr::sBuild.stereo;
}

extern "C" void Vr_OnPlayDrawStart(PlayState* play) {
    SohVr::CaptureTick(play);
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

extern "C" void Vr_OnGraphBuilt(Gfx* overlayCall, Gfx* overlay, Gfx* masterEnd) {
    SohVr::sBuild.overlayCall = overlayCall;
    SohVr::sBuild.overlay = overlay;
    SohVr::sBuild.masterEnd = masterEnd;
}

extern "C" void Vr_OnSkyboxDrawn(Mtx* mtx, MtxF* matrix) {
    SohVr::TickState& tick = SohVr::sBuild;
    if (tick.skyboxCount < SohVr::TickState::kMaxSkyboxes) {
        tick.skybox[tick.skyboxCount] = mtx;
        tick.skyboxMatrix[tick.skyboxCount] = *matrix;
        tick.skyboxCount++;
    }
}

extern "C" bool Vr_GetInputDirYaw(s16* yaw) {
    if (!Vr_IsWorldStereo() || !SohVr::Runtime::Get().GetHead().valid) {
        return false;
    }
    SohVr::Vec3 forward = SohVr::RotateYaw(SohVr::sYaw, SohVr::HeadForward());
    *yaw = RAD_TO_BINANG(atan2f(forward.x, forward.z));
    return true;
}

static bool IsVrPlayer(Player* player) {
    return Vr_IsWorldStereo() && gPlayState != nullptr && player == GET_PLAYER(gPlayState);
}

// Curled up, a Goron is a ball around the eyes, so none of him is drawn.
extern "C" bool Vr_HidesLink(Player* player) {
    return IsVrPlayer(player) && ((player->stateFlags3 & PLAYER_STATE3_1000) ||
                                  (player->transformation == PLAYER_FORM_GORON &&
                                   (player->stateFlags1 & PLAYER_STATE1_400000)));
}

extern "C" bool Vr_DrawsOnlyHands(Player* player) {
    return IsVrPlayer(player);
}

extern "C" void Vr_PutWorldMatrix(MtxF* matrix) {
    // Built from scratch and then applied, so frame interpolation blends it as it is (see the same function for
    // Ocarina of Time).
    Matrix_Translate(0.0f, 0.0f, 0.0f, MTXMODE_NEW);
    Matrix_Mult(matrix, MTXMODE_APPLY);
}

extern "C" bool Vr_HandLimb(s32 limbIndex) {
    return limbIndex == PLAYER_LIMB_LEFT_FOREARM || limbIndex == PLAYER_LIMB_LEFT_HAND ||
           limbIndex == PLAYER_LIMB_RIGHT_FOREARM || limbIndex == PLAYER_LIMB_RIGHT_HAND;
}

extern "C" void Vr_OverridePlayerLimb(Player* player, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot) {
    // During a spin attack the sword goes where the game's spin takes it, around the player, so it sweeps everything
    // around them; the hands come back to the controllers after.
    if (SohVr::GameSpinning(player)) {
        if (!Vr_HandLimb(limbIndex)) {
            *dList = nullptr;
        }
        return;
    }
    switch (limbIndex) {
        case PLAYER_LIMB_LEFT_FOREARM:
            SohVr::PlaceForearm(player, SohVr::LINK_LEFT, PLAYER_LIMB_LEFT_HAND, pos, rot);
            *dList = nullptr;
            break;
        case PLAYER_LIMB_RIGHT_FOREARM:
            SohVr::PlaceForearm(player, SohVr::LINK_RIGHT, PLAYER_LIMB_RIGHT_HAND, pos, rot);
            *dList = nullptr;
            break;
        case PLAYER_LIMB_LEFT_HAND:
        case PLAYER_LIMB_RIGHT_HAND:
            break;
        default:
            *dList = nullptr;
            break;
    }
}

extern "C" bool Vr_LimbMirrored(Player* player, s32 limbIndex) {
    if (!IsVrPlayer(player)) {
        return false;
    }
    switch (limbIndex) {
        case PLAYER_LIMB_LEFT_FOREARM:
        case PLAYER_LIMB_LEFT_HAND:
            return SohVr::HandMirrored(SohVr::LINK_LEFT);
        case PLAYER_LIMB_RIGHT_FOREARM:
        case PLAYER_LIMB_RIGHT_HAND:
            return SohVr::HandMirrored(SohVr::LINK_RIGHT);
        default:
            return false;
    }
}

extern "C" bool Vr_BladePoints(Player* player, f32 gameLength, Vec3f* tips, Vec3f* bases) {
    SohVr::BladeShape shape;
    if (!IsVrPlayer(player) || !SohVr::BladeShapeFor(player, gameLength, shape)) {
        return false;
    }
    SohVr::FillBladePoints(shape, tips, bases);
    return true;
}

// A sword, the Fierce Deity's included, or a Deku stick in hand.
extern "C" bool Vr_HoldsBlade(Player* player) {
    if (!IsVrPlayer(player)) {
        return false;
    }
    PlayerMeleeWeapon weapon = Player_GetMeleeWeaponHeld(player);
    return (weapon >= PLAYER_MELEEWEAPON_SWORD_KOKIRI && weapon <= PLAYER_MELEEWEAPON_DEKU_STICK) ||
           player->transformation == PLAYER_FORM_FIERCE_DEITY;
}

extern "C" bool Vr_SwordSwing(Player* player, Vec3f* tip) {
    static Vec3f sLastTip;
    static bool sHasLastTip = false;
    static SohVr::Vec3 sLastMove = {};
    if (!IsVrPlayer(player)) {
        sHasLastTip = false;
        SohVr::sSwingActive = false;
        return false;
    }

    // The tip's movement relative to Link, so running or stepping doesn't count as swinging.
    Vec3f tipFromLink = { tip->x - player->actor.world.pos.x, tip->y - player->actor.world.pos.y,
                          tip->z - player->actor.world.pos.z };
    SohVr::Vec3 move = {};
    if (sHasLastTip) {
        move = { tipFromLink.x - sLastTip.x, tipFromLink.y - sLastTip.y, tipFromLink.z - sLastTip.z };
    }
    float travel = sqrtf(SohVr::Dot(move, move)) / SohVr::sBuild.unitsPerMeter; // meters this tick
    float speed = travel * SohVr::kTicksPerSecond;                               // meters per second
    bool turnedBack = SohVr::Dot(move, sLastMove) < 0.0f;
    SohVr::Hand swordHand = SohVr::ControllerFor(SohVr::LINK_LEFT);
    float handSpeed = SohVr::HandSpeed(swordHand);
    sLastTip = tipFromLink;
    sLastMove = move;
    sHasLastTip = true;

    // A hit from last tick ends the damage for the rest of the swing.
    for (ColliderQuad& quad : player->meleeWeaponQuads) {
        if (SohVr::sSwingActive && (quad.base.atFlags & AT_HIT)) {
            SohVr::sSwingHit = true;
            quad.base.atFlags &= ~AT_HIT;
            SohVr::Runtime::Get().Vibrate(swordHand, 0.9f, 0.08f);
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
    // During the game's own attacks its damage stays as it set it. A very hard swing hits as hard as a jump attack.
    if (live && player->meleeWeaponState == PLAYER_MELEE_WEAPON_STATE_0) {
        Player_SetVrSwingDamage(player, speed >= SohVr::kHeavySwingSpeed);
    }
    return live;
}

// A Goron's or Zora's fist, punched by hand. Called from the hand's limb, with its matrix current.
extern "C" void Vr_FistSwing(PlayState* play, Player* player, s32 hand) {
    if (!IsVrPlayer(player) ||
        (player->transformation != PLAYER_FORM_GORON && player->transformation != PLAYER_FORM_ZORA) ||
        player->meleeWeaponState != PLAYER_MELEE_WEAPON_STATE_0 || SohVr::GameSpinning(player)) {
        SohVr::sFist[hand].active = false;
        return;
    }
    // Across the knuckles, so the punch sweeps a band as wide as the fist.
    float reach = SohVr::HandFistReach(player);
    Vec3f knuckles[2] = { { -reach * 0.4f, reach * 1.2f, 0.0f }, { reach * 0.4f, reach * 1.2f, 0.0f } };
    Vec3f tip, base;
    Matrix_MultVec3f(&knuckles[0], &base);
    Matrix_MultVec3f(&knuckles[1], &tip);

    SohVr::Hand controller = SohVr::ControllerFor(hand == 0 ? SohVr::LINK_LEFT : SohVr::LINK_RIGHT);
    ColliderQuad* quad = &player->meleeWeaponQuads[hand];
    if (quad->base.atFlags & AT_HIT) {
        quad->base.atFlags &= ~AT_HIT;
        SohVr::sFistHit[hand] = true;
        SohVr::Runtime::Get().Vibrate(controller, 1.0f, 0.1f);
    }
    float speed = SohVr::HandSpeed(controller);
    if (speed < SohVr::kPunchSpeed) {
        // Let go of the last punch's hit once the fist has stopped.
        SohVr::sFistHit[hand] = false;
        func_80126440(play, NULL, &SohVr::sFist[hand], &tip, &base);
        return;
    }
    if (SohVr::sFistHit[hand]) {
        func_80126440(play, NULL, &SohVr::sFist[hand], &tip, &base);
        return;
    }
    bool heavy = speed >= SohVr::kHeavyPunchSpeed;
    bool goron = player->transformation == PLAYER_FORM_GORON;
    quad->elem.atDmgInfo.dmgFlags = goron ? DMG_GORON_PUNCH : DMG_ZORA_PUNCH;
    quad->elem.atDmgInfo.damage = goron ? (heavy ? 4 : 2) : (heavy ? 2 : 1);
    quad->elem.atElemFlags = ATELEM_ON | ATELEM_NEAREST;
    func_80126440(play, quad, &SohVr::sFist[hand], &tip, &base);
}

// The shield in Link's hand blocks wherever the hand holds it.
extern "C" bool Vr_HoldsShield(Player* player) {
    return IsVrPlayer(player) && player->rightHandType == PLAYER_MODELTYPE_RH_SHIELD;
}

extern "C" void Vr_DrawPlayerExtras(PlayState* play) {
    if (Vr_IsWorldStereo()) {
        SohVr::Items_Draw(play);
    }
}

namespace SohVr {
namespace {

// The ocarina's four C notes on the right stick, pushed up, down, left or right.
u16 OcarinaStickNotes(const float stick[2]) {
    if (fabsf(stick[1]) > 0.7f && fabsf(stick[1]) > fabsf(stick[0])) {
        return stick[1] > 0.0f ? BTN_CUP : BTN_CDOWN;
    }
    if (fabsf(stick[0]) > 0.7f && fabsf(stick[0]) > fabsf(stick[1])) {
        return stick[0] > 0.0f ? BTN_CRIGHT : BTN_CLEFT;
    }
    return 0;
}

// In the world: A and B are A and B. Each trigger uses the item its hand chose from the ring, on that side's C button,
// and X is C-down, where a chosen mask goes, to take it off or put it back on. Y opens the ring (see Items_OnFrame).
// The left grip is R: the shield, or another form's guard. The right stick pushed up is C-up, for Tatl. With the
// ocarina out X and Y play C-left and C-right, and the right stick its four C notes. The menu
// button is Start (not the stick click, which got pressed by accident while walking).
u16 WorldButtons() {
    const ControllerState& left = Runtime::Get().GetController(HAND_LEFT);
    const ControllerState& right = Runtime::Get().GetController(HAND_RIGHT);
    u16 buttons = 0;
    buttons |= right.primary ? BTN_A : 0;
    buttons |= right.secondary ? BTN_B : 0;
    bool menuOpen = Items_MenuOpen();
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        const ControllerState& state = Runtime::Get().GetController(static_cast<Hand>(hand));
        if (menuOpen) {
            sTriggerBlocked[hand] = true;
        } else if (!state.triggerHeld) {
            sTriggerBlocked[hand] = false;
        }
        if (state.triggerHeld && !sTriggerBlocked[hand]) {
            buttons |= hand == HAND_LEFT ? BTN_CLEFT : BTN_CRIGHT;
        }
    }
    if (Game_PlayingOcarina()) {
        buttons |= left.primary ? BTN_CLEFT : 0;
        buttons |= left.secondary ? BTN_CRIGHT : 0;
        buttons |= OcarinaStickNotes(right.stick);
    } else {
        buttons |= left.primary ? BTN_CDOWN : 0;
        if (right.stick[1] > 0.7f && right.stick[1] > fabsf(right.stick[0])) {
            buttons |= BTN_CUP;
        }
    }
    // Not while the left hand is at the face, where its grip is for masks.
    buttons |= (left.gripHeld && !menuOpen && !Items_HandAtFace(HAND_LEFT)) ? BTN_R : 0;
    buttons |= sFormButtons;
    buttons |= left.menu ? BTN_START : 0;
    return buttons;
}

// On the floating screen (the pause menu, title screen and file select) the controllers are a gamepad: A and B are
// A and B, X and Y are C-left and C-right, the right stick's up and down are C-up and C-down, the left trigger is Z,
// the right trigger R, and the menu button is Start.
u16 ScreenButtons() {
    const ControllerState& left = Runtime::Get().GetController(HAND_LEFT);
    const ControllerState& right = Runtime::Get().GetController(HAND_RIGHT);
    u16 buttons = 0;
    buttons |= right.primary ? BTN_A : 0;
    buttons |= right.secondary ? BTN_B : 0;
    buttons |= left.primary ? BTN_CLEFT : 0;
    buttons |= left.secondary ? BTN_CRIGHT : 0;
    if (fabsf(right.stick[1]) > 0.7f && fabsf(right.stick[1]) > fabsf(right.stick[0])) {
        buttons |= right.stick[1] > 0.0f ? BTN_CUP : BTN_CDOWN;
    }
    buttons |= left.triggerHeld ? BTN_Z : 0;
    buttons |= right.triggerHeld ? BTN_R : 0;
    buttons |= left.menu ? BTN_START : 0;
    return buttons;
}

} // namespace
} // namespace SohVr

extern "C" void Vr_OverridePads(OSContPad* pads) {
    if (!SohVr::SessionRunning()) {
        return;
    }
    using namespace SohVr;
    const ControllerState& left = Runtime::Get().GetController(HAND_LEFT);
    const ControllerState& right = Runtime::Get().GetController(HAND_RIGHT);
    float x = left.stick[0];
    float y = left.stick[1];
    if (x * x + y * y > kStickDeadzone * kStickDeadzone) {
        pads[0].stick_x = static_cast<int8_t>(lroundf(std::clamp(x, -1.0f, 1.0f) * kStickRange));
        pads[0].stick_y = static_cast<int8_t>(lroundf(std::clamp(y, -1.0f, 1.0f) * kStickRange));
    }
    u16 buttons = Vr_IsWorldStereo() ? WorldButtons() : ScreenButtons();
    // What was chosen from the item ring goes on its C button, and may press one.
    if (Vr_IsWorldStereo()) {
        buttons |= Items_TakeButtons();
    }
    pads[0].button |= buttons | sLatchedButtons;
    sLatchedButtons = 0;

    // A whistled, hummed or sung note presses its button, and how far off it is bends the ocarina as the stick's up
    // and down do, two semitones at most each way. The game only plays a note on a new press, so the same note
    // started again is let go for a tick first.
    if (Game_PlayingOcarina() && CVarGetInteger(CVAR_VR_MIC_OCARINA, 1)) {
        MicNote heard = OcarinaMic_Current();
        bool again = heard.onset != sMicOnset && heard.button == sMicButton;
        sMicOnset = heard.onset;
        if (heard.button != 0 && !again) {
            pads[0].button |= heard.button;
            pads[0].stick_y = static_cast<int8_t>(std::clamp(lroundf(heard.bend * 32.0f), -64L, 64L));
        }
        sMicButton = again ? 0 : heard.button;
    }
}

extern "C" bool Vr_FirstPersonAim(Vec3s* rot) {
    using namespace SohVr;
    if (!Vr_IsWorldStereo() || !Runtime::Get().GetHead().valid) {
        return false;
    }
    Vec3 forward = RotateYaw(sYaw, HeadForward());
    float flat = sqrtf(forward.x * forward.x + forward.z * forward.z);
    rot->y = RAD_TO_BINANG(atan2f(forward.x, forward.z));
    // Pitch is positive downward, as the game's projectiles take it.
    rot->x = std::clamp<s16>(RAD_TO_BINANG(atan2f(-forward.y, flat)), -0x36B0, 0x36B0);
    return true;
}
