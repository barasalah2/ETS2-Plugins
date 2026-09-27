// Stand-in for the game, for testing the plugin without ETS2.
// Loads ets2_city_overlay.dll through the SDK entry points, feeds it truck positions and job
// events, renders frames on a real D3D11 flip-model swapchain, and saves screenshots.
//
// usage: test_host.exe <path to ets2_city_overlay.dll> <screenshot dir>

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <scssdk_telemetry.h>
#include <common/scssdk_telemetry_common_configs.h>
#include <common/scssdk_telemetry_common_gameplay_events.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static scs_telemetry_event_callback_t   g_events[16] = {};
struct Channel { scs_telemetry_channel_callback_t cb = nullptr; scs_context_t ctx = nullptr; };
static std::map<std::string, Channel> g_channels;  // like the game, hand back the registered context
static void channel(const char* name, const scs_value_t* v)
{
    auto it = g_channels.find(name);
    if (it != g_channels.end() && it->second.cb) it->second.cb(name, SCS_U32_NIL, v, it->second.ctx);
}

static SCSAPI_VOID host_log(const scs_log_type_t type, const scs_string_t msg)
{
    printf("[game.log %s] %s\n", type == 0 ? "info" : type == 1 ? "WARN" : "ERROR", msg);
}
static SCSAPI_RESULT reg_event(const scs_event_t ev, const scs_telemetry_event_callback_t cb, const scs_context_t)
{
    g_events[ev] = cb;
    return SCS_RESULT_ok;
}
static SCSAPI_RESULT unreg_event(const scs_event_t ev) { g_events[ev] = nullptr; return SCS_RESULT_ok; }
static SCSAPI_RESULT reg_channel(const scs_string_t name, const scs_u32_t, const scs_value_type_t,
                                 const scs_u32_t, const scs_telemetry_channel_callback_t cb, const scs_context_t ctx)
{
    g_channels[name] = {cb, ctx};
    return SCS_RESULT_ok;
}
static SCSAPI_RESULT unreg_channel(const scs_string_t, const scs_u32_t, const scs_value_type_t) { return SCS_RESULT_ok; }

static void send_position(double x, double z, float heading = 0.0f)
{
    scs_value_t v = {};
    v.type = SCS_VALUE_TYPE_dplacement;
    v.value_dplacement.position.x = x;
    v.value_dplacement.position.z = z;
    v.value_dplacement.orientation.heading = heading;
    channel("truck.world.placement", &v);
}

static void send_float(const char* name, float f)
{
    scs_value_t v = {};
    v.type = SCS_VALUE_TYPE_float;
    v.value_float.value = f;
    channel(name, &v);
}

static void send_rest(int minutes)
{
    scs_value_t v = {};
    v.type = SCS_VALUE_TYPE_s32;
    v.value_s32.value = minutes;
    channel("rest.stop", &v);
}

static void send_event(scs_event_t ev, const void* info = nullptr)
{
    if (g_events[ev]) g_events[ev](ev, info, nullptr);
}

static void send_job(const char* dest_id, const char* dest_name)
{
    scs_named_value_t attrs[3] = {};
    attrs[0].name = SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city_id;
    attrs[0].value.type = SCS_VALUE_TYPE_string;
    attrs[0].value.value_string.value = dest_id;
    attrs[1].name = SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city;
    attrs[1].value.type = SCS_VALUE_TYPE_string;
    attrs[1].value.value_string.value = dest_name;
    scs_telemetry_configuration_t cfg = {SCS_TELEMETRY_CONFIG_job, attrs};
    send_event(SCS_TELEMETRY_EVENT_configuration, &cfg);
}

// A full job config, as the game sends it for a company-to-company job.
static void send_job_full(const char* dcity, const char* dcomp, const char* dcity_name, const char* dcomp_name,
                          const char* scity, const char* scomp, bool loaded, const char* market)
{
    scs_named_value_t a[11] = {};  // 10 attributes + the zeroed terminator
    int n = 0;
    auto str = [&](const char* name, const char* v) {
        a[n].name = name;
        a[n].value.type = SCS_VALUE_TYPE_string;
        a[n].value.value_string.value = v;
        ++n;
    };
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city_id, dcity);
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_company_id, dcomp);
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city, dcity_name);
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_company, dcomp_name);
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city_id, scity);
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_company_id, scomp);
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city, "Berlin");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_company, "Eurogoodies");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_job_market, market);
    a[n].name = SCS_TELEMETRY_CONFIG_ATTRIBUTE_is_cargo_loaded;
    a[n].value.type = SCS_VALUE_TYPE_bool;
    a[n].value.value_bool.value = loaded ? 1 : 0;
    ++n;  // a[n] stays zeroed: the terminator
    scs_telemetry_configuration_t cfg = {SCS_TELEMETRY_CONFIG_job, a};
    send_event(SCS_TELEMETRY_EVENT_configuration, &cfg);
}

