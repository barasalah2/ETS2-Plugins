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

static const unsigned kWidth = 768;  // plenty for "what's in the picture"; ~1100 image tokens

// Request -> copy (render thread) -> read back a frame or two later -> pixels for the worker.
static std::atomic<bool>       g_wanted{false};
static std::mutex              g_mutex;
static std::condition_variable g_cv;
static bool                    g_ready = false;  // g_pixels holds the answer to the last request
static std::vector<uint8_t>    g_pixels;         // 24-bit BGR, g_w x g_h (empty = failed)
static unsigned                g_w = 0, g_h = 0;

// Render thread only.
static ID3D11Texture2D* g_staging = nullptr;
static DXGI_FORMAT      g_format = DXGI_FORMAT_UNKNOWN;
static unsigned         g_src_w = 0, g_src_h = 0;
static int              g_frames = 0;
static bool             g_format_warned = false;

enum class Layout { RGBA8, BGRA8, RGB10A2, Unsupported };

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

// Typeless formats can't be resolved; pick the plain typed one.
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

static void deliver(std::vector<uint8_t> pixels, unsigned w, unsigned h)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pixels = std::move(pixels);
        g_w = w;
        g_h = h;
        g_ready = true;
    }
    g_cv.notify_all();
}

void capture_request()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_ready = false;
        g_pixels.clear();
    }
    g_wanted = true;
}

void capture_reset()
{
    if (!g_staging) return;
    g_staging->Release();
    g_staging = nullptr;
    deliver({}, 0, 0);
}

// Shrinks the mapped frame to kWidth, averaging 2x2 samples per output pixel.
static std::vector<uint8_t> downscale(const uint8_t* data, unsigned pitch, Layout layout, unsigned& ow, unsigned& oh)
{
    ow = std::min(kWidth, g_src_w);
    oh = std::max(1u, (unsigned)((uint64_t)g_src_h * ow / g_src_w));
    std::vector<uint8_t> out((size_t)ow * oh * 3);
    auto sample = [&](unsigned x, unsigned y, unsigned& r, unsigned& g, unsigned& b) {
        const uint8_t* p = data + (size_t)y * pitch + (size_t)x * 4;
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
        const unsigned y0 = std::min(g_src_h - 1, (unsigned)(((uint64_t)oy * 4 + 1) * g_src_h / (oh * 4)));
        const unsigned y1 = std::min(g_src_h - 1, (unsigned)(((uint64_t)oy * 4 + 3) * g_src_h / (oh * 4)));
        uint8_t* row = &out[(size_t)oy * ow * 3];
        for (unsigned ox = 0; ox < ow; ++ox) {
            const unsigned x0 = std::min(g_src_w - 1, (unsigned)(((uint64_t)ox * 4 + 1) * g_src_w / (ow * 4)));
            const unsigned x1 = std::min(g_src_w - 1, (unsigned)(((uint64_t)ox * 4 + 3) * g_src_w / (ow * 4)));
            unsigned r = 0, g = 0, b = 0;
            sample(x0, y0, r, g, b);
            sample(x1, y0, r, g, b);
            sample(x0, y1, r, g, b);
            sample(x1, y1, r, g, b);
            row[ox * 3 + 0] = (uint8_t)(b / 4);
            row[ox * 3 + 1] = (uint8_t)(g / 4);
            row[ox * 3 + 2] = (uint8_t)(r / 4);
        }
    }
    return out;
}

void capture_frame(ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc)
{
    if (!g_staging) {
        if (!g_wanted.exchange(false)) return;
        ID3D11Texture2D* bb = nullptr;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) { deliver({}, 0, 0); return; }
        D3D11_TEXTURE2D_DESC td;
        bb->GetDesc(&td);
        if (layout_of(td.Format) == Layout::Unsupported) {
            if (!g_format_warned) log_info("co-driver: screenshots need an 8-bit picture (back buffer format %d)", (int)td.Format);
            g_format_warned = true;
            bb->Release();
            deliver({}, 0, 0);
            return;
        }
        ID3D11Texture2D* src = bb;
        ID3D11Texture2D* resolved = nullptr;
        if (td.SampleDesc.Count > 1) {  // multisampled: resolve to a plain texture first
            D3D11_TEXTURE2D_DESC rd = td;
            rd.Format = typed(td.Format);
            rd.SampleDesc = {1, 0};
            rd.Usage = D3D11_USAGE_DEFAULT;
            rd.BindFlags = 0;
            rd.CPUAccessFlags = 0;
            rd.MiscFlags = 0;
            if (SUCCEEDED(dev->CreateTexture2D(&rd, nullptr, &resolved))) {
                ctx->ResolveSubresource(resolved, 0, bb, 0, rd.Format);
                src = resolved;
            }
        }
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Format = src == resolved ? typed(td.Format) : td.Format;
        sd.SampleDesc = {1, 0};
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        const bool ok = (src != bb || td.SampleDesc.Count == 1) && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &g_staging));
        if (ok) {
            ctx->CopyResource(g_staging, src);
            g_format = sd.Format;
            g_src_w = td.Width;
            g_src_h = td.Height;
            g_frames = 0;
        }
        if (resolved) resolved->Release();
        bb->Release();
        if (!ok) deliver({}, 0, 0);
        return;
    }

    // A frame or two later the copy is done; after a few tries just wait for it.
    D3D11_MAPPED_SUBRESOURCE m;
    const HRESULT hr = ctx->Map(g_staging, 0, D3D11_MAP_READ, ++g_frames < 6 ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    std::vector<uint8_t> pixels;
    unsigned w = 0, h = 0;
    if (SUCCEEDED(hr)) {
        pixels = downscale(static_cast<const uint8_t*>(m.pData), m.RowPitch, layout_of(g_format), w, h);
        ctx->Unmap(g_staging, 0);
    }
    g_staging->Release();
    g_staging = nullptr;
    deliver(std::move(pixels), w, h);
}

// --- JPEG ---------------------------------------------------------------------------------------

template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

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
    std::vector<uint8_t> pixels;
    unsigned w, h;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        if (!g_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] { return g_ready; })) {
            g_wanted = false;  // nobody is drawing frames (minimized, loading...)
            return false;
        }
        pixels.swap(g_pixels);
        w = g_w;
        h = g_h;
        g_ready = false;
    }
    return !pixels.empty() && encode_jpeg(pixels, w, h, jpeg);
}
