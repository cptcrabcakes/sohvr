#include "VrRender.h"
#include "VrGame.h"
#include "VrHud.h"
#include "VrRuntime.h"

extern "C" Gfx* Vr_WhiteBackgroundList(Gfx* overlay);

#include "soh/cvar_prefixes.h"
#include <libultraship/bridge/consolevariablebridge.h>
#include <fast/Fast3dWindow.h>
#include <fast/interpreter.h>
#include <ship/window/Window.h>
#include <ship/window/MouseStateManager.h>
#include <ship/window/gui/Gui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>
#include <dxgi1_3.h>
#include <d3d11.h>
#include <d3dcompiler.h>

// libultraship only defines this for its own sources; its DX11 headers are empty without it.
#ifndef ENABLE_DX11
#define ENABLE_DX11
#endif
#include <fast/backends/gfx_window_manager_api.h>
#include <fast/backends/gfx_direct3d_common.h>
#include <fast/backends/gfx_dxgi.h>

#define CVAR_VR_ENABLED CVAR_SETTING("VR.Enabled")
#define CVAR_VR_RUNTIME CVAR_SETTING("VR.Runtime")

namespace SohVr {

namespace {

constexpr auto kHeadsetRetryInterval = std::chrono::seconds(3);

bool sInitFailed = false;
bool sWaitingLogged = false;
std::chrono::steady_clock::time_point sRetryAt;

Fast::GfxRenderingAPIDX11* GetDx11(Fast::Interpreter* intp) {
    return dynamic_cast<Fast::GfxRenderingAPIDX11*>(intp->mRapi);
}

// The texture the interpreter just finished rendering into. Its framebuffer keeps it alive.
ID3D11Texture2D* FrameTexture(Fast::Interpreter* intp) {
    auto* view = reinterpret_cast<ID3D11ShaderResourceView*>(intp->mGfxFrameBuffer);
    if (view == nullptr) {
        return nullptr;
    }
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    resource.As(&texture);
    return texture.Get();
}

// Sets the size the next Run renders at. It must differ from the window's game view, or the interpreter draws
// straight to the window instead of into a framebuffer that can be copied to the headset.
void SetRenderSize(Fast::Interpreter* intp, uint32_t width, uint32_t height) {
    if (width == static_cast<uint32_t>(intp->mGameWindowViewport.width) &&
        height == static_cast<uint32_t>(intp->mGameWindowViewport.height)) {
        height -= 1;
    }
    intp->mCurDimensions.width = width;
    intp->mCurDimensions.height = height;
    intp->StartFrame();
}

// Draws the overlay into the HUD texture twice, over black and over white, to recover its transparency.
void DrawHud(Fast::Interpreter* intp, const std::unordered_map<Mtx*, MtxF>& mtxReplacements) {
    Runtime& runtime = Runtime::Get();
    Fast::GfxRenderingAPIDX11* dx11 = GetDx11(intp);
    Gfx* overlay = Game_Overlay();
    if (dx11 == nullptr || overlay == nullptr) {
        return;
    }
    ID3D11Device* device = dx11->mDevice.Get();
    ID3D11DeviceContext* context = dx11->mContext.Get();
    HudCompositor& compositor = HudCompositor::Get();

    // This is the frame's last render, so the next tick lays the HUD out for this 4:3 texture too.
    SetRenderSize(intp, runtime.GetHudWidth(), runtime.GetHudHeight());
    intp->Run(overlay, mtxReplacements);
    if (!compositor.CaptureBlack(device, context, FrameTexture(intp))) {
        return;
    }
    intp->Run(Vr_WhiteBackgroundList(overlay), mtxReplacements);
    auto* onWhite = reinterpret_cast<ID3D11ShaderResourceView*>(intp->mGfxFrameBuffer);
    runtime.SubmitHud(compositor.Compose(device, context, onWhite));
}

// Presents the window without its own frame pacing, since xrWaitFrame paces the game while in VR.
void PresentWindow(Fast::Interpreter* intp) {
    Fast::GfxRenderingAPIDX11* dx11 = GetDx11(intp);
    if (dx11 == nullptr || dx11->mWindowBackend == nullptr) {
        return;
    }
    if (IDXGISwapChain1* swapChain = dx11->mWindowBackend->GetSwapChain()) {
        swapChain->Present(0, 0);
    }
}

// The window shows what the left eye sees, cut to the window's shape around where that eye looks straight ahead.
// Otherwise it shows whatever the frame drew last (the HUD, or the right eye), stretched to fit.
struct Mirror {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    D3D11_TEXTURE2D_DESC desc = {};
};
Mirror sMirror;

// Copies the eye just rendered into the mirror. Returns whether the mirror holds it.
bool CopyToMirror(Fast::Interpreter* intp, const Fov& fov) {
    Fast::GfxRenderingAPIDX11* dx11 = GetDx11(intp);
    ID3D11Texture2D* eye = FrameTexture(intp);
    if (dx11 == nullptr || eye == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC eyeDesc;
    eye->GetDesc(&eyeDesc);
    // A region of a multisampled texture can't be copied out on its own.
    if (eyeDesc.SampleDesc.Count != 1) {
        return false;
    }
    float aspect = intp->mGameWindowViewport.height > 0
                       ? static_cast<float>(intp->mGameWindowViewport.width) / intp->mGameWindowViewport.height
                       : 16.0f / 9.0f;
    uint32_t width = eyeDesc.Width;
    uint32_t height = eyeDesc.Height;
    if (width > height * aspect) {
        width = std::max(1u, static_cast<uint32_t>(height * aspect));
    } else {
        height = std::max(1u, static_cast<uint32_t>(width / aspect));
    }
    if (sMirror.view == nullptr || sMirror.desc.Width != width || sMirror.desc.Height != height ||
        sMirror.desc.Format != eyeDesc.Format) {
        sMirror = {};
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = eyeDesc.Format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device* device = dx11->mDevice.Get();
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &sMirror.texture)) ||
            FAILED(device->CreateShaderResourceView(sMirror.texture.Get(), nullptr, &sMirror.view))) {
            sMirror = {};
            return false;
        }
        sMirror.desc = desc;
    }
    // Where the eye looks straight ahead in its image, from how far its view reaches each way.
    float left = tanf(fov.left);
    float right = tanf(fov.right);
    float up = tanf(fov.up);
    float down = tanf(fov.down);
    float centerX = eyeDesc.Width * -left / (right - left);
    float centerY = eyeDesc.Height * up / (up - down);
    UINT x = static_cast<UINT>(std::clamp(centerX - width / 2.0f, 0.0f, static_cast<float>(eyeDesc.Width - width)));
    UINT y = static_cast<UINT>(std::clamp(centerY - height / 2.0f, 0.0f, static_cast<float>(eyeDesc.Height - height)));
    D3D11_BOX box = { x, y, 0, x + width, y + height, 1 };
    dx11->mContext->CopySubresourceRegion(sMirror.texture.Get(), 0, 0, 0, 0, eye, 0, &box);
    return true;
}

