#include "alerts.h"
#include "log.h"
#include "spoken.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static const double kSafety = 1.15;         // extra margin on planned distances/times
static const double kTwinSeparation = 500;  // map m: closer than this = same place, other carriageway
static const double kJump = 200;            // map m in one update = teleport/ferry/train, not driving

static std::string fmt_km(double km)
{
    char b[32];
    snprintf(b, sizeof(b), "%.0f km", std::max(0.0, km));
    return b;
}

static std::string fmt_time(double minutes)
{
    const int m = std::max(0, (int)std::lround(minutes));
    char b[32];
    if (m >= 60) snprintf(b, sizeof(b), "%d h %02d min", m / 60, m % 60);
    else snprintf(b, sizeof(b), "%d min", m);
    return b;
}

static Target route_target(const RouteView::Item& it, double x, double z, double heading)
{
    Target t;
    t.valid = true;
    t.on_route = true;
    t.label = it.label;
    t.km = it.km;
    t.at_m = it.at_m;
    t.x = it.x;
    t.z = it.z;
    t.distance = std::hypot(it.x - x, it.z - z);
    t.rel_deg = bearing_to(x, z, heading, it.x, it.z);
    return t;
}

// --- calibration ----------------------------------------------------------------------------

static double read_ini(const std::wstring& file, const wchar_t* key)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(L"calibration", key, L"", buf, 64, file.c_str());
    return wcstod(buf, nullptr);
}

static void write_ini(const std::wstring& file, const wchar_t* key, double value)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%.6f", value);
    WritePrivateProfileStringW(L"calibration", key, buf, file.c_str());
}

void AlertEngine::init(const Config& cfg, const PoiDb* pois, const std::wstring& dir)
{
    cfg_ = cfg;
    pois_ = pois;
    dir_ = dir;
    const std::wstring f = dir + L"\\calibration.ini";
    auto load_zone = [&](Zone& z, const wchar_t* ratio_key, const wchar_t* time_key) {
        const double r = read_ini(f, ratio_key);
        if (r > 0.0005 && r < 0.2) { z.ratio = r; z.calibrated = true; }
        const double t = read_ini(f, time_key);
        if (t > 0.5 && t < 100) z.time_scale = t;
    };
    load_zone(road_, L"km_per_world_m", L"time_scale");
    load_zone(city_, L"km_per_world_m_city", L"time_scale_city");
    const double v = read_ini(f, L"avg_speed_kmh");
    if (v > 20 && v < 150) avg_speed_ = v;
    const double fc = read_ini(f, L"fuel_l_per_km");
    if (fc > 0.1 && fc < 1.5) { measured_ = fc; measured_km_ = 10; }  // last session's truck: a start
    log_info("distance scale: open road %.2f, cities %.2f game km per map km (%s); average speed %.0f km/h",
             road_.ratio * 1000.0, city_.ratio * 1000.0,
             road_.calibrated ? "learned" : "default until measured", avg_speed_);
}

void AlertEngine::save() const
{
    const std::wstring f = dir_ + L"\\calibration.ini";
    if (road_.calibrated) write_ini(f, L"km_per_world_m", road_.ratio);
    if (city_.calibrated) write_ini(f, L"km_per_world_m_city", city_.ratio);
    write_ini(f, L"time_scale", road_.time_scale);
    write_ini(f, L"time_scale_city", city_.time_scale);
    write_ini(f, L"avg_speed_kmh", avg_speed_);
    if (measured_km_ >= 20) write_ini(f, L"fuel_l_per_km", measured_);
}

Scales AlertEngine::scales() const
{
    Scales s;
    s.km_road = road_.ratio;
    s.km_city = city_.ratio;
    s.time_road = road_.time_scale;
    s.time_city = city_.time_scale;
    return s;
}

