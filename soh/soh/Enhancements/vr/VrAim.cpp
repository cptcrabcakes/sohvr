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

// Plain color, see-through, both sides, on top of what's at the same depth, or on top of everything.
void SetupFlat(Gfx*& xlu, Color_RGBA8 color, bool onTop = false) {
    gDPPipeSync(xlu++);
    gSPTexture(xlu++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(xlu++, G_LIGHTING | G_CULL_BOTH | G_TEXTURE_GEN | G_FOG);
    gDPSetCombineMode(xlu++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    if (onTop) {
        gDPSetRenderMode(xlu++, G_RM_XLU_SURF, G_RM_XLU_SURF2);
    } else {
        gDPSetRenderMode(xlu++, G_RM_ZB_XLU_SURF, G_RM_ZB_XLU_SURF2);
    }
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

// A flat ring around a point, facing along normal (game units), drawn over everything if onTop.
void DrawRing(PlayState* play, const Vec3f& center, const Vec3f& normal, float radius, float width,
              Color_RGBA8 color, bool onTop = false) {
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
    SetupFlat(xlu, color, onTop);
    PutBasis(gfxCtx, xlu, center, side, up, across);
    gSPVertex(xlu++, reinterpret_cast<uintptr_t>(vertices), kRingSegments * 2, 0);
    for (int i = 0; i < kRingSegments; i++) {
        int j = (i + 1) % kRingSegments;
        gSP2Triangles(xlu++, i * 2, i * 2 + 1, j * 2, 0, i * 2 + 1, j * 2 + 1, j * 2, 0);
    }
}

// The Lens of Truth's window. Vertices are whole numbers, with the glass's edge this far out; the rest of the mask is
// a bowl around the player that reaches thirty times as far, past where the eyes can be from the glass (the near
// part of a plane alone isn't enough: looking across the glass, part of the view never meets its plane).
constexpr int kLensSegments = 32;
constexpr float kGlassVertexRadius = 1000.0f;
constexpr float kBowlVertexSize = 30000.0f;

void SetLensVertex(Vtx& v, float x, float y, float z) {
    v = {};
    v.v.ob[0] = static_cast<s16>(lroundf(x));
    v.v.ob[1] = static_cast<s16>(lroundf(y));
    v.v.ob[2] = static_cast<s16>(lroundf(z));
    v.v.cn[3] = 255;
}

// The current matrix at the glass, its Z along normal, with whole-number vertices in thousandths of its radius.
void PutLens(GraphicsContext* gfxCtx, Gfx*& xlu, const Vec3f& center, const Vec3f& normal, float radius) {
    Vec3f z = Unit(normal);
    Vec3f x = Unit(Cross(fabsf(z.y) < 0.9f ? Vec3f{ 0.0f, 1.0f, 0.0f } : Vec3f{ 1.0f, 0.0f, 0.0f }, z));
    Vec3f y = Cross(z, x);
    float scale = radius * kVertexScale / kGlassVertexRadius;
    PutBasis(gfxCtx, xlu, center, { x.x * scale, x.y * scale, x.z * scale }, { y.x * scale, y.y * scale, y.z * scale },
             { z.x * scale, z.y * scale, z.z * scale });
}

void DrawLensDisc(GraphicsContext* gfxCtx, Gfx*& xlu) {
    Vtx* vertices = static_cast<Vtx*>(Graph_Alloc(gfxCtx, sizeof(Vtx) * (kLensSegments + 1)));
    SetLensVertex(vertices[0], 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < kLensSegments; i++) {
        float angle = 2.0f * static_cast<float>(M_PI) * i / kLensSegments;
        SetLensVertex(vertices[i + 1], kGlassVertexRadius * cosf(angle), kGlassVertexRadius * sinf(angle), 0.0f);
    }
    // Loaded in runs that all share the middle.
    for (int i = 0; i < kLensSegments; i++) {
        Vtx* run = static_cast<Vtx*>(Graph_Alloc(gfxCtx, sizeof(Vtx) * 3));
        run[0] = vertices[0];
        run[1] = vertices[i + 1];
        run[2] = vertices[(i + 1) % kLensSegments + 1];
        gSPVertex(xlu++, reinterpret_cast<uintptr_t>(run), 3, 0);
        gSP1Triangle(xlu++, 0, 1, 2, 0);
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

// Where a segment first enters an upright cylinder standing on base, as a share of the way along it. One that starts
// inside enters at once.
bool SegmentHitsCylinder(const Vec3f& a, const Vec3f& b, const Vec3f& base, float radius, float height, float* t) {
    float dx = b.x - a.x;
    float dz = b.z - a.z;
    float fx = a.x - base.x;
    float fz = a.z - base.z;
    float qa = dx * dx + dz * dz;
    float qb = 2.0f * (fx * dx + fz * dz);
    float qc = fx * fx + fz * fz - radius * radius;
    float enter = 0.0f;
    if (qc > 0.0f) {
        float discriminant = qb * qb - 4.0f * qa * qc;
        if (qa < 0.0001f || discriminant < 0.0f) {
            return false;
        }
        enter = (-qb - sqrtf(discriminant)) / (2.0f * qa);
        if (enter < 0.0f || enter > 1.0f) {
            return false;
        }
    }
    float y = a.y + (b.y - a.y) * enter;
    if (y < base.y || y > base.y + height) {
        return false;
    }
    *t = enter;
    return true;
}

// The first enemy body a segment passes into, as a share of the way along it, by each enemy's own collision
// cylinder. Arrows and seeds stop at them, so the marker goes there instead of on the ground behind.
bool SegmentHitsEnemy(PlayState* play, const Vec3f& a, const Vec3f& b, float* t) {
    bool hit = false;
    for (int category : { ACTORCAT_ENEMY, ACTORCAT_BOSS }) {
        for (Actor* actor = play->actorCtx.actorLists[category].head; actor != nullptr; actor = actor->next) {
            const CollisionCheckInfo& info = actor->colChkInfo;
            if (actor->update == nullptr || info.cylRadius <= 0 || info.cylHeight <= 0) {
                continue;
            }
            Vec3f base = { actor->world.pos.x, actor->world.pos.y + info.cylYShift, actor->world.pos.z };
            float along;
            if (SegmentHitsCylinder(a, b, base, info.cylRadius, info.cylHeight, &along) && (!hit || along < *t)) {
                *t = along;
                hit = true;
            }
        }
    }
    return hit;
}

// Where a shot leaving from origin along direction lands, as EnArrow flies it, on a wall, the ground or an enemy, and
// the points it passes each tick. False if it vanishes before hitting anything.
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
        bool wall = BgCheck_ProjectileLineTest(&play->colCtx, &pos, &next, &hit, &poly, true, true, true, true, &bgId);
        Vec3f end = wall ? hit : next;
        float along;
        if (SegmentHitsEnemy(play, pos, end, &along)) {
            // On the enemy, facing back the way the shot came.
            *landing = { pos.x + (end.x - pos.x) * along, pos.y + (end.y - pos.y) * along,
                         pos.z + (end.z - pos.z) * along };
            *normal = { pos.x - end.x, pos.y - end.y, pos.z - end.z };
            return true;
        }
        if (wall) {
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

void Aim_DrawArrow(PlayState* play, const Vec3f& tail, const Vec3f& tip, float width) {
    // The last tenth is the head.
    Vec3f neck = { tail.x + (tip.x - tail.x) * 0.9f, tail.y + (tip.y - tail.y) * 0.9f, tail.z + (tip.z - tail.z) * 0.9f };
    DrawBeam(play, tail, neck, width, { 150, 105, 60, 255 });
    DrawBeam(play, neck, tip, width * 1.8f, { 200, 200, 210, 255 });
}

void Aim_DrawLensMask(PlayState* play, const Vec3f& center, const Vec3f& normal, float radius, const Vec3f& eyes,
                      bool showInside) {
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Gfx*& xlu = gfxCtx->polyXlu.p;
    // The mask's Z comes toward the eyes, so the bowl closes around them.
    Vec3f toEyes = { eyes.x - center.x, eyes.y - center.y, eyes.z - center.z };
    bool facing = toEyes.x * normal.x + toEyes.y * normal.y + toEyes.z * normal.z >= 0.0f;
    Vec3f z = facing ? normal : Vec3f{ -normal.x, -normal.y, -normal.z };

    gDPPipeSync(xlu++);
    gSPTexture(xlu++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
    gSPClearGeometryMode(xlu++, G_LIGHTING | G_CULL_BOTH | G_TEXTURE_GEN | G_FOG);
    gDPSetCombineMode(xlu++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    // Nothing to see, only the nearest depth written, over whatever was there.
    gDPSetOtherMode(xlu++,
                    G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE |
                        G_TD_CLAMP | G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
                    G_AC_NONE | G_ZS_PRIM | Z_UPD | G_RM_CLD_SURF | G_RM_CLD_SURF2);
    gDPSetPrimColor(xlu++, 0, 0, 0, 0, 0, 0);
    gDPSetPrimDepth(xlu++, 0, 0);
    PutLens(gfxCtx, xlu, center, z, radius);

    if (showInside) {
        // Around the glass, out to the bowl's rim, back past the eyes, and closed behind them, in runs of seven
        // vertices.
        Vtx* vertices = static_cast<Vtx*>(Graph_Alloc(gfxCtx, sizeof(Vtx) * kLensSegments * 7));
        for (int i = 0; i < kLensSegments; i++) {
            float angle0 = 2.0f * static_cast<float>(M_PI) * i / kLensSegments;
            float angle1 = 2.0f * static_cast<float>(M_PI) * (i + 1) / kLensSegments;
            float c0 = cosf(angle0);
            float s0 = sinf(angle0);
            float c1 = cosf(angle1);
            float s1 = sinf(angle1);
            Vtx* run = vertices + i * 7;
            SetLensVertex(run[0], kGlassVertexRadius * c0, kGlassVertexRadius * s0, 0.0f);
            SetLensVertex(run[1], kGlassVertexRadius * c1, kGlassVertexRadius * s1, 0.0f);
            SetLensVertex(run[2], kBowlVertexSize * c0, kBowlVertexSize * s0, 0.0f);
            SetLensVertex(run[3], kBowlVertexSize * c1, kBowlVertexSize * s1, 0.0f);
            SetLensVertex(run[4], kBowlVertexSize * c0, kBowlVertexSize * s0, kBowlVertexSize);
            SetLensVertex(run[5], kBowlVertexSize * c1, kBowlVertexSize * s1, kBowlVertexSize);
            SetLensVertex(run[6], 0.0f, 0.0f, kBowlVertexSize);
            gSPVertex(xlu++, reinterpret_cast<uintptr_t>(run), 7, 0);
            gSP2Triangles(xlu++, 0, 2, 1, 0, 1, 2, 3, 0);
            gSP2Triangles(xlu++, 2, 4, 3, 0, 3, 4, 5, 0);
            gSP1Triangle(xlu++, 4, 6, 5, 0);
        }
    } else {
        DrawLensDisc(gfxCtx, xlu);
    }
    gDPPipeSync(xlu++);
    gDPSetDepthSource(xlu++, G_ZS_PIXEL);
}

void Aim_DrawLensGlass(PlayState* play, const Vec3f& center, const Vec3f& normal, float radius, Color_RGBA8 color) {
    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Gfx*& xlu = gfxCtx->polyXlu.p;
    SetupFlat(xlu, color);
    PutLens(gfxCtx, xlu, center, normal, radius);
    DrawLensDisc(gfxCtx, xlu);
}

void Aim_DrawRing(PlayState* play, const Vec3f& center, const Vec3f& normal, float radius, float width,
                  Color_RGBA8 color) {
    DrawRing(play, center, normal, radius, width, color);
}

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
        // Raised off the surface a little, and drawn over everything, so nothing in the way hides it: not the
        // enemy it's on, nor anything between the eyes and where the shot, leaving from the hand, will land.
        Vec3f up = Unit(normal);
        Vec3f at = { landing.x + up.x * 0.6f, landing.y + up.y * 0.6f, landing.z + up.z * 0.6f };
        DrawRing(play, at, up, 7.0f, 2.0f, { 255, 210, 80, 210 }, true);
        DrawRing(play, at, up, 1.5f, 1.5f, { 255, 210, 80, 230 }, true);
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
