#include "capture.h"
#include "log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

static const unsigned kWidth = 1280;  // wide enough to read road signs and the displays

// Request -> copy and shrink on the GPU (render thread) -> read back a frame or two later (a
// small image, one memcpy) -> the worker converts and encodes it. The render thread never
// touches the pixels itself: shrinking a full frame on the CPU cost it tens of milliseconds.
static std::atomic<bool>       g_wanted{false};
static std::mutex              g_mutex;
static std::condition_variable g_cv;
static bool                    g_ready = false;  // g_raw holds the answer to the last request
static std::vector<uint8_t>    g_raw;            // 4 bytes a pixel, rows packed, g_w x g_h (empty = failed)
static unsigned                g_w = 0, g_h = 0;

enum class Layout { RGBA8, BGRA8, RGB10A2, Unsupported };
static Layout                  g_layout = Layout::Unsupported;

// Render thread only.
static ID3D11Texture2D* g_staging = nullptr;
static Layout           g_staging_layout = Layout::Unsupported;
static unsigned         g_staging_w = 0, g_staging_h = 0;
static int              g_frames = 0;
static bool             g_format_warned = false;

static Layout layout_of(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return Layout::RGBA8;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return Layout::BGRA8;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM: return Layout::RGB10A2;
        default: return Layout::Unsupported;
    }
}

// Typeless formats can't be resolved or rendered to; pick the plain typed one.
static DXGI_FORMAT typed(DXGI_FORMAT f)
{
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:    return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        default:                               return f;
    }
}

static void deliver(std::vector<uint8_t> raw, unsigned w, unsigned h, Layout layout)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_raw = std::move(raw);
        g_w = w;
        g_h = h;
        g_layout = layout;
        g_ready = true;
    }
    g_cv.notify_all();
}

void capture_request()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ready = false;
        g_raw.clear();
    }
    g_wanted = true;
}

void capture_reset()
{
    if (!g_staging) return;
    g_staging->Release();
    g_staging = nullptr;
    deliver({}, 0, 0, Layout::Unsupported);
}

template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static double now_ms()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000.0 / freq.QuadPart;
}

static void capture_frame_impl(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc);

// Times what the screenshot costs the render thread (it should be well under a millisecond or two).
void capture_frame(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc)
{
    if (!g_staging && !g_wanted) return;
    const double t0 = now_ms();
    const bool reading = g_staging != nullptr;
    capture_frame_impl(dev, ctx, sc);
    const double ms = now_ms() - t0;
    if (ms > 3.0) log_info("assistant: screenshot %s took %.1f ms on the render thread", reading ? "read-back" : "copy", ms);
}

static void capture_frame_impl(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc)
{
    if (!g_staging) {
        if (!g_wanted.exchange(false)) return;
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) { deliver({}, 0, 0, Layout::Unsupported); return; }
        D3D11_TEXTURE2D_DESC td;
        bb->GetDesc(&td);
        const Layout layout = layout_of(td.Format);
        if (layout == Layout::Unsupported) {
            if (!g_format_warned) log_info("assistant: screenshots need an 8-bit picture (back buffer format %d)", (int)td.Format);
            g_format_warned = true;
            bb->Release();
            deliver({}, 0, 0, Layout::Unsupported);
            return;
        }
        // Shrink on the GPU: a mip chain halves the picture per level; read back the smallest
        // level that is still at least kWidth wide (1080p: full size; 1440p: 1280; 4K: 1920).
        const DXGI_FORMAT fmt = typed(td.Format);
        UINT level = 0;
        while ((td.Width >> (level + 1)) >= kWidth) ++level;
        UINT support = 0;
        dev->CheckFormatSupport(fmt, &support);
        const bool mips = level > 0 && (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) &&
                          (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);
        if (!mips) level = 0;

        ID3D11Texture2D* src = bb;
        ID3D11Texture2D* work = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        UINT src_level = 0;
        bool ok = true;
        if (mips || td.SampleDesc.Count > 1) {  // multisampled needs resolving first anyway
            D3D11_TEXTURE2D_DESC wd = {};
            wd.Width = td.Width;
            wd.Height = td.Height;
            wd.MipLevels = mips ? level + 1 : 1;
            wd.ArraySize = 1;
            wd.Format = fmt;
            wd.SampleDesc = {1, 0};
            wd.Usage = D3D11_USAGE_DEFAULT;
            wd.BindFlags = mips ? (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) : 0;
            wd.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
            if (SUCCEEDED(dev->CreateTexture2D(&wd, nullptr, &work))) {
                if (td.SampleDesc.Count > 1) ctx->ResolveSubresource(work, 0, bb, 0, fmt);
                else ctx->CopySubresourceRegion(work, 0, 0, 0, 0, bb, 0, nullptr);
                if (mips && SUCCEEDED(dev->CreateShaderResourceView(work, nullptr, &srv))) {
                    ctx->GenerateMips(srv);
                    src_level = level;
                }
                src = work;
            } else if (td.SampleDesc.Count > 1) {
                ok = false;
            }
        }
        if (ok) {
            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width = std::max(1u, td.Width >> src_level);
            sd.Height = std::max(1u, td.Height >> src_level);
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = src == bb ? td.Format : fmt;
            sd.SampleDesc = {1, 0};
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ok = SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &g_staging));
            if (ok) {
                ctx->CopySubresourceRegion(g_staging, 0, 0, 0, 0, src, src_level, nullptr);
                g_staging_w = sd.Width;
                g_staging_h = sd.Height;
                g_staging_layout = layout;
                g_frames = 0;
            }
        }
        release(srv);
        release(work);
        bb->Release();
        if (!ok) deliver({}, 0, 0, Layout::Unsupported);
        return;
    }

    // A frame or two later the GPU is done; after a few tries just wait for it.
    D3D11_MAPPED_SUBRESOURCE m;
    const HRESULT hr = ctx->Map(g_staging, 0, D3D11_MAP_READ, ++g_frames < 6 ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    std::vector<uint8_t> raw;
    if (SUCCEEDED(hr)) {
        const size_t row = (size_t)g_staging_w * 4;
        raw.resize(row * g_staging_h);
        for (unsigned y = 0; y < g_staging_h; ++y)
            memcpy(&raw[y * row], static_cast<const uint8_t*>(m.pData) + (size_t)y * m.RowPitch, row);
        ctx->Unmap(g_staging, 0);
    }
    release(g_staging);
    deliver(std::move(raw), g_staging_w, g_staging_h, g_staging_layout);
}

