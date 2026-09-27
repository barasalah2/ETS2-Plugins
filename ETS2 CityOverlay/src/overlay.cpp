#include "overlay.h"
#include "capture.h"
#include "log.h"
#include "mic.h"
#include "route.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

// ---------------------------------------------------------------------------------------------
// Shared state (written by telemetry callbacks, read by the render thread)

static std::mutex g_state_mutex;
static Location   g_loc;
static double     g_loc_changed_at = -1e9;  // seconds (QPC clock)
static bool       g_paused = true;
static AlertState g_alerts;
static StripState g_strip;
static Guidance   g_guidance;
static HudState   g_hud;
static GuideCard  g_guide;
static double     g_guide_at = -1e9;

static Config g_cfg;

static double now_seconds()
{
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

void overlay_set_location(const Location& loc)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (loc.kind != g_loc.kind || loc.id != g_loc.id) g_loc_changed_at = now_seconds();
    g_loc = loc;
}

void overlay_set_alerts(const AlertState& alerts)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_alerts = alerts;
}

void overlay_set_strip(const StripState& strip)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_strip = strip;
}

void overlay_set_hud(const HudState& hud)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_hud = hud;
}

void overlay_set_guidance(const Guidance& g)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_guidance = g;
}

void overlay_set_guide(const GuideCard& card)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_guide = card;
    g_guide_at = now_seconds();
}

void overlay_set_paused(bool paused)
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_paused = paused;
}

// ---------------------------------------------------------------------------------------------
// D3D11 / ImGui state (render thread only, plus uninstall after hooks are drained)

using PresentFn       = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn      = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn       g_orig_present = nullptr;
static Present1Fn      g_orig_present1 = nullptr;
static ResizeBuffersFn g_orig_resize = nullptr;
static void*           g_target_present = nullptr;
static void*           g_target_present1 = nullptr;
static void*           g_target_resize = nullptr;

static std::atomic<bool> g_active{false};
static std::atomic<int>  g_inflight{0};

static IDXGISwapChain*         g_swapchain = nullptr;   // not ref-counted; identity only
static IDXGISwapChain*         g_rejected = nullptr;    // swapchain that is not D3D11
static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static float                   g_bb_width = 0, g_bb_height = 0;
static bool                    g_imgui_ctx = false;
static bool                    g_imgui_dx11 = false;
static ImFont*                 g_font = nullptr;
static double                  g_last_frame = 0;
static bool                    g_visible = true;
static bool                    g_key_was_down = false;
static bool                    g_strip_visible = true;
static bool                    g_strip_key_was_down = false;
static bool                    g_guide_key_was_down = false;

template <class T> static void safe_release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static std::string to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static void load_font()
{
    ImGuiIO& io = ImGui::GetIO();
    // ImGui 1.92+ rasterizes glyphs on demand, so any script the font covers
    // (accented Latin, Cyrillic, Greek, Turkish) works without glyph ranges.
    const std::wstring candidates[] = {g_cfg.font_path, L"C:\\Windows\\Fonts\\segoeui.ttf",
                                       L"C:\\Windows\\Fonts\\arial.ttf"};
    for (const auto& path : candidates) {
        if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        g_font = io.Fonts->AddFontFromFileTTF(to_utf8(path).c_str(), g_cfg.font_size);
        if (g_font) { log_info("font: %s", to_utf8(path).c_str()); return; }
    }
    g_font = io.Fonts->AddFontDefault();
    log_warn("no TrueType font found, using ImGui default (ASCII only)");
}

static void create_rtv(IDXGISwapChain* sc)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) return;
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    g_bb_width = (float)td.Width;
    g_bb_height = (float)td.Height;

    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    switch (td.Format) {  // typeless back buffers need an explicit view format
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:    rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        default:                               rd.Format = td.Format; break;
    }
    rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_device->CreateRenderTargetView(bb, &rd, &g_rtv)))
        log_warn("CreateRenderTargetView failed (back buffer format %d)", (int)td.Format);
    bb->Release();
}

static void shutdown_renderer()
{
    capture_reset();
    if (g_imgui_dx11) { ImGui_ImplDX11_Shutdown(); g_imgui_dx11 = false; }
    safe_release(g_rtv);
    safe_release(g_context);
    safe_release(g_device);
    g_swapchain = nullptr;
}

static bool ensure_renderer(IDXGISwapChain* sc, const char* via)
{
    if (sc == g_swapchain && g_device) return true;
    if (sc == g_rejected) return false;

    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&dev)))) {
        g_rejected = sc;
        log_warn("swapchain %p is not DirectX 11; overlay needs the game's dx11 renderer", (void*)sc);
        return false;
    }
    if (dev != g_device) {
        shutdown_renderer();
        g_device = dev;
        g_device->GetImmediateContext(&g_context);
        if (!g_imgui_ctx) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
            load_font();
            g_imgui_ctx = true;
        }
        g_imgui_dx11 = ImGui_ImplDX11_Init(g_device, g_context);
        if (!g_imgui_dx11) log_error("ImGui_ImplDX11_Init failed");
    } else {
        dev->Release();
        safe_release(g_rtv);
    }
    g_swapchain = sc;
    log_info("drawing on swapchain %p (hooked %s)", (void*)sc, via);
    return g_imgui_dx11;
}

