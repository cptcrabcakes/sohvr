#include "VrRuntime.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace SohVr {

namespace {

constexpr float kScreenDistance = 2.2f; // meters in front of the player
constexpr float kScreenWidth = 2.4f;
constexpr uint32_t kScreenPixelsWide = 1600;
constexpr uint32_t kScreenPixelsHigh = 900;
// The HUD renders at 4:3 so its elements keep the positions the N64 screen gave them.
constexpr uint32_t kHudPixelsWide = 1280;
constexpr uint32_t kHudPixelsHigh = 960;
// One projection layer, the HUD pieces, and room to spare under OpenXR's guaranteed minimum of 16.
constexpr size_t kMaxLayers = 16;
// Triggers and grips count as pressed above the first value and let go below the second.
constexpr float kPressAbove = 0.6f;
constexpr float kReleaseBelow = 0.35f;

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    uint32_t width = 0;
    uint32_t height = 0;
};

bool Check(XrInstance instance, XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char text[XR_MAX_RESULT_STRING_SIZE] = "";
    if (instance != XR_NULL_HANDLE) {
        xrResultToString(instance, result, text);
    }
    SPDLOG_ERROR("[VR] {} failed: {} ({})", what, text, static_cast<int>(result));
    return false;
}

Pose ToPose(const XrPosef& pose, bool valid) {
    Pose out;
    out.orientation[0] = pose.orientation.x;
    out.orientation[1] = pose.orientation.y;
    out.orientation[2] = pose.orientation.z;
    out.orientation[3] = pose.orientation.w;
    out.position[0] = pose.position.x;
    out.position[1] = pose.position.y;
    out.position[2] = pose.position.z;
    out.valid = valid;
    return out;
}

bool HasExtension(const std::vector<XrExtensionProperties>& available, const char* name) {
    return std::any_of(available.begin(), available.end(),
                       [name](const XrExtensionProperties& ext) { return strcmp(ext.extensionName, name) == 0; });
}

// Full path of a running program, or empty if it isn't running.
std::wstring RunningProcessPath(const wchar_t* exeName) {
    std::wstring path;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return path;
    }
    PROCESSENTRY32W entry{ sizeof(entry) };
    for (BOOL found = Process32FirstW(snapshot, &entry); found && path.empty();
         found = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, exeName) != 0) {
            continue;
        }
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
            wchar_t buffer[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
                path.assign(buffer, size);
            }
            CloseHandle(process);
        }
    }
    CloseHandle(snapshot);
    return path;
}

// SteamVR's runtime manifest. It's found through the running vrserver.exe so any Steam library works; when SteamVR
// isn't running, installedToo falls back to looking in the main Steam folder.
std::filesystem::path SteamVrManifest(bool installedToo) {
    namespace fs = std::filesystem;
    std::error_code error;
    std::wstring server = RunningProcessPath(L"vrserver.exe");
    if (!server.empty()) {
        // vrserver.exe lives in <SteamVR>\bin\win64
        fs::path manifest = fs::path(server).parent_path().parent_path().parent_path() / L"steamxr_win64.json";
        if (fs::exists(manifest, error)) {
            return manifest;
        }
    }
    if (installedToo) {
        wchar_t steam[MAX_PATH];
        DWORD size = sizeof(steam);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, steam,
                         &size) == ERROR_SUCCESS) {
            fs::path manifest = fs::path(steam) / L"steamapps" / L"common" / L"SteamVR" / L"steamxr_win64.json";
            if (fs::exists(manifest, error)) {
                return manifest;
            }
        }
    }
    return {};
}

// Points the OpenXR loader at the chosen runtime, for this process only. False if SteamVR was asked for but
// can't be found.
bool SelectRuntime(RuntimeChoice choice) {
    std::filesystem::path manifest;
    if (choice != RUNTIME_SYSTEM) {
        manifest = SteamVrManifest(choice == RUNTIME_STEAMVR);
        if (manifest.empty() && choice == RUNTIME_STEAMVR) {
            return false;
        }
    }
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON", manifest.empty() ? nullptr : manifest.c_str());
    return true;
}

} // namespace

struct Runtime::Impl {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool lost = false;
    bool headsetMissing = false;

    XrSpace appSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    int64_t colorFormat = 0;
    std::array<Swapchain, 2> eyeSwapchains;
    Swapchain screenSwapchain;
    Swapchain hudSwapchain;

    XrFrameState frameState{ XR_TYPE_FRAME_STATE };
    bool frameBegun = false;
    std::array<XrView, 2> views{};
    std::array<bool, 2> eyeSubmitted{};
    bool screenSubmitted = false;
    bool hudSubmitted = false;
    XrPosef screenPose{};
    bool screenPlaced = false;

