#pragma once

namespace SohVr {

// Hands over to the other game without leaving the headset: asks the launcher to start it once this one is gone, and
// closes this one. key is the launcher's name for it ("oot" or "mm"). False, with nothing closed, if there's no
// launcher to ask.
bool SwitchToGame(const char* key);

} // namespace SohVr