static void poll_hotkey()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool focused = pid == GetCurrentProcessId();
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool down = focused && (GetAsyncKeyState(g_cfg.toggle_key) & 0x8000) && (!g_cfg.toggle_ctrl || ctrl);
    if (down && !g_key_was_down) g_visible = !g_visible;
    g_key_was_down = down;
    const bool strip_down = focused && ctrl && (GetAsyncKeyState(g_cfg.strip_key) & 0x8000);
    if (strip_down && !g_strip_key_was_down) g_strip_visible = !g_strip_visible;
    g_strip_key_was_down = strip_down;
    // Co-driver key: the mic module tells a tap ("about here") from holding it to talk.
    const bool guide_down = g_cfg.guide && focused && ctrl && (GetAsyncKeyState(g_cfg.guide_key) & 0x8000);
    if (guide_down != g_guide_key_was_down) mic_hold(guide_down);
    g_guide_key_was_down = guide_down;
}

// Panel opacity: full for banner_seconds after the location changes, then settles to idle.
static float panel_alpha(const Location& loc, double since_change)
{
    if (loc.kind == Location::None) return 0.0f;
    const float idle = g_cfg.always_show ? g_cfg.idle_opacity : 0.0f;
    if (loc.kind == Location::Near) return g_cfg.show_nearby ? std::min(g_cfg.idle_opacity, 0.85f) : 0.0f;
    const float t = (float)since_change;
    if (t < 0.35f) return t / 0.35f;               // fade in
    if (t < g_cfg.banner_seconds) return 1.0f;      // banner
    const float f = std::min(1.0f, (t - g_cfg.banner_seconds) / 1.0f);
    return 1.0f + (idle - 1.0f) * f;                // settle to idle
}

static void draw_text_shadowed(ImDrawList* dl, float size, ImVec2 pos, ImU32 col, float alpha, const char* text)
{
    const float o = std::max(1.0f, size * 0.05f);
    dl->AddText(g_font, size, ImVec2(pos.x + o, pos.y + o), IM_COL32(0, 0, 0, (int)(160 * alpha)), text);
    dl->AddText(g_font, size, pos, col, text);
}

// Returns the bottom edge of the panel so the alerts can stack below it.
static float draw_panel(const Location& loc, float alpha)
{
    const float scale = g_bb_height / 1080.0f;
    const float big = g_cfg.font_size * scale;
    const float sub_size = big * 0.52f;
    const float pad = sub_size * 0.9f;

    std::string title, subtitle;
    float title_size = big;
    if (loc.kind == Location::InCity) {
        title = loc.name;
        subtitle = loc.country;
    } else {
        char km[32];
        snprintf(km, sizeof(km), "%.0f km", loc.km);
        title = "Near " + loc.name;
        subtitle = km;
        title_size = big * 0.72f;
    }

    const ImVec2 ts = g_font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, title.c_str());
    const ImVec2 ss = subtitle.empty() ? ImVec2(0, 0) : g_font->CalcTextSizeA(sub_size, FLT_MAX, 0.0f, subtitle.c_str());
    const float w = std::max(ts.x, ss.x) + pad * 2.0f;
    const float h = ts.y + (subtitle.empty() ? 0.0f : ss.y) + pad * 1.2f;
    const ImVec2 p0((g_bb_width - w) * 0.5f, g_bb_height * g_cfg.position_y);
    const ImVec2 p1(p0.x + w, p0.y + h);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float r = sub_size * 0.5f;
    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, (int)(175 * alpha)), r);
    // ETS2-style amber accent along the bottom edge
    dl->AddRectFilled(ImVec2(p0.x + r, p1.y - 3.0f * scale), ImVec2(p1.x - r, p1.y),
                      IM_COL32(255, 176, 32, (int)(230 * alpha)));

    const float ty = p0.y + pad * 0.5f;
    draw_text_shadowed(dl, title_size, ImVec2(p0.x + (w - ts.x) * 0.5f, ty),
                       IM_COL32(255, 255, 255, (int)(255 * alpha)), alpha, title.c_str());
    if (!subtitle.empty())
        draw_text_shadowed(dl, sub_size, ImVec2(p0.x + (w - ss.x) * 0.5f, ty + ts.y),
                           IM_COL32(210, 214, 220, (int)(235 * alpha)), alpha, subtitle.c_str());
    return p1.y;
}

// Arrow in a circle pointing at rel_deg (0 = up = straight ahead, +90 = right).
static void draw_arrow(ImDrawList* dl, ImVec2 c, float r, float rel_deg, ImU32 col, bool ring = true)
{
    if (ring) dl->AddCircle(c, r, IM_COL32(255, 255, 255, 90), 0, std::max(1.0f, r * 0.08f));
    const float a = rel_deg * 3.14159265f / 180.0f;
    auto rot = [&](float x, float y) {  // y up in arrow space
        return ImVec2(c.x + x * std::cos(a) + y * std::sin(a), c.y - (y * std::cos(a) - x * std::sin(a)));
    };
    const float s = r * 0.72f;
    dl->AddTriangleFilled(rot(0, s), rot(-s * 0.62f, -s * 0.7f), rot(0, -s * 0.28f), col);
    dl->AddTriangleFilled(rot(0, s), rot(0, -s * 0.28f), rot(s * 0.62f, -s * 0.7f), col);
}