void AlertEngine::on_position(double x, double z, bool in_city)
{
    Zone& zone = in_city ? city_ : road_;
    if (in.local_scale > 0 && in.speed_kmh > 20.0f) {  // parked at a depot it can read oddly
        zone.time_scale = in.local_scale;
        // Until the odometer has measured this zone, the SDK's scale is the best guess.
        if (!zone.calibrated) zone.ratio = in.local_scale / 1000.0;
    }

    if (!have_last_) {
        have_last_ = true;
        last_x_ = x; last_z_ = z;
        last_in_city_ = in_city;
        return;
    }
    const double d = std::hypot(x - last_x_, z - last_z_);
    last_x_ = x; last_z_ = z;
    // Teleported, no odometer yet, or crossed a city boundary: restart the measurement window so
    // each sample belongs to one zone.
    if (d > kJump || !in.have_odometer || in_city != last_in_city_) {
        last_in_city_ = in_city;
        window_m_ = 0;
        window_odo_ = in.have_odometer ? in.odometer_km : -1;
        return;
    }
    if (window_odo_ < 0) window_odo_ = in.odometer_km;
    window_m_ += d;
    if (window_m_ < (in_city ? 500.0 : 1000.0)) return;

    const double odo = in.odometer_km - window_odo_;
    const double sample = odo / window_m_;
    if (sample > 0.0005 && sample < 0.2) {
        zone.ratio = zone.calibrated ? zone.ratio * 0.8 + sample * 0.2 : sample;
        zone.calibrated = true;
        ++zone.samples;
        // The first few samples go to the log so the conversion can be checked against the game.
        if (zone.samples <= 3 || zone.samples % 50 == 0)
            log_info("calibration (%s): drove %.0f map m = %.2f odometer km (local.scale %.1f) -> %.2f game km "
                     "per map km; GPS left %.0f m / %.0f s, speed %.0f km/h",
                     in_city ? "city" : "open road", window_m_, odo, in.local_scale, zone.ratio * 1000.0,
                     in.nav_distance_m, in.nav_time_s, in.speed_kmh);
        if (zone.samples % 10 == 0) save();
    }
    window_m_ = 0;
    window_odo_ = in.odometer_km;
}

// --- warnings -------------------------------------------------------------------------------

// The game's own range can be wildly optimistic (its average consumption may come from lighter
// driving) - on one run it fell from 512 km to 100 km in a few minutes. So the plugin measures what
// the truck really burns and plans with the more pessimistic of the two, plus a margin.
void AlertEngine::track_fuel()
{
    if (!in.have_fuel || !in.have_odometer) return;
    const bool refuelled = fuel_mark_ >= 0 && in.fuel_l > fuel_mark_ + 1.0f;
    if (fuel_mark_ < 0 || refuelled || in.odometer_km < odo_mark_) {
        fuel_mark_ = in.fuel_l;
        odo_mark_ = in.odometer_km;
        return;
    }
    const double km = in.odometer_km - odo_mark_;
    if (km < 10.0) return;
    const double sample = (fuel_mark_ - in.fuel_l) / km;
    if (sample > 0.08 && sample < 1.5) {
        measured_ = session_km_ > 0 ? measured_ * 0.7 + sample * 0.3 : sample;
        measured_km_ += km;
        session_km_ += km;
    }
    fuel_mark_ = in.fuel_l;
    odo_mark_ = in.odometer_km;
}

double AlertEngine::consumption() const
{
    // Until this session has measured ~30 km, assume at least a loaded truck's 0.40 L/km (the
    // game's own average can be far too low - that's how the Ivalo run ran dry).
    double c = session_km_ >= 30 ? measured_ : std::max(measured_, 0.40);
    c = std::max(c, (double)in.consumption);  // the game's average, if it's higher
    return std::max(c, 0.25) * 1.10;          // hills, speed and traffic
}

float AlertEngine::range_km() const
{
    if (!in.have_fuel) return 0.0f;
    const double ours = in.fuel_l / consumption();
    return (float)(in.range_km > 0 ? std::min<double>(in.range_km, ours) : ours);
}

