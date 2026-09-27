// ETS2 City Overlay - SCS telemetry plugin that shows the current city name inside the game.
//
// The game loads every DLL in bin\win_x64\plugins\ and calls scs_telemetry_init. We listen to the
// truck's world position, look it up against the map's city boundaries, and draw the result on
// the game's own swapchain (see overlay.cpp).

#include "alerts.h"
#include "cities.h"
#include "codriver.h"
#include "config.h"
#include "log.h"
#include "mic.h"
#include "overlay.h"
#include "pois.h"
#include "route.h"
#include "rules.h"
#include "announce.h"
#include "speech.h"
#include "spoken.h"

#include <windows.h>

#include <scssdk_telemetry.h>
#include <scssdk_eut2.h>
#include <scssdk_telemetry_eut2.h>
#include <common/scssdk_telemetry_common_configs.h>
#include <common/scssdk_telemetry_common_gameplay_events.h>
#include <common/scssdk_telemetry_common_channels.h>
#include <common/scssdk_telemetry_trailer_common_channels.h>
#include <common/scssdk_telemetry_truck_common_channels.h>

#include <cmath>
#include <cstring>
#include <deque>
#include <set>
#include <string>

static Config      g_cfg;
static CityDb      g_db;
static std::string g_current_city;  // id of the city we are in, "" when outside
static bool        g_have_pos = false;
static double      g_x = 0, g_z = 0;
static double      g_heading = 0;          // SDK units: 0..1, 0 = north, counter-clockwise

// Alerts
static PoiDb       g_pois;
static AlertEngine g_alerts;
static ULONGLONG   g_last_alert_update = 0;
static ULONGLONG   g_last_locate = 0;
static bool        g_paused = true;
static std::string g_last_spoken;          // city id last announced
static ULONGLONG   g_last_spoken_at = 0;

struct Job { std::string dest_id, dest_name; };
static Job g_job;  // last destination, kept until delivery for city learning

// Job route
static RouteEngine  g_route;
static RouteTracker g_tracker;
static std::string  g_route_target;  // "city.company" we route to ("" = no job)
static std::string  g_route_label;
static bool         g_route_wanted = false;
static bool         g_trailer_connected = false;

// The current job, as the game describes it (empty dest_city = no job).
struct JobState
{
    std::string dest_city, dest_company, dest_city_name, dest_company_name;
    std::string src_city, src_company, src_city_name, src_company_name;
    std::string market;       // cargo_market, quick_job, freight_market, external_contracts, ...
    std::string cargo;        // display name, in the game's language
    float cargo_kg = 0;
    uint32_t due = 0;         // game time (minutes) the delivery window ends; 0 = none
    uint64_t income = 0;
    bool loaded = true, special = false;
    bool picked_up = false;   // on the way to the destination; stays true for the rest of the job
    bool active() const { return !dest_city.empty(); }
};
static JobState g_jobstate;
static ULONGLONG    g_last_route_request = 0;
static uint64_t     g_gps_logged_route = 0;
static RouteView    g_view;  // last route view, for "Near X" distances along the route
static RulesMonitor g_rules;   // speed limit, headlights, fuel prices across borders
static std::string  g_country; // the country the truck is in, as best we know
static bool         g_have_lights = false, g_low_beam = false, g_high_beam = false, g_wipers = false;
static Location     g_loc;   // where we are, for the co-driver

// Co-driver
static uint32_t  g_game_time = 0;       // in-game minutes since the first day's midnight
static bool      g_have_game_time = false;
static float     g_speed_limit = 0;     // navigation speed limit, m/s (0 = none)
static ULONGLONG g_brief_due = 0;       // a new job: brief once its route is known
static std::set<std::string> g_border_said;
static ULONGLONG g_last_look = 0;
struct Happened { ULONGLONG at; std::string text; };
static std::deque<Happened> g_happened;  // recent events for the situation report
static AlertState g_alert_state;         // the last warnings, for the report
static void tell_codriver(MomentInfo m);

static std::wstring data_dir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&data_dir), &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L"\\/"));
    return dir + L"\\ets2_city_overlay";
}

static void replace_all(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
}

static void announce(const Location& loc)
{
    if (!g_cfg.speak || g_paused) return;
    // Don't repeat a city when the route clips its edge again a moment later.
    const ULONGLONG now = GetTickCount64();
    if (loc.id == g_last_spoken && now - g_last_spoken_at < 3 * 60 * 1000) return;
    g_last_spoken = loc.id;
    g_last_spoken_at = now;

    std::string text = g_cfg.voice_text;
    replace_all(text, "{city}", loc.name);
    replace_all(text, "{country}", loc.country);
    speech_say(text, SpeechPriority::Low, "city", 20);
}

static void update_location(bool force)
{
    if (!g_have_pos) return;
    const ULONGLONG now = GetTickCount64();
    if (!force && now - g_last_locate < 250) return;
    g_last_locate = now;

    Location loc = g_db.locate(g_x, g_z, g_current_city, g_cfg.in_city_radius,
                               g_cfg.near_km / g_alerts.km_per_world_m());
    loc.km = loc.distance * g_alerts.km_per_world_m();
    // A city on the route: say how far it is along the road, like the strip does.
    if (loc.kind == Location::Near && g_view.valid)
        for (const auto& it : g_view.ahead)
            if ((it.kinds & RouteStop::City) && it.label == loc.name) {
                loc.km = it.km;
                break;
            }
    const std::string city = loc.kind == Location::InCity ? loc.id : std::string();
    if (city != g_current_city) {
        if (city.empty())
            log_info("left city '%s' at x=%.0f z=%.0f", g_current_city.c_str(), g_x, g_z);
        else
        {
            log_info("entered '%s' (%s) at x=%.0f z=%.0f, %.0f m from centre",
                     loc.name.c_str(), loc.id.c_str(), g_x, g_z, loc.distance);
            announce(loc);
            if (g_cfg.guide && g_cfg.guide_auto && !g_paused) {
                MomentInfo m;
                m.kind = Moment::City;
                m.place_id = loc.id;
                m.place = loc.name;
                m.country = loc.country;
                tell_codriver(std::move(m));
            }
        }
        g_current_city = city;
    }
    g_loc = loc;
    if (loc.kind == Location::InCity && !loc.country.empty()) g_country = loc.country;
    overlay_set_location(loc);
}