static std::string direction_words(const Target& t)
{
    const float a = std::fabs(t.rel_deg);
    if (a <= 25.0f) return "ahead";
    if (a >= 110.0f) return "behind you";
    return t.rel_deg > 0 ? "to the right" : "to the left";
}

static std::string format_km(double km)
{
    char buf[32];
    snprintf(buf, sizeof(buf), km < 10 ? "%.1f km" : "%.0f km", km);
    return buf;
}

// One warning box: coloured headline, then an arrow + where to go.
static float draw_alert(float top, const std::string& headline, const std::string& detail, bool critical,
                        const Target& target, const char* none_text, double now)
{
    const float scale = g_bb_height / 1080.0f;
    const float head = g_cfg.font_size * scale * 0.62f;
    const float body = g_cfg.font_size * scale * 0.55f;
    const float pad = body * 0.75f;
    const float arrow_r = body * 0.75f;

    // Along a route the straight-line bearing says little (the road bends), so no arrow there.
    const bool arrow = target.valid && !target.on_route;
    std::string where = !target.valid  ? std::string(none_text)
                      : target.on_route ? target.label + " in " + format_km(target.km) + " on your route"
                      : target.by_road  ? target.label + " " + format_km(target.km) + " by road, " + direction_words(target)
                                        : target.label + " " + format_km(target.km) + " " + direction_words(target);
    const std::string line1 = detail.empty() ? headline : headline + "   " + detail;

    const ImVec2 s1 = g_font->CalcTextSizeA(head, FLT_MAX, 0.0f, line1.c_str());
    const ImVec2 s2 = g_font->CalcTextSizeA(body, FLT_MAX, 0.0f, where.c_str());
    const float row2 = std::max(s2.y, arrow_r * 2.0f);
    const float w = std::max(s1.x, s2.x + (arrow ? arrow_r * 2.0f + pad * 0.6f : 0.0f)) + pad * 2.0f;
    const float h = s1.y + row2 + pad * 1.4f;
    const ImVec2 p0((g_bb_width - w) * 0.5f, top);
    const ImVec2 p1(p0.x + w, p0.y + h);

    // Critical warnings pulse so they catch the eye.
    const float pulse = critical ? 0.55f + 0.45f * (float)std::fabs(std::sin(now * 3.0)) : 1.0f;
    const ImU32 accent = critical ? IM_COL32(235, 64, 52, (int)(255 * pulse)) : IM_COL32(255, 176, 32, 255);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float r = body * 0.45f;
    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 200), r);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + r), ImVec2(p0.x + 4.0f * scale, p1.y - r), accent);

    float y = p0.y + pad * 0.5f;
    draw_text_shadowed(dl, head, ImVec2(p0.x + (w - s1.x) * 0.5f, y), accent, 1.0f, line1.c_str());
    y += s1.y + pad * 0.3f;

    const float content = s2.x + (arrow ? arrow_r * 2.0f + pad * 0.6f : 0.0f);
    float x = p0.x + (w - content) * 0.5f;
    if (arrow) {
        draw_arrow(dl, ImVec2(x + arrow_r, y + row2 * 0.5f), arrow_r, target.rel_deg, IM_COL32(255, 255, 255, 240));
        x += arrow_r * 2.0f + pad * 0.6f;
    }
    draw_text_shadowed(dl, body, ImVec2(x, y + (row2 - s2.y) * 0.5f), IM_COL32(235, 238, 242, 255), 1.0f, where.c_str());
    return p1.y;
}

// Arrow angle for a maneuver, degrees (0 = straight on, + = right).
static float turn_angle(const Turn& t)
{
    switch (t.turn) {
        case 1: return -40; case 11: return 40;
        case 2: return -90; case 12: return 90;
        case 3: return -135; case 13: return 135;
        case 4: return -180; case 14: return 180;
        case 21: return 135; case 22: return 90; case 23: return 45; case 24: return 0;
        case 25: return -45; case 26: return -90; case 27: return -135; case 28: return 180;
        default: return 0;
    }
}

