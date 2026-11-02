#pragma once

// Playing the ocarina by whistling, humming or singing into a microphone: the note heard presses that note's button,
// and how far off the note it is bends the ocarina's pitch, as the stick does, so the ocarina follows the voice.

#include <cstdint>

namespace SohVr {

// What the microphone hears, as of the latest analysis.
struct MicNote {
    uint16_t button = 0;     // the note's button (BTN_A, BTN_CDOWN, ...), or 0 while no note is heard
    float bend = 0.0f;       // semitones above (+) or below (-) the note
    uint32_t onset = 0;      // goes up each time a note starts, including the same note again
    bool listening = false;  // whether a microphone is open
};

// Opens the microphone while on, and closes it a few seconds after it goes off, so it isn't left listening between
// songs. Called every frame.
void OcarinaMic_Listen(bool on);
MicNote OcarinaMic_Current();

} // namespace SohVr