    std::array<EyeView, 2> eyes;
    Pose head;
    std::array<ControllerState, HAND_COUNT> controllers;
    uint32_t refreshRate = 0;

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrPath handPaths[HAND_COUNT] = {};
    XrAction stickAction = XR_NULL_HANDLE;
    XrAction triggerAction = XR_NULL_HANDLE;
    XrAction gripAction = XR_NULL_HANDLE;
    XrAction primaryAction = XR_NULL_HANDLE;
    XrAction secondaryAction = XR_NULL_HANDLE;
    XrAction stickClickAction = XR_NULL_HANDLE;
    XrAction menuAction = XR_NULL_HANDLE;
    XrAction gripPoseAction = XR_NULL_HANDLE;
    XrAction aimPoseAction = XR_NULL_HANDLE;
    XrAction hapticAction = XR_NULL_HANDLE;
    XrSpace gripSpaces[HAND_COUNT] = {};
    XrSpace aimSpaces[HAND_COUNT] = {};
    // The controller extensions turned on (see CreateInstance).
    std::vector<std::string> controllerExtensions;

    bool CreateInstance();
    bool CheckAdapter();
    bool CreateSession();
    bool CreateSpaces();
    bool CreateSwapchain(Swapchain& swapchain, uint32_t width, uint32_t height);
    bool CreateSwapchains();
    bool CreateActions();
    XrAction CreateAction(XrActionType type, const char* name, const char* localizedName, bool perHand);
    void UpdateInput(XrTime time);
    void LocateViews(XrTime time);
    void CopyToSwapchain(Swapchain& swapchain, ID3D11Texture2D* texture);
    void PlaceScreen();
    void Destroy();
};

bool Runtime::Impl::CreateInstance() {
    uint32_t count = 0;
    if (!Check(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr),
               "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }
    std::vector<XrExtensionProperties> available(count, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data());

    if (!HasExtension(available, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) {
        SPDLOG_ERROR("[VR] The OpenXR runtime doesn't support DirectX 11");
        return false;
    }
    std::vector<const char*> extensions = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    if (HasExtension(available, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME)) {
        extensions.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
    }
    // Controllers some runtimes only take bindings for behind their own extension (see CreateActions).
    for (const char* controllers : { "XR_EXT_hp_mixed_reality_controller", "XR_EXT_samsung_odyssey_controller",
                                     "XR_BD_controller_interaction", "XR_HTC_vive_focus3_controller_interaction" }) {
        if (HasExtension(available, controllers)) {
            extensions.push_back(controllers);
            controllerExtensions.push_back(controllers);
        }
    }

    XrInstanceCreateInfo createInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
    strncpy(createInfo.applicationInfo.applicationName, "Ship of Harkinian VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    strncpy(createInfo.applicationInfo.engineName, "libultraship", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
#ifdef XR_API_VERSION_1_0
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
#else
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
#endif
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.data();
    if (!Check(XR_NULL_HANDLE, xrCreateInstance(&createInfo, &instance), "xrCreateInstance")) {
        return false;
    }

    // Logged only when it changes, since a missing headset makes this run every few seconds.
    static std::string sLoggedRuntime;
    XrInstanceProperties properties{ XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties(instance, &properties);
    if (sLoggedRuntime != properties.runtimeName) {
        SPDLOG_INFO("[VR] OpenXR runtime: {} {}.{}.{}", properties.runtimeName,
                    XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion),
                    XR_VERSION_PATCH(properties.runtimeVersion));
        sLoggedRuntime = properties.runtimeName;
    }

    XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult result = xrGetSystem(instance, &systemInfo, &systemId);
    if (result == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        headsetMissing = true;
        return false;
    }
    return Check(instance, result, "xrGetSystem");
}

bool Runtime::Impl::CheckAdapter() {
    PFN_xrGetD3D11GraphicsRequirementsKHR getRequirements = nullptr;
    if (!Check(instance,
               xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
               "xrGetInstanceProcAddr")) {
        return false;
    }
    // The spec requires this call before xrCreateSession.
    XrGraphicsRequirementsD3D11KHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    if (!Check(instance, getRequirements(instance, systemId, &requirements), "xrGetD3D11GraphicsRequirementsKHR")) {
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&desc))) {
        SPDLOG_ERROR("[VR] Couldn't read the renderer's GPU");
        return false;
    }
    if (desc.AdapterLuid.LowPart != requirements.adapterLuid.LowPart ||
        desc.AdapterLuid.HighPart != requirements.adapterLuid.HighPart) {
        SPDLOG_ERROR("[VR] The game is rendering on a different GPU than the headset uses. Set the game to use the "
                     "headset's GPU in Windows graphics settings.");
        return false;
    }
    return true;
}

bool Runtime::Impl::CreateSession() {
    XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    binding.device = device;
    XrSessionCreateInfo createInfo{ XR_TYPE_SESSION_CREATE_INFO };
    createInfo.next = &binding;
    createInfo.systemId = systemId;
    return Check(instance, xrCreateSession(instance, &createInfo, &session), "xrCreateSession");
}

bool Runtime::Impl::CreateSpaces() {
    uint32_t count = 0;
    xrEnumerateReferenceSpaces(session, 0, &count, nullptr);
    std::vector<XrReferenceSpaceType> types(count);
    xrEnumerateReferenceSpaces(session, count, &count, types.data());
    auto supports = [&](XrReferenceSpaceType type) { return std::find(types.begin(), types.end(), type) != types.end(); };

    // Floor-level spaces give real standing height; LOCAL is eye-level, so its origin is lowered by an average
    // eye height instead.
    XrReferenceSpaceCreateInfo createInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    createInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (supports(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT)) {
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    } else if (supports(XR_REFERENCE_SPACE_TYPE_STAGE)) {
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else {
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        createInfo.poseInReferenceSpace.position.y = -1.6f;
    }
    if (!Check(instance, xrCreateReferenceSpace(session, &createInfo, &appSpace), "xrCreateReferenceSpace")) {
        return false;
    }

    XrReferenceSpaceCreateInfo viewInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewInfo.poseInReferenceSpace.orientation.w = 1.0f;
    return Check(instance, xrCreateReferenceSpace(session, &viewInfo, &viewSpace), "xrCreateReferenceSpace (view)");
}

bool Runtime::Impl::CreateSwapchain(Swapchain& swapchain, uint32_t width, uint32_t height) {
    XrSwapchainCreateInfo createInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    createInfo.format = colorFormat;
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    if (!Check(instance, xrCreateSwapchain(session, &createInfo, &swapchain.handle), "xrCreateSwapchain")) {
        return false;
    }
    uint32_t count = 0;
    xrEnumerateSwapchainImages(swapchain.handle, 0, &count, nullptr);
    swapchain.images.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    if (!Check(instance,
               xrEnumerateSwapchainImages(swapchain.handle, count, &count,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchain.images.data())),
               "xrEnumerateSwapchainImages")) {
        return false;
    }
    swapchain.width = width;
    swapchain.height = height;
    return true;
}

bool Runtime::Impl::CreateSwapchains() {
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    xrEnumerateSwapchainFormats(session, count, &count, formats.data());

    // The game renders R8G8B8A8_UNORM in gamma space. Copying those bytes into an sRGB swapchain makes the
    // compositor read them as gamma-encoded, which is what they are.
    for (int64_t preferred : { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM }) {
        if (std::find(formats.begin(), formats.end(), preferred) != formats.end()) {
            colorFormat = preferred;
            break;
        }
    }
    if (colorFormat == 0) {
        SPDLOG_ERROR("[VR] The OpenXR runtime offers no RGBA8 swapchain format");
        return false;
    }

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
                                      nullptr);
    std::vector<XrViewConfigurationView> configViews(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
    xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
                                      &viewCount, configViews.data());
    if (viewCount != 2) {
        SPDLOG_ERROR("[VR] Expected 2 stereo views, the runtime reports {}", viewCount);
        return false;
    }
    for (uint32_t i = 0; i < 2; i++) {
        if (!CreateSwapchain(eyeSwapchains[i], configViews[i].recommendedImageRectWidth,
                             configViews[i].recommendedImageRectHeight)) {
            return false;
        }
    }
    SPDLOG_INFO("[VR] Eye resolution {}x{}", eyeSwapchains[0].width, eyeSwapchains[0].height);
    return CreateSwapchain(screenSwapchain, kScreenPixelsWide, kScreenPixelsHigh) &&
           CreateSwapchain(hudSwapchain, kHudPixelsWide, kHudPixelsHigh);
}