// Next exit/turn: arrow, distance and instruction, and the lanes as the driver sees them.
static float draw_guidance(const Guidance& g, float top)
{
    const float scale = g_bb_height / 1080.0f;
    const float head = g_cfg.font_size * scale * 0.62f;
    const float body = g_cfg.font_size * scale * 0.5f;
    const float pad = body * 0.8f;
    const float arrow_r = head * 0.85f;

    std::string dist = format_km(g.km);
    const std::string line = dist + "   " + g.text;
    const ImVec2 ls = g_font->CalcTextSizeA(head, FLT_MAX, 0.0f, line.c_str());
    const ImVec2 ts = g.then_text.empty() ? ImVec2(0, 0) : g_font->CalcTextSizeA(body, FLT_MAX, 0.0f, g.then_text.c_str());
    const int lanes = (g.turn.lanes >= 2 && g.turn.lanes <= 8) ? g.turn.lanes : 0;
    const float lane_w = body * 1.25f, lane_h = body * 1.55f, lane_gap = body * 0.3f;
    const float lanes_w = lanes ? lanes * lane_w + (lanes - 1) * lane_gap : 0.0f;
    const float text_w = std::max(ls.x, ts.x);
    const float w = arrow_r * 2.0f + pad * 0.8f + text_w + pad * 2.0f;
    const float w_total = std::max(w, lanes_w + pad * 2.0f);
    const float h = std::max(arrow_r * 2.0f, ls.y + ts.y) + (lanes ? lane_h + pad * 0.6f : 0.0f) + pad * 1.2f;
    const ImVec2 p0((g_bb_width - w_total) * 0.5f, top), p1(p0.x + w_total, p0.y + h);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 205), body * 0.45f);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + body * 0.45f), ImVec2(p0.x + 4.0f * scale, p1.y - body * 0.45f),
                      IM_COL32(80, 170, 255, 255));

    const float row_h = std::max(arrow_r * 2.0f, ls.y + ts.y);
    float x = p0.x + (w_total - w) * 0.5f + pad;
    const float cy = p0.y + pad * 0.6f + row_h * 0.5f;
    if (g.turn.flags & 2)  // roundabout: ring behind the arrow
        dl->AddCircle(ImVec2(x + arrow_r, cy), arrow_r * 0.55f, IM_COL32(255, 255, 255, 140), 0, std::max(1.0f, arrow_r * 0.12f));
    draw_arrow(dl, ImVec2(x + arrow_r, cy), arrow_r, turn_angle(g.turn), IM_COL32(255, 255, 255, 250));
    x += arrow_r * 2.0f + pad * 0.8f;
    const float ty = cy - (ls.y + ts.y) * 0.5f;
    draw_text_shadowed(dl, head, ImVec2(x, ty), IM_COL32(255, 255, 255, 255), 1.0f, line.c_str());
    if (!g.then_text.empty())
        draw_text_shadowed(dl, body, ImVec2(x, ty + ls.y), IM_COL32(190, 200, 212, 255), 1.0f, g.then_text.c_str());

    if (lanes) {  // left to right as seen from the cab
        float lx = p0.x + (w_total - lanes_w) * 0.5f;
        const float ly = p0.y + pad * 0.6f + row_h + pad * 0.5f;
        for (int i = 0; i < lanes; ++i) {
            const int bit = g.turn.lht ? lanes - 1 - i : i;
            const bool ok = (g.turn.mask >> bit) & 1u;
            const ImVec2 a(lx, ly), b(lx + lane_w, ly + lane_h);
            if (ok) dl->AddRectFilled(a, b, IM_COL32(80, 170, 255, 235), body * 0.18f);
            else dl->AddRect(a, b, IM_COL32(255, 255, 255, 70), body * 0.18f, 0, std::max(1.0f, scale * 1.5f));
            const float ang = ok ? turn_angle(g.turn) * 0.5f : 0.0f;
            draw_arrow(dl, ImVec2(lx + lane_w * 0.5f, ly + lane_h * 0.5f), lane_w * 0.42f, ang,
                       ok ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 80), false);
            lx += lane_w + lane_gap;
        }
    }
    return p1.y;
}

// The tour guide's words, on the left, for about as long as they take to read (then fade).
static void draw_guide(const GuideCard& c, double age)
{
    const double words = (double)std::count(c.text.begin(), c.text.end(), ' ') + 1;
    const double show_for = c.text.empty() ? 45.0 : 10.0 + words * 0.45;  // "Thinking..." until the answer
    if (age > show_for + 1.0) return;
    const float alpha = age > show_for ? (float)(show_for + 1.0 - age) : 1.0f;
    const std::string& body_text = c.text.empty() ? c.status : c.text;
    const float scale = g_bb_height / 1080.0f;
    const float title_size = g_cfg.font_size * scale * 0.56f;
    const float body = g_cfg.font_size * scale * 0.5f;
    const float pad = body * 0.8f;
    const float wrap = g_bb_width * 0.24f;
    const ImVec2 ts = g_font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, c.title.c_str());
    const ImVec2 bs = g_font->CalcTextSizeA(body, FLT_MAX, wrap, body_text.c_str());
    const float w = std::max(ts.x, bs.x) + pad * 2.0f;
    const float h = ts.y + bs.y + pad * 1.6f;
    const ImVec2 p0(g_bb_width * 0.015f, g_bb_height * 0.30f), p1(p0.x + w, p0.y + h);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, (int)(195 * alpha)), body * 0.45f);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + body * 0.45f), ImVec2(p0.x + 4.0f * scale, p1.y - body * 0.45f),
                      IM_COL32(120, 200, 120, (int)(235 * alpha)));
    draw_text_shadowed(dl, title_size, ImVec2(p0.x + pad, p0.y + pad * 0.6f), IM_COL32(255, 255, 255, (int)(255 * alpha)),
                       alpha, c.title.c_str());
    const ImU32 body_col = c.text.empty() ? IM_COL32(150, 210, 150, (int)(255 * alpha)) : IM_COL32(225, 230, 236, (int)(255 * alpha));
    dl->AddText(g_font, body, ImVec2(p0.x + pad, p0.y + pad * 0.9f + ts.y), body_col, body_text.c_str(), nullptr, wrap);
}

static void draw_alerts(const AlertState& a, float top, double now)
{
    const float gap = g_bb_height * 0.008f;
    for (const Alert* al : {&a.fuel, &a.rest})
        if (al->active)
            top = draw_alert(top + gap, al->headline, al->detail, al->critical, al->target, al->none_text.c_str(), now);
}

