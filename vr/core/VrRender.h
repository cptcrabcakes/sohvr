#pragma once

#include "include/z64math.h"

#include <cstdint>
#include <unordered_map>

union Gfx;
namespace Fast {
class Fast3dWindow;
}

namespace SohVr {

// Draws one frame to the headset and mirrors it to the window. Returns false when no VR session is running,
// in which case the caller draws the frame normally.
bool DrawFrame(Fast::Fast3dWindow* wnd, Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtxReplacements);
// The headset's refresh rate while a session is running, otherwise 0.
uint32_t GetRefreshRate();
// Called once per game tick when its display list is handed over for rendering.
void OnTickSubmitted();
void Shutdown();

} // namespace SohVr