// Starts or stops the runtime to match the setting. Returns whether a session is ready for frames.
bool UpdateRuntime(Fast::Interpreter* intp) {
    Runtime& runtime = Runtime::Get();
    if (!CVarGetInteger(CVAR_VR_ENABLED, 0)) {
        runtime.Shutdown();
        // Turning the setting off and on again retries after a failure.
        sInitFailed = false;
        sWaitingLogged = false;
        sRetryAt = {};
        return false;
    }
    if (sInitFailed) {
        return false;
    }
    if (runtime.IsLost()) {
        SPDLOG_INFO("[VR] The headset ended the session. Turn VR off and on again to restart it.");
        runtime.Shutdown();
        sInitFailed = true;
        return false;
    }
    if (!runtime.IsInitialized()) {
        auto now = std::chrono::steady_clock::now();
        if (now < sRetryAt) {
            return false;
        }
        Fast::GfxRenderingAPIDX11* dx11 = GetDx11(intp);
        if (dx11 == nullptr) {
            SPDLOG_ERROR("[VR] VR needs the DirectX 11 renderer (Settings > Graphics > Renderer API)");
            sInitFailed = true;
            return false;
        }
        auto choice = static_cast<RuntimeChoice>(CVarGetInteger(CVAR_VR_RUNTIME, RUNTIME_AUTO));
        switch (runtime.Init(dx11->mDevice.Get(), dx11->mContext.Get(), choice)) {
            case InitResult::Ok:
                sWaitingLogged = false;
                break;
            case InitResult::NoHeadset:
                if (!sWaitingLogged) {
                    SPDLOG_INFO("[VR] No headset found. Checking again every few seconds; start SteamVR or Quest "
                                "Link to begin.");
                    sWaitingLogged = true;
                }
                sRetryAt = now + kHeadsetRetryInterval;
                return false;
            case InitResult::Failed:
                sInitFailed = true;
                return false;
        }
    }
    runtime.PollEvents();
    return runtime.IsSessionRunning();
}

} // namespace