// The route line + stops the plugin writes when ETS2_CITY_OVERLAY_DEBUG is set.
struct RoutePt { double x, z, at; };
struct RouteSt { double x, z, at; int kinds; std::string label; };
static bool read_route(const std::string& path, std::vector<RoutePt>& pts, std::vector<RouteSt>& stops)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[512];
    fgets(line, sizeof(line), f);  // header
    while (fgets(line, sizeof(line), f)) {
        char type = 0, label[256] = "";
        double x, z, at;
        int kinds;
        if (sscanf(line, "%c,%lf,%lf,%lf,%d,%255[^\r\n]", &type, &x, &z, &at, &kinds, label) < 5) continue;
        if (type == 'p') pts.push_back({x, z, at});
        else stops.push_back({x, z, at, kinds, label});
    }
    fclose(f);
    return pts.size() > 1;
}

static float heading_of(double dx, double dz)
{
    double h = std::atan2(-dx, -dz) / (2 * 3.14159265358979323846);
    return (float)(h < 0 ? h + 1 : h);
}

static void send_bool(const char* name, bool b)
{
    scs_value_t v = {};
    v.type = SCS_VALUE_TYPE_bool;
    v.value_bool.value = b ? 1 : 0;
    channel(name, &v);
}

static void send_delivered()
{
    scs_named_value_t attrs[1] = {};
    scs_telemetry_gameplay_event_t ev = {SCS_TELEMETRY_GAMEPLAY_EVENT_job_delivered, attrs};
    send_event(SCS_TELEMETRY_EVENT_gameplay, &ev);
}

static void send_u32(const char* name, uint32_t u)
{
    scs_value_t v = {};
    v.type = SCS_VALUE_TYPE_u32;
    v.value_u32.value = u;
    channel(name, &v);
}

// A job config with the cargo details the co-driver talks about.
static void send_job_cargo(const char* cargo, float kg, uint64_t income, uint32_t due)
{
    scs_named_value_t a[16] = {};
    int n = 0;
    auto str = [&](const char* name, const char* v) {
        a[n].name = name;
        a[n].value.type = SCS_VALUE_TYPE_string;
        a[n].value.value_string.value = v;
        ++n;
    };
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city_id, "prague");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_company_id, "quarry");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city, "Prague");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_company, "Quarry");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city_id, "berlin");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_company_id, "eurogoodies");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city, "Berlin");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_company, "Eurogoodies");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_job_market, "freight_market");
    str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo, cargo);
    a[n].name = SCS_TELEMETRY_CONFIG_ATTRIBUTE_is_cargo_loaded;
    a[n].value.type = SCS_VALUE_TYPE_bool;
    a[n].value.value_bool.value = 1;
    ++n;
    a[n].name = SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo_mass;
    a[n].value.type = SCS_VALUE_TYPE_float;
    a[n].value.value_float.value = kg;
    ++n;
    a[n].name = SCS_TELEMETRY_CONFIG_ATTRIBUTE_income;
    a[n].value.type = SCS_VALUE_TYPE_u64;
    a[n].value.value_u64.value = income;
    ++n;
    a[n].name = SCS_TELEMETRY_CONFIG_ATTRIBUTE_delivery_time;
    a[n].value.type = SCS_VALUE_TYPE_u32;
    a[n].value.value_u32.value = due;
    ++n;  // a[n] stays zeroed: the terminator
    scs_telemetry_configuration_t cfg = {SCS_TELEMETRY_CONFIG_job, a};
    send_event(SCS_TELEMETRY_EVENT_configuration, &cfg);
}

static void send_fined(long long amount, const char* offence)
{
    scs_named_value_t a[3] = {};
    a[0].name = SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_offence;
    a[0].value.type = SCS_VALUE_TYPE_string;
    a[0].value.value_string.value = offence;
    a[1].name = SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_amount;
    a[1].value.type = SCS_VALUE_TYPE_s64;
    a[1].value.value_s64.value = amount;
    scs_telemetry_gameplay_event_t ev = {SCS_TELEMETRY_GAMEPLAY_EVENT_player_fined, a};
    send_event(SCS_TELEMETRY_EVENT_gameplay, &ev);
}

static IDXGISwapChain1* g_sc;
static ID3D11DeviceContext* g_ctx;
static ID3D11RenderTargetView* g_rtv;
static HWND g_hwnd;
static bool g_landscape = false;  // draw sky / fields / road instead of a flat colour
static ID3D11Texture2D* g_scene = nullptr;  // a picture to show as the game's frame (same size as the swapchain)