XrAction Runtime::Impl::CreateAction(XrActionType type, const char* name, const char* localizedName, bool perHand) {
    XrActionCreateInfo createInfo{ XR_TYPE_ACTION_CREATE_INFO };
    createInfo.actionType = type;
    strncpy(createInfo.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(createInfo.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (perHand) {
        createInfo.countSubactionPaths = HAND_COUNT;
        createInfo.subactionPaths = handPaths;
    }
    XrAction action = XR_NULL_HANDLE;
    Check(instance, xrCreateAction(actionSet, &createInfo, &action), name);
    return action;
}

bool Runtime::Impl::CreateActions() {
    XrActionSetCreateInfo setInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strncpy(setInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(setInfo.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    if (!Check(instance, xrCreateActionSet(instance, &setInfo, &actionSet), "xrCreateActionSet")) {
        return false;
    }
    xrStringToPath(instance, "/user/hand/left", &handPaths[HAND_LEFT]);
    xrStringToPath(instance, "/user/hand/right", &handPaths[HAND_RIGHT]);

    stickAction = CreateAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Thumbstick", true);
    triggerAction = CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", true);
    gripAction = CreateAction(XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip", true);
    primaryAction = CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "A / X", true);
    secondaryAction = CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "B / Y", true);
    stickClickAction = CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Thumbstick click", true);
    menuAction = CreateAction(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu", false);
    gripPoseAction = CreateAction(XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Hand grip", true);
    aimPoseAction = CreateAction(XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Hand aim", true);
    hapticAction = CreateAction(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration", true);

    // Bindings for every kind of controller the PC runtimes know, so the same controls work whichever headset is
    // used: the runtime picks the profile that matches what's in the player's hands. What each one lacks is left
    // out: the Index has no menu button (the left stick click pauses), and the Vive wands' trackpad is their stick
    // and their trackpad click A or X, with the menu button as B or Y; there, pausing and recentering are on the
    // item wheel and the VR settings. A profile the runtime refuses only loses that controller.
    struct Layout {
        const char* profile;
        const char* extension; // needed for the runtime to know it, or nullptr
        const char* stick;
        const char* squeeze;
        const char* stickClick; // or nullptr
        const char* primary[HAND_COUNT];
        const char* secondary[HAND_COUNT];
        const char* menu; // on the left hand, or nullptr
    };
    const Layout layouts[] = {
        { "/interaction_profiles/oculus/touch_controller", nullptr, "thumbstick", "squeeze/value", "thumbstick/click",
          { "x/click", "a/click" }, { "y/click", "b/click" }, "menu/click" },
        { "/interaction_profiles/valve/index_controller", nullptr, "thumbstick", "squeeze/value", "thumbstick/click",
          { "a/click", "a/click" }, { "b/click", "b/click" }, nullptr },
        { "/interaction_profiles/htc/vive_controller", nullptr, "trackpad", "squeeze/click", nullptr,
          { "trackpad/click", "trackpad/click" }, { "menu/click", "menu/click" }, nullptr },
        { "/interaction_profiles/microsoft/motion_controller", nullptr, "thumbstick", "squeeze/click",
          "thumbstick/click", { "trackpad/click", "trackpad/click" }, { "menu/click", "menu/click" }, nullptr },
        { "/interaction_profiles/hp/mixed_reality_controller", "XR_EXT_hp_mixed_reality_controller", "thumbstick",
          "squeeze/value", "thumbstick/click", { "x/click", "a/click" }, { "y/click", "b/click" }, "menu/click" },
        { "/interaction_profiles/samsung/odyssey_controller", "XR_EXT_samsung_odyssey_controller", "thumbstick",
          "squeeze/click", "thumbstick/click", { "trackpad/click", "trackpad/click" }, { "menu/click", "menu/click" },
          nullptr },
        { "/interaction_profiles/bytedance/pico4_controller", "XR_BD_controller_interaction", "thumbstick",
          "squeeze/value", "thumbstick/click", { "x/click", "a/click" }, { "y/click", "b/click" }, "menu/click" },
        { "/interaction_profiles/htc/vive_focus3_controller", "XR_HTC_vive_focus3_controller_interaction",
          "thumbstick", "squeeze/click", "thumbstick/click", { "x/click", "a/click" }, { "y/click", "b/click" },
          "menu/click" },
    };
    const char* const handNames[HAND_COUNT] = { "/user/hand/left", "/user/hand/right" };
    int profilesBound = 0;
    for (const Layout& layout : layouts) {
        if (layout.extension != nullptr &&
            std::find(controllerExtensions.begin(), controllerExtensions.end(), layout.extension) ==
                controllerExtensions.end()) {
            continue;
        }
        std::vector<XrActionSuggestedBinding> bindings;
        auto bind = [&](XrAction action, int hand, const char* input) {
            XrPath xrPath = XR_NULL_PATH;
            std::string path = std::string(handNames[hand]) + (strncmp(input, "output/", 7) == 0 ? "/" : "/input/") +
                               input;
            xrStringToPath(instance, path.c_str(), &xrPath);
            bindings.push_back({ action, xrPath });
        };
        for (int hand = 0; hand < HAND_COUNT; hand++) {
            bind(stickAction, hand, layout.stick);
            bind(triggerAction, hand, "trigger/value");
            bind(gripAction, hand, layout.squeeze);
            if (layout.stickClick != nullptr) {
                bind(stickClickAction, hand, layout.stickClick);
            }
            bind(primaryAction, hand, layout.primary[hand]);
            bind(secondaryAction, hand, layout.secondary[hand]);
            bind(gripPoseAction, hand, "grip/pose");
            bind(aimPoseAction, hand, "aim/pose");
            bind(hapticAction, hand, "output/haptic");
        }
        if (layout.menu != nullptr) {
            bind(menuAction, HAND_LEFT, layout.menu);
        }
        XrPath profile = XR_NULL_PATH;
        xrStringToPath(instance, layout.profile, &profile);
        XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        if (Check(instance, xrSuggestInteractionProfileBindings(instance, &suggested), layout.profile)) {
            profilesBound++;
        }
    }
    // Any runtime takes the simple controller: a select button (the trigger), a menu button, and the poses.
    {
        std::vector<XrActionSuggestedBinding> bindings;
        for (int hand = 0; hand < HAND_COUNT; hand++) {
            for (auto [action, input] : { std::pair{ triggerAction, "/input/select/click" },
                                          std::pair{ secondaryAction, "/input/menu/click" },
                                          std::pair{ gripPoseAction, "/input/grip/pose" },
                                          std::pair{ aimPoseAction, "/input/aim/pose" },
                                          std::pair{ hapticAction, "/output/haptic" } }) {
                XrPath xrPath = XR_NULL_PATH;
                xrStringToPath(instance, (std::string(handNames[hand]) + input).c_str(), &xrPath);
                bindings.push_back({ action, xrPath });
            }
        }
        XrPath profile = XR_NULL_PATH;
        xrStringToPath(instance, "/interaction_profiles/khr/simple_controller", &profile);
        XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        if (Check(instance, xrSuggestInteractionProfileBindings(instance, &suggested), "simple controller bindings")) {
            profilesBound++;
        }
    }
    if (profilesBound == 0) {
        return false;
    }

    for (int hand = 0; hand < HAND_COUNT; hand++) {
        XrActionSpaceCreateInfo spaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        spaceInfo.subactionPath = handPaths[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        spaceInfo.action = gripPoseAction;
        xrCreateActionSpace(session, &spaceInfo, &gripSpaces[hand]);
        spaceInfo.action = aimPoseAction;
        xrCreateActionSpace(session, &spaceInfo, &aimSpaces[hand]);
    }

    XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet;
    return Check(instance, xrAttachSessionActionSets(session, &attachInfo), "xrAttachSessionActionSets");
}

void Runtime::Impl::UpdateInput(XrTime time) {
    XrActiveActionSet active{ actionSet, XR_NULL_PATH };
    XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session, &syncInfo))) {
        return;
    }

    auto getBool = [&](XrAction action, XrPath subaction) {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = action;
        info.subactionPath = subaction;
        XrActionStateBoolean value{ XR_TYPE_ACTION_STATE_BOOLEAN };
        return XR_SUCCEEDED(xrGetActionStateBoolean(session, &info, &value)) && value.isActive && value.currentState;
    };
    auto getFloat = [&](XrAction action, XrPath subaction) {
        XrActionStateGetInfo info{ XR_TYPE_ACTION_STATE_GET_INFO };
        info.action = action;
        info.subactionPath = subaction;
        XrActionStateFloat value{ XR_TYPE_ACTION_STATE_FLOAT };
        bool ok = XR_SUCCEEDED(xrGetActionStateFloat(session, &info, &value)) && value.isActive;
        return ok ? value.currentState : 0.0f;
    };
    auto locate = [&](XrSpace space) {
        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        bool ok = XR_SUCCEEDED(xrLocateSpace(space, appSpace, time, &location));
        bool valid = ok && (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                     (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        return ToPose(location.pose, valid);
    };

    for (int hand = 0; hand < HAND_COUNT; hand++) {
        ControllerState& state = controllers[hand];
        XrPath path = handPaths[hand];

        XrActionStateGetInfo stickInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
        stickInfo.action = stickAction;
        stickInfo.subactionPath = path;
        XrActionStateVector2f stick{ XR_TYPE_ACTION_STATE_VECTOR2F };
        bool stickOk = XR_SUCCEEDED(xrGetActionStateVector2f(session, &stickInfo, &stick)) && stick.isActive;
        state.stick[0] = stickOk ? stick.currentState.x : 0.0f;
        state.stick[1] = stickOk ? stick.currentState.y : 0.0f;

        state.trigger = getFloat(triggerAction, path);
        state.grip = getFloat(gripAction, path);
        state.triggerHeld = state.trigger > (state.triggerHeld ? kReleaseBelow : kPressAbove);
        state.gripHeld = state.grip > (state.gripHeld ? kReleaseBelow : kPressAbove);
        state.primary = getBool(primaryAction, path);
        state.secondary = getBool(secondaryAction, path);
        state.stickClick = getBool(stickClickAction, path);
        state.gripPose = locate(gripSpaces[hand]);
        state.aimPose = locate(aimSpaces[hand]);
    }
    controllers[HAND_LEFT].menu = getBool(menuAction, XR_NULL_PATH);

    XrSpaceLocation headLocation{ XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(xrLocateSpace(viewSpace, appSpace, time, &headLocation))) {
        head = ToPose(headLocation.pose, (headLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0);
    }
}

void Runtime::Impl::LocateViews(XrTime time) {
    XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = time;
    locateInfo.space = appSpace;
    XrViewState viewState{ XR_TYPE_VIEW_STATE };
    uint32_t count = 0;
    views = { XrView{ XR_TYPE_VIEW }, XrView{ XR_TYPE_VIEW } };
    if (XR_FAILED(xrLocateViews(session, &locateInfo, &viewState, 2, &count, views.data())) || count != 2) {
        return;
    }
    bool valid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    for (int i = 0; i < 2; i++) {
        eyes[i].pose = ToPose(views[i].pose, valid);
        eyes[i].fov = { views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp,
                        views[i].fov.angleDown };
    }
}

void Runtime::Impl::CopyToSwapchain(Swapchain& swapchain, ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1) {
        return;
    }

    XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    uint32_t index = 0;
    if (!Check(instance, xrAcquireSwapchainImage(swapchain.handle, &acquireInfo, &index), "xrAcquireSwapchainImage")) {
        return;
    }
    XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    waitInfo.timeout = XR_INFINITE_DURATION;
    if (Check(instance, xrWaitSwapchainImage(swapchain.handle, &waitInfo), "xrWaitSwapchainImage")) {
        D3D11_BOX box{ 0, 0, 0, std::min(desc.Width, swapchain.width), std::min(desc.Height, swapchain.height), 1 };
        context->CopySubresourceRegion(swapchain.images[index].texture, 0, 0, 0, 0, texture, 0, &box);
    }
    XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(swapchain.handle, &releaseInfo);
}

void Runtime::Impl::PlaceScreen() {
    if (!head.valid) {
        return;
    }
    // Keep only the head's yaw so the screen stands upright.
    const float* q = head.orientation;
    float forwardX = -2.0f * (q[0] * q[2] + q[3] * q[1]);
    float forwardZ = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
    float yaw = atan2f(-forwardX, -forwardZ);
    float length = sqrtf(forwardX * forwardX + forwardZ * forwardZ);
    if (length < 0.001f) {
        return;
    }
    screenPose.orientation = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
    screenPose.position = { head.position[0] + forwardX / length * kScreenDistance, head.position[1],
                            head.position[2] + forwardZ / length * kScreenDistance };
    screenPlaced = true;
}

void Runtime::Impl::Destroy() {
    for (int hand = 0; hand < HAND_COUNT; hand++) {
        if (gripSpaces[hand] != XR_NULL_HANDLE) {
            xrDestroySpace(gripSpaces[hand]);
        }
        if (aimSpaces[hand] != XR_NULL_HANDLE) {
            xrDestroySpace(aimSpaces[hand]);
        }
    }
    for (Swapchain* swapchain : { &eyeSwapchains[0], &eyeSwapchains[1], &screenSwapchain, &hudSwapchain }) {
        if (swapchain->handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(swapchain->handle);
        }
    }
    if (viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(viewSpace);
    }
    if (appSpace != XR_NULL_HANDLE) {
        xrDestroySpace(appSpace);
    }
    if (actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(actionSet);
    }
    if (session != XR_NULL_HANDLE) {
        xrDestroySession(session);
    }
    if (instance != XR_NULL_HANDLE) {
        xrDestroyInstance(instance);
    }
}

Runtime& Runtime::Get() {
    static Runtime runtime;
    return runtime;
}

InitResult Runtime::Init(ID3D11Device* device, ID3D11DeviceContext* context, RuntimeChoice choice) {
    Shutdown();
    if (!SelectRuntime(choice)) {
        // SteamVR was asked for but isn't running or installed yet.
        return InitResult::NoHeadset;
    }
    mImpl = new Impl();
    mImpl->device = device;
    mImpl->context = context;
    if (!mImpl->CreateInstance() || !mImpl->CheckAdapter() || !mImpl->CreateSession() || !mImpl->CreateSpaces() ||
        !mImpl->CreateSwapchains() || !mImpl->CreateActions()) {
        InitResult result = mImpl->headsetMissing ? InitResult::NoHeadset : InitResult::Failed;
        Shutdown();
        return result;
    }
    SPDLOG_INFO("[VR] OpenXR session created");
    return InitResult::Ok;
}

void Runtime::Shutdown() {
    if (mImpl == nullptr) {
        return;
    }
    if (mImpl->running) {
        xrEndSession(mImpl->session);
    }
    mImpl->Destroy();
    delete mImpl;
    mImpl = nullptr;
}

bool Runtime::IsInitialized() const {
    return mImpl != nullptr;
}

bool Runtime::IsLost() const {
    return mImpl != nullptr && mImpl->lost;
}

bool Runtime::IsSessionRunning() const {
    return IsInitialized() && mImpl->running && !mImpl->lost;
}

void Runtime::PollEvents() {
    if (mImpl == nullptr) {
        return;
    }
    XrEventDataBuffer event{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(mImpl->instance, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
                mImpl->state = changed.state;
                if (changed.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
                    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    mImpl->running =
                        Check(mImpl->instance, xrBeginSession(mImpl->session, &beginInfo), "xrBeginSession");
                    mImpl->screenPlaced = false;
                } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(mImpl->session);
                    mImpl->running = false;
                } else if (changed.state == XR_SESSION_STATE_EXITING ||
                           changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                    mImpl->running = false;
                    mImpl->lost = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                mImpl->running = false;
                mImpl->lost = true;
                break;
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
                // Which controller layout the runtime chose for each hand (see CreateActions), for the log.
                for (int hand = 0; hand < HAND_COUNT; hand++) {
                    XrInteractionProfileState profile{ XR_TYPE_INTERACTION_PROFILE_STATE };
                    if (XR_FAILED(xrGetCurrentInteractionProfile(mImpl->session, mImpl->handPaths[hand], &profile)) ||
                        profile.interactionProfile == XR_NULL_PATH) {
                        continue;
                    }
                    char name[XR_MAX_PATH_LENGTH] = "";
                    uint32_t length = 0;
                    xrPathToString(mImpl->instance, profile.interactionProfile, sizeof(name), &length, name);
                    SPDLOG_INFO("[VR] {} hand: {}", hand == HAND_LEFT ? "Left" : "Right", name);
                }
                break;
            }
            default:
                break;
        }
        event = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

bool Runtime::BeginFrame() {
    if (!IsSessionRunning()) {
        return false;
    }
    XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
    mImpl->frameState = { XR_TYPE_FRAME_STATE };
    if (!Check(mImpl->instance, xrWaitFrame(mImpl->session, &waitInfo, &mImpl->frameState), "xrWaitFrame")) {
        return false;
    }
    XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
    if (!Check(mImpl->instance, xrBeginFrame(mImpl->session, &beginInfo), "xrBeginFrame")) {
        return false;
    }
    mImpl->frameBegun = true;
    mImpl->eyeSubmitted = { false, false };
    mImpl->screenSubmitted = false;
    mImpl->hudSubmitted = false;

    if (mImpl->frameState.predictedDisplayPeriod > 0) {
        mImpl->refreshRate =
            static_cast<uint32_t>(lround(1.0e9 / static_cast<double>(mImpl->frameState.predictedDisplayPeriod)));
    }
    mImpl->UpdateInput(mImpl->frameState.predictedDisplayTime);
    if (mImpl->frameState.shouldRender) {
        mImpl->LocateViews(mImpl->frameState.predictedDisplayTime);
    }
    if (!mImpl->screenPlaced) {
        mImpl->PlaceScreen();
    }
    return true;
}

bool Runtime::ShouldRender() const {
    return IsSessionRunning() && mImpl->frameBegun && mImpl->frameState.shouldRender;
}

void Runtime::SubmitEye(int eye, ID3D11Texture2D* texture) {
    if (!ShouldRender() || texture == nullptr) {
        return;
    }
    mImpl->CopyToSwapchain(mImpl->eyeSwapchains[eye], texture);
    mImpl->eyeSubmitted[eye] = true;
}

void Runtime::SubmitScreen(ID3D11Texture2D* texture) {
    if (!ShouldRender() || texture == nullptr) {
        return;
    }
    mImpl->CopyToSwapchain(mImpl->screenSwapchain, texture);
    mImpl->screenSubmitted = true;
}

void Runtime::SubmitHud(ID3D11Texture2D* texture) {
    if (!ShouldRender() || texture == nullptr) {
        return;
    }
    mImpl->CopyToSwapchain(mImpl->hudSwapchain, texture);
    mImpl->hudSubmitted = true;
}

void Runtime::EndFrame(bool stereo, const std::vector<HudQuad>& hud) {
    if (mImpl == nullptr || !mImpl->frameBegun) {
        return;
    }
    mImpl->frameBegun = false;

    std::array<XrCompositionLayerProjectionView, 2> projectionViews{};
    XrCompositionLayerProjection projection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    std::array<XrCompositionLayerQuad, kMaxLayers - 1> hudQuads{};
    std::vector<const XrCompositionLayerBaseHeader*> layers;

    if (mImpl->frameState.shouldRender && stereo && mImpl->eyeSubmitted[0] && mImpl->eyeSubmitted[1]) {
        for (int i = 0; i < 2; i++) {
            const Swapchain& swapchain = mImpl->eyeSwapchains[i];
            projectionViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            projectionViews[i].pose = mImpl->views[i].pose;
            projectionViews[i].fov = mImpl->views[i].fov;
            projectionViews[i].subImage.swapchain = swapchain.handle;
            projectionViews[i].subImage.imageRect.extent = { static_cast<int32_t>(swapchain.width),
                                                             static_cast<int32_t>(swapchain.height) };
        }
        projection.space = mImpl->appSpace;
        projection.viewCount = 2;
        projection.views = projectionViews.data();
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection));

        size_t count = mImpl->hudSubmitted ? std::min(hud.size(), hudQuads.size()) : 0;
        for (size_t i = 0; i < count; i++) {
            const HudQuad& piece = hud[i];
            if (!piece.pose.valid || piece.rect[2] <= 0 || piece.rect[3] <= 0) {
                continue;
            }
            XrCompositionLayerQuad& layer = hudQuads[i];
            layer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
            layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            layer.space = mImpl->appSpace;
            layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            layer.subImage.swapchain = mImpl->hudSwapchain.handle;
            layer.subImage.imageRect = { { piece.rect[0], piece.rect[1] }, { piece.rect[2], piece.rect[3] } };
            layer.pose = { { piece.pose.orientation[0], piece.pose.orientation[1], piece.pose.orientation[2],
                             piece.pose.orientation[3] },
                           { piece.pose.position[0], piece.pose.position[1], piece.pose.position[2] } };
            layer.size = { piece.width, piece.width * piece.rect[3] / piece.rect[2] };
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer));
        }
    } else if (mImpl->frameState.shouldRender && !stereo && mImpl->screenSubmitted && mImpl->screenPlaced) {
        const Swapchain& swapchain = mImpl->screenSwapchain;
        quad.space = mImpl->appSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain.handle;
        quad.subImage.imageRect.extent = { static_cast<int32_t>(swapchain.width),
                                           static_cast<int32_t>(swapchain.height) };
        quad.pose = mImpl->screenPose;
        quad.size = { kScreenWidth, kScreenWidth * swapchain.height / swapchain.width };
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
    }

    XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
    endInfo.displayTime = mImpl->frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = static_cast<uint32_t>(layers.size());
    endInfo.layers = layers.empty() ? nullptr : layers.data();
    Check(mImpl->instance, xrEndFrame(mImpl->session, &endInfo), "xrEndFrame");
}

void Runtime::RecenterScreen() {
    if (mImpl != nullptr) {
        mImpl->PlaceScreen();
    }
}

bool Runtime::GetScreen(Pose& pose, float& width, float& height) const {
    if (mImpl == nullptr || !mImpl->screenPlaced) {
        return false;
    }
    const XrPosef& screen = mImpl->screenPose;
    pose.orientation[0] = screen.orientation.x;
    pose.orientation[1] = screen.orientation.y;
    pose.orientation[2] = screen.orientation.z;
    pose.orientation[3] = screen.orientation.w;
    pose.position[0] = screen.position.x;
    pose.position[1] = screen.position.y;
    pose.position[2] = screen.position.z;
    pose.valid = true;
    width = kScreenWidth;
    height = kScreenWidth * kScreenPixelsHigh / kScreenPixelsWide;
    return true;
}

void Runtime::Vibrate(Hand hand, float amplitude, float seconds) {
    if (!IsSessionRunning()) {
        return;
    }
    XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.duration = static_cast<XrDuration>(seconds * 1.0e9f);
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo info{ XR_TYPE_HAPTIC_ACTION_INFO };
    info.action = mImpl->hapticAction;
    info.subactionPath = mImpl->handPaths[hand];
    xrApplyHapticFeedback(mImpl->session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
}

namespace {
const std::array<EyeView, 2> kNoEyes{};
const Pose kNoPose{};
const ControllerState kNoController{};
} // namespace

const std::array<EyeView, 2>& Runtime::GetEyes() const {
    return mImpl != nullptr ? mImpl->eyes : kNoEyes;
}

const Pose& Runtime::GetHead() const {
    return mImpl != nullptr ? mImpl->head : kNoPose;
}

const ControllerState& Runtime::GetController(Hand hand) const {
    return mImpl != nullptr ? mImpl->controllers[hand] : kNoController;
}

uint32_t Runtime::GetEyeWidth() const {
    return mImpl != nullptr ? mImpl->eyeSwapchains[0].width : 0;
}

uint32_t Runtime::GetEyeHeight() const {
    return mImpl != nullptr ? mImpl->eyeSwapchains[0].height : 0;
}

uint32_t Runtime::GetScreenWidth() const {
    return mImpl != nullptr ? mImpl->screenSwapchain.width : 0;
}

uint32_t Runtime::GetScreenHeight() const {
    return mImpl != nullptr ? mImpl->screenSwapchain.height : 0;
}

uint32_t Runtime::GetHudWidth() const {
    return mImpl != nullptr ? mImpl->hudSwapchain.width : 0;
}

uint32_t Runtime::GetHudHeight() const {
    return mImpl != nullptr ? mImpl->hudSwapchain.height : 0;
}

uint32_t Runtime::GetRefreshRate() const {
    return IsInitialized() ? mImpl->refreshRate : 0;
}

} // namespace SohVr

#else

// VR is only implemented for the DirectX 11 renderer on Windows.
namespace SohVr {

struct Runtime::Impl {};

namespace {
const std::array<EyeView, 2> kNoEyes{};
const Pose kNoPose{};
const ControllerState kNoController{};
} // namespace

Runtime& Runtime::Get() {
    static Runtime runtime;
    return runtime;
}
InitResult Runtime::Init(ID3D11Device*, ID3D11DeviceContext*, RuntimeChoice) {
    return InitResult::Failed;
}
void Runtime::Shutdown() {
}
bool Runtime::IsInitialized() const {
    return false;
}
bool Runtime::IsLost() const {
    return false;
}
bool Runtime::IsSessionRunning() const {
    return false;
}
void Runtime::PollEvents() {
}
bool Runtime::BeginFrame() {
    return false;
}
bool Runtime::ShouldRender() const {
    return false;
}
void Runtime::SubmitEye(int, ID3D11Texture2D*) {
}
void Runtime::SubmitScreen(ID3D11Texture2D*) {
}
void Runtime::SubmitHud(ID3D11Texture2D*) {
}
void Runtime::EndFrame(bool, const std::vector<HudQuad>&) {
}
void Runtime::RecenterScreen() {
}
bool Runtime::GetScreen(Pose&, float&, float&) const {
    return false;
}
void Runtime::Vibrate(Hand, float, float) {
}
const std::array<EyeView, 2>& Runtime::GetEyes() const {
    return kNoEyes;
}
const Pose& Runtime::GetHead() const {
    return kNoPose;
}
const ControllerState& Runtime::GetController(Hand) const {
    return kNoController;
}
uint32_t Runtime::GetEyeWidth() const {
    return 0;
}
uint32_t Runtime::GetEyeHeight() const {
    return 0;
}
uint32_t Runtime::GetScreenWidth() const {
    return 0;
}
uint32_t Runtime::GetScreenHeight() const {
    return 0;
}
uint32_t Runtime::GetHudWidth() const {
    return 0;
}
uint32_t Runtime::GetHudHeight() const {
    return 0;
}
uint32_t Runtime::GetRefreshRate() const {
    return 0;
}

} // namespace SohVr

#endif