// --- upcoming-stops strip ---------------------------------------------------------------------

static void icon_fuel(ImDrawList* dl, ImVec2 c, float s, ImU32 col)
{
    const float w = s * 0.5f, h = s * 0.78f;
    const ImVec2 b0(c.x - s * 0.36f, c.y - h * 0.5f), b1(b0.x + w, b0.y + h);
    dl->AddRectFilled(b0, b1, col, s * 0.08f);
    dl->AddRectFilled(ImVec2(b0.x + w * 0.2f, b0.y + h * 0.14f), ImVec2(b1.x - w * 0.2f, b0.y + h * 0.4f),
                      IM_COL32(20, 22, 26, 255));
    dl->AddBezierCubic(ImVec2(b1.x, b0.y + h * 0.22f), ImVec2(b1.x + s * 0.3f, b0.y + h * 0.22f),
                       ImVec2(b1.x + s * 0.3f, b1.y - h * 0.12f), ImVec2(b1.x + s * 0.12f, b1.y - h * 0.08f), col,
                       s * 0.09f);
}

static void icon_parking(ImDrawList* dl, ImVec2 c, float s)
{
    const float h = s * 0.42f;
    dl->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), IM_COL32(32, 96, 200, 255), s * 0.12f);
    const float fs = s * 0.8f;
    const ImVec2 ts = g_font->CalcTextSizeA(fs, FLT_MAX, 0.0f, "P");
    dl->AddText(g_font, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), "P");
}

static void icon_city(ImDrawList* dl, ImVec2 c, float s)
{
    dl->AddCircle(c, s * 0.3f, IM_COL32(230, 232, 236, 255), 0, std::max(1.0f, s * 0.1f));
    dl->AddCircleFilled(c, s * 0.12f, IM_COL32(230, 232, 236, 255));
}

static void icon_ferry(ImDrawList* dl, ImVec2 c, float s)
{
    const ImU32 col = IM_COL32(90, 200, 230, 255);
    const ImVec2 hull[4] = {ImVec2(c.x - s * 0.42f, c.y - s * 0.02f), ImVec2(c.x + s * 0.42f, c.y - s * 0.02f),
                            ImVec2(c.x + s * 0.28f, c.y + s * 0.22f), ImVec2(c.x - s * 0.28f, c.y + s * 0.22f)};
    dl->AddConvexPolyFilled(hull, 4, col);
    dl->AddRectFilled(ImVec2(c.x - s * 0.16f, c.y - s * 0.3f), ImVec2(c.x + s * 0.12f, c.y - s * 0.02f), col);
    dl->AddLine(ImVec2(c.x - s * 0.45f, c.y + s * 0.36f), ImVec2(c.x + s * 0.45f, c.y + s * 0.36f), col,
                std::max(1.0f, s * 0.08f));
}

static void icon_flag(ImDrawList* dl, ImVec2 c, float s)
{
    const float q = s * 0.2f;
    const ImVec2 o(c.x - q * 1.5f, c.y - q * 1.5f);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            dl->AddRectFilled(ImVec2(o.x + i * q, o.y + j * q), ImVec2(o.x + (i + 1) * q, o.y + (j + 1) * q),
                              (i + j) % 2 ? IM_COL32(20, 22, 26, 255) : IM_COL32(245, 245, 245, 255));
}

static void icon_border(ImDrawList* dl, ImVec2 c, float s)
{
    const float w = s * 0.62f, h = s * 0.42f;
    const ImVec2 p0(c.x - w * 0.5f + s * 0.06f, c.y - h * 0.62f);
    dl->AddLine(ImVec2(p0.x - s * 0.06f, p0.y - s * 0.04f), ImVec2(p0.x - s * 0.06f, c.y + s * 0.4f),
                IM_COL32(230, 232, 236, 255), std::max(1.0f, s * 0.08f));
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h * 0.5f), IM_COL32(40, 110, 220, 255));
    dl->AddRectFilled(ImVec2(p0.x, p0.y + h * 0.5f), ImVec2(p0.x + w, p0.y + h), IM_COL32(250, 210, 40, 255));
}

static std::string format_hm(float minutes)
{
    const int m = std::max(0, (int)std::lround(minutes));
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", m / 60, m % 60);
    return b;
}

