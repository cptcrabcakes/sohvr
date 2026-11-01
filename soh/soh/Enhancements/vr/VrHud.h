#pragma once

// The renderer has no transparent clear, so the HUD is drawn twice, over black and over white. Where the HUD
// is opaque both renders agree; where it's see-through they differ by exactly how see-through it is.

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;

namespace SohVr {

class HudCompositor {
  public:
    static HudCompositor& Get();

    // Keeps a copy of the HUD drawn over black.
    bool CaptureBlack(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* frame);
    // Combines that copy with the HUD drawn over white into a premultiplied-alpha texture. Null on failure.
    ID3D11Texture2D* Compose(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* onWhite);
    void Shutdown();

  private:
    HudCompositor() = default;
    struct Impl;
    Impl* mImpl = nullptr;
};

} // namespace SohVr
