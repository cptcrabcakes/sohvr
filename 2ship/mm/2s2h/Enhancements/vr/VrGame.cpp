#include "VrGame.h"
#include "VrCvars.h"
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

Gfx sOverlayCallSaved;
Gfx* sOverlayCutAt = nullptr;
bool sOverlayCut = false;

Pose sHudPose;

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

    // The HUD's panel stays put while the player looks around, and comes around in front of them when they've
    // turned well away from it.
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
    BuildProjection(view.fov, kNearPlane, sRender.zFar, width, height, replacements[sRender.projection]);
    BuildView(eyeWorld, view.pose.orientation, replacements[sRender.viewing]);
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

Gfx* Game_Overlay() {
    return sRender.overlay;
}

void Game_GetHudQuads(std::vector<HudQuad>& out) {
    if (!Game_SplitHud() || !sHudPose.valid) {
        return;
    }
    // For now the whole HUD, text boxes included, on one panel in front of the player.
    HudQuad quad;
    quad.pose = sHudPose;
    quad.width = kHudWidth;
    uint32_t scale = Runtime::Get().GetHudWidth() / SCREEN_WIDTH;
    quad.rect[0] = 0;
    quad.rect[1] = 0;
    quad.rect[2] = SCREEN_WIDTH * scale;
    quad.rect[3] = SCREEN_HEIGHT * scale;
    out.push_back(quad);
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

extern "C" bool Vr_HidesLink(Player* player) {
    return Vr_IsWorldStereo() && gPlayState != nullptr && player == GET_PLAYER(gPlayState);
}

// For now the controllers are a gamepad: the left stick walks where the head looks, A and B are A and B, X and Y are
// C-left and C-right, the right stick's up and down are C-up and C-down, the left trigger is Z, the right trigger
// R, and the left stick click or menu button is Start. Physical controls come next.
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
    buttons |= (left.menu || left.stickClick) ? BTN_START : 0;
    pads[0].button |= buttons | sLatchedButtons;
    sLatchedButtons = 0;
}
