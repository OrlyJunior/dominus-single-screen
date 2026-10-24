#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <memory>

namespace dominus { struct ImageDiff; }
namespace dominus::gfx {
struct SourceRect { float x=0, y=0, width=1, height=1; };
enum class Mode : unsigned { Original=0, Gameplay=1, Map=2 };
// patch, if set, is copied at (patchLeft, patchTop) over the compositor's own
// copy of the frame before cropping; the game's back buffer is never written.
struct Request {
    Mode mode=Mode::Original; SourceRect source{}; bool integerScale=false;
    ID3D11Texture2D* patch=nullptr; unsigned patchLeft=0,patchTop=0;
};
bool valid_rect(const SourceRect&) noexcept;
// Synchronous CPU readback of `region` of `source`, sampled at the centres of a
// columns x rows grid into `output` (0x00RRGGBB, row-major). For rare decisions
// only: it waits for the GPU. Never writes `source`; false if unsupported.
bool sample_grid(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D* source, const SourceRect& region,
                 unsigned columns, unsigned rows, unsigned* output) noexcept;

// Per-frame readback of one panel without waiting for the GPU. capture()
// records a GPU copy of `region`; collect() returns the oldest finished one,
// sampled at DS pixel centres (NativeWidth x NativeHeight, 0x00RRGGBB). Right
// after a collect(), keep() can store that capture's full-resolution pixels;
// kept_image() returns them with the region's position, for Request::patch.
// Never writes the captured source.
class PanelProbe {
public:
    PanelProbe();
    ~PanelProbe();
    PanelProbe(const PanelProbe&)=delete;
    PanelProbe& operator=(const PanelProbe&)=delete;
    bool capture(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D* source, const SourceRect& region,
                 unsigned long long timeMs, unsigned long long identityEpoch = 0) noexcept;
    bool collect(ID3D11DeviceContext*, unsigned* image, unsigned long long& timeMs,
                 unsigned long long* identityEpoch = nullptr) noexcept;
    bool keep(ID3D11DeviceContext*, unsigned which) noexcept;
    bool kept(unsigned which) const noexcept;
    ID3D11Texture2D* kept_image(unsigned which, unsigned& left, unsigned& top) const noexcept;
    // Prepare only the changed DS-pixel union (+ one pixel guard band), using
    // the original captured pixels. Up to 16 slots; repeated slots are legal.
    // Allocates once per plan, never while selecting a replay frame.
    bool prepare_cycle(ID3D11DeviceContext*, const unsigned* slots, unsigned count, const ImageDiff&) noexcept;
    ID3D11Texture2D* cycle_image(unsigned slot, unsigned& left, unsigned& top) const noexcept;
    void release_cycle() noexcept;
    void invalidate_kept() noexcept;
    void discard_pending() noexcept;
    void reset() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// GPU compositor only. This class neither knows nor guesses which image is a map.
// The caller must supply a validated context and source rectangle.
class Presenter {
public:
    Presenter();
    ~Presenter();
    Presenter(const Presenter&)=delete;
    Presenter& operator=(const Presenter&)=delete;
    bool compose(ID3D11Device*, ID3D11DeviceContext*, ID3D11Texture2D*, const Request&);
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