// A station found by the road search (not necessarily on the route): distance is along the roads.
Target AlertEngine::road_target(const NearestPlace& n, double x, double z, double heading) const
{
    Target t;
    t.valid = true;
    t.by_road = true;
    t.label = n.label;
    t.x = n.x;
    t.z = n.z;
    t.distance = std::hypot(n.x - x, n.z - z);
    t.km = n.dist_m * road_.ratio;
    t.rel_deg = bearing_to(x, z, heading, n.x, n.z);
    return t;
}

const AlertState& AlertEngine::update(double x, double z, double heading, const RouteView* route,
                                      const NearestPlace* nearest_fuel)
{
    if (in.speed_kmh > 10.0f) avg_speed_ = avg_speed_ * 0.98 + in.speed_kmh * 0.02;
    nearest_ = nearest_fuel && nearest_fuel->valid ? nearest_fuel : nullptr;
    track_fuel();
    const unsigned long long now = GetTickCount64();
    if (in.have_fuel && in.speed_kmh > 10.0f && now - last_fuel_log_ > 120000) {
        last_fuel_log_ = now;  // for checking the numbers against the game later
        log_info("fuel: %.0f L, game range %.0f km, measured %.1f L/100 km over %.0f km, game average %.1f, "
                 "planning with %.1f -> range %.0f km%s",
                 in.fuel_l, in.range_km, measured_ * 100.0, measured_km_, in.consumption * 100.0, consumption() * 100.0,
                 range_km(), nearest_ ? (", nearest fuel by road " + fmt_km(nearest_->dist_m * road_.ratio)).c_str() : "");
    }
    // Use the route unless the game's GPS is clearly on a different one (e.g. your own waypoints).
    const bool use_route = route && route->valid && !(route->gps_checked && !route->matches_gps);
    if (use_route) {
        if (route->route_id != latch_route_id_) {  // positions are relative to each route
            fuel_latch_at_m_ = rest_latch_at_m_ = -1;
            latch_route_id_ = route->route_id;
        }
        fuel_on_route(x, z, heading, *route);
        rest_on_route(x, z, heading, *route);
    } else {
        state_.fuel_stop_at_m = state_.rest_stop_at_m = -1;
        fuel_nearby(x, z, heading);
        rest_nearby(x, z, heading);
    }
    return state_;
}

