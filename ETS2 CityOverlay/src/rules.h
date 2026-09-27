#pragma once

#include "config.h"
#include "route.h"

#include <set>
#include <string>
#include <unordered_map>

// Road rules the navigation voice helps with, so the game doesn't fine you by surprise:
//  - "Speed limit 60" when the limit drops and you're above it (speed cameras fine from 5 km/h over),
//    and optionally a warning while you stay over it;
//  - headlights off while driving (low beams are required in the dark, in rain, in tunnels and in
//    daytime in some countries);
//  - diesel much cheaper or dearer across the next border on the route (the game's own prices).
struct RulesInputs
{
    float speed_kmh = 0;
    float limit_kmh = 0;        // the current speed limit, 0 = unknown / none
    bool  have_lights = false;  // the light channels have reported
    bool  low_beam = false, high_beam = false, wipers = false;
    int   game_hour = -1;       // 0..23, -1 = unknown
    std::string country;        // where the truck is ("" = unknown)
    float fuel_l = 0, range_km = 0;
    bool  fuel_warning = false; // a fuel warning is already up: no price talk then
};

class RulesMonitor
{
public:
    void load_prices(const std::wstring& dir);           // countries.csv: name, code, fuel_price
    double price(const std::string& country) const;      // per liter, 0 = unknown
    size_t price_count() const { return prices_.size(); }

    // A few times a second (game thread, not while paused). target = the job's destination key;
    // a new one forgets which borders were already talked about.
    void update(const Config& cfg, const RulesInputs& in, const RouteView* route, const std::string& target);

    // Forget the last speed limit and light state (after being switched off: no stale calls).
    void reset() { last_limit_ = 0; pending_limit_ = 0; over_since_ = 0; lights_off_since_ = 0; }

    // For the stops strip: "diesel -15%" on a border row when it's worth knowing, else "".
    std::string border_tag(const std::string& from, const std::string& to) const;

private:
    void speed(const Config& cfg, const RulesInputs& in, unsigned long long now);
    void lights(const Config& cfg, const RulesInputs& in, unsigned long long now);
    void fuel_prices(const RulesInputs& in, const RouteView& r);

    std::unordered_map<std::string, double> prices_;

    float last_limit_ = 0, pending_limit_ = 0;
    unsigned long long pending_since_ = 0;  // when the current reading first appeared
    unsigned long long last_limit_call_ = 0, over_since_ = 0, last_speeding_call_ = 0;
    unsigned long long lights_off_since_ = 0, last_lights_call_ = 0;
    bool lights_rain_said_ = false, lights_day_said_ = false;
    std::string target_;
    std::set<std::string> borders_said_;
    float fuel_max_ = 0;  // the most fuel seen: a stand-in for the tank size
};
