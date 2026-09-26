#pragma once

#include "cities.h"
#include "config.h"
#include "pois.h"
#include "route.h"

#include <cstdint>
#include <string>
#include <vector>

// Hooks IDXGISwapChain::Present / Present1 (DirectX 11) and draws the city panel with Dear ImGui.
bool overlay_install(const Config& cfg);
void overlay_uninstall();

// Called from telemetry callbacks (game thread). Thread-safe.
void overlay_set_location(const Location& loc);
void overlay_set_paused(bool paused);

// One warning box, fully worded by alerts.cpp; the overlay only draws it.
struct Alert
{
    bool        active = false, critical = false;
    std::string headline, detail;
    std::string none_text;  // shown instead of the target line when no place was found
    Target      target;
    std::string speech;      // the same warning, worded to be spoken
    std::string speech_key;  // changes when the warning means something new (so it's said again)
};

struct AlertState
{
    Alert fuel, rest;
    // The route stop each warning names (map meters along the route; -1 = none), so the strip
    // can highlight it.
    double fuel_stop_at_m = -1, rest_stop_at_m = -1;
};

// The upcoming-stops strip: what's next on the job route.
struct StripItem
{
    uint8_t     kinds = 0;  // RouteStop kinds
    std::string label;
    float       km = 0, minutes = 0;
    uint8_t     tone = 0;   // 0 normal, 1 recommended (amber), 2 urgent (red)
};

struct StripState
{
    bool        visible = false;
    std::string title;      // "To Quarry, Prague"
    std::string summary;    // "488 km  ·  6 h 05 min"
    std::string note;       // e.g. "differs from your GPS" (empty when fine)
    std::vector<StripItem> items;
};
void overlay_set_strip(const StripState& strip);

// The next exit or turn on the route, with its lanes (drawn under the city name).
struct Guidance
{
    bool        active = false;
    Turn        turn;
    float       km = 0;
    std::string text;       // "Take the exit on the right"
    std::string then_text;  // "then turn left" (a second maneuver right after), or ""
};
void overlay_set_guidance(const Guidance& g);

// The co-driver's words, shown on the left for a while after they arrive.
struct GuideCard
{
    bool        active = false;
    std::string title, text;
    std::string status;  // "Thinking..." while a reply is on its way (text is empty then)
};
void overlay_set_guide(const GuideCard& card);
void overlay_set_alerts(const AlertState& alerts);