void AlertEngine::fuel_on_route(double x, double z, double heading, const RouteView& r)
{
    Alert& a = state_.fuel;
    const bool was_active = a.active;
    a = Alert{};
    a.none_text = "No fuel station left on your route";
    state_.fuel_stop_at_m = -1;
    if (!in.have_fuel) return;

    std::vector<const RouteView::Item*> stations;
    for (const auto& it : r.ahead)
        if (it.kinds & RouteStop::Fuel) stations.push_back(&it);

    const float range = range_km();
    const double reserve_km = cfg_.reserve_l / consumption();

    // Refuelled, or drove past the station we asked for.
    if (fuel_latch_at_m_ >= 0 && (in.fuel_l > fuel_at_latch_ + 20.0f || r.progress_m > fuel_latch_at_m_ + 500.0))
        fuel_latch_at_m_ = -1;
    size_t ni = 0;
    const RouteView::Item* next = nullptr;
    if (fuel_latch_at_m_ >= 0)
        for (size_t i = 0; i < stations.size() && !next; ++i)
            if (std::fabs(stations[i]->at_m - fuel_latch_at_m_) < 1.0) { next = stations[i]; ni = i; }
    if (!next && !stations.empty()) next = stations[0], ni = 0;
    const RouteView::Item* after = next && ni + 1 < stations.size() ? stations[ni + 1] : nullptr;

    // Road distances along the route, with margins: arrive with half the reserve at worst, and
    // only skip a station if the next one is comfortably within range.
    // A station off the route costs half its detour to get there.
    auto reach = [](const RouteView::Item* it) { return it->km + it->detour_km * 0.5; };
    const bool reach_dest = range >= r.remaining_km * 1.05 + reserve_km;
    fuel_low_ = fuel_low_ ? in.fuel_l < cfg_.fuel_warn_l + 20.0f : in.fuel_l < cfg_.fuel_warn_l;
    const bool cant_reach_next = next && range < reach(next) * 1.15 + reserve_km * 0.5;
    const bool skip_is_risky = cfg_.smart_fuel && next && !reach_dest &&
                               (after ? range < reach(after) * 1.1 + reserve_km : true);
    const bool stranded = !next && !reach_dest;  // no station left on the route, can't make it

    // When the route's next station is out of reach (or there is none), the nearest one by road
    // in any direction may still be: send the driver there instead.
    const Target escape = nearest_ ? road_target(*nearest_, x, z, heading) : Target{};
    const bool leave_route = (cant_reach_next || stranded) && escape.valid && (!next || escape.km + 1.0 < reach(next));

    if (!(fuel_low_ || cant_reach_next || skip_is_risky || stranded || fuel_latch_at_m_ >= 0)) return;

    a.active = true;
    if (leave_route) {
        a.target = escape;
    } else if (next) {
        a.target = route_target(*next, x, z, heading);
        state_.fuel_stop_at_m = next->at_m;
    }
    a.critical = in.fuel_l < cfg_.fuel_critical_l || cant_reach_next || stranded;
    // "Leave the route" with a well-filled tank is advice, not an emergency.
    const bool plenty = leave_route && range > escape.km * 2.0 && in.fuel_l > cfg_.fuel_critical_l * 2.0f;
    if (plenty) a.critical = false;
    const std::string reserve = reserve_km > 0 ? " + " + std::to_string((int)cfg_.reserve_l) + " L reserve" : "";
    const std::string place = next ? spoken_place(next->label == "Parking" ? "parking area" : next->label) : "";
    const std::string at_key = next ? std::to_string((long long)next->at_m) : "";
    if (leave_route && plenty) {
        a.headline = "REFUEL OFF YOUR ROUTE";
        a.detail = (next ? "no fuel on your route for " + fmt_km(reach(next)) : std::string("no fuel left on your route")) +
                   ", range " + fmt_km(range);
        a.speech = "Heads up: " +
                   (next ? "there's no fuel on your route for the next " + spoken_km(reach(next))
                         : std::string("there's no fuel left on your route")) +
                   ", and your range is about " + spoken_km(range) + ". The nearest " + spoken_place(escape.label) +
                   " is " + spoken_km(escape.km) + " away by road. Refuel there first.";
        a.speech_key = "escape:" + std::to_string((long long)escape.x) + ":" + std::to_string((long long)escape.z);
    } else if (leave_route) {
        a.headline = escape.km > range ? "FUEL CRITICAL - YOU MAY NOT MAKE IT" : "FUEL CRITICAL - LEAVE THE ROUTE";
        a.detail = "range " + fmt_km(range) + ", nearest fuel by road " + fmt_km(escape.km) +
                   (next ? ", next on your route " + fmt_km(reach(next)) : "");
        a.speech = escape.km > range
            ? "Fuel is critical. The nearest " + spoken_place(escape.label) + " is " + spoken_km(escape.km) +
                  " away by road and your range is about " + spoken_km(range) + ". Head there now and drive gently."
            : "Fuel is critical. Leave your route: the nearest " + spoken_place(escape.label) + " is " +
                  spoken_km(escape.km) + " away by road, and your range is only " + spoken_km(range) + ".";
        a.speech_key = "escape:" + std::to_string((long long)escape.x) + ":" + std::to_string((long long)escape.z);
    } else if (stranded) {
        a.headline = "NOT ENOUGH FUEL FOR THIS ROUTE";
        a.detail = "range " + fmt_km(range) + ", destination " + fmt_km(r.remaining_km);
        a.speech = "You don't have enough fuel for this route. Your range is " + spoken_km(range) +
                   ", and there's no fuel station left on it.";
        a.speech_key = "stranded";
    } else if (cant_reach_next) {
        a.headline = "FUEL CRITICAL";
        a.detail = "range " + fmt_km(range) + ", next station on your route is " + fmt_km(next->km) + " away";
        a.speech = range + 5 < next->km
            ? "Fuel is critical. Your range is about " + spoken_km(range) + ", but the next " + place +
                  " on your route is " + spoken_km(next->km) + " away. Look for fuel off the route, and drive gently."
            : "Fuel is critical. Your range is about " + spoken_km(range) + ", and the next " + place + " is " +
                  spoken_km(next->km) + " away. Stop there, and drive gently to save fuel.";
        a.speech_key = "critical:" + at_key;
    } else if (skip_is_risky || fuel_latch_at_m_ >= 0) {
        a.headline = "REFUEL AT NEXT STATION";
        a.detail = "range " + fmt_km(range) + ", " +
                   (after ? "the one after is " + fmt_km(after->km) + reserve
                          : "last one before the destination (" + fmt_km(r.remaining_km) + ")");
        a.speech = "Please refuel at the next " + place + ", in " + spoken_km(next->km) + ". " +
                   (after ? "The one after is too far for your tank." : "It's the last one before your destination.");
        a.speech_key = "refuel:" + at_key;
    } else {
        char b[64];
        snprintf(b, sizeof(b), "%.0f L  -  %s range", in.fuel_l, fmt_km(range).c_str());
        a.headline = "LOW FUEL";
        a.detail = b;
        a.speech = next ? "Fuel is getting low. The next " + place + " on your route is " + spoken_km(next->km) + " ahead."
                        : "Fuel is getting low.";
        a.speech_key = "low";
    }

    if ((skip_is_risky || cant_reach_next) && fuel_latch_at_m_ < 0 && next) {
        fuel_latch_at_m_ = next->at_m;
        fuel_at_latch_ = in.fuel_l;
        log_info("fuel (route): %s - %s; next %s in %.0f km, after %s; destination %.0f km",
                 a.headline.c_str(), a.detail.c_str(), next->label.c_str(), next->km,
                 after ? fmt_km(after->km).c_str() : "none", r.remaining_km);
    } else if (!was_active) {
        log_info("fuel (route): %s - %s", a.headline.c_str(), a.detail.c_str());
    }
}

