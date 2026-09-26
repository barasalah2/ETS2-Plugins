#pragma once

#include <string>

struct Config
{
    // [overlay]
    bool        enabled        = true;
    int         toggle_key     = 0x78;   // virtual-key code, default F9
    bool        toggle_ctrl    = true;   // require Ctrl with the toggle key
    std::wstring font_path     = L"C:\\Windows\\Fonts\\segoeuib.ttf";
    float       font_size      = 34.0f;  // city name size in pixels at 1080p; scales with resolution
    float       position_y     = 0.035f; // top of the panel, as a fraction of screen height
    float       banner_seconds = 6.0f;   // how long the panel stays fully visible after entering a city
    bool        always_show    = true;   // keep a dimmed panel after the banner fades
    float       idle_opacity   = 0.70f;  // panel opacity after the banner when always_show=1
    bool        show_nearby    = true;   // show "Near X · 12 km" when between cities

    // [voice]
    bool        speak          = false;  // announce the city with the Windows text-to-speech voice
    std::string voice_text     = "Welcome to {city}";  // {city} and {country} are replaced
    std::wstring voice_name;             // part of an installed voice's name, e.g. "Zira"; empty = default
    int         voice_volume   = 100;    // 0-100
    int         voice_rate     = 0;      // -10 (slow) to 10 (fast)
    std::wstring voice_engine  = L"piper";                     // piper (natural) or windows
    std::wstring voice_model   = L"en_GB-jenny_dioco-medium";  // Piper voice file in voice\

    // [announce] spoken alerts
    bool        announce       = true;
    bool        say_fuel       = true, say_sleep = true, say_ferry = true, say_border = true, say_destination = true;
    bool        say_lanes      = true;   // exits, turns and lanes ahead (only when our route matches the GPS)
    double      border_km      = 10.0;   // game km before a border
    double      destination_km = 5.0;    // game km before the destination

    // [alerts]
    bool        alerts          = true;
    float       fuel_warn_l     = 100.0f;  // warn below this many liters
    float       fuel_critical_l = 40.0f;   // turn the warning red below this
    int         rest_warn_min   = 120;     // warn when sleep is due within this many in-game minutes
    double      search_angle    = 60.0;    // degrees either side of the heading that count as "ahead"
    double      search_km       = 300.0;   // game km; ignore places further than this
    bool        smart_fuel      = true;    // warn when fuel won't reach the station after the next one
    bool        smart_rest      = true;    // same for the sleep timer and rest places
    double      detour_factor   = 1.3;     // road distance / straight-line distance
    float       reserve_l       = 30.0f;   // liters to keep in the tank when planning

    // [route]
    bool        route          = true;    // compute the job route from the map
    bool        strip          = true;    // show the upcoming-stops strip
    int         strip_key      = 0x79;    // virtual-key code (with Ctrl), default F10
    int         strip_items    = 7;
    float       strip_x        = 0.985f;  // right edge of the strip, fraction of screen width
    float       strip_y        = 0.26f;   // top of the strip, fraction of screen height
    double      max_detour_km  = 8.0;     // game km: longest detour for a stop to count as "on the route"
    double      max_fuel_detour_km = 20.0; // game km: the same for fuel (stations can be sparse)
    bool        guidance       = true;    // panel with the next exit/turn and its lanes

    // [guide] AI co-driver (Google Gemini, free tier)
    bool        guide          = true;    // needs a Gemini API key; off without one
    bool        guide_auto     = true;    // talks by itself: new job, cities, borders, events, now and then
    int         guide_key      = 0x7A;    // Ctrl + this key: tap = about here, hold = talk (default F11)
    // Free tier (per model, per day): the Flash models 20 requests, Flash Lite 500, Gemma 14,400.
    std::wstring guide_model   = L"gemini-3.5-flash-lite";  // 500 free requests a day, fast
    std::wstring guide_fallback_models = L"gemma-4-31b-it"; // comma-separated, when it's used up or busy (14,400 a day)
    bool        guide_speak    = true;
    bool        guide_show     = true;
    std::wstring guide_voice_engine = L"fish";             // fish (fish.audio), google or local (Piper)
    std::wstring guide_voice   = L"Sulafat";               // Google voice for its replies
    std::wstring guide_tts_model = L"gemini-3.8-flash-tts";
    std::wstring fish_voice;                               // fish.audio voice (reference_id), e.g. your clone
    std::wstring fish_model    = L"s2.1-pro-free";
    double      fish_temperature = 0.7, fish_top_p = 0.7, fish_speed = 1.0;
    bool        guide_screenshots = true;  // let it see the game picture (sent to Google)
    bool        guide_listen   = true;    // hold the key to talk to it (microphone, sent to Google)
    double      guide_chat_minutes = 10;  // real minutes between remarks when nothing happens (0 = never)
    int         guide_daily_limit = 300;  // requests a day at most (the free tier has its own limit too)

    // [detection]
    double      in_city_radius = 1800.0;  // world units (m) from a city point that count as "in" it
    double      near_km        = 250.0;   // max distance for "Near X", game km
    bool        learn          = true;    // record city positions from job deliveries
};

// Loads <dir>\ets2_city_overlay.ini; missing keys keep their defaults.
Config load_config(const std::wstring& dir);