static void draw_strip(const StripState& st)
{
    const float scale = g_bb_height / 1080.0f;
    const float body = g_cfg.font_size * scale * 0.5f;
    const float title_size = g_cfg.font_size * scale * 0.56f;
    const float pad = body * 0.8f;
    const float row_h = body * 1.55f;
    const float icon = body * 1.25f;
    const float gap = body * 0.6f;

    // Column widths from the content.
    float label_w = 0, km_w = 0, time_w = 0;
    std::vector<std::string> kms, times;
    for (const auto& it : st.items) {
        kms.push_back(format_km(it.km));
        times.push_back(format_hm(it.minutes));
        label_w = std::max(label_w, g_font->CalcTextSizeA(body, FLT_MAX, 0.0f, it.label.c_str()).x);
        km_w = std::max(km_w, g_font->CalcTextSizeA(body, FLT_MAX, 0.0f, kms.back().c_str()).x);
        time_w = std::max(time_w, g_font->CalcTextSizeA(body * 0.85f, FLT_MAX, 0.0f, times.back().c_str()).x);
    }
    const float icons_w = icon * 2.0f;  // room for two icons (truck stop = fuel + parking)
    const ImVec2 t_size = g_font->CalcTextSizeA(title_size, FLT_MAX, 0.0f, st.title.c_str());
    const ImVec2 s_size = g_font->CalcTextSizeA(body * 0.9f, FLT_MAX, 0.0f, st.summary.c_str());
    const ImVec2 n_size =
        st.note.empty() ? ImVec2(0, 0) : g_font->CalcTextSizeA(body * 0.8f, FLT_MAX, 0.0f, st.note.c_str());
    const float rows_w = icons_w + gap + km_w + gap + label_w + gap + time_w;
    const float w = std::max({rows_w, t_size.x, s_size.x, n_size.x}) + pad * 2.0f;
    const float header_h = t_size.y + s_size.y + (st.note.empty() ? 0.0f : n_size.y) + pad * 0.6f;
    const float h = pad * 0.7f + header_h + row_h * st.items.size() + pad * 0.5f;

    const float right = g_bb_width * g_cfg.strip_x;
    const ImVec2 p0(right - w, g_bb_height * g_cfg.strip_y), p1(right, p0.y + h);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 190), body * 0.45f);
    dl->AddRectFilled(ImVec2(p0.x, p0.y + body * 0.45f), ImVec2(p0.x + 4.0f * scale, p1.y - body * 0.45f),
                      IM_COL32(255, 176, 32, 230));

    float y = p0.y + pad * 0.7f;
    draw_text_shadowed(dl, title_size, ImVec2(p0.x + pad, y), IM_COL32(255, 255, 255, 255), 1.0f, st.title.c_str());
    y += t_size.y;
    draw_text_shadowed(dl, body * 0.9f, ImVec2(p0.x + pad, y), IM_COL32(200, 205, 212, 255), 1.0f,
                       st.summary.c_str());
    y += s_size.y;
    if (!st.note.empty()) {
        draw_text_shadowed(dl, body * 0.8f, ImVec2(p0.x + pad, y), IM_COL32(255, 150, 120, 255), 1.0f,
                           st.note.c_str());
        y += n_size.y;
    }
    y += pad * 0.6f;

    for (size_t i = 0; i < st.items.size(); ++i) {
        const StripItem& it = st.items[i];
        const float cy = y + row_h * 0.5f;
        if (it.tone == 1 || it.tone == 2) {  // recommended / urgent stop
            const ImU32 bg = it.tone == 2 ? IM_COL32(170, 40, 30, 120) : IM_COL32(200, 130, 20, 90);
            dl->AddRectFilled(ImVec2(p0.x + 6.0f * scale, y + 1), ImVec2(p1.x - 4.0f * scale, y + row_h - 1), bg,
                              body * 0.3f);
        } else if (i > 0) {
            dl->AddLine(ImVec2(p0.x + pad, y), ImVec2(p1.x - pad, y), IM_COL32(255, 255, 255, 22));
        }
        float x = p0.x + pad;
        const ImU32 fuel_col = IM_COL32(255, 176, 32, 255);
        const ImVec2 c1(x + icon * 0.5f, cy);
        if ((it.kinds & RouteStop::Fuel) && (it.kinds & RouteStop::Rest)) {
            icon_fuel(dl, c1, icon, fuel_col);
            icon_parking(dl, ImVec2(x + icon * 1.5f, cy), icon);
        } else if (it.kinds & RouteStop::Fuel) {
            icon_fuel(dl, c1, icon, fuel_col);
        } else if (it.kinds & RouteStop::Rest) {
            icon_parking(dl, c1, icon);
        } else if (it.kinds & RouteStop::Ferry) {
            icon_ferry(dl, c1, icon);
        } else if (it.kinds & RouteStop::Border) {
            icon_border(dl, c1, icon);
        } else if (it.kinds & RouteStop::Destination) {
            icon_flag(dl, c1, icon);
        } else {
            icon_city(dl, c1, icon);
        }
        x += icons_w + gap;

        const ImU32 km_col = it.tone == 2   ? IM_COL32(255, 120, 100, 255)
                             : it.tone == 1 ? IM_COL32(255, 196, 80, 255)
                             : it.tone == 3 ? IM_COL32(130, 135, 142, 255)   // can't make it there in time
                                            : IM_COL32(235, 238, 242, 255);
        const ImVec2 ks = g_font->CalcTextSizeA(body, FLT_MAX, 0.0f, kms[i].c_str());
        draw_text_shadowed(dl, body, ImVec2(x + km_w - ks.x, cy - ks.y * 0.5f), km_col, 1.0f, kms[i].c_str());
        x += km_w + gap;
        const ImVec2 ls = g_font->CalcTextSizeA(body, FLT_MAX, 0.0f, it.label.c_str());
        draw_text_shadowed(dl, body, ImVec2(x, cy - ls.y * 0.5f),
                           it.tone == 3 ? IM_COL32(130, 135, 142, 255) : IM_COL32(235, 238, 242, 255), 1.0f,
                           it.label.c_str());
        const ImVec2 tsz = g_font->CalcTextSizeA(body * 0.85f, FLT_MAX, 0.0f, times[i].c_str());
        draw_text_shadowed(dl, body * 0.85f, ImVec2(p1.x - pad - tsz.x, cy - tsz.y * 0.5f),
                           IM_COL32(170, 176, 184, 255), 1.0f, times[i].c_str());
        y += row_h;
    }
}