void AlertEngine::rest_on_route(double x, double z, double heading, const RouteView& r)
{
    Alert& a = state_.rest;
    const bool was_active = a.active;
    a = Alert{};
    a.none_text = "No place to sleep left on your route";
    state_.rest_stop_at_m = -1;
    if (in.rest_min < 0) return;  // fatigue simulation off
    const double left = in.rest_min;

    std::vector<const RouteView::Item*> places;
    for (const auto& it : r.ahead)
        if (it.kinds & RouteStop::Rest) places.push_back(&it);

    if (rest_latch_at_m_ >= 0 && (in.rest_min > rest_at_latch_ + 60 || r.progress_m > rest_latch_at_m_ + 500.0))
        rest_latch_at_m_ = -1;
    size_t ni = 0;
    const RouteView::Item* next = nullptr;
    if (rest_latch_at_m_ >= 0)
        for (size_t i = 0; i < places.size() && !next; ++i)
            if (std::fabs(places[i]->at_m - rest_latch_at_m_) < 1.0) { next = places[i]; ni = i; }
    if (!next && !places.empty()) next = places[0], ni = 0;
    const RouteView::Item* after = next && ni + 1 < places.size() ? places[ni + 1] : nullptr;

    const bool reach_dest = left > r.remaining_min + 15;
    const bool soon = left <= cfg_.rest_warn_min;
    const bool cant_reach_next = next && left < next->minutes;
    const bool skip_is_risky = cfg_.smart_rest && next && !reach_dest &&
                               (after ? left < after->minutes * 1.1 + 15 : true);

    if (!(soon || cant_reach_next || skip_is_risky || rest_latch_at_m_ >= 0)) return;

    a.active = true;
    if (next) {
        a.target = route_target(*next, x, z, heading);
        state_.rest_stop_at_m = next->at_m;
    }
    a.critical = left <= 30 || cant_reach_next;
    const std::string place = next ? spoken_place(next->label == "Parking" ? "parking area" : next->label) : "";
    const std::string at_key = next ? std::to_string((long long)next->at_m) : "";
    if (left <= 0) {
        a.headline = "SLEEP NOW";
        a.speech = "You need to sleep now." +
                   (next ? " The nearest place to rest on your route is " + spoken_km(next->km) + " ahead." : std::string());
        a.speech_key = "now";
    } else if (skip_is_risky || cant_reach_next || rest_latch_at_m_ >= 0) {
        a.headline = "SLEEP AT NEXT STOP";
        a.detail = cant_reach_next
            ? fmt_time(left) + " left, it is ~" + fmt_time(next->minutes) + " away"
            : fmt_time(left) + " left, " +
                  (after ? "the one after is ~" + fmt_time(after->minutes) + " + margin"
                         : "last place to sleep before the destination");
        a.speech = cant_reach_next
            ? "You're getting tired. Stop at the next " + place + ", " + spoken_km(next->km) + " ahead."
            : "You'll need to sleep in about " + spoken_minutes(left) + ". Rest at the next " + place + ", " +
                  spoken_km(next->km) + " ahead. " +
                  (after ? "The one after is too far." : "It's the last place to rest before your destination.");
        a.speech_key = (cant_reach_next ? "critical:" : "next:") + at_key;
    } else {
        a.headline = "SLEEP SOON";
        a.detail = "in " + fmt_time(left);
        a.speech = "You'll need to sleep in about " + spoken_minutes(left) + ".";
        a.speech_key = "soon";
    }

    if ((skip_is_risky || cant_reach_next) && rest_latch_at_m_ < 0 && next) {
        rest_latch_at_m_ = next->at_m;
        rest_at_latch_ = in.rest_min;
        log_info("sleep (route): %s - %s; next %s in %.0f km / %.0f min; destination %.0f min",
                 a.headline.c_str(), a.detail.c_str(), next->label.c_str(), next->km, next->minutes, r.remaining_min);
    } else if (!was_active) {
        log_info("sleep (route): %s - %s", a.headline.c_str(), a.detail.c_str());
    }
}

