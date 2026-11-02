#include "VrOcarinaMic.h"

#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include <libultraship/libultra/controller.h>

namespace SohVr {

namespace {

// The microphone is read at 48 kHz, and every 10 ms the last 1024 samples are analysed for their pitch with the YIN
// method (de Cheveigné and Kawahara, 2002): the lag at which the signal best repeats itself, between 4 kHz (a high
// whistle) and 68 Hz (a low voice).
constexpr int kRate = 48000;
constexpr int kWindow = 1024;
constexpr int kMinLag = 12;
constexpr int kMaxLag = 706;
constexpr int kHop = 480;
constexpr float kHopSeconds = static_cast<float>(kHop) / kRate;
constexpr int kHistory = 2048; // at least kWindow + kMaxLag
// Loudness is measured over the newest 10 ms only, so a short breath between two notes shows.
constexpr int kLevelWindow = 480;
// YIN's threshold on its normalized difference: below it, the lag counts as the period. Lower is stricter.
constexpr float kPeriodThreshold = 0.15f;
// A pitch counts once it's this sure (1 - the normalized difference at its lag).
constexpr float kMinConfidence = 0.8f;
// Quieter than this many times the background level, or than the floor, is silence (RMS, full scale 1).
constexpr float kGateOverNoise = 4.0f;
constexpr float kGateFloor = 0.01f;

// A note starts once it's been heard this long, a different note takes over after this long, and a note ends after
// this long without one (seconds). Dropping below the dip share of its loudest for the dip time and coming back
// starts the same note again, as a breath between two notes does.
constexpr float kNoteStart = 0.04f;
constexpr float kNoteSwitch = 0.04f;
constexpr float kNoteEnd = 0.06f;
constexpr float kDipShare = 0.35f;
constexpr float kDipTime = 0.01f;
constexpr float kRecoverShare = 0.6f;
// Further than this from every note (semitones), what's heard plays nothing.
constexpr float kMaxOffNote = 1.5f;
// The microphone closes this long after listening is turned off (seconds).
constexpr float kCloseAfter = 3.0f;
// Blowing into the microphone is loud and has no pitch: at least the blow floor and above the gate, unsure of any
// pitch, for the blow start time; it ends once that's stopped for the blow end time. A blow that goes on past the
// longest blow is taken for a new, louder background instead (seconds).
constexpr float kBlowFloor = 0.03f;
constexpr float kBlowMaxConfidence = 0.6f;
constexpr float kBlowStart = 0.08f;
constexpr float kBlowEnd = 0.12f;
constexpr float kLongestBlow = 4.0f;

// The ocarina's five notes, D4 F4 A4 B4 D5, as pitch classes (semitones above C). Whatever octave is whistled or sung
// in, a pitch is matched to its nearest class, and the two Ds by whether it's above the octave the other notes were
// last heard in.
struct NoteClass {
    float pitchClass;
    uint16_t button;
};
constexpr NoteClass kClasses[] = { { 2.0f, BTN_A }, { 5.0f, BTN_CDOWN }, { 9.0f, BTN_CRIGHT }, { 11.0f, BTN_CLEFT } };

// Which octave a pitch is in, counted from D, so that D4 to C#5 are one octave (MIDI numbers; a little below D
// still counts as D's octave).
int DOctave(float midi) {
    return static_cast<int>(std::floor((midi - 2.0f + 1.5f) / 12.0f));
}

struct Tracker {
    // Analysis, on the microphone's thread.
    std::array<float, kHistory> history{};
    int written = 0;
    int sinceAnalysis = 0;
    float noise = 0.02f; // background level, following the quietest moments
    std::array<float, kMaxLag + 1> difference{};

    // The note, also on the microphone's thread.
    uint16_t candidate = 0;
    float candidateFor = 0.0f;
    uint16_t note = 0;
    float silentFor = 0.0f;
    float peak = 0.0f;
    float dipFor = 0.0f;
    bool dipped = false;
    float bend = 0.0f;
    int lastOctave = INT_MIN; // the octave the last F, A or B was in