// Worker thread: to 24-bit BGR at most kWidth wide, averaging 2x2 samples per output pixel.
static std::vector<uint8_t> to_bgr(const std::vector<uint8_t>& raw, unsigned w, unsigned h, Layout layout,
                                   unsigned& ow, unsigned& oh)
{
    ow = std::min(kWidth, w);
    oh = std::max(1u, (unsigned)((uint64_t)h * ow / w));
    std::vector<uint8_t> out((size_t)ow * oh * 3);
    auto sample = [&](unsigned x, unsigned y, unsigned& r, unsigned& g, unsigned& b) {
        const uint8_t* p = &raw[((size_t)y * w + x) * 4];
        switch (layout) {
            case Layout::RGBA8: r += p[0]; g += p[1]; b += p[2]; break;
            case Layout::BGRA8: b += p[0]; g += p[1]; r += p[2]; break;
            default: {
                uint32_t v;
                memcpy(&v, p, 4);
                r += (v & 1023) >> 2;
                g += ((v >> 10) & 1023) >> 2;
                b += ((v >> 20) & 1023) >> 2;
            }
        }
    };
    for (unsigned oy = 0; oy < oh; ++oy) {
        const unsigned y0 = std::min(h - 1, (unsigned)(((uint64_t)oy * 4 + 1) * h / (oh * 4)));
        const unsigned y1 = std::min(h - 1, (unsigned)(((uint64_t)oy * 4 + 3) * h / (oh * 4)));
        uint8_t* dst = &out[(size_t)oy * ow * 3];
        for (unsigned ox = 0; ox < ow; ++ox) {
            const unsigned x0 = std::min(w - 1, (unsigned)(((uint64_t)ox * 4 + 1) * w / (ow * 4)));
            const unsigned x1 = std::min(w - 1, (unsigned)(((uint64_t)ox * 4 + 3) * w / (ow * 4)));
            unsigned r = 0, g = 0, b = 0;
            sample(x0, y0, r, g, b);
            sample(x1, y0, r, g, b);
            sample(x0, y1, r, g, b);
            sample(x1, y1, r, g, b);
            dst[ox * 3 + 0] = (uint8_t)(b / 4);
            dst[ox * 3 + 1] = (uint8_t)(g / 4);
            dst[ox * 3 + 2] = (uint8_t)(r / 4);
        }
    }
    return out;
}

// --- JPEG ---------------------------------------------------------------------------------------


static bool encode_jpeg(const std::vector<uint8_t>& bgr, unsigned w, unsigned h, std::vector<char>& out)
{
    IWICImagingFactory* factory = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
              SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(encoder->CreateNewFrame(&frame, &props));
    if (ok && props) {
        PROPBAG2 opt = {};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.8f;
        props->Write(1, &opt, &v);
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(frame->Initialize(props)) && SUCCEEDED(frame->SetSize(w, h)) &&
         SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
         SUCCEEDED(frame->WritePixels(h, w * 3, (UINT)bgr.size(), const_cast<BYTE*>(bgr.data()))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (ok) {
        STATSTG st = {};
        HGLOBAL mem = nullptr;
        ok = SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(stream, &mem));
        if (ok) {
            const void* p = GlobalLock(mem);
            ok = p != nullptr;
            if (ok) out.assign(static_cast<const char*>(p), static_cast<const char*>(p) + st.cbSize.LowPart);
            GlobalUnlock(mem);
        }
    }
    release(props);
    release(frame);
    release(encoder);
    release(stream);
    release(factory);
    return ok && !out.empty();
}

bool capture_wait_jpeg(std::vector<char>& jpeg, unsigned timeout_ms)
{
    jpeg.clear();
    std::vector<uint8_t> raw;
    unsigned w, h;
    Layout layout;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] { return g_ready; })) {
            g_wanted = false;  // nobody is drawing frames (minimized, loading...)
            return false;
        }
        raw.swap(g_raw);
        w = g_w;
        h = g_h;
        layout = g_layout;
        g_ready = false;
    }
    if (raw.empty() || !w || !h) return false;
    unsigned ow, oh;
    const std::vector<uint8_t> bgr = to_bgr(raw, w, h, layout, ow, oh);  // here, not on the render thread
    return encode_jpeg(bgr, ow, oh, jpeg);
}