// --- without a route: nearest places ahead of the truck --------------------------------------

void AlertEngine::fuel_nearby(double x, double z, double heading)
{
    Alert& a = state_.fuel;
    const bool was_active = a.active;
    a = Alert{};
    a.none_text = "No fuel station found ahead";
    if (!in.have_fuel) return;

    const double max_world = cfg_.search_km / road_.ratio;
    // Refuelled, or drove past the station we asked for (find_ahead lets go of a sticky target
    // once it's behind us): drop the latch and decide afresh.
    if (fuel_latched_.valid) {
        const Target t = pois_->find_ahead(PoiKind::Fuel, x, z, heading, cfg_.search_angle, max_world, fuel_latched_);
        if (in.fuel_l > fuel_at_latch_ + 20.0f || t.x != fuel_latched_.x || t.z != fuel_latched_.z)
            fuel_latched_ = Target{};
    }
    const Target next = with_km(pois_->find_ahead(PoiKind::Fuel, x, z, heading, cfg_.search_angle, max_world,
                                                  fuel_latched_));
    const Target after = with_km(pois_->find_after(PoiKind::Fuel, x, z, heading, cfg_.search_angle, max_world,
                                                   next, kTwinSeparation));

    const float range = range_km();
    const double reserve_km = cfg_.reserve_l / consumption();
    // The road search knows the real distance to the nearest station; prefer it to the
    // straight-line guess when it's nearer or there's nothing ahead.
    Target next_by_road;
    if (nearest_) next_by_road = road_target(*nearest_, x, z, heading);
    const bool use_road = next_by_road.valid && (!next.valid || next_by_road.km <= next.km * cfg_.detour_factor);
    const double need_next = use_road ? next_by_road.km * 1.15 + reserve_km * 0.5
                                      : next.km * cfg_.detour_factor * 1.15 + reserve_km * 0.5;
    const double need_after = (after.valid ? after.km * cfg_.detour_factor : cfg_.search_km) * kSafety + reserve_km;
    // With a GPS route that the tank comfortably covers, there's nothing to plan.
    const bool route_covered = in.nav_distance_m > 0 && range * 0.9 > in.nav_distance_m / 1000.0;

    // Plain threshold, with hysteresis so it doesn't blink while refuelling.
    fuel_low_ = fuel_low_ ? in.fuel_l < cfg_.fuel_warn_l + 20.0f : in.fuel_l < cfg_.fuel_warn_l;
    const bool cant_reach_next = (next.valid || use_road) && range < need_next;
    const bool skip_is_risky = cfg_.smart_fuel && next.valid && !route_covered && range < need_after;

    if (!(fuel_low_ || cant_reach_next || skip_is_risky || fuel_latched_.valid)) return;

    a.active = true;
    a.target = use_road ? next_by_road : next;
    a.critical = in.fuel_l < cfg_.fuel_critical_l || cant_reach_next;
    const std::string place = spoken_place(next.label == "Parking" ? "parking area" : next.label);
    const std::string where = next.valid ? spoken_km(next.km) + " ahead" : "";
    if (cant_reach_next) {
        a.headline = "FUEL CRITICAL";
        a.detail = "range " + fmt_km(range) + ", nearest station " + fmt_km(a.target.km) + (use_road ? " by road" : "");
        a.speech = "Fuel is critical. Your range is about " + spoken_km(range) + ". The nearest " +
                   spoken_place(a.target.label) + " is " + spoken_km(a.target.km) +
                   (use_road ? " away by road." : " away.");
        a.speech_key = "critical";
    } else if (skip_is_risky || fuel_latched_.valid) {
        a.headline = "REFUEL AT NEXT STATION";
        // Say why: the next-but-one plus the reserve is more than the tank covers.
        const std::string reserve = reserve_km > 0 ? " + " + std::to_string((int)cfg_.reserve_l) + " L reserve" : "";
        a.detail = "range " + fmt_km(range) + ", " +
                   (after.valid ? "the one after is ~" + fmt_km(after.km * cfg_.detour_factor) + reserve
                                : "nothing else within " + fmt_km(cfg_.search_km));
        a.speech = "Please refuel at the next " + place + ", about " + where + ". The one after is too far for your tank.";
        a.speech_key = "refuel:" + std::to_string((long long)next.x) + ":" + std::to_string((long long)next.z);
    } else {
        char b[64];
        snprintf(b, sizeof(b), "%.0f L  -  %s range", in.fuel_l, fmt_km(range).c_str());
        a.headline = "LOW FUEL";
        a.detail = b;
        a.speech = next.valid ? "Fuel is getting low. There's a " + place + " about " + where + "." : "Fuel is getting low.";
        a.speech_key = "low";
    }

    if ((skip_is_risky || cant_reach_next) && !fuel_latched_.valid && next.valid) {
        fuel_latched_ = next;
        fuel_at_latch_ = in.fuel_l;
        log_info("fuel: %s - %s; next %s %.0f km, after %s; range %.0f km, need %.0f km",
                 a.headline.c_str(), a.detail.c_str(), next.label.c_str(), next.km,
                 after.valid ? fmt_km(after.km).c_str() : "none", range, need_after);
    } else if (!was_active) {
        log_info("fuel: %s - %s", a.headline.c_str(), a.detail.c_str());
    }
}

