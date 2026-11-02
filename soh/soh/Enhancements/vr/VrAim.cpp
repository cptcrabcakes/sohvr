#include "VrAim.h"
#include "VrGame.h"

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge/consolevariablebridge.h>

#include <algorithm>
#include <cmath>

extern "C" {
#include "soh/Enhancements/vr/Vr.h"
}

// Whether the bow and slingshot show where their shot will land, and the path it takes there.
#define CVAR_VR_SHOT_MARKER CVAR_SETTING("VR.ShotMarker")
#define CVAR_VR_SHOT_PATH CVAR_SETTING("VR.ShotPath")

namespace SohVr {

namespace {

// How EnArrow flies a shot (see EnArrow_Shoot and EnArrow_Fly): its speed, the ticks until it vanishes, falling
// from when its timer drops below the fall time, at the fall's gravity, never faster than its fastest fall, and
// moving its velocity times half the update rate each tick.
constexpr float kArrowSpeed = 150.0f;
constexpr float kSeedSpeed = 80.0f;
constexpr int kArrowTicks = 12;
constexpr int kSeedTicks = 15;
constexpr float kFallTimer = 7.2f;
constexpr float kFallGravity = -0.4f;
constexpr float kFastestFall = -150.0f;

constexpr int kRingSegments = 16;
// Vertex positions are whole numbers, so shapes are built at ten times their size and drawn at a tenth.
constexpr float kVertexScale = 10.0f;

Vec3f Cross(const Vec3f& a, const Vec3f& b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

Vec3f Unit(const Vec3f& v) {
    float length = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return length > 0.0001f ? Vec3f{ v.x / length, v.y / length, v.z / length } : Vec3f{ 0.0f, 1.0f, 0.0f };
}

// Plain color, see-through, both sides, on top of what's at the same depth.
void SetupFlat(Gfx*& xlu, Color_RGBA8 color) {
    gDPPipeSync(xlu++);
    gSPTexture(xlu++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(xlu++, G_LIGHTING | G_CULL_BOTH | G_TEXTURE_GEN | G_FOG);
    gDPSetCombineMode(xlu++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetRenderMode(xlu++, G_RM_ZB_XLU_SURF, G_RM_ZB_XLU_SURF2);
    gDPSetPrimColor(xlu++, 0, 0, color.r, color.g, color.b, color.a);
}

// Puts the current matrix at a point with the given axes, then a tenth the size, so whole-number vertices can be
// in tenths of a unit. Built from scratch, so the frames between ticks blend it as it is.
void PutBasis(GraphicsContext* gfxCtx, Gfx*& xlu, const Vec3f& at, const Vec3f& x, const Vec3f& y, const Vec3f& z) {
    MtxF basis;
    SkinMatrix_Clear(&basis);
    const Vec3f axes[3] = { x, y, z };
    for (int i = 0; i < 3; i++) {
        basis.mf[i][0] = axes[i].x / kVertexScale;
        basis.mf[i][1] = axes[i].y / kVertexScale;
        basis.mf[i][2] = axes[i].z / kVertexScale;
    }
    basis.mf[3][0] = at.x;
    basis.mf[3][1] = at.y;
    basis.mf[3][2] = at.z;
    Vr_PutWorldMatrix(&basis);
    gSPMatrix(xlu++, MATRIX_NEWMTX(gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
}

// A flat ring around a point, facing along normal (game units).
void DrawRing(PlayState* play, const Vec3f& center, const Vec3f& normal, float radius, float width,
              Color_RGBA8 color) {
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Vec3f up = Unit(normal);
    Vec3f side = Unit(Cross(fabsf(up.y) < 0.9f ? Vec3f{ 0.0f, 1.0f, 0.0f } : Vec3f{ 1.0f, 0.0f, 0.0f }, up));
    Vec3f across = Cross(up, side);
    Vtx* vertices = static_cast<Vtx*>(Graph_Alloc(gfxCtx, sizeof(Vtx) * kRingSegments * 2));
    for (int i = 0; i < kRingSegments; i++) {
        float angle = 2.0f * static_cast<float>(M_PI) * i / kRingSegments;
        float c = cosf(angle);
        float s = sinf(angle);
        for (int edge = 0; edge < 2; edge++) {
            float r = (edge == 0 ? radius - width * 0.5f : radius + width * 0.5f) * kVertexScale;
            Vtx& v = vertices[i * 2 + edge];
            v = {};
            v.v.ob[0] = static_cast<s16>(lroundf(r * c));
            v.v.ob[1] = 0;
            v.v.ob[2] = static_cast<s16>(lroundf(r * s));
            v.v.cn[3] = 255;
        }
    }
    Gfx*& xlu = gfxCtx->polyXlu.p;
    SetupFlat(xlu, color);
    PutBasis(gfxCtx, xlu, center, side, up, across);
    gSPVertex(xlu++, reinterpret_cast<uintptr_t>(vertices), kRingSegments * 2, 0);
    for (int i = 0; i < kRingSegments; i++) {
        int j = (i + 1) % kRingSegments;
        gSP2Triangles(xlu++, i * 2, i * 2 + 1, j * 2, 0, i * 2 + 1, j * 2 + 1, j * 2, 0);
    }
}

// A flat strip from one point to another, turned to face the eye (game units wide).
void DrawBeam(PlayState* play, const Vec3f& from, const Vec3f& to, float width, Color_RGBA8 color) {
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Vec3f along = { to.x - from.x, to.y - from.y, to.z - from.z };
    float length = sqrtf(along.x * along.x + along.y * along.y + along.z * along.z);
    if (length < 1.0f) {
        return;
    }
    Vec3f forward = Unit(along);
    Vec3f toEye = { play->view.eye.x - from.x, play->view.eye.y - from.y, play->view.eye.z - from.z };
    Vec3f side = Unit(Cross(forward, toEye));
    Vec3f facing = Cross(side, forward);
    Vtx* vertices = static_cast<Vtx*>(Graph_Alloc(gfxCtx, sizeof(Vtx) * 4));
    float half = width * 0.5f * kVertexScale;
    float far = std::min(length * kVertexScale, 32000.0f);
    const float corners[4][2] = { { -half, 0.0f }, { half, 0.0f }, { -half, far }, { half, far } };
    for (int i = 0; i < 4; i++) {
        vertices[i] = {};
        vertices[i].v.ob[0] = static_cast<s16>(lroundf(corners[i][0]));
        vertices[i].v.ob[1] = static_cast<s16>(lroundf(corners[i][1]));
        vertices[i].v.cn[3] = 255;
    }
    Gfx*& xlu = gfxCtx->polyXlu.p;
    SetupFlat(xlu, color);
    PutBasis(gfxCtx, xlu, from, side, forward, facing);
    gSPVertex(xlu++, reinterpret_cast<uintptr_t>(vertices), 4, 0);
    gSP2Triangles(xlu++, 0, 1, 2, 0, 1, 3, 2, 0);
}

// Where a shot leaving from origin along direction lands, as EnArrow flies it, and the points it passes each tick.
// False if it vanishes before hitting anything.
bool FlyShot(PlayState* play, Vec3f origin, Vec3f direction, bool seed, float power, Vec3f* landing, Vec3f* normal,
             Vec3f* path, int* pathCount) {
    Vec3f forward = Unit(direction);
    float speed = (seed ? kSeedSpeed : kArrowSpeed) * power;
    float horizontal = sqrtf(forward.x * forward.x + forward.z * forward.z);
    float speedXZ = speed * horizontal;
    Vec3f heading = horizontal > 0.0001f ? Vec3f{ forward.x / horizontal, 0.0f, forward.z / horizontal }
                                         : Vec3f{ 0.0f, 0.0f, 0.0f };
    float velocityY = speed * forward.y;
    float gravity = 0.0f;
    float step = R_UPDATE_RATE * 0.5f;
    int timer = seed ? kSeedTicks : kArrowTicks;
    Vec3f pos = origin;
    *pathCount = 0;
    while (--timer > 0) {
        if (timer < kFallTimer) {
            gravity = kFallGravity;
        }
        velocityY = std::max(velocityY + gravity, kFastestFall);
        Vec3f next = { pos.x + heading.x * speedXZ * step, pos.y + velocityY * step, pos.z + heading.z * speedXZ * step };
        Vec3f hit;
        CollisionPoly* poly = nullptr;
        s32 bgId = 0;
        if (BgCheck_ProjectileLineTest(&play->colCtx, &pos, &next, &hit, &poly, true, true, true, true, &bgId)) {
            *landing = hit;
            *normal = { COLPOLY_GET_NORMAL(poly->normal.x), COLPOLY_GET_NORMAL(poly->normal.y),
                        COLPOLY_GET_NORMAL(poly->normal.z) };
            return true;
        }
        pos = next;
        if (*pathCount < kSeedTicks) {
            path[(*pathCount)++] = pos;
        }
    }
    return false;
}

} // namespace

void Aim_DrawShotMarker(PlayState* play, Player* player) {
    bool marker = CVarGetInteger(CVAR_VR_SHOT_MARKER, 1);
    bool pathShown = CVarGetInteger(CVAR_VR_SHOT_PATH, 0);
    Vec3f origin;
    Vec3f direction;
    float power = 1.0f;
    if ((!marker && !pathShown) || !Game_DrawingShot() || !Game_ShotAim(player, &origin, &direction, &power)) {
        return;
    }
    bool seed = Player_HoldsSlingshot(player);
    Vec3f landing;
    Vec3f normal;
    Vec3f path[kSeedTicks];
    int pathCount = 0;
    bool lands = FlyShot(play, origin, direction, seed, power, &landing, &normal, path, &pathCount);
    if (pathShown) {
        for (int i = 0; i < pathCount; i++) {
            Vec3f along = (i + 1 < pathCount) ? Vec3f{ path[i + 1].x - path[i].x, path[i + 1].y - path[i].y,
                                                        path[i + 1].z - path[i].z }
                                              : direction;
            DrawRing(play, path[i], along, 1.2f, 0.8f, { 255, 255, 255, 110 });
        }
    }
    if (marker && lands) {
        // Raised off the surface a little, so it isn't lost in it.
        Vec3f up = Unit(normal);
        Vec3f at = { landing.x + up.x * 0.6f, landing.y + up.y * 0.6f, landing.z + up.z * 0.6f };
        DrawRing(play, at, up, 7.0f, 2.0f, { 255, 210, 80, 210 });
        DrawRing(play, at, up, 1.5f, 1.5f, { 255, 210, 80, 230 });
    }
}

} // namespace SohVr

extern "C" void Vr_DrawHookshotSight(PlayState* play, Vec3f* tip, Vec3f* hit, s32 hookable) {
    // A laser from the hookshot to where it would catch, and a ring there: green where it will hook, red where it
    // would only bounce off.
    Color_RGBA8 color = hookable ? Color_RGBA8{ 90, 255, 110, 150 } : Color_RGBA8{ 255, 70, 60, 150 };
    SohVr::DrawBeam(play, *tip, *hit, 0.5f, color);
    Vec3f toTip = { tip->x - hit->x, tip->y - hit->y, tip->z - hit->z };
    color.a = 230;
    SohVr::DrawRing(play, *hit, toTip, 2.5f, 1.0f, color);
}