static void request_route(const char* why, ULONGLONG min_interval_ms = 4000)
{
    if (!g_cfg.route || !g_route.available() || g_route_target.empty() || !g_have_pos) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_route_request < min_interval_ms) return;
    g_last_route_request = now;
    g_route_wanted = false;
    RouteRequest rq;
    rq.x = g_x;
    rq.z = g_z;
    rq.heading = g_heading;
    rq.target_key = g_route_target;
    rq.dest_label = g_route_label;
    rq.max_detour_m = g_cfg.max_detour_km / g_alerts.km_per_world_m();
    rq.max_fuel_detour_m = g_cfg.max_fuel_detour_km / g_alerts.km_per_world_m();
    g_route.request(rq);
    log_info("route: computing to '%s' (%s)", g_route_target.c_str(), why);
}

static std::string fmt_duration(double minutes)
{
    const int m = std::max(0, (int)std::lround(minutes));
    char b[32];
    snprintf(b, sizeof(b), "%d h %02d min", m / 60, m % 60);
    return b;
}

// Rows for the upcoming-stops strip: the destination and the stops the warnings name always
// make it in; the rest are the next few of each kind.
static StripState build_strip(const RouteView& v, const AlertState& a)
{
    StripState s;
    if (!g_cfg.strip || !v.valid) return s;
    s.visible = true;
    s.title = "To " + v.dest_label;
    char b[96];
    snprintf(b, sizeof(b), "%.0f km  \xC2\xB7  %s", v.remaining_km, fmt_duration(v.remaining_min).c_str());
    s.summary = b;
    if (v.gps_checked && !v.matches_gps) s.note = "Your GPS takes a different route";

    auto is_named = [&](const RouteView::Item& it, double at_m, uint8_t kind) {
        return at_m >= 0 && (it.kinds & kind) && std::fabs(it.at_m - at_m) < 1.0;
    };
    const size_t limit = (size_t)std::max(2, g_cfg.strip_items);
    std::vector<bool> take(v.ahead.size(), false);
    size_t taken = 0;
    for (size_t i = 0; i < v.ahead.size(); ++i) {
        const auto& it = v.ahead[i];
        if ((it.kinds & RouteStop::Destination) || is_named(it, a.fuel_stop_at_m, RouteStop::Fuel) ||
            is_named(it, a.rest_stop_at_m, RouteStop::Rest)) {
            take[i] = true;
            ++taken;
        }
    }
    int fuel = 0, rest = 0, city = 0;
    for (size_t i = 0; i < v.ahead.size() && taken < limit; ++i) {
        if (take[i]) continue;
        const uint8_t k = v.ahead[i].kinds;
        bool want = false;
        if (k & (RouteStop::Ferry | RouteStop::Border)) want = true;
        else if (k & RouteStop::Fuel) want = fuel++ < 2;
        else if (k & RouteStop::Rest) want = rest++ < 2;
        else if (k & RouteStop::City) want = city++ < 3;
        if (want) {
            take[i] = true;
            ++taken;
        }
    }
    std::string country = g_country;  // for "diesel -15%" on each border: the one before it
    for (size_t i = 0; i < v.ahead.size(); ++i) {
        const auto& it = v.ahead[i];
        const std::string from = country;
        if (it.kinds & RouteStop::Border) country = it.label;
        if (!take[i]) continue;
        StripItem row;
        row.kinds = it.kinds;
        row.label = it.label;
        if ((it.kinds & RouteStop::Border) && g_cfg.say_fuel_prices) {
            const std::string tag = g_rules.border_tag(from, it.label);
            if (!tag.empty()) row.label += " (" + tag + ")";
        }
        if (it.detour_km >= 3.0) row.label += " (+" + std::to_string((int)std::lround(it.detour_km)) + " km)";
        row.km = (float)it.km;
        row.minutes = (float)it.minutes;
        if (is_named(it, a.fuel_stop_at_m, RouteStop::Fuel)) row.tone = a.fuel.critical ? 2 : 1;
        if (is_named(it, a.rest_stop_at_m, RouteStop::Rest)) row.tone = std::max<uint8_t>(row.tone, a.rest.critical ? 2 : 1);
        // A place to sleep you can't reach before you must sleep: dimmed.
        const int rest = g_alerts.in.rest_min;
        if (row.tone == 0 && (it.kinds & RouteStop::Rest) && !(it.kinds & RouteStop::Fuel) && rest >= 0 &&
            it.minutes > rest)
            row.tone = 3;
        s.items.push_back(row);
    }
    return s;
}