// The slim bar in the top-right corner: fuel range, time until sleep, speed limit, arrival.
static void draw_hud(const HudState& h)
{
    const float scale = g_bb_height / 1080.0f;
    const float body = g_cfg.font_size * scale * 0.5f;
    const float icon = body * 1.2f;
    const float pad = body * 0.7f, gap = body * 0.55f, sep = body * 1.1f;
    struct Cell { int kind; std::string text; ImU32 col; };  // kind: 0 fuel, 1 rest, 2 limit, 3 arrival
    std::vector<Cell> cells;
    const ImU32 normal = IM_COL32(235, 238, 242, 255), amber = IM_COL32(255, 196, 80, 255),
                red = IM_COL32(255, 120, 100, 255);
    if (h.range_km >= 0) cells.push_back({0, format_km(h.range_km), h.fuel_low ? amber : normal});
    if (h.rest_min >= 0) cells.push_back({1, format_hm((float)h.rest_min), h.rest_low ? amber : normal});
    if (h.limit_kmh > 0) cells.push_back({2, std::to_string(h.limit_kmh), h.speeding ? red : IM_COL32(20, 22, 26, 255)});
    if (!h.eta.empty()) cells.push_back({3, h.eta, h.late ? red : normal});
    if (cells.empty()) return;

    float w = pad * 2.0f;
    std::vector<float> widths;
    for (const auto& c : cells) {
        const float tw = c.kind == 2 ? 0.0f : g_font->CalcTextSizeA(body, FLT_MAX, 0.0f, c.text.c_str()).x;
        widths.push_back(c.kind == 2 ? icon * 1.15f : icon + gap + tw);
        w += widths.back();
    }
    w += sep * (cells.size() - 1);
    const float hgt = body * 1.9f;
    const float right = g_bb_width * g_cfg.strip_x;
    const ImVec2 p0(right - w, g_bb_height * g_cfg.hud_y), p1(right, p0.y + hgt);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 175), hgt * 0.5f);
    const float cy = p0.y + hgt * 0.5f;
    float x = p0.x + pad;
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& c = cells[i];
        const ImVec2 ic(x + icon * 0.5f, cy);
        if (c.kind == 0) icon_fuel(dl, ic, icon, IM_COL32(255, 176, 32, 255));
        else if (c.kind == 1) icon_parking(dl, ic, icon);
        else if (c.kind == 3) icon_flag(dl, ic, icon);
        if (c.kind == 2) {  // a speed limit sign: red ring, white disc, the number
            const float r = icon * 0.56f;
            const ImVec2 sc(x + widths[i] * 0.5f, cy);
            dl->AddCircleFilled(sc, r, IM_COL32(210, 30, 30, 255));
            dl->AddCircleFilled(sc, r * 0.78f, IM_COL32(255, 255, 255, 255));
            const float fs = body * (c.text.size() > 2 ? 0.72f : 0.86f);
            const ImVec2 ts = g_font->CalcTextSizeA(fs, FLT_MAX, 0.0f, c.text.c_str());
            dl->AddText(g_font, fs, ImVec2(sc.x - ts.x * 0.5f, sc.y - ts.y * 0.5f), c.col, c.text.c_str());
        } else {
            const ImVec2 ts = g_font->CalcTextSizeA(body, FLT_MAX, 0.0f, c.text.c_str());
            draw_text_shadowed(dl, body, ImVec2(x + icon + gap, cy - ts.y * 0.5f), c.col, 1.0f, c.text.c_str());
        }
        x += widths[i] + sep;
    }
}

static void on_present(IDXGISwapChain* sc, const char* via)
{
    if (!ensure_renderer(sc, via)) return;
    if (!g_rtv) create_rtv(sc);
    if (!g_rtv || g_bb_width <= 0) return;

    capture_frame(g_device, g_context, sc);  // the co-driver's screenshot: the game's picture only
    poll_hotkey();

    Location loc;
    double changed_at;
    bool paused;
    AlertState alerts;
    StripState strip;
    Guidance guidance;
    HudState hud;
    GuideCard guide;
    double guide_at;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        guidance = g_guidance;
        hud = g_hud;
        guide = g_guide;
        guide_at = g_guide_at;
        loc = g_loc;
        changed_at = g_loc_changed_at;
        paused = g_paused;
        alerts = g_alerts;
        strip = g_strip;
    }
    const double now = now_seconds();
    const float alpha = (g_visible && !paused) ? panel_alpha(loc, now - changed_at) : 0.0f;
    const double dt = g_last_frame > 0 ? now - g_last_frame : 1.0 / 60.0;
    g_last_frame = now;
    const bool show_alerts = g_visible && !paused && (alerts.fuel.active || alerts.rest.active);
    const bool show_strip = g_visible && g_strip_visible && !paused && strip.visible && !strip.items.empty();
    const bool show_guidance = g_visible && !paused && guidance.active;
    if (mic_listening()) {  // holding the co-driver key
        guide = GuideCard{true, "Assistant", "", "Listening... let go to send"};
        guide_at = now;
    }
    const bool show_guide = g_visible && !paused && g_cfg.guide_show && guide.active && now - guide_at < 60.0;
    const bool show_hud = g_visible && !paused && g_cfg.hud && hud.visible;
    if (alpha <= 0.01f && !show_alerts && !show_strip && !show_guidance && !show_guide && !show_hud) return;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(g_bb_width, g_bb_height);
    io.DeltaTime = (float)std::clamp(dt, 1e-4, 0.5);

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    float bottom = g_bb_height * g_cfg.position_y;
    if (alpha > 0.01f) bottom = draw_panel(loc, alpha);
    if (show_guidance) bottom = draw_guidance(guidance, bottom + g_bb_height * 0.008f);
    if (show_alerts) draw_alerts(alerts, bottom, now);
    if (show_strip) draw_strip(strip);
    if (show_hud) draw_hud(hud);
    if (show_guide) draw_guide(guide, now - guide_at);
    ImGui::Render();

    // ImGui's backend saves/restores pipeline state, but not the bound render targets.
    ID3D11RenderTargetView* old_rtv = nullptr;
    ID3D11DepthStencilView* old_dsv = nullptr;
    g_context->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_context->OMSetRenderTargets(1, &old_rtv, old_dsv);
    safe_release(old_rtv);
    safe_release(old_dsv);
}

