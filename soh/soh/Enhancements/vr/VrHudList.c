#include "global.h"
#include "soh/Enhancements/vr/Vr.h"

static Gfx sWhiteBackground[8];

Gfx* Vr_WhiteBackgroundList(Gfx* overlay) {
    Gfx* gfx = sWhiteBackground;

    gDPPipeSync(gfx++);
    gDPSetScissor(gfx++, G_SC_NON_INTERLACE, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    gDPSetCycleType(gfx++, G_CYC_FILL);
    gDPSetRenderMode(gfx++, G_RM_NOOP, G_RM_NOOP2);
    gDPSetFillColor(gfx++, (GPACK_RGBA5551(255, 255, 255, 1) << 16) | GPACK_RGBA5551(255, 255, 255, 1));
    gDPFillRectangle(gfx++, 0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1);
    gDPPipeSync(gfx++);
    gSPBranchList(gfx++, overlay);
    return sWhiteBackground;
}