// The next exit or turn within 3 km, when our route is the one the GPS is on.
static Guidance build_guidance(const RouteView& v, double time_scale)
{
    Guidance g;
    if (!g_cfg.guidance || !v.valid || !v.matches_gps || std::fabs(v.gps_ratio - 1.0) >= 0.1) return g;
    const RouteView::Item* m1 = nullptr;
    const RouteView::Item* m2 = nullptr;
    for (const auto& it : v.ahead) {
        if (!(it.kinds & RouteStop::Maneuver) || it.km < 0.03) continue;
        if (!m1) m1 = &it;
        else { m2 = &it; break; }
    }
    // About 25 real seconds ahead (the game compresses time too: ~10 km at 80 km/h), at least 1 km.
    if (!m1 || (m1->km > 1.0 && seconds_to(m1->km, g_alerts.in.speed_kmh, time_scale) > 25.0)) return g;
    std::string text = spoken_turn(m1->turn.turn, m1->turn.flags, m1->turn.exit_no);
    if (text.empty()) return g;
    text[0] = (char)std::toupper((unsigned char)text[0]);
    g.active = true;
    g.turn = m1->turn;
    g.km = (float)m1->km;
    g.text = text;
    if (m2 && seconds_to(m2->km - m1->km, g_alerts.in.speed_kmh, time_scale) < 4) g.then_text = "then " + spoken_turn(m2->turn.turn, m2->turn.flags, m2->turn.exit_no);
    return g;
}

// --- co-driver --------------------------------------------------------------------------------