// ---------------------------------------------------------------------------------------------
// Hooks

static thread_local bool t_in_present = false;  // Present may call Present1 internally

static HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    g_inflight++;
    if (g_active && !t_in_present && !(flags & DXGI_PRESENT_TEST)) {
        t_in_present = true;
        on_present(sc, "Present");
        t_in_present = false;
    }
    const bool outer = !t_in_present;
    if (outer) t_in_present = true;
    HRESULT hr = g_orig_present(sc, sync, flags);
    if (outer) t_in_present = false;
    g_inflight--;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain1* sc, UINT sync, UINT flags,
                                             const DXGI_PRESENT_PARAMETERS* params)
{
    g_inflight++;
    if (g_active && !t_in_present && !(flags & DXGI_PRESENT_TEST)) {
        t_in_present = true;
        on_present(sc, "Present1");
        t_in_present = false;
    }
    const bool outer = !t_in_present;
    if (outer) t_in_present = true;
    HRESULT hr = g_orig_present1(sc, sync, flags, params);
    if (outer) t_in_present = false;
    g_inflight--;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    g_inflight++;
    // The swapchain can only resize once nobody holds a reference to its buffers.
    if (sc == g_swapchain) {
        safe_release(g_rtv);
        capture_reset();
    }
    HRESULT hr = g_orig_resize(sc, count, w, h, fmt, flags);
    g_inflight--;
    return hr;
}

// Present/ResizeBuffers live in dxgi.dll and are shared by every swapchain, so the addresses
// read from a throwaway swapchain are the ones the game will call.
static bool find_swapchain_functions()
{
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ets2_city_overlay_dummy";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 8, 8,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { log_error("dummy window creation failed"); return false; }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           &sd, &sc, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) break;
    }
    if (SUCCEEDED(hr)) {
        void** vt = *reinterpret_cast<void***>(sc);
        g_target_present = vt[8];   // IDXGISwapChain::Present
        g_target_resize = vt[13];   // IDXGISwapChain::ResizeBuffers
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            g_target_present1 = (*reinterpret_cast<void***>(sc1))[22];  // IDXGISwapChain1::Present1
            sc1->Release();
        }
        sc->Release(); ctx->Release(); dev->Release();
    } else {
        log_error("dummy D3D11 device creation failed (hr=0x%08lx)", (unsigned long)hr);
    }
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return SUCCEEDED(hr);
}

bool overlay_install(const Config& cfg)
{
    g_cfg = cfg;
    g_visible = cfg.enabled;
    g_strip_visible = cfg.strip;
    if (!find_swapchain_functions()) return false;

    if (MH_Initialize() != MH_OK) { log_error("MinHook init failed"); return false; }
    bool ok = MH_CreateHook(g_target_present, (void*)&hk_present, (void**)&g_orig_present) == MH_OK &&
              MH_CreateHook(g_target_resize, (void*)&hk_resize, (void**)&g_orig_resize) == MH_OK;
    if (ok && g_target_present1)
        ok = MH_CreateHook(g_target_present1, (void*)&hk_present1, (void**)&g_orig_present1) == MH_OK;
    if (!ok || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        log_error("failed to hook DXGI Present");
        MH_Uninitialize();
        return false;
    }
    g_active = true;
    log_info("DXGI hooks installed (Present%s, ResizeBuffers)", g_target_present1 ? ", Present1" : "");
    return true;
}

void overlay_uninstall()
{
    if (!g_target_present) return;
    g_active = false;
    MH_DisableHook(MH_ALL_HOOKS);
    // Wait for any render-thread call still running inside a hook before freeing trampolines.
    for (int i = 0; i < 200 && g_inflight > 0; ++i) Sleep(10);
    MH_Uninitialize();

    shutdown_renderer();
    if (g_imgui_ctx) { ImGui::DestroyContext(); g_imgui_ctx = false; }
    g_font = nullptr;
    g_target_present = g_target_present1 = g_target_resize = nullptr;
    log_info("overlay removed");
}
