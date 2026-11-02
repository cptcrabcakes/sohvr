#include "VrHud.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <spdlog/spdlog.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace SohVr {

namespace {

// Over black a pixel is a*hud; over white it's a*hud + (1 - a). The difference gives a, and the black render
// is already the premultiplied color.
const char kComposeShader[] = R"(
Texture2D<float4> onBlack : register(t0);
Texture2D<float4> onWhite : register(t1);
RWTexture2D<float4> result : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    float3 black = onBlack[id.xy].rgb;
    float3 white = onWhite[id.xy].rgb;
    float alpha = saturate(1.0 - dot(white - black, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)));
    result[id.xy] = float4(black, alpha);
}
)";

} // namespace

struct HudCompositor::Impl {
    bool failed = false;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Texture2D> black;
    ComPtr<ID3D11ShaderResourceView> blackView;
    ComPtr<ID3D11Texture2D> result;
    ComPtr<ID3D11UnorderedAccessView> resultView;
    uint32_t width = 0;
    uint32_t height = 0;

    bool CreateShader(ID3D11Device* device);
    bool CreateTextures(ID3D11Device* device, uint32_t newWidth, uint32_t newHeight);
};

bool HudCompositor::Impl::CreateShader(ID3D11Device* device) {
    if (shader != nullptr) {
        return true;
    }
    // libultraship loads the same compiler at runtime instead of linking it.
    HMODULE compiler = LoadLibraryW(L"D3DCompiler_47.dll");
    auto compile = compiler != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (compile == nullptr) {
        SPDLOG_ERROR("[VR] D3DCompiler_47.dll not found; the HUD can't be shown");
        return false;
    }
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    if (FAILED(compile(kComposeShader, strlen(kComposeShader), "VrHudCompose", nullptr, nullptr, "main", "cs_5_0", 0,
                       0, &code, &errors))) {
        SPDLOG_ERROR("[VR] HUD shader failed to compile: {}",
                     errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    return SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
}

bool HudCompositor::Impl::CreateTextures(ID3D11Device* device, uint32_t newWidth, uint32_t newHeight) {
    if (black != nullptr && width == newWidth && height == newHeight) {
        return true;
    }
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = newWidth;
    desc.Height = newHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    black.Reset();
    blackView.Reset();
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &black)) ||
        FAILED(device->CreateShaderResourceView(black.Get(), nullptr, &blackView))) {
        return false;
    }

    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    result.Reset();
    resultView.Reset();
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &result)) ||
        FAILED(device->CreateUnorderedAccessView(result.Get(), nullptr, &resultView))) {
        return false;
    }
    width = newWidth;
    height = newHeight;
    return true;
}

HudCompositor& HudCompositor::Get() {
    static HudCompositor compositor;
    return compositor;
}

bool HudCompositor::CaptureBlack(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame) {
    if (mImpl == nullptr) {
        mImpl = new Impl();
    }
    if (mImpl->failed || frame == nullptr) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    frame->GetDesc(&desc);
    if (desc.SampleDesc.Count != 1 || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        return false;
    }
    if (!mImpl->CreateShader(device) || !mImpl->CreateTextures(device, desc.Width, desc.Height)) {
        mImpl->failed = true;
        return false;
    }
    context->CopyResource(mImpl->black.Get(), frame);
    return true;
}

ID3D11Texture2D* HudCompositor::Compose(ID3D11Device* device, ID3D11DeviceContext* context,
                                        ID3D11ShaderResourceView* onWhite) {
    if (mImpl == nullptr || mImpl->failed || mImpl->shader == nullptr || onWhite == nullptr) {
        return nullptr;
    }
    ID3D11ShaderResourceView* inputs[2] = { mImpl->blackView.Get(), onWhite };
    ID3D11UnorderedAccessView* output = mImpl->resultView.Get();
    context->CSSetShader(mImpl->shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 2, inputs);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((mImpl->width + 7) / 8, (mImpl->height + 7) / 8, 1);

    // Unbind so the game's framebuffer can be drawn into again.
    ID3D11ShaderResourceView* noInputs[2] = {};
    ID3D11UnorderedAccessView* noOutput = nullptr;
    context->CSSetShaderResources(0, 2, noInputs);
    context->CSSetUnorderedAccessViews(0, 1, &noOutput, nullptr);
    context->CSSetShader(nullptr, nullptr, 0);
    return mImpl->result.Get();
}

void HudCompositor::Shutdown() {
    delete mImpl;
    mImpl = nullptr;
}

} // namespace SohVr

#else

namespace SohVr {

struct HudCompositor::Impl {};

HudCompositor& HudCompositor::Get() {
    static HudCompositor compositor;
    return compositor;
}
bool HudCompositor::CaptureBlack(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*) {
    return false;
}
ID3D11Texture2D* HudCompositor::Compose(ID3D11Device*, ID3D11DeviceContext*, ID3D11ShaderResourceView*) {
    return nullptr;
}
void HudCompositor::Shutdown() {
}

} // namespace SohVr

#endif