static std::string game_clock(bool with_day)
{
    if (!g_have_game_time) return {};
    static const char* days[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    const uint32_t t = g_game_time;  // day 0 is a Monday, as the game's own calendar shows
    char b[48];
    if (with_day) snprintf(b, sizeof(b), "%s %02u:%02u", days[(t / 1440) % 7], (t / 60) % 24, t % 60);
    else snprintf(b, sizeof(b), "%02u:%02u", (t / 60) % 24, t % 60);
    return b;
}

static std::string where_words()
{
    const Location& l = g_loc;
    const std::string country = l.country.empty() ? "" : ", " + l.country;
    if (l.kind == Location::InCity) return "in " + l.name + country;
    if (l.kind == Location::Near) return "near " + l.name + country;
    return "on the open road";
}

static std::string round_km(double km)
{
    char b[32];
    snprintf(b, sizeof(b), "%.0f km", km < 20 ? std::round(km) : std::round(km / 5) * 5);
    return b;
}

static std::string game_duration(double minutes)
{
    const int m = std::max(0, (int)std::lround(minutes));
    char b[48];
    if (m < 60) snprintf(b, sizeof(b), "%d min", m);
    else snprintf(b, sizeof(b), "%d h %02d min", m / 60, m % 60);
    return b;
}

// Plain sentences for the model: when, where, the job, what's ahead, fuel/sleep, recent events.
static std::string situation(const RouteView& v, const AlertState& a)
{
    std::string r;
    if (g_have_game_time) {
        const uint32_t h = (g_game_time / 60) % 24;
        const char* part = h < 5 ? "night" : h < 12 ? "morning" : h < 17 ? "afternoon" : h < 21 ? "evening" : "night";
        r += "Game time: " + game_clock(true) + " (" + part + ").\n";
    }
    r += "Where: " + where_words();
    if (g_loc.kind == Location::Near && g_loc.km > 0) r += ", about " + round_km(g_loc.km) + " away";
    r += ".\n";
    const float kmh = g_alerts.in.speed_kmh;
    if (std::fabs(kmh) < 3) {
        r += "The truck is stopped.\n";
    } else {
        char b[96];
        snprintf(b, sizeof(b), "Driving at %.0f km/h", std::fabs(kmh));
        r += b;
        if (g_speed_limit > 1) {
            snprintf(b, sizeof(b), " (speed limit %.0f)", g_speed_limit * 3.6f);
            r += b;
        }
        r += ".\n";
    }

    const JobState& j = g_jobstate;
    if (j.active()) {
        const bool pickup = !j.picked_up;
        r += "Job: " + (j.cargo.empty() ? std::string("cargo") : j.cargo);
        if (j.cargo_kg > 500) {
            char b[32];
            snprintf(b, sizeof(b), " (%.0f t)", j.cargo_kg / 1000.0f);
            r += b;
        }
        r += " from " + (j.src_company_name.empty() ? "" : j.src_company_name + " in ") + j.src_city_name + " to " +
             (j.dest_company_name.empty() ? "" : j.dest_company_name + " in ") + j.dest_city_name + ".";
        if (pickup) r += " We're on the way to pick it up first.";
        if (v.valid)
            r += " " + round_km(v.remaining_km) + " to go" + (pickup ? " to the pick-up" : "") + ", about " +
                 game_duration(v.remaining_min) + " of game time.";
        if (j.due && g_have_game_time) {
            if (j.due > g_game_time) r += " Delivery due in " + game_duration(j.due - g_game_time) + " of game time.";
            else r += " The delivery is already late.";
        }
        if (j.income) r += " It pays " + std::to_string(j.income) + " euros.";
        r += "\n";
    } else {
        r += "No job right now.\n";
    }

    if (v.valid) {
        std::string ahead, borders;
        int cities = 0, fuel = 0, rest = 0, ferries = 0, nb = 0;
        for (const auto& it : v.ahead) {
            std::string what;
            if (it.kinds & RouteStop::Destination) {
                what = "the destination (" + v.dest_label + ")";
            } else if (it.kinds & RouteStop::Border) {
                if (nb < 5) borders += std::string(nb++ ? ", " : "") + it.label + " in " + round_km(it.km);
                continue;
            } else if (it.kinds & RouteStop::Ferry) {
                if (ferries++ < 2) what = "a ferry or train crossing";
            } else if (it.kinds & RouteStop::City) {
                if (cities++ < 4) what = "the city of " + it.label;
            } else if (it.kinds & RouteStop::Fuel) {
                if (fuel++ < 1) what = "the next fuel station";
            } else if (it.kinds & RouteStop::Rest) {
                if (rest++ < 1) what = "a parking area";
            }
            if (what.empty()) continue;
            ahead += (ahead.empty() ? "" : ", ") + what + " in " + round_km(it.km);
        }
        if (!ahead.empty()) r += "Ahead on the route: " + ahead + ".\n";
        if (!borders.empty()) r += "Countries we'll enter: " + borders + ".\n";
    }

    if (g_alerts.in.have_fuel) {
        r += "Fuel: " + std::to_string((int)std::lround(g_alerts.in.fuel_l)) + " litres, enough for about " +
             round_km(g_alerts.range_km()) + ".";
        if (a.fuel.active) r += " The navigation is already warning: " + a.fuel.headline + ".";
        r += "\n";
    }
    if (g_alerts.in.rest_min >= 0) {
        r += "Sleep needed in " + game_duration(g_alerts.in.rest_min) + " of game time.";
        if (a.rest.active) r += " The navigation is already warning: " + a.rest.headline + ".";
        r += "\n";
    }

    const ULONGLONG now = GetTickCount64();
    while (!g_happened.empty() && now - g_happened.front().at > 30 * 60000) g_happened.pop_front();
    if (!g_happened.empty()) {
        r += "Recently:";
        for (const auto& h : g_happened) {
            const int mins = (int)((now - h.at) / 60000);
            r += " " + h.text + " (" + (mins < 1 ? std::string("just now") : std::to_string(mins) + " min ago") + ").";
        }
        r += "\n";
    }
    return r;
}

static void happened(const std::string& text)
{
    g_happened.push_back({GetTickCount64(), text});
    while (g_happened.size() > 6) g_happened.pop_front();
}

static std::string brief() { return game_clock(true) + (g_have_game_time ? ", " : "") + where_words(); }

// The report goes out once a second; something that just happened (a new job...) must not be
// talked about with a report from before it. So each moment brings a fresh one.
static void tell_codriver(MomentInfo m)
{
    codriver_set_situation(situation(g_view, g_alert_state), brief());
    codriver_moment(std::move(m));
}

static void moment(Moment kind, const std::string& detail = {})
{
    if (!g_cfg.guide || !g_cfg.guide_auto) return;
    MomentInfo m;
    m.kind = kind;
    m.detail = detail;
    tell_codriver(std::move(m));
}

// Situation report, key presses and the things that make it speak up by itself.
static void update_codriver(const RouteView& v, const AlertState& a, bool turn_coming)
{
    if (!g_cfg.guide) return;
    const ULONGLONG now = GetTickCount64();
    static ULONGLONG last_report = 0;
    if (now - last_report >= 1000) {
        last_report = now;
        codriver_set_situation(situation(v, a), brief());
    }

    // The key: a tap asks about here, holding it records what the driver says.
    MicResult press;
    while (mic_take(press)) {
        MomentInfo m;
        if (!press.tap) {
            m.kind = Moment::Talk;
            m.audio = std::move(press.wav);
        } else {
            m.kind = Moment::AskHere;
            if (g_loc.kind != Location::None) {
                m.place_id = g_loc.id;
                m.place = g_loc.name;
                m.country = g_loc.country;
                m.nearby = g_loc.kind == Location::Near;
            }
        }
        tell_codriver(std::move(m));
    }

    if (!g_cfg.guide_auto || g_paused) return;
    // A new job: brief once the route is known (or without it after a while).
    if (g_brief_due && (v.valid || now - g_brief_due > 20000)) {
        g_brief_due = 0;
        moment(Moment::JobStart);
    }
    // A border coming up.
    if (v.valid)
        for (const auto& it : v.ahead)
            if ((it.kinds & RouteStop::Border) && it.km < 25 && it.km > 1) {
                const std::string id =
                    it.label + ":" + std::to_string((int)(it.x / 200)) + ":" + std::to_string((int)(it.z / 200));
                if (g_border_said.insert(id).second) {
                    MomentInfo m;
                    m.kind = Moment::Border;
                    m.place = it.label;
                    m.detail = round_km(it.km);
                    tell_codriver(std::move(m));
                }
                break;
            }
    // A regular look through the front camera while driving: not right at a junction, not in the
    // middle of an urgent warning, and not right after it spoke.
    const double every_s = g_cfg.guide_look_minutes * 60.0;
    if (!g_last_look) g_last_look = now;
    if (every_s > 0 && g_cfg.guide_screenshots && std::fabs(g_alerts.in.speed_kmh) > 10 && !turn_coming &&
        !a.fuel.critical && !a.rest.critical && (now - g_last_look) / 1000.0 > every_s &&
        codriver_quiet_seconds() > 45.0) {  // the assistant waits that long after its last words anyway
        g_last_look = now;
        moment(Moment::Look);
    }
}

static void update_route()
{
    if (g_route_target.empty()) return;
    // Pick up a freshly computed route.
    auto latest = g_route.latest();
    if (latest && latest.get() != g_tracker.route()) g_tracker.set(latest);
    // A new job, or the last attempt failed (e.g. the truck wasn't on a road yet): try again.
    const ULONGLONG since = GetTickCount64() - g_last_route_request;
    if (g_route_wanted || (!g_tracker.route() && since > 10000))
        request_route(g_route_wanted ? "new job" : "retry");
    else if (g_tracker.off_route() && since > 15000)
        request_route("still off the route", 15000);
}

static void update_alerts(bool force)
{
    if (!g_have_pos) return;
    const ULONGLONG now = GetTickCount64();
    if (!force && now - g_last_alert_update < 250) return;  // 4x a second: turn calls need it
    g_last_alert_update = now;

    update_route();
    RouteView view;
    if (g_tracker.route()) view = g_tracker.view(g_alerts.scales(), g_alerts.in.nav_distance_m, g_alerts.in.nav_time_s);
    if (view.valid && view.gps_checked && view.route_id != g_gps_logged_route) {
        g_gps_logged_route = view.route_id;
        log_info("route vs game GPS: ours %.0f km, GPS %.0f km (%s)", view.remaining_km,
                 g_alerts.in.nav_distance_m / 1000.0, view.matches_gps ? "same route" : "different route");
    }
    g_view = view;
    if (view.valid && !view.country.empty()) g_country = view.country;
    // [alerts] enabled=0 turns off the warnings only; the route and the strip keep working.
    AlertState st;
    // While fuel is getting low, keep a fresh "nearest fuel by road, any direction" answer.
    static ULONGLONG last_fuel_search = 0;
    const ULONGLONG t_now = GetTickCount64();
    const bool fuel_matters = g_alerts.in.have_fuel;
    if (fuel_matters && g_route.available() && t_now - last_fuel_search > 20000) {
        last_fuel_search = t_now;
        const double max_km = std::min(800.0, g_alerts.range_km() * 1.5 + 50.0);
        g_route.request_nearest_fuel(g_x, g_z, g_heading, max_km / g_alerts.km_per_world_m());
    }
    NearestPlace nearest = g_route.nearest_fuel();
    if (!fuel_matters || t_now - nearest.stamp > 60000) nearest = NearestPlace{};  // stale: don't use it
    if (g_cfg.alerts) st = g_alerts.update(g_x, g_z, g_heading, view.valid ? &view : nullptr, &nearest);
    overlay_set_alerts(st);
    g_alert_state = st;
    overlay_set_strip(build_strip(view, st));
    const Scales scales = g_alerts.scales();
    const double time_scale = g_current_city.empty() ? scales.time_road : scales.time_city;
    announce_update(g_cfg, view, st, g_route_target, g_alerts.in.speed_kmh, time_scale);
    const Guidance guidance = build_guidance(view, time_scale);
    overlay_set_guidance(guidance);
    update_codriver(view, st, guidance.active);

    const float limit_kmh = g_speed_limit > 0.5f ? g_speed_limit * 3.6f : 0.0f;
    if (!g_paused) {
        RulesInputs ri;
        ri.speed_kmh = std::fabs(g_alerts.in.speed_kmh);
        ri.limit_kmh = limit_kmh;
        ri.have_lights = g_have_lights;
        ri.low_beam = g_low_beam;
        ri.high_beam = g_high_beam;
        ri.wipers = g_wipers;
        ri.game_hour = g_have_game_time ? (int)((g_game_time / 60) % 24) : -1;
        ri.country = g_country;
        ri.fuel_l = g_alerts.in.fuel_l;
        ri.range_km = g_alerts.in.have_fuel ? g_alerts.range_km() : 0.0f;
        ri.fuel_warning = st.fuel.active;
        g_rules.update(g_cfg, ri, view.valid ? &view : nullptr, g_route_target);
    }

    // The slim bar: fuel range, time until sleep, speed limit, arrival (game clock).
    HudState hud;
    hud.visible = g_cfg.hud;
    if (g_alerts.in.have_fuel) hud.range_km = g_alerts.range_km();
    hud.rest_min = g_alerts.in.rest_min;
    hud.limit_kmh = (int)std::lround(limit_kmh);
    hud.speeding = hud.limit_kmh > 0 && std::fabs(g_alerts.in.speed_kmh) >= hud.limit_kmh + 5;
    if (view.valid && g_have_game_time) {
        const uint32_t arrive = g_game_time + (uint32_t)std::lround(std::max(0.0, view.remaining_min));
        char b[16];
        snprintf(b, sizeof(b), "%02u:%02u", (arrive / 60) % 24, arrive % 60);
        hud.eta = b;
        hud.late = g_jobstate.picked_up && g_jobstate.due && arrive > g_jobstate.due;
    }
    hud.fuel_low = st.fuel.active;
    hud.rest_low = st.rest.active;
    overlay_set_hud(hud);
}

// --- telemetry callbacks (game thread) -------------------------------------------------------

static SCSAPI_VOID on_placement(const scs_string_t, const scs_u32_t, const scs_value_t* const value,
                                const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_dplacement) return;
    if (!g_have_pos)
        log_info("first truck position x=%.0f z=%.0f", value->value_dplacement.position.x,
                 value->value_dplacement.position.z);
    g_x = value->value_dplacement.position.x;
    g_z = value->value_dplacement.position.z;
    g_heading = value->value_dplacement.orientation.heading;
    g_have_pos = true;
    g_alerts.on_position(g_x, g_z, !g_current_city.empty());
    if (g_tracker.update(g_x, g_z, g_heading, g_alerts.in.speed_kmh)) request_route("left the route");
    update_location(false);
    update_alerts(false);
}

