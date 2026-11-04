#include "VrLauncher.h"

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SohVr {

#ifdef _WIN32
bool SwitchToGame(const char* key) {
    namespace fs = std::filesystem;
    wchar_t self[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 4);
    fs::path folder = fs::path(self).parent_path();
    // Beside this game, or, for Majora's Mask in a build tree, beside Ocarina of Time.
    fs::path launcher;
    std::error_code error;
    for (const wchar_t* place : { L".", L"..", L"../../../x64/Release" }) {
        fs::path candidate = folder / place / L"HarkinianVR.exe";
        if (fs::is_regular_file(candidate, error)) {
            launcher = fs::weakly_canonical(candidate, error);
            break;
        }
    }
    if (launcher.empty()) {
        SPDLOG_ERROR("[VR] Can't switch games: HarkinianVR.exe isn't beside this one");
        return false;
    }
    std::wstring command = L"\"" + launcher.wstring() + L"\" --after " + std::to_wstring(GetCurrentProcessId()) +
                           L" --play " + std::wstring(key, key + strlen(key));
    std::wstring launcherFolder = launcher.parent_path().wstring();
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(launcher.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, launcherFolder.c_str(),
                        &startup, &process)) {
        SPDLOG_ERROR("[VR] Can't switch games: the launcher didn't start ({})", GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    SPDLOG_INFO("[VR] Switching to {}", key);
    Ship::Context::GetRawInstance()->GetWindow()->Close();
    return true;
}
#else
bool SwitchToGame(const char*) {
    return false;
}
#endif

} // namespace SohVr