void AlertEngine::rest_nearby(double x, double z, double heading)
{
    Alert& a = state_.rest;
    const bool was_active = a.active;
    a = Alert{};
    a.none_text = "No rest area found ahead";
    if (in.rest_min < 0) return;  // fatigue simulation off

    const double max_world = cfg_.search_km / road_.ratio;
    // Slept (timer jumped up), or drove past the place we asked for.
    if (rest_latched_.valid) {
        const Target t = pois_->find_ahead(PoiKind::Rest, x, z, heading, cfg_.search_angle, max_world, rest_latched_);
        if (in.rest_min > rest_at_latch_ + 60 || t.x != rest_latched_.x || t.z != rest_latched_.z)
            rest_latched_ = Target{};
    }
    const Target next = with_km(pois_->find_ahead(PoiKind::Rest, x, z, heading, cfg_.search_angle, max_world,
                                                  rest_latched_));
    const Target after = with_km(pois_->find_after(PoiKind::Rest, x, z, heading, cfg_.search_angle, max_world,
                                                   next, kTwinSeparation));

    // Speedometer km/h is game km per game hour, so this gives in-game minutes like rest.stop.
    const double minutes_to = next.km * cfg_.detour_factor / avg_speed_ * 60.0;
    const double minutes_after = (after.valid ? after.km * cfg_.detour_factor : cfg_.search_km) / avg_speed_ * 60.0;
    const bool arrive_first = in.nav_time_s > 0 && in.rest_min > in.nav_time_s / 60.0 * 1.1 + 15;

    const bool soon = in.rest_min <= cfg_.rest_warn_min;
    const bool cant_reach_next = next.valid && in.rest_min < minutes_to * kSafety;
    const bool skip_is_risky = cfg_.smart_rest && next.valid && !arrive_first &&
                               in.rest_min < minutes_after * 1.2 + 15;

    if (!(soon || cant_reach_next || skip_is_risky || rest_latched_.valid)) return;

    a.active = true;
    a.target = next;
    a.critical = in.rest_min <= 30 || cant_reach_next;
    const std::string place = spoken_place(next.label == "Parking" ? "parking area" : next.label);
    if (in.rest_min <= 0) {
        a.headline = "SLEEP NOW";
        a.speech = "You need to sleep now." +
                   (next.valid ? " There's a " + place + " about " + spoken_km(next.km) + " ahead." : std::string());
        a.speech_key = "now";
    } else if (skip_is_risky || cant_reach_next || rest_latched_.valid) {
        a.headline = "SLEEP AT NEXT STOP";
        a.detail = cant_reach_next
            ? fmt_time(in.rest_min) + " left, this one is ~" + fmt_time(minutes_to) + " away"
            : fmt_time(in.rest_min) + " left, " +
                  (after.valid ? "the one after is ~" + fmt_time(minutes_after) + " + margin"
                               : "nothing else within " + fmt_time(minutes_after));
        a.speech = "You'll need to sleep in about " + spoken_minutes(in.rest_min) + ". Rest at the next " + place +
                   ", about " + spoken_km(next.km) + " ahead.";
        a.speech_key = "next:" + std::to_string((long long)next.x) + ":" + std::to_string((long long)next.z);
    } else {
        a.headline = "SLEEP SOON";
        a.detail = "in " + fmt_time(in.rest_min);
        a.speech = "You'll need to sleep in about " + spoken_minutes(in.rest_min) + ".";
        a.speech_key = "soon";
    }

    if ((skip_is_risky || cant_reach_next) && !rest_latched_.valid && next.valid) {
        rest_latched_ = next;
        rest_at_latch_ = in.rest_min;
        log_info("sleep: %s - %s; next %s %.0f km (~%.0f min at %.0f km/h)", a.headline.c_str(), a.detail.c_str(),
                 next.label.c_str(), next.km, minutes_to, avg_speed_);
    } else if (!was_active) {
        log_info("sleep: %s - %s", a.headline.c_str(), a.detail.c_str());
    }
}