// Float channels the alerts need; the channel name picks the field.
static SCSAPI_VOID on_float(const scs_string_t name, const scs_u32_t, const scs_value_t* const value,
                            const scs_context_t context)
{
    if (!value || value->type != SCS_VALUE_TYPE_float) return;
    const float v = value->value_float.value;
    AlertInputs& in = g_alerts.in;
    switch ((intptr_t)context) {
        case 0: in.fuel_l = v; in.have_fuel = true; break;
        case 1: in.range_km = v; break;
        case 2: in.consumption = v; break;
        case 3: in.speed_kmh = v * 3.6f; break;
        case 4: in.local_scale = v; break;
        case 5: in.odometer_km = v; in.have_odometer = true; break;
        case 6: in.nav_distance_m = v; break;
        case 7: in.nav_time_s = v; break;
        case 8: g_speed_limit = v; break;
    }
}

// Headlights and wipers, for the headlight reminder.
static SCSAPI_VOID on_light(const scs_string_t, const scs_u32_t, const scs_value_t* const value,
                            const scs_context_t context)
{
    const bool on = value && value->type == SCS_VALUE_TYPE_bool && value->value_bool.value;
    switch ((intptr_t)context) {
        case 0: g_low_beam = on; break;
        case 1: g_high_beam = on; break;
        case 2: g_wipers = on; break;
    }
    g_have_lights = true;
}