bool DrawFrame(Fast::Fast3dWindow* wnd, Gfx* commands, const std::unordered_map<Mtx*, MtxF>& mtxReplacements) {
    std::shared_ptr<Fast::Interpreter> intp = wnd->GetInterpreterWeak().lock();
    if (intp == nullptr || !UpdateRuntime(intp.get())) {
        return false;
    }
    Runtime& runtime = Runtime::Get();
    if (!runtime.BeginFrame()) {
        return false;
    }
    Game_OnFrame();

    auto gui = wnd->GetGui();
    wnd->GetMouseStateManager()->StartFrame();
    gui->StartDraw();

    bool stereo = Game_RenderStereo();
    bool mirrored = false;
    if (!runtime.ShouldRender()) {
        // The headset isn't showing this app; keep the window drawing as usual.
        intp->StartFrame();
        intp->Run(commands, mtxReplacements);
    } else if (stereo) {
        // The eyes get only the world; the HUD goes onto the wrists instead of stuck to the face.
        bool splitHud = Game_SplitHud();
        Game_CutOverlay(splitHud);
        SetRenderSize(intp.get(), runtime.GetEyeWidth(), runtime.GetEyeHeight());
        for (int eye = 0; eye < 2; eye++) {
            std::unordered_map<Mtx*, MtxF> replacements = mtxReplacements;
            Game_AddEyeMatrices(eye, intp->mInterpolationT, intp->mCurDimensions.width, intp->mCurDimensions.height,
                                replacements);
            intp->Run(commands, replacements);
            runtime.SubmitEye(eye, FrameTexture(intp.get()));
            if (eye == 0) {
                mirrored = CopyToMirror(intp.get(), runtime.GetEyes()[0].fov);
            }
        }
        Game_CutOverlay(false);
        if (splitHud) {
            DrawHud(intp.get(), mtxReplacements);
        }
    } else {
        SetRenderSize(intp.get(), runtime.GetScreenWidth(), runtime.GetScreenHeight());
        intp->Run(commands, mtxReplacements);
        runtime.SubmitScreen(FrameTexture(intp.get()));
    }
    std::vector<HudQuad> hud;
    if (stereo) {
        Game_GetHudQuads(hud);
    }
    runtime.EndFrame(stereo, hud);

    if (mirrored) {
        intp->mGfxFrameBuffer = reinterpret_cast<uintptr_t>(sMirror.view.Get());
    }
    gui->EndDraw();
    intp->mRapi->EndFrame();
    PresentWindow(intp.get());
    intp->mRapi->FinishRender();
    return true;
}

uint32_t GetRefreshRate() {
    return Runtime::Get().IsSessionRunning() ? Runtime::Get().GetRefreshRate() : 0;
}

void OnTickSubmitted() {
    Game_OnTickSubmitted();
}

void Shutdown() {
    sMirror = {};
    HudCompositor::Get().Shutdown();
    Runtime::Get().Shutdown();
}

} // namespace SohVr

#else

namespace SohVr {

bool DrawFrame(Fast::Fast3dWindow*, Gfx*, const std::unordered_map<Mtx*, MtxF>&) {
    return false;
}

uint32_t GetRefreshRate() {
    return 0;
}

void OnTickSubmitted() {
}

void Shutdown() {
}

} // namespace SohVr

#endif