    void Reset() {
        candidate = 0;
        candidateFor = 0.0f;
        note = 0;
        silentFor = 0.0f;
        peak = 0.0f;
        dipped = false;
        dipFor = 0.0f;
        bend = 0.0f;
    }
};

Tracker sTracker;
std::atomic<uint16_t> sButton{ 0 };
std::atomic<float> sBend{ 0.0f };
std::atomic<uint32_t> sOnset{ 0 };
std::atomic<bool> sBlowing{ false };
float sBlowFor = 0.0f;  // seconds of blowing heard so far
float sQuietFor = 0.0f; // seconds since it was last heard

SDL_AudioDeviceID sDevice = 0;
bool sSdlAudioStarted = false;
std::chrono::steady_clock::time_point sLastWanted;

// The pitch of the last kWindow + kMaxLag samples as a MIDI number, or 0 when there's none to be sure of.
float DetectPitch(const float* x, float& confidence) {
    Tracker& t = sTracker;
    float* d = t.difference.data();
    d[0] = 0.0f;
    for (int lag = 1; lag <= kMaxLag; lag++) {
        float sum = 0.0f;
        for (int i = 0; i < kWindow; i++) {
            float delta = x[i] - x[i + lag];
            sum += delta * delta;
        }
        d[lag] = sum;
    }
    // Normalized by the running mean, so a short lag doesn't win just by being short.
    float running = 0.0f;
    for (int lag = 1; lag <= kMaxLag; lag++) {
        running += d[lag];
        d[lag] = running > 0.0f ? d[lag] * lag / running : 1.0f;
    }
    int found = -1;
    for (int lag = kMinLag; lag < kMaxLag; lag++) {
        if (d[lag] < kPeriodThreshold) {
            while (lag + 1 < kMaxLag && d[lag + 1] < d[lag]) {
                lag++;
            }
            found = lag;
            break;
        }
    }
    if (found < 0) {
        confidence = 0.0f;
        return 0.0f;
    }
    // Between samples, where the parabola through the minimum and its neighbours bottoms out.
    float before = d[found - 1];
    float at = d[found];
    float after = d[found + 1];
    float curve = before - 2.0f * at + after;
    float lag = static_cast<float>(found) + (curve > 0.0f ? 0.5f * (before - after) / curve : 0.0f);
    confidence = 1.0f - at;
    float frequency = kRate / lag;
    return 69.0f + 12.0f * std::log2(frequency / 440.0f);
}

// The note a pitch is closest to and how far off it is, or no button when it's off every note.
uint16_t MatchNote(float midi, float& bend) {
    float pitchClass = std::fmod(midi, 12.0f);
    uint16_t best = 0;
    float bestOff = kMaxOffNote;
    for (const NoteClass& note : kClasses) {
        float off = std::remainder(pitchClass - note.pitchClass, 12.0f);
        if (std::fabs(off) < std::fabs(bestOff) || best == 0) {
            if (std::fabs(off) <= kMaxOffNote) {
                best = note.button;
                bestOff = off;
            }
        }
    }
    if (best == 0) {
        return 0;
    }
    bend = bestOff;
    Tracker& t = sTracker;
    int octave = DOctave(midi);
    if (best != BTN_A) {
        t.lastOctave = octave;
        return best;
    }
    // A D above the octave the other notes were in is the high one. Until another note has been heard, a D is
    // the low one.
    if (t.lastOctave != INT_MIN && octave > t.lastOctave) {
        return BTN_CUP;
    }
    return BTN_A;
}

void UpdateNote(float midi, float confidence, float level) {
    Tracker& t = sTracker;
    float gate = std::max(kGateFloor, t.noise * kGateOverNoise);
    float bend = 0.0f;
    uint16_t heard = 0;
    if (level > gate && confidence >= kMinConfidence && midi > 0.0f) {
        heard = MatchNote(midi, bend);
    }

    auto startNote = [&](uint16_t button) {
        t.note = button;
        t.peak = level;
        t.dipped = false;
        t.dipFor = 0.0f;
        t.silentFor = 0.0f;
        t.bend = bend;
        sOnset.fetch_add(1);
    };

    if (t.note == 0) {
        if (heard != 0 && heard == t.candidate) {
            t.candidateFor += kHopSeconds;
        } else {
            t.candidate = heard;
            t.candidateFor = kHopSeconds;
        }
        if (heard != 0 && t.candidateFor >= kNoteStart) {
            startNote(heard);
        }
    } else if (heard == 0) {
        t.silentFor += kHopSeconds;
        if (t.silentFor >= kNoteEnd) {
            t.note = 0;
            t.candidate = 0;
        }
    } else {
        // A moment's silence before the same note again is a breath between two of them.
        bool breath = t.silentFor >= kDipTime;
        t.silentFor = 0.0f;
        if (heard == t.note && breath) {
            startNote(heard);
        } else if (heard != t.note) {
            if (heard == t.candidate) {
                t.candidateFor += kHopSeconds;
            } else {
                t.candidate = heard;
                t.candidateFor = kHopSeconds;
            }
            if (t.candidateFor >= kNoteSwitch) {
                startNote(heard);
            }
        } else {
            t.candidate = heard;
            // Smoothed, so the pitch follows the voice without trembling.
            t.bend += (bend - t.bend) * 0.3f;
            t.peak = std::max(t.peak * 0.995f, level);
            if (level < t.peak * kDipShare) {
                t.dipFor += kHopSeconds;
                t.dipped = t.dipped || t.dipFor >= kDipTime;
            } else {
                t.dipFor = 0.0f;
                if (t.dipped && level > t.peak * kRecoverShare) {
                    startNote(heard);
                }
            }
        }
    }
    sButton.store(t.note);
    sBend.store(t.note != 0 ? t.bend : 0.0f);
}

void Analyse() {
    Tracker& t = sTracker;
    std::array<float, kWindow + kMaxLag> x;
    int start = t.written - static_cast<int>(x.size());
    for (size_t i = 0; i < x.size(); i++) {
        x[i] = t.history[(start + static_cast<int>(i) + kHistory * 4) % kHistory];
    }
    float energy = 0.0f;
    for (int i = kMaxLag + kWindow - kLevelWindow; i < kMaxLag + kWindow; i++) {
        energy += x[i] * x[i];
    }
    float level = std::sqrt(energy / kLevelWindow);
    float confidence = 0.0f;
    float midi = DetectPitch(x.data(), confidence);

    float gate = std::max(kGateFloor, t.noise * kGateOverNoise);
    bool blow = level > std::max(gate, kBlowFloor) && confidence < kBlowMaxConfidence;
    if (blow) {
        sBlowFor += kHopSeconds;
        sQuietFor = 0.0f;
    } else {
        sQuietFor += kHopSeconds;
        if (sQuietFor >= kBlowEnd) {
            sBlowFor = 0.0f;
        }
    }
    bool blowing = sBlowFor >= kBlowStart && sBlowFor < kLongestBlow;
    sBlowing.store(blowing);

    // The background drops at once and rises slowly, and only while no pitch is heard, so it follows the room's
    // quietest moments and never a held note. Not while the player blows either, which would soon count as the room.
    if (level < t.noise) {
        t.noise = level;
    } else if (confidence < kMinConfidence && !blowing) {
        t.noise += (level - t.noise) * 0.002f;
    }
    UpdateNote(midi, confidence, level);
}

void SDLCALL OnSamples(void*, Uint8* stream, int length) {
    const float* samples = reinterpret_cast<const float*>(stream);
    int count = length / static_cast<int>(sizeof(float));
    Tracker& t = sTracker;
    for (int i = 0; i < count; i++) {
        t.history[t.written % kHistory] = samples[i];
        t.written++;
        if (++t.sinceAnalysis >= kHop && t.written >= kWindow + kMaxLag) {
            t.sinceAnalysis = 0;
            Analyse();
        }
    }
}

// A headset's own microphone, when one is plugged in or streamed (Quest Link, Steam Link, Virtual Desktop, the
// Index and Vive), and otherwise the system's default.
std::string PickDevice() {
    const char* headsetNames[] = { "Oculus", "Meta", "Quest", "Virtual Desktop", "Steam Streaming", "Valve", "Index",
                                   "Vive", "Headset Microphone" };
    int count = SDL_GetNumAudioDevices(1);
    for (const char* wanted : headsetNames) {
        for (int i = 0; i < count; i++) {
            const char* name = SDL_GetAudioDeviceName(i, 1);
            if (name != nullptr && strstr(name, wanted) != nullptr) {
                return name;
            }
        }
    }
    return "";
}

void Open() {
    if (!sSdlAudioStarted) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            SPDLOG_ERROR("[VR] The microphone can't listen: SDL audio didn't start ({})", SDL_GetError());
            return;
        }
        sSdlAudioStarted = true;
    }
    SDL_AudioSpec want = {};
    want.freq = kRate;
    want.format = AUDIO_F32SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = OnSamples;
    SDL_AudioSpec have = {};
    std::string device = PickDevice();
    sTracker = Tracker();
    sDevice = SDL_OpenAudioDevice(device.empty() ? nullptr : device.c_str(), 1, &want, &have, 0);
    if (sDevice == 0) {
        SPDLOG_ERROR("[VR] The microphone can't listen: none opened ({})", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(sDevice, 0);
    SPDLOG_INFO("[VR] Listening to {}", device.empty() ? "the default microphone" : device);
}

void Close() {
    SDL_CloseAudioDevice(sDevice);
    sDevice = 0;
    sButton.store(0);
    sBend.store(0.0f);
    sBlowing.store(false);
    sBlowFor = 0.0f;
    SPDLOG_INFO("[VR] The microphone stopped listening");
}

bool sOpenFailed = false;

} // namespace

void OcarinaMic_Listen(bool on) {
    auto now = std::chrono::steady_clock::now();
    if (on) {
        sLastWanted = now;
        if (sDevice == 0 && !sOpenFailed) {
            Open();
            // Tried once per time listening is turned on, so a missing microphone isn't retried every frame.
            sOpenFailed = sDevice == 0;
        }
        return;
    }
    sOpenFailed = false;
    if (sDevice != 0 && std::chrono::duration<float>(now - sLastWanted).count() > kCloseAfter) {
        Close();
    }
}

MicNote OcarinaMic_Current() {
    MicNote note;
    note.listening = sDevice != 0;
    if (note.listening) {
        note.button = sButton.load();
        note.bend = sBend.load();
        note.onset = sOnset.load();
        note.blowing = sBlowing.load();
    }
    return note;
}

} // namespace SohVr