static SCSAPI_VOID on_game_time(const scs_string_t, const scs_u32_t, const scs_value_t* const value,
                                const scs_context_t)
{
    if (!value || value->type != SCS_VALUE_TYPE_u32) return;
    g_game_time = value->value_u32.value;
    g_have_game_time = true;
}

static SCSAPI_VOID on_rest(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    // No value means fatigue simulation is off.
    g_alerts.in.rest_min = (value && value->type == SCS_VALUE_TYPE_s32) ? value->value_s32.value : -1;
}

static const char* attr_string(const scs_named_value_t* attrs, const char* name)
{
    for (const scs_named_value_t* a = attrs; a->name; ++a)
        if (strcmp(a->name, name) == 0 && a->value.type == SCS_VALUE_TYPE_string)
            return a->value.value_string.value;
    return nullptr;
}

static const scs_value_t* attr_value(const scs_named_value_t* attrs, const char* name)
{
    for (const scs_named_value_t* a = attrs; a->name; ++a)
        if (strcmp(a->name, name) == 0) return &a->value;
    return nullptr;
}

// Any numeric attribute as a double (the SDK uses u32, s32, s64, u64 and float for these).
static double attr_num(const scs_named_value_t* attrs, const char* name, double def = 0)
{
    const scs_value_t* v = attr_value(attrs, name);
    if (!v) return def;
    switch (v->type) {
        case SCS_VALUE_TYPE_u32:    return v->value_u32.value;
        case SCS_VALUE_TYPE_s32:    return v->value_s32.value;
        case SCS_VALUE_TYPE_u64:    return (double)v->value_u64.value;
        case SCS_VALUE_TYPE_s64:    return (double)v->value_s64.value;
        case SCS_VALUE_TYPE_float:  return v->value_float.value;
        case SCS_VALUE_TYPE_double: return v->value_double.value;
        default:                    return def;
    }
}

static bool attr_bool(const scs_named_value_t* attrs, const char* name, bool def)
{
    for (const scs_named_value_t* a = attrs; a->name; ++a)
        if (strcmp(a->name, name) == 0 && a->value.type == SCS_VALUE_TYPE_bool)
            return a->value.value_bool.value != 0;
    return def;
}

// Where the route should go. Like the game's GPS: to the pick-up first while there's no cargo
// to deliver yet, then to the destination.
//  - cargo market: your own trailer; drive to the source to load (cargo.loaded is false until then)
//  - quick job: truck and trailer start at the source, so straight to the destination
//  - freight market / external: the trailer waits at the source; hook it first
static void choose_target()
{
    JobState& j = g_jobstate;
    std::string target, label;
    if (j.active() && !j.dest_company.empty()) {
        bool pickup = false;
        if (!j.picked_up && !j.src_city.empty() && !j.src_company.empty()) {
            if (j.market == "cargo_market") pickup = !j.loaded;
            else if (j.market == "quick_job") pickup = false;
            else if (!j.market.empty()) pickup = !g_trailer_connected;
            else pickup = !j.loaded;  // older games without job.market
        }
        if (!pickup) j.picked_up = true;  // the trailer unhooking at delivery must not send us back
        const std::string& company = pickup ? j.src_company_name : j.dest_company_name;
        target = pickup ? j.src_city + "." + j.src_company : j.dest_city + "." + j.dest_company;
        label = (pickup ? "pick-up: " : "") + (company.empty() ? std::string() : company + ", ") +
                (pickup ? j.src_city_name : j.dest_city_name);
    }
    if (target == g_route_target) return;

    g_route_target = target;
    g_route_label = label;
    g_tracker.set(nullptr);
    g_route.clear();
    g_view = RouteView{};
    if (target.empty()) {
        if (j.active()) log_info("job: special transport follows a fixed route; no route from the plugin");
        return;
    }
    if (!g_route.available()) return;
    if (!g_route.knows(target)) {
        log_warn("job: '%s' is not in the road graph (map mod or newer map?); no route", target.c_str());
        return;
    }
    log_info("job: route to '%s' (%s)", target.c_str(), label.c_str());
    g_route_wanted = true;
    g_last_route_request = 0;
}

static void on_job_config(const scs_named_value_t* attrs)
{
    auto str = [&](const char* name) { const char* v = attr_string(attrs, name); return std::string(v ? v : ""); };
    JobState j;
    j.dest_city = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city_id);
    j.dest_company = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_company_id);
    j.dest_city_name = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city);
    j.dest_company_name = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_company);
    j.src_city = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city_id);
    j.src_company = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_company_id);
    j.src_city_name = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city);
    j.src_company_name = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_company);
    j.market = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_job_market);
    j.loaded = attr_bool(attrs, SCS_TELEMETRY_CONFIG_ATTRIBUTE_is_cargo_loaded, true);
    j.cargo = str(SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo);
    j.cargo_kg = (float)attr_num(attrs, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo_mass);
    j.due = (uint32_t)attr_num(attrs, SCS_TELEMETRY_CONFIG_ATTRIBUTE_delivery_time);
    j.income = (uint64_t)attr_num(attrs, SCS_TELEMETRY_CONFIG_ATTRIBUTE_income);
    j.special = attr_bool(attrs, SCS_TELEMETRY_CONFIG_ATTRIBUTE_special_job, false);

    const JobState& old = g_jobstate;
    const bool same_job = j.dest_city == old.dest_city && j.dest_company == old.dest_company &&
                          j.src_city == old.src_city && j.src_company == old.src_company;
    if (same_job) j.picked_up = old.picked_up;
    if (!same_job) {
        if (j.active()) {
            g_brief_due = GetTickCount64();
            g_border_said.clear();
        }
        if (j.active())
            log_info("job: %s, %s.%s -> %s.%s (cargo %s, trailer %s)", j.market.empty() ? "?" : j.market.c_str(),
                     j.src_city.c_str(), j.src_company.c_str(), j.dest_city.c_str(), j.dest_company.c_str(),
                     j.loaded ? "loaded" : "not loaded", g_trailer_connected ? "attached" : "not attached");
        else if (old.active())
            log_info("job: finished");
    }
    g_jobstate = j;
    choose_target();
}