// Loads a raw 8-bit RGBA picture (w x h) to show instead of the drawn landscape.
static void load_scene(ID3D11Device* dev, const char* path, UINT w, UINT h)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("scene %s not found\n", path); return; }
    std::vector<unsigned char> px((size_t)w * h * 4);
    const size_t got = fread(px.data(), 1, px.size(), f);
    fclose(f);
    if (got != px.size()) { printf("scene %s: wrong size\n", path); return; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA init = {px.data(), w * 4, 0};
    if (FAILED(dev->CreateTexture2D(&td, &init, &g_scene))) printf("scene texture failed\n");
}

static void frames(int n)
{
    for (int i = 0; i < n; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        const float c[4] = {0.30f, 0.42f, 0.55f, 1.0f};  // sky-ish backdrop
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, c);
        ID3D11DeviceContext1* ctx1 = nullptr;
        ID3D11Texture2D* bb = nullptr;
        if (g_scene && SUCCEEDED(g_sc->GetBuffer(0, IID_PPV_ARGS(&bb)))) {
            g_ctx->CopyResource(bb, g_scene);
            bb->Release();
        } else if (g_landscape && SUCCEEDED(g_ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
            DXGI_SWAP_CHAIN_DESC1 d;
            g_sc->GetDesc1(&d);
            const LONG w = (LONG)d.Width, h = (LONG)d.Height;
            const float sky[4] = {0.45f, 0.63f, 0.88f, 1.0f}, field[4] = {0.30f, 0.52f, 0.22f, 1.0f},
                        road[4] = {0.22f, 0.22f, 0.24f, 1.0f}, line[4] = {0.95f, 0.95f, 0.95f, 1.0f},
                        sign[4] = {0.0f, 0.36f, 0.72f, 1.0f};
            D3D11_RECT r1 = {0, 0, w, h * 55 / 100}, r2 = {0, h * 55 / 100, w, h};
            ctx1->ClearView(g_rtv, sky, &r1, 1);
            ctx1->ClearView(g_rtv, field, &r2, 1);
            for (LONG y = h * 55 / 100; y < h; y += 4) {  // a road narrowing towards the horizon
                const LONG half = (y - h * 55 / 100) * w / (2 * (h - h * 55 / 100)) + 6;
                D3D11_RECT rr = {w / 2 - half, y, w / 2 + half, y + 4};
                ctx1->ClearView(g_rtv, road, &rr, 1);
                if ((y / 24) % 2) {
                    D3D11_RECT lr = {w / 2 - half / 30 - 1, y, w / 2 + half / 30 + 1, y + 4};
                    ctx1->ClearView(g_rtv, line, &lr, 1);
                }
            }
            D3D11_RECT sr = {w * 68 / 100, h * 30 / 100, w * 88 / 100, h * 42 / 100};  // a blue motorway sign
            ctx1->ClearView(g_rtv, sign, &sr, 1);
            ctx1->Release();
        }
        g_sc->Present(1, 0);
        send_event(SCS_TELEMETRY_EVENT_frame_end);
    }
}

// Render for a wall-clock duration (frame counts depend on the monitor's refresh rate).
static void run_for(double seconds)
{
    const ULONGLONG end = GetTickCount64() + (ULONGLONG)(seconds * 1000);
    while (GetTickCount64() < end) frames(1);
}

// Ctrl + key held for hold_ms, with our window in front (the plugin ignores keys otherwise).
static bool press_ctrl_key(int vk, DWORD hold_ms)
{
    for (int attempt = 0; attempt < 6 && GetForegroundWindow() != g_hwnd; ++attempt) {
        HWND fg = GetForegroundWindow();
        const DWORD fg_thread = GetWindowThreadProcessId(fg, nullptr), me = GetCurrentThreadId();
        AttachThreadInput(fg_thread, me, TRUE);
        ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        BringWindowToTop(g_hwnd);
        SetFocus(g_hwnd);
        AttachThreadInput(fg_thread, me, FALSE);
        run_for(0.4);
    }
    if (GetForegroundWindow() != g_hwnd) {
        printf("KEY: the test window isn't in front; key press skipped\n");
        return false;
    }
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event((BYTE)vk, 0, 0, 0);
    run_for(hold_ms / 1000.0);
    keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    return true;
}

