#pragma once

// OpenXR session for the DirectX 11 renderer: swapchains, frame timing and controller input.
// Knows nothing about the game; VrRender and VrGame build on it.

#include <array>
#include <cstdint>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace SohVr {

struct Pose {
    float orientation[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // x, y, z, w
    float position[3] = {};                            // meters, in the floor-level play space
    bool valid = false;
};

// Tangent-space half angles in radians; left and down are negative.
struct Fov {
    float left = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float down = 0.0f;
};

struct EyeView {
    Pose pose;
    Fov fov;
};

enum Hand { HAND_LEFT, HAND_RIGHT, HAND_COUNT };

struct ControllerState {
    float stick[2] = {};
    float trigger = 0.0f;
    float grip = 0.0f;
    // Squeezed past 60% and not yet let go below 35%, so a grip held near one threshold doesn't flicker.
    bool triggerHeld = false;
    bool gripHeld = false;
    bool primary = false;   // A on the right hand, X on the left
    bool secondary = false; // B on the right hand, Y on the left
    bool stickClick = false;
    bool menu = false; // left hand only
    Pose gripPose;
    Pose aimPose;
};

// A piece of the HUD texture shown somewhere in the room, such as on a wrist.
struct HudQuad {
    Pose pose;          // in the play space; the picture faces along the pose's +Z
    float width = 0.0f; // meters; the height follows the rect's shape
    int32_t rect[4] = {}; // x, y, width, height in HUD texture pixels
    float alpha = 1.0f;   // how opaque, where the runtime can fade a layer (XR_KHR_composition_layer_color_scale_bias)
};

enum class InitResult {
    Ok,
    NoHeadset, // worth retrying; the headset may connect later
    Failed,
};

// Which OpenXR runtime to load. The choice only applies to this process; the system default is left alone.
enum RuntimeChoice {
    RUNTIME_AUTO,   // SteamVR while it's running, otherwise the system default
    RUNTIME_SYSTEM, // whatever runtime Windows has set as active
    RUNTIME_STEAMVR,
};

class Runtime {
  public:
    static Runtime& Get();

    InitResult Init(ID3D11Device* device, ID3D11DeviceContext* context, RuntimeChoice choice);
    void Shutdown();
    bool IsInitialized() const;
    // True once the runtime has ended the session for good, such as when the player quits from the headset.
    bool IsLost() const;
    // True between the runtime's READY and STOPPING states, when frames must be submitted.
    bool IsSessionRunning() const;

    // Handles runtime events. Call once per rendered frame before BeginFrame.
    void PollEvents();
    // Waits for the next display slot, then updates views, head and controllers.
    bool BeginFrame();
    // False while the headset is off or the app is hidden; the frame must still be ended.
    bool ShouldRender() const;
    // Copies the finished image into this frame's swapchain image. eye 0 is left, 1 is right.
    void SubmitEye(int eye, ID3D11Texture2D* texture);
    // Copies a flat image onto the virtual screen shown when not in stereo.
    void SubmitScreen(ID3D11Texture2D* texture);
    // Copies the HUD (premultiplied alpha) that the hud quads show pieces of.
    void SubmitHud(ID3D11Texture2D* texture);
    // stereo submits both eyes plus the hud quads; otherwise the virtual screen is submitted.
    void EndFrame(bool stereo, const std::vector<HudQuad>& hud);

    // Places the virtual screen in front of where the player is looking.
    void RecenterScreen();
    // Where the virtual screen is (its center, facing the player along its +Z) and its size in meters. False until
    // it has been placed.
    bool GetScreen(Pose& pose, float& width, float& height) const;
    void Vibrate(Hand hand, float amplitude, float seconds);

    const std::array<EyeView, 2>& GetEyes() const;
    const Pose& GetHead() const;
    const ControllerState& GetController(Hand hand) const;
    uint32_t GetEyeWidth() const;
    uint32_t GetEyeHeight() const;
    uint32_t GetScreenWidth() const;
    uint32_t GetScreenHeight() const;
    uint32_t GetHudWidth() const;
    uint32_t GetHudHeight() const;
    // Display refresh rate in Hz, or 0 if the runtime doesn't report one.
    uint32_t GetRefreshRate() const;
    // How many display refreshes went by without a frame of ours since this was last asked.
    uint32_t TakeMissedFrames();

  private:
    Runtime() = default;
    struct Impl;
    Impl* mImpl = nullptr;
};

} // namespace SohVr