static SCSAPI_VOID on_trailer(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    const bool connected = value && value->type == SCS_VALUE_TYPE_bool && value->value_bool.value;
    if (connected == g_trailer_connected) return;
    g_trailer_connected = connected;
    if (g_jobstate.active()) choose_target();
}

static std::string offence_words(const std::string& id)
{
    static const std::pair<const char*, const char*> names[] = {
        {"crash", "causing a crash"}, {"avoid_sleeping", "driving without enough sleep"},
        {"wrong_way", "driving the wrong way"}, {"speeding_camera", "speeding, caught by a camera"},
        {"no_lights", "driving without lights"}, {"red_signal", "running a red light"}, {"speeding", "speeding"},
        {"avoid_weighing", "skipping a weigh station"}, {"illegal_trailer", "pulling an illegal trailer"},
        {"avoid_inspection", "avoiding an inspection"}, {"illegal_border_crossing", "an illegal border crossing"},
        {"hard_shoulder_violation", "driving on the hard shoulder"},
        {"damaged_vehicle_usage", "driving a damaged vehicle"}, {"generic", "a traffic offence"}};
    for (const auto& n : names)
        if (id == n.first) return n.second;
    std::string s = id;
    for (char& c : s)
        if (c == '_') c = ' ';
    return s.empty() ? "a traffic offence" : s;
}

// Things worth a word from the co-driver (and a line in its situation report).
static void on_gameplay_for_codriver(const char* id, const scs_named_value_t* attrs)
{
    if (!g_cfg.guide) return;
    auto str = [&](const char* name) { const char* v = attr_string(attrs, name); return std::string(v ? v : ""); };
    auto money = [](double v) { return std::to_string((long long)std::llround(v)) + " euros"; };
    if (strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_fined) == 0) {
        const std::string what = money(attr_num(attrs, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_amount)) + " for " +
                                 offence_words(str(SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_offence));
        happened("Fined " + what);
        moment(Moment::Fined, what);
    } else if (strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_ferry) == 0 ||
               strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_train) == 0) {
        const bool train = strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_train) == 0;
        const std::string what = std::string(train ? "the train" : "the ferry") + " from " +
                                 str(SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_source_name) + " to " +
                                 str(SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_target_name);
        happened("Took " + what);
        moment(Moment::Ferry, what);
    } else if (strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_player_tollgate_paid) == 0) {
        happened("Paid a toll of " + money(attr_num(attrs, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_pay_amount)));
    } else if (strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_delivered) == 0) {
        char b[160];
        snprintf(b, sizeof(b), "earned %s and %.0f XP for %.0f km, cargo damage %.0f%%",
                 money(attr_num(attrs, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_revenue)).c_str(),
                 attr_num(attrs, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_earned_xp),
                 attr_num(attrs, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_distance_km),
                 attr_num(attrs, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_cargo_damage) * 100.0);
        happened(std::string("Delivered the load, ") + b);
        moment(Moment::Delivered, b);
    } else if (strcmp(id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_cancelled) == 0) {
        happened("Cancelled the job");
    }
}

static SCSAPI_VOID on_event(const scs_event_t event, const void* const info, const scs_context_t)
{
    if (event == SCS_TELEMETRY_EVENT_frame_end) {
        // Channel callbacks are throttled; this makes sure the last values land even when parked.
        update_location(false);
        update_alerts(false);
        log_flush();
    } else if (event == SCS_TELEMETRY_EVENT_paused) {
        g_paused = true;
        overlay_set_paused(true);
        speech_set_paused(true);
    } else if (event == SCS_TELEMETRY_EVENT_started) {
        g_paused = false;
        overlay_set_paused(false);
        speech_set_paused(false);
    } else if (event == SCS_TELEMETRY_EVENT_configuration) {
        const auto* cfg = static_cast<const scs_telemetry_configuration_t*>(info);
        if (strcmp(cfg->id, SCS_TELEMETRY_CONFIG_job) != 0) return;
        on_job_config(cfg->attributes);
        const char* id = attr_string(cfg->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city_id);
        const char* name = attr_string(cfg->attributes, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city);
        // An empty job config (job finished) may arrive before job.delivered, so keep the last
        // destination; only a cancellation forgets it.
        if (id && *id) {
            g_job.dest_id = id;
            g_job.dest_name = name ? name : "";
        }
    } else if (event == SCS_TELEMETRY_EVENT_gameplay) {
        const auto* ev = static_cast<const scs_telemetry_gameplay_event_t*>(info);
        on_gameplay_for_codriver(ev->id, ev->attributes);
        if (strcmp(ev->id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_cancelled) == 0) { g_job = {}; return; }
        if (strcmp(ev->id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_delivered) != 0) return;
        log_info("job delivered, destination '%s'", g_job.dest_id.c_str());
        // The truck is at the destination company, so this position is inside the destination city.
        if (g_cfg.learn && g_have_pos && !g_job.dest_id.empty()) {
            g_db.learn(g_job.dest_id, g_job.dest_name, g_x, g_z);
            g_route.set_cities(g_db.all(), g_cfg.in_city_radius);
            update_location(true);
        }
        g_job = {};
    }
}

