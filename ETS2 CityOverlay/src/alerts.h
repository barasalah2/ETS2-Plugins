#pragma once

#include "config.h"
#include "overlay.h"
#include "pois.h"
#include "route.h"

#include <string>

// Latest telemetry values the fuel/sleep logic needs. Written by plugin.cpp's channel callbacks.
struct AlertInputs
{
    bool  have_fuel = false;
    float fuel_l = 0;
    float range_km = 0;         // game's own estimate, game km
    float consumption = 0;      // liters per game km
    float speed_kmh = 0;        // speedometer
    int   rest_min = -1;        // in-game minutes until sleep is needed; -1 = fatigue off
    float local_scale = 0;      // map scale from the SDK (0 = not received yet)
    bool  have_odometer = false;
    float odometer_km = 0;
    float nav_distance_m = 0;   // route advisor: distance left on the GPS route (0 = no route)
    float nav_time_s = 0;       // route advisor: time left
};

// Fuel and sleep warnings, plus the conversion from map coordinates to the game's km.
//
// Map coordinates are compressed (~1:19) but the game counts fuel range, odometer and GPS
// distance in its own km, and it may compress cities differently from open road. Both ratios
// are learned while driving (odometer km per map meter) and saved to calibration.ini.
//
// With a job route (RouteView) the warnings use the stops actually on the route and road
// distances; without one they fall back to "nearest place ahead of the truck".
class AlertEngine
{
public:
    void init(const Config& cfg, const PoiDb* pois, const std::wstring& dir);
    void save() const;

    AlertInputs in;

    // Call on every position update: tracks the distance actually driven for calibration.
    void on_position(double x, double z, bool in_city);
    // Re-evaluates the warnings (call a few times a second). `route` and `nearest_fuel` may be null;
    // nearest_fuel is the closest station by road in any direction.
    const AlertState& update(double x, double z, double heading, const RouteView* route,
                             const NearestPlace* nearest_fuel);

    // Fuel planning: liters per game km the plugin assumes (measured, pessimistic) and the range
    // that gives with the fuel in the tank.
    double consumption() const;
    float range_km() const;

    Scales scales() const;
    double km_per_world_m() const { return road_.ratio; }  // open road (used for "Near X")

private:
    struct Zone  // open road or city
    {
        double ratio = 19.0 / 1000.0;  // game km per map meter
        bool   calibrated = false;
        double time_scale = 19.0;      // game seconds per real second, from local.scale
        int    samples = 0;
    };

    void fuel_nearby(double x, double z, double heading);
    void rest_nearby(double x, double z, double heading);
    void fuel_on_route(double x, double z, double heading, const RouteView& r);
    void rest_on_route(double x, double z, double heading, const RouteView& r);
    Target with_km(Target t) const { t.km = t.distance * road_.ratio; return t; }
    void track_fuel();
    Target road_target(const NearestPlace& n, double x, double z, double heading) const;

    Config       cfg_;
    const PoiDb* pois_ = nullptr;
    std::wstring dir_;
    AlertState   state_;

    Zone   road_, city_;
    double avg_speed_ = 65.0;  // km/h while moving, for sleep planning without a route
    bool   have_last_ = false, last_in_city_ = false;
    double last_x_ = 0, last_z_ = 0;
    double window_m_ = 0;      // map meters driven in the current window (one zone only)
    float  window_odo_ = -1;   // odometer at the start of the window

    // A warning stays up for the place it named until we pass it or refuel/sleep.
    Target   fuel_latched_, rest_latched_;          // without a route
    double   fuel_latch_at_m_ = -1, rest_latch_at_m_ = -1;  // with a route
    uint64_t latch_route_id_ = 0;
    bool     fuel_low_ = false;

    // Measured fuel use: liters per game km from the tank and the odometer (refuels ignored).
    double   measured_ = 0, measured_km_ = 0, session_km_ = 0;
    float    fuel_mark_ = -1, odo_mark_ = -1;
    unsigned long long last_fuel_log_ = 0;
    const NearestPlace* nearest_ = nullptr;
    float    fuel_at_latch_ = 0;
    int      rest_at_latch_ = 0;
};
