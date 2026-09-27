#include "config.h"

#include <windows.h>

#include <cstdlib>

static std::wstring read_str(const std::wstring& file, const wchar_t* section, const wchar_t* key,
                             const std::wstring& def)
{
    wchar_t buf[512];
    GetPrivateProfileStringW(section, key, def.c_str(), buf, 512, file.c_str());
    return buf;
}

static double read_num(const std::wstring& file, const wchar_t* section, const wchar_t* key, double def)
{
    std::wstring s = read_str(file, section, key, L"");
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    // base 0 for integers accepts hex like 0x78 for key codes
    double v = (s.find(L'.') == std::wstring::npos) ? (double)wcstol(s.c_str(), &end, 0)
                                                     : wcstod(s.c_str(), &end);
    return end == s.c_str() ? def : v;
}

Config load_config(const std::wstring& dir)
{
    Config c;
    const std::wstring f = dir + L"\\ets2_city_overlay.ini";
    const wchar_t* o = L"overlay";
    const wchar_t* d = L"detection";

    c.enabled        = read_num(f, o, L"enabled", c.enabled) != 0;
    c.toggle_key     = (int)read_num(f, o, L"toggle_key", c.toggle_key);
    c.toggle_ctrl    = read_num(f, o, L"toggle_ctrl", c.toggle_ctrl) != 0;
    c.font_path      = read_str(f, o, L"font", c.font_path);
    c.font_size      = (float)read_num(f, o, L"font_size", c.font_size);
    c.position_y     = (float)read_num(f, o, L"position_y", c.position_y);
    c.banner_seconds = (float)read_num(f, o, L"banner_seconds", c.banner_seconds);
    c.always_show    = read_num(f, o, L"always_show", c.always_show) != 0;
    c.idle_opacity   = (float)read_num(f, o, L"idle_opacity", c.idle_opacity);
    c.show_nearby    = read_num(f, o, L"show_nearby", c.show_nearby) != 0;

    const wchar_t* v = L"voice";
    c.speak          = read_num(f, v, L"speak", c.speak) != 0;
    c.voice_name     = read_str(f, v, L"voice", c.voice_name);
    c.voice_volume   = (int)read_num(f, v, L"volume", c.voice_volume);
    c.voice_rate     = (int)read_num(f, v, L"rate", c.voice_rate);
    c.voice_engine   = read_str(f, v, L"engine", c.voice_engine);
    c.voice_model    = read_str(f, v, L"voice_model", c.voice_model);

    const wchar_t* an = L"announce";
    c.announce        = read_num(f, an, L"enabled", c.announce) != 0;
    c.say_fuel        = read_num(f, an, L"fuel", c.say_fuel) != 0;
    c.say_sleep       = read_num(f, an, L"sleep", c.say_sleep) != 0;
    c.say_ferry       = read_num(f, an, L"ferry", c.say_ferry) != 0;
    c.say_border      = read_num(f, an, L"border", c.say_border) != 0;
    c.say_destination = read_num(f, an, L"destination", c.say_destination) != 0;
    c.say_lanes       = read_num(f, an, L"lanes", c.say_lanes) != 0;
    c.border_km       = read_num(f, an, L"border_km", c.border_km);
    c.destination_km  = read_num(f, an, L"destination_km", c.destination_km);
    const std::wstring text = read_str(f, v, L"text", L"");
    if (!text.empty()) {
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        c.voice_text.assign(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), c.voice_text.data(), n, nullptr, nullptr);
    }

    const wchar_t* a = L"alerts";
    c.alerts          = read_num(f, a, L"enabled", c.alerts) != 0;
    c.fuel_warn_l     = (float)read_num(f, a, L"fuel_warn_liters", c.fuel_warn_l);
    c.fuel_critical_l = (float)read_num(f, a, L"fuel_critical_liters", c.fuel_critical_l);
    c.rest_warn_min   = (int)read_num(f, a, L"rest_warn_minutes", c.rest_warn_min);
    c.search_angle    = read_num(f, a, L"search_angle", c.search_angle);
    c.search_km       = read_num(f, a, L"search_distance_km", c.search_km);
    c.smart_fuel      = read_num(f, a, L"smart_fuel", c.smart_fuel) != 0;
    c.smart_rest      = read_num(f, a, L"smart_sleep", c.smart_rest) != 0;
    c.detour_factor   = read_num(f, a, L"detour_factor", c.detour_factor);
    c.reserve_l       = (float)read_num(f, a, L"reserve_liters", c.reserve_l);

    const wchar_t* r = L"route";
    c.route          = read_num(f, r, L"enabled", c.route) != 0;
    c.strip          = read_num(f, r, L"strip", c.strip) != 0;
    c.strip_key      = (int)read_num(f, r, L"strip_key", c.strip_key);
    c.strip_items    = (int)read_num(f, r, L"strip_items", c.strip_items);
    c.strip_x        = (float)read_num(f, r, L"strip_x", c.strip_x);
    c.strip_y        = (float)read_num(f, r, L"strip_y", c.strip_y);
    c.max_detour_km  = read_num(f, r, L"max_detour_km", c.max_detour_km);
    c.max_fuel_detour_km = read_num(f, r, L"max_fuel_detour_km", c.max_fuel_detour_km);
    c.guidance       = read_num(f, r, L"guidance", c.guidance) != 0;

    const wchar_t* g = L"guide";
    c.guide          = read_num(f, g, L"enabled", c.guide) != 0;
    c.guide_auto     = read_num(f, g, L"auto", c.guide_auto) != 0;
    c.guide_key      = (int)read_num(f, g, L"key", c.guide_key);
    c.guide_model    = read_str(f, g, L"model", c.guide_model);
    c.guide_fallback_models = read_str(f, g, L"fallback_models", c.guide_fallback_models);
    c.guide_speak    = read_num(f, g, L"speak", c.guide_speak) != 0;
    c.guide_show     = read_num(f, g, L"show", c.guide_show) != 0;
    c.guide_voice_engine = read_str(f, g, L"voice_engine", c.guide_voice_engine);
    c.guide_voice    = read_str(f, g, L"google_voice", c.guide_voice);
    c.guide_tts_model = read_str(f, g, L"google_voice_model", c.guide_tts_model);
    c.fish_voice     = read_str(f, g, L"fish_voice", c.fish_voice);
    c.fish_model     = read_str(f, g, L"fish_model", c.fish_model);
    c.fish_temperature = read_num(f, g, L"fish_temperature", c.fish_temperature);
    c.fish_top_p     = read_num(f, g, L"fish_top_p", c.fish_top_p);
    c.fish_speed     = read_num(f, g, L"fish_speed", c.fish_speed);
    c.guide_screenshots = read_num(f, g, L"screenshots", c.guide_screenshots) != 0;
    c.guide_listen   = read_num(f, g, L"listen", c.guide_listen) != 0;
    c.guide_look_minutes = read_num(f, g, L"look_minutes", c.guide_look_minutes);
    c.guide_daily_limit = (int)read_num(f, g, L"daily_limit", c.guide_daily_limit);

    c.in_city_radius = read_num(f, d, L"in_city_radius", c.in_city_radius);
    c.near_km        = read_num(f, d, L"near_km", c.near_km);
    c.learn          = read_num(f, d, L"learn_from_deliveries", c.learn) != 0;
    return c;
}