// --- SDK entry points ----------------------------------------------------------------------

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params)
{
    if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01)
        return SCS_RESULT_unsupported;

    const auto* p = static_cast<const scs_telemetry_init_params_v100_t*>(params);
    log_set_sink(p->common.log);
    log_info("loading in %s (%s), telemetry api %u.%u", p->common.game_name, p->common.game_id,
             SCS_GET_MAJOR_VERSION(version), SCS_GET_MINOR_VERSION(version));

    const std::wstring dir = data_dir();
    g_cfg = load_config(dir);
    g_db.load(dir);
    g_pois.load(dir);
    g_alerts.init(g_cfg, &g_pois, dir);
    if (g_cfg.route && g_route.init(dir)) {
        g_route.set_cities(g_db.all(), g_cfg.in_city_radius);
        g_route.set_fuel_points(g_pois.positions(PoiKind::Fuel));
    }
    g_rules.load_prices(dir);
    log_info("fuel prices: %zu countries", g_rules.price_count());
    log_info("city table: %zu cities, %zu boundary areas, %zu learned", g_db.size(), g_db.area_count(),
             g_db.learned_count());
    log_info("places: %zu fuel stations, %zu rest areas", g_pois.count(PoiKind::Fuel), g_pois.count(PoiKind::Rest));
    if (g_db.size() == 0)
        log_warn("no cities loaded - is the ets2_city_overlay folder next to the DLL?");

    bool ok = p->register_for_event(SCS_TELEMETRY_EVENT_frame_end, on_event, nullptr) == SCS_RESULT_ok &&
              p->register_for_event(SCS_TELEMETRY_EVENT_paused, on_event, nullptr) == SCS_RESULT_ok &&
              p->register_for_event(SCS_TELEMETRY_EVENT_started, on_event, nullptr) == SCS_RESULT_ok &&
              p->register_for_event(SCS_TELEMETRY_EVENT_configuration, on_event, nullptr) == SCS_RESULT_ok &&
              p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_U32_NIL,
                                      SCS_VALUE_TYPE_dplacement, SCS_TELEMETRY_CHANNEL_FLAG_none,
                                      on_placement, nullptr) == SCS_RESULT_ok;
    if (!ok) {
        log_error("telemetry registration failed");
        log_set_sink(nullptr);
        return SCS_RESULT_generic_error;
    }
    const char* floats[] = {SCS_TELEMETRY_TRUCK_CHANNEL_fuel, SCS_TELEMETRY_TRUCK_CHANNEL_fuel_range,
                            SCS_TELEMETRY_TRUCK_CHANNEL_fuel_average_consumption, SCS_TELEMETRY_TRUCK_CHANNEL_speed,
                            SCS_TELEMETRY_CHANNEL_local_scale, SCS_TELEMETRY_TRUCK_CHANNEL_odometer,
                            SCS_TELEMETRY_TRUCK_CHANNEL_navigation_distance, SCS_TELEMETRY_TRUCK_CHANNEL_navigation_time,
                            SCS_TELEMETRY_TRUCK_CHANNEL_navigation_speed_limit};
    for (intptr_t i = 0; i < (intptr_t)(sizeof(floats) / sizeof(floats[0])); ++i)
        if (p->register_for_channel(floats[i], SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none,
                                    on_float, (scs_context_t)i) != SCS_RESULT_ok)
            log_warn("channel %s unavailable", floats[i]);
    // First trailer: "trailer.connected" (older name) or "trailer.0.connected" (SDK 1.14+).
    for (const char* name : {SCS_TELEMETRY_TRAILER_CHANNEL_connected, "trailer.0.connected"})
        p->register_for_channel(name, SCS_U32_NIL, SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none, on_trailer,
                                nullptr);
    p->register_for_channel(SCS_TELEMETRY_CHANNEL_next_rest_stop, SCS_U32_NIL, SCS_VALUE_TYPE_s32,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_rest, nullptr);
    const char* lights[] = {SCS_TELEMETRY_TRUCK_CHANNEL_light_low_beam, SCS_TELEMETRY_TRUCK_CHANNEL_light_high_beam,
                            SCS_TELEMETRY_TRUCK_CHANNEL_wipers};
    for (intptr_t i = 0; i < 3; ++i)
        p->register_for_channel(lights[i], SCS_U32_NIL, SCS_VALUE_TYPE_bool, SCS_TELEMETRY_CHANNEL_FLAG_none, on_light,
                                (scs_context_t)i);
    p->register_for_channel(SCS_TELEMETRY_CHANNEL_game_time, SCS_U32_NIL, SCS_VALUE_TYPE_u32,
                            SCS_TELEMETRY_CHANNEL_FLAG_none, on_game_time, nullptr);

    // Gameplay events need SDK 1.14+ (ETS2 1.35+); without them we just don't learn new cities.
    if (p->register_for_event(SCS_TELEMETRY_EVENT_gameplay, on_event, nullptr) != SCS_RESULT_ok)
        log_warn("gameplay events unavailable; city learning disabled");

    if (g_cfg.speak || g_cfg.announce || g_cfg.guide) speech_start(g_cfg, dir);
    if (g_cfg.guide) {
        codriver_start(g_cfg, dir);
        mic_start(g_cfg.guide_listen);
    }
    if (!overlay_install(g_cfg))
        log_error("overlay could not be installed; the plugin will do nothing visible");
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
    overlay_uninstall();
    mic_stop();
    codriver_stop();
    speech_stop();
    g_route.shutdown();
    g_alerts.save();
    log_set_sink(nullptr);
}