static void screenshot(const std::string& path)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    POINT tl = {0, 0};
    ClientToScreen(g_hwnd, &tl);
    const int w = rc.right, h = rc.bottom;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    SelectObject(mem, bmp);
    // The window's own composed content (works even if another window covers it).
    if (!PrintWindow(g_hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT))
        BitBlt(mem, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY);

    BITMAPINFOHEADER bi = {sizeof(bi), w, -h, 1, 24, BI_RGB};
    const int stride = (w * 3 + 3) & ~3;
    std::string pixels(stride * h, '\0');
    GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER bf = {0x4D42, (DWORD)(sizeof(bf) + sizeof(bi) + pixels.size()), 0, 0, sizeof(bf) + sizeof(bi)};
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(pixels.data(), 1, pixels.size(), f);
    fclose(f);
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    printf("screenshot %s\n", path.c_str());
}

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: test_host <dll> <outdir>\n"); return 2; }
    setvbuf(stdout, nullptr, _IONBF, 0);  // show progress live, even when redirected to a file
    const std::string out = argv[2];
    SetProcessDPIAware();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"fake_ets2";
    RegisterClassW(&wc);
    RECT r = {0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"fake ETS2", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    // 1. plugin loads before the game creates its device (like the real game)
    SetEnvironmentVariableW(L"ETS2_CITY_OVERLAY_DEBUG", L"1");  // plugin writes route_debug.csv
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto init = (decltype(&scs_telemetry_init))GetProcAddress(dll, "scs_telemetry_init");
    auto shutdown = (decltype(&scs_telemetry_shutdown))GetProcAddress(dll, "scs_telemetry_shutdown");

    scs_telemetry_init_params_v101_t params = {};
    params.common.game_name = "Fake Truck Simulator";
    params.common.game_id = "eut2";
    params.common.game_version = SCS_MAKE_VERSION(1, 19);
    params.common.log = host_log;
    params.register_for_event = reg_event;
    params.unregister_from_event = unreg_event;
    params.register_for_channel = reg_channel;
    params.unregister_from_channel = unreg_channel;
    if (init(SCS_TELEMETRY_VERSION_1_01, &params) != SCS_RESULT_ok) { printf("init failed\n"); return 1; }

    // 2. game creates its renderer: flip-model swapchain via CreateSwapChainForHwnd
    ID3D11Device* dev = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &g_ctx);
    IDXGIDevice* dxdev; dev->QueryInterface(IID_PPV_ARGS(&dxdev));
    IDXGIAdapter* adapter; dxdev->GetAdapter(&adapter);
    IDXGIFactory2* factory; adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 1280; sd.Height = 720;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    factory->CreateSwapChainForHwnd(dev, g_hwnd, &sd, nullptr, nullptr, &g_sc);
    ID3D11Texture2D* bb; g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release();

    send_event(SCS_TELEMETRY_EVENT_started);

    // "test_host.exe <dll> <outdir> codriver <question.wav> [scene.rgba]": the AI assistant with the real
    // Gemini API (needs GEMINI_API_KEY); scene.rgba (1280x720 RGBA) is shown as the game picture.
    // A job with cargo, the driver talking (the WAV stands in for the
    // microphone), a tap for "about here", and a fine.
    if (argc > 3 && std::string(argv[3]) == "codriver") {
        std::string dir = argv[1];
        dir = dir.substr(0, dir.find_last_of("\\/")) + "\\ets2_city_overlay";
        remove((dir + "\\route_debug.csv").c_str());
        remove((dir + "\\codriver_requests.txt").c_str());
        if (argc > 4) SetEnvironmentVariableA("ETS2_CITY_OVERLAY_FAKE_MIC", argv[4]);
        g_landscape = true;
        if (argc > 5) load_scene(dev, argv[5], 1280, 720);
        const uint32_t t0 = 1 * 1440 + 14 * 60 + 5;  // Tuesday 14:05
        send_u32("game.time", t0);
        send_float("local.scale", 19.0f);
        send_float("truck.speed", 22.0f);
        send_float("truck.navigation.speed.limit", 80.0f / 3.6f);
        send_float("truck.fuel.amount", 420.0f);
        send_float("truck.fuel.range", 1100.0f);
        send_float("truck.fuel.consumption.average", 0.36f);
        send_float("truck.odometer", 20000.0f);
        send_rest(700);
        send_bool("trailer.connected", true);
        send_position(10055.44, -10584.99, 0.8385f);
        send_job_cargo("Canned beef", 18200.0f, 14350, t0 + 11 * 60);
        run_for(3.0);
        std::vector<RoutePt> pts;
        std::vector<RouteSt> stops;
        if (!read_route(dir + "\\route_debug.csv", pts, stops)) {
            printf("CODRIVER TEST: no route\n");
        } else {
            // A little way down the road.
            size_t i = 0;
            while (i + 1 < pts.size() && pts[i + 1].at < pts.back().at * 0.15) ++i;
            send_position(pts[i].x, pts[i].z, heading_of(pts[i + 1].x - pts[i].x, pts[i + 1].z - pts[i].z));
            send_float("truck.navigation.distance", (float)((pts.back().at - pts[i].at) * 19.0));
            send_u32("game.time", t0 + 25);
            printf("CODRIVER TEST: new job -> expect a briefing\n");
            run_for(16.0);
            screenshot(out + "\\cd1_briefing.bmp");
            printf("CODRIVER TEST: holding the key and talking\n");
            if (press_ctrl_key(VK_F11, 1800)) {
                run_for(1.0);
                screenshot(out + "\\cd2_thinking.bmp");
                run_for(14.0);
                screenshot(out + "\\cd3_answer.bmp");
            }
            printf("CODRIVER TEST: a quick tap -> about here\n");
            if (press_ctrl_key(VK_F11, 120)) {
                run_for(15.0);
                screenshot(out + "\\cd4_about_here.bmp");
            }
            printf("CODRIVER TEST: a speeding fine (event remarks wait 15 s after the last words)\n");
            send_fined(250, "speeding");
            run_for(50.0);
            screenshot(out + "\\cd5_fined.bmp");
            printf("CODRIVER TEST: quiet driving -> expect regular camera looks\n");
            run_for(80.0);
        }
        shutdown();
        frames(5);
        FreeLibrary(dll);
        return 0;
    }

    // "test_host.exe <dll> <outdir> rules": speed limit calls, the headlight reminder, fuel prices
    // across the German-Czech border, the HUD bar and dimmed rest stops (no AI needed).
    if (argc > 3 && std::string(argv[3]) == "rules") {
        std::string dir = argv[1];
        dir = dir.substr(0, dir.find_last_of("\\/")) + "\\ets2_city_overlay";
        remove((dir + "\\route_debug.csv").c_str());
        const uint32_t t0 = 2 * 1440 + 21 * 60 + 30;  // Wednesday 21:30: dark
        send_u32("game.time", t0);
        send_float("local.scale", 19.0f);
        send_float("truck.speed", 22.0f);  // ~79 km/h
        send_float("truck.navigation.speed.limit", 90.0f / 3.6f);
        send_float("truck.fuel.amount", 500.0f);
        send_float("truck.fuel.range", 1300.0f);
        send_float("truck.fuel.consumption.average", 0.36f);
        send_float("truck.odometer", 30000.0f);
        send_rest(150);  // must sleep in 2.5 h of game time: later parking stops are out of reach
        send_bool("truck.light.beam.low", false);
        send_bool("truck.light.beam.high", false);
        send_bool("truck.wipers", false);
        send_bool("trailer.connected", true);
        send_position(10055.44, -10584.99, 0.8385f);
        send_job_cargo("Canned beef", 18200.0f, 14350, t0 + 11 * 60);
        run_for(3.0);
        std::vector<RoutePt> pts;
        std::vector<RouteSt> stops;
        if (!read_route(dir + "\\route_debug.csv", pts, stops)) {
            printf("RULES TEST: no route\n");
        } else {
            double border_at = -1;
            for (const auto& st : stops)
                if (st.kinds & 32) { border_at = st.at; break; }
            // About 100 game km before the border.
            size_t i = 0;
            while (i + 1 < pts.size() && (border_at - pts[i + 1].at) * 0.019 > 100) ++i;
            send_float("truck.navigation.distance", (float)((pts.back().at - pts[i].at) * 19.0));
            printf("RULES TEST: %.0f km before the border -> expect cheaper diesel in the Czech Republic\n",
                   (border_at - pts[i].at) * 0.019);
            // Like the game, keep reporting the position: the plugin notices the jump and re-routes.
            const ULONGLONG until = GetTickCount64() + 12000;
            while (GetTickCount64() < until) {
                send_position(pts[i].x, pts[i].z, heading_of(pts[i + 1].x - pts[i].x, pts[i + 1].z - pts[i].z));
                frames(1);
            }
            screenshot(out + "\\r1_hud.bmp");
            printf("RULES TEST: limit 90 -> 60 at 79 km/h -> expect \"Speed limit 60.\"\n");
            send_float("truck.navigation.speed.limit", 60.0f / 3.6f);
            run_for(3.0);
            screenshot(out + "\\r2_limit.bmp");
            printf("RULES TEST: lights off at night -> expect a headlight reminder after ~20 s\n");
            run_for(20.0);
            printf("RULES TEST: wipers on -> expect the rain reminder\n");
            send_bool("truck.wipers", true);
            run_for(8.0);
            printf("RULES TEST: Ctrl+F8 (switch off), then the limit drops 60 -> 40 -> expect silence\n");
            if (press_ctrl_key(VK_F8, 120)) {
                run_for(0.8);
                screenshot(out + "\\r3_switched_off.bmp");
                send_float("truck.navigation.speed.limit", 90.0f / 3.6f);
                run_for(2.0);  // a new limit counts after 1.5 s
                send_float("truck.navigation.speed.limit", 40.0f / 3.6f);
                run_for(3.0);
                printf("RULES TEST: Ctrl+F8 (switch on), then the limit drops 90 -> 50 -> expect \"Speed limit 50.\"\n");
                press_ctrl_key(VK_F8, 120);
                send_float("truck.navigation.speed.limit", 90.0f / 3.6f);
                run_for(2.0);
                send_float("truck.navigation.speed.limit", 50.0f / 3.6f);
                run_for(3.0);
            }
        }
        shutdown();
        frames(5);
        FreeLibrary(dll);
        return 0;
    }

    if (argc > 3 && std::string(argv[3]) == "fuel") {
        std::string dir = argv[1];
        dir = dir.substr(0, dir.find_last_of("\\/")) + "\\ets2_city_overlay";
        remove((dir + "\\route_debug.csv").c_str());
        send_float("local.scale", 19.0f);
        send_float("truck.speed", 30.0f);  // ~108 km/h
        float fuel = 260.0f, odo = 50000.0f;
        const float real_use = 0.45f;  // liters per game km, what the truck really burns
        auto send_fuel = [&] {
            send_float("truck.fuel.amount", fuel);
            send_float("truck.fuel.range", fuel / 0.20f);          // the game's optimistic estimate
            send_float("truck.fuel.consumption.average", 0.20f);  // ...from a too-low average
        };
        send_fuel();
        send_float("truck.odometer", odo);
        send_rest(900);
        send_bool("trailer.connected", true);
        send_position(43851.8, -64909.9, 0.8327f);
        send_job_full("graz", "lkwlog", "Graz", "LkwLog GmbH", "ivalo", "lintukainen", true, "freight_market");
        run_for(3.0);
        std::vector<RoutePt> pts;
        std::vector<RouteSt> stops;
        if (!read_route(dir + "\\route_debug.csv", pts, stops)) {
            printf("FUEL TEST: no route\n");
        } else {
            printf("FUEL TEST: route %.1f map km; fuel stations at game km:", pts.back().at / 1000.0);
            for (const auto& st : stops)
                if ((st.kinds & 1) && st.at * 0.019 < 800) printf(" %.0f", st.at * 0.019);
            printf("\n");
            // Drive (without refuelling) until the tank is nearly empty, logging what the plugin says.
            size_t i = 0;
            while (i + 1 < pts.size() && fuel > 2.0f) {
                const RoutePt& a = pts[i];
                const RoutePt& b = pts[i + 1];
                const int steps = std::max(1, (int)std::ceil((b.at - a.at) / 20.0));
                for (int k = 1; k <= steps && fuel > 2.0f; ++k) {
                    const double f = (double)k / steps;
                    const double at = a.at + (b.at - a.at) * f;
                    send_position(a.x + (b.x - a.x) * f, a.z + (b.z - a.z) * f, heading_of(b.x - a.x, b.z - a.z));
                    const float km = (float)((b.at - a.at) / steps * 0.019);
                    odo += km;
                    fuel -= km * real_use;
                    send_float("truck.odometer", odo);
                    send_fuel();
                    send_float("truck.navigation.distance", (float)((pts.back().at - at) * 19.0));
                    run_for((b.at - a.at) / steps / 300.0);
                }
                ++i;
            }
            printf("FUEL TEST: ran dry at game km %.0f\n", pts[i].at * 0.019);
        }
        shutdown();
        FreeLibrary(dll);
        return 0;
    }

    // Berlin centre (from cities.csv)
    send_position(10183.1, -10001.2);
    frames(40);   screenshot(out + "\\1_enter_berlin.bmp");
    frames(460);  screenshot(out + "\\2_berlin_idle.bmp");

    // Out on the road towards Berlin: "Near"
    send_position(10183.1 + 9000, -10001.2);
    frames(30);   screenshot(out + "\\3_near_berlin.bmp");

    // Pause (menu / map): overlay hidden
    send_event(SCS_TELEMETRY_EVENT_paused);
    frames(10);   screenshot(out + "\\4_paused.bmp");
    send_event(SCS_TELEMETRY_EVENT_started);

    // Resize, like switching resolution
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g_rtv->Release();
    HRESULT hr = g_sc->ResizeBuffers(0, 1920, 1080, DXGI_FORMAT_UNKNOWN, 0);
    printf("ResizeBuffers hr=0x%08lx\n", (unsigned long)hr);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 1920 + 16, 1080 + 39, SWP_NOMOVE | SWP_NOZORDER);
    g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release();

    // Learning: deliver a job to a city not in the table, far from all others
    send_job("test_town", "Łódź Testowo");
    send_position(200000, 200000);
    frames(5);
    send_delivered();
    frames(40);   screenshot(out + "\\5_learned_city.bmp");

    // A city with accents
    send_position(28343.1, -20619.5);  // Gdańsk
    frames(40);   screenshot(out + "\\6_gdansk.bmp");

    // --- Fuel / sleep planning -------------------------------------------------------------
    send_float("local.scale", 19.0f);
    send_float("truck.fuel.consumption.average", 0.35f);  // L per game km
    float odo = 1000.0f;
    send_float("truck.odometer", odo);
    send_float("truck.fuel.amount", 400.0f);
    send_float("truck.fuel.range", 1000.0f);
    send_rest(300);
    // Drive 1.2 km of map east into Berlin centre: the odometer should calibrate the scale to 19.
    for (int i = 0; i <= 60; ++i) {
        send_position(9905.9 - 1200 + i * 20, -9929.0, 0.75f);
        odo += 20 * 0.019f;
        send_float("truck.odometer", odo);
        frames(1);
    }
    run_for(1.3);  screenshot(out + "\\7_plenty.bmp");                 // expect: no warnings

    send_float("truck.fuel.amount", 300.0f);   // well above 100 L...
    send_float("truck.fuel.range", 100.0f);    // ...but can't reach the station after the next
    run_for(1.3);  screenshot(out + "\\8_refuel_at_next.bmp");

    send_float("truck.fuel.amount", 330.0f);   // refuelled 30 L: latch released
    send_float("truck.navigation.distance", 50000.0f);  // GPS route 50 km, range covers it
    run_for(1.3);  screenshot(out + "\\9_route_covered.bmp");         // expect: no warnings

    send_float("truck.navigation.distance", 0.0f);
    send_float("truck.fuel.range", 20.0f);     // can't even reach the next one
    send_rest(100);                            // won't last past the next rest stop
    run_for(1.3);  screenshot(out + "\\10_critical_and_sleep.bmp");

    // --- Job route: Berlin (Eurogoodies) -> Prague (Quarry) ------------------------------------
    std::string dir = argv[1];
    dir = dir.substr(0, dir.find_last_of("\\/")) + "\\ets2_city_overlay";
    remove((dir + "\\route_debug.csv").c_str());
    send_float("truck.speed", 22.0f);  // m/s, ~80 km/h
    send_float("truck.fuel.amount", 400.0f);
    send_float("truck.fuel.range", 1200.0f);
    send_float("truck.navigation.distance", 0.0f);
    send_rest(900);
    send_position(10055.44, -10584.99, 0.8385f);  // on the road outside the Berlin depot
    send_bool("trailer.connected", false);
    printf("JOB TEST: freight market job accepted, trailer not attached -> expect route to the pick-up\n");
    send_job_full("prague", "quarry", "Prague", "Quarry", "berlin", "eurogoodies", true, "freight_market");
    run_for(2.0);
    printf("JOB TEST: trailer attached -> expect route to the destination\n");
    send_bool("trailer.connected", true);
    run_for(3.0);
    std::vector<RoutePt> pts;
    std::vector<RouteSt> stops;
    if (!read_route(dir + "\\route_debug.csv", pts, stops)) {
        printf("ROUTE TEST: no route_debug.csv - route was not computed\n");
    } else {
        int fuel = 0, rest = 0, city = 0;
        for (const auto& s : stops) { fuel += (s.kinds & 1) != 0; rest += (s.kinds & 2) != 0; city += (s.kinds & 4) != 0; }
        printf("ROUTE TEST: %zu points, %.1f map km, %d fuel / %d sleep stops, %d cities\n", pts.size(),
               pts.back().at / 1000.0, fuel, rest, city);
        for (const auto& s : stops)
            if (s.kinds & 4 || s.kinds & 16) printf("  %6.1f map km  %s\n", s.at / 1000.0, s.label.c_str());
        screenshot(out + "\\11_route_start.bmp");

        // Drive 40% of the way along the route.
        size_t i = 0;
        const double until = pts.back().at * 0.4;
        while (i + 1 < pts.size() && pts[i + 1].at <= until) {
            const RoutePt& a = pts[i];
            const RoutePt& b = pts[i + 1];
            send_position(b.x, b.z, heading_of(b.x - a.x, b.z - a.z));
            odo += (float)((b.at - a.at) * 0.019);
            send_float("truck.odometer", odo);
            frames(1);
            ++i;
        }
        const double here = pts[i].at;
        // The game's GPS agrees with our route (19 game m per map m).
        send_float("truck.navigation.distance", (float)((pts.back().at - here) * 19.0));
        // Fuel: plenty of liters, but the range only just covers the next station on the route.
        double next_km = -1, after_km = -1;
        for (const auto& s : stops) {
            if (!(s.kinds & 1) || s.at < here) continue;
            if (next_km < 0) next_km = (s.at - here) * 0.019;
            else if (s.at - here > 0 && (s.at - here) * 0.019 > next_km + 0.5) { after_km = (s.at - here) * 0.019; break; }
        }
        const double reserve_km = 30.0 / 0.35;
        const double range = after_km > 0 ? (next_km + (after_km + reserve_km)) / 2 : next_km + 10;
        printf("ROUTE TEST: at %.1f map km; next fuel %.1f km, after %.1f km -> range %.0f km\n", here / 1000.0,
               next_km, after_km, range);
        send_float("truck.fuel.amount", 250.0f);
        send_float("truck.fuel.range", (float)range);
        send_rest(50);
        run_for(1.5);  screenshot(out + "\\12_route_refuel.bmp");

        // The game's GPS goes a different way (e.g. your own waypoints): strip says so,
        // warnings fall back to "nearest ahead".
        send_float("truck.navigation.distance", (float)((pts.back().at - here) * 19.0 * 2.0));
        run_for(1.5);  screenshot(out + "\\13_gps_differs.bmp");
        send_float("truck.navigation.distance", (float)((pts.back().at - here) * 19.0));

        // Pull into the recommended stop and crawl around it: not "leaving the route".
        for (const auto& st : stops) {
            if (!(st.kinds & 1) || st.at < here) continue;
            printf("PARK TEST: at the %s for 8 s (expect no 'left the route')\n", st.label.c_str());
            send_float("truck.speed", 3.0f);
            const ULONGLONG park_end = GetTickCount64() + 8000;
            while (GetTickCount64() < park_end) {
                send_position(st.x + 30, st.z - 20, 0.1f);
                frames(1);
            }
            printf("PARK TEST: done\n");
            send_float("truck.speed", 22.0f);
            break;
        }
        // Back on the route where we left it.
        send_position(pts[i].x, pts[i].z, heading_of(pts[i + 1].x - pts[i].x, pts[i + 1].z - pts[i].z));
        run_for(1.2);
        printf("JOB TEST: trailer unhooked mid-job -> expect NO new route\n");
        send_bool("trailer.connected", false);
        run_for(1.2);
        send_bool("trailer.connected", true);

        // Drive the rest of the way - over the German-Czech border and to the destination - at
        // ~150 map m per second (7x real speed), so the once-a-second checks still see each stretch.
        printf("DRIVE TEST: to the destination (expect border + destination announcements)\n");
        send_float("truck.fuel.amount", 600.0f);  // plenty: no fuel talk while driving
        send_float("truck.fuel.range", 1500.0f);
        send_rest(900);
        bool shot = false;
        while (i + 1 < pts.size()) {
            const RoutePt& a = pts[i];
            const RoutePt& b = pts[i + 1];
            // Screenshot the guidance panel once, about 1 km before a junction maneuver.
            for (const auto& st : stops)
                if (!shot && (st.kinds & 64) && st.at > b.at && (st.at - b.at) * 0.019 < 2.5 &&
                    (st.at - b.at) * 0.019 > 0.4) {
                    printf("GUIDANCE TEST: %s in %.1f km\n", st.label.c_str(), (st.at - b.at) * 0.019);
                    run_for(1.2);
                    screenshot(out + "\\14_guidance.bmp");
                    shot = true;
                }
            // Move in steps of at most 20 map m, like a real truck, so no call window is skipped.
            const int steps = std::max(1, (int)std::ceil((b.at - a.at) / 20.0));
            for (int k = 1; k <= steps; ++k) {
                const double f = (double)k / steps;
                const double at = a.at + (b.at - a.at) * f;
                send_position(a.x + (b.x - a.x) * f, a.z + (b.z - a.z) * f, heading_of(b.x - a.x, b.z - a.z));
                odo += (float)((b.at - a.at) / steps * 0.019);
                send_float("truck.odometer", odo);
                send_float("truck.navigation.distance", (float)((pts.back().at - at) * 19.0));
                bool near_turn = false;  // real driving speed (~22 map m/s) in the last 150 map m before a junction
                for (const auto& st : stops)
                    if ((st.kinds & 64) && st.at >= at && st.at - at < 150) near_turn = true;
                run_for((b.at - a.at) / steps / (near_turn ? 22.0 : 150.0));
            }
            ++i;
        }
        run_for(4.0);  // let the voice finish
        i = pts.size() - 2;

        // Leave the route: after ~5 s the plugin should compute a new one - once, not every frame.
        printf("OFFROAD TEST: 7 s off the road (expect one 'left the route')\n");
        const ULONGLONG end = GetTickCount64() + 7000;
        while (GetTickCount64() < end) {
            send_position(pts[i].x + 300, pts[i].z + 300, 0.25f);
            frames(1);
        }
    }

    shutdown();
    frames(5);
    FreeLibrary(dll);
    printf("done, ResizeBuffers %s\n", SUCCEEDED(hr) ? "ok" : "FAILED");
    return 0;
}
