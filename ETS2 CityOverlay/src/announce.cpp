#include "announce.h"
#include "log.h"

#include <windows.h>
#include "speech.h"
#include "spoken.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>

static std::string g_target;                // job route the "already said" set belongs to
static std::unordered_set<std::string> g_said;
static std::string g_fuel_key, g_rest_key;  // last fuel/sleep warning spoken
static ULONGLONG g_fuel_said = 0, g_rest_said = 0;
static bool g_fuel_was_critical = false, g_rest_was_critical = false;

// A changed warning is said again, but not more than every 45 s - unless it just turned urgent.
static bool may_repeat(ULONGLONG& last, bool& was_critical, bool critical)
{
    const ULONGLONG now = GetTickCount64();
    const bool escalated = critical && !was_critical;
    if (!escalated && last && now - last < 45000) return false;
    last = now;
    was_critical = critical;
    return true;
}
struct Upcoming { std::string label, detail; double km; };
static std::unordered_map<std::string, Upcoming> g_borders_ahead;

// A stop's identity that survives re-routing (positions along the route change, the place doesn't).
static std::string place_id(const char* kind, float x, float z)
{
    return std::string(kind) + ":" + std::to_string((long)std::lround(x / 100.0)) + ":" +
           std::to_string((long)std::lround(z / 100.0));
}

static bool once(const std::string& key)
{
    return g_said.insert(key).second;
}

// "Quarry, Prague" -> "Quarry in Prague"; "pick-up: X, Y" -> "your pick-up, X in Y"
static std::string spoken_destination(std::string label)
{
    std::string prefix = "Your destination";
    if (label.rfind("pick-up: ", 0) == 0) {
        prefix = "Your pick-up";
        label = label.substr(9);
    }
    const size_t comma = label.find(", ");
    if (comma != std::string::npos) label = label.substr(0, comma) + " in " + label.substr(comma + 2);
    return prefix + ", " + label + ",";
}

static float g_speed_kmh = 0;
static double g_time_scale = 19.0;  // game seconds per real second where the truck is

double seconds_to(double km, float speed_kmh, double time_scale)
{
    const double v = std::max(20.0f, speed_kmh);  // crawling or stopped: assume town speed
    return km / (v / 3600.0 * time_scale);
}

static double secs(double km) { return seconds_to(km, g_speed_kmh, g_time_scale); }

// "The truck stop is 6 kilometres ahead. Time to refuel." About a minute and 15 seconds before.
static void remind(const Target& t, const char* what, const char* tail)
{
    if (!t.valid || !t.on_route) return;
    const double s = secs(t.km);
    for (double threshold : {60.0, 15.0}) {
        if (s > threshold || s < threshold * 0.4) continue;
        if (!once(place_id(what, (float)t.x, (float)t.z) + ":" + std::to_string((int)threshold))) continue;
        const std::string place = spoken_place(t.label == "Parking" ? "parking area" : t.label);
        speech_say("The " + place + " is " + spoken_km(t.km) + " ahead. " + tail, SpeechPriority::Normal,
                   std::string(what) + ":remind", 10);
    }
}

void announce_update(const Config& cfg, const RouteView& v, const AlertState& a, const std::string& target,
                     float speed_kmh, double time_scale)
{
    if (!cfg.announce || !speech_available()) return;
    g_speed_kmh = speed_kmh;
    g_time_scale = time_scale > 0.5 ? time_scale : 19.0;
    if (target != g_target) {  // new job: everything may be said again
        g_target = target;
        g_said.clear();
        g_borders_ahead.clear();
    }

    // Fuel and sleep warnings: say them when they appear or change meaning.
    if (cfg.say_fuel) {
        // Each distinct warning once per job, even if it flickers off and on (e.g. when the GPS
        // briefly disagrees with our route).
        if (a.fuel.active && a.fuel.speech_key != g_fuel_key && !a.fuel.speech.empty() &&
            !g_said.count("fuel:" + a.fuel.speech_key)) {
            if (may_repeat(g_fuel_said, g_fuel_was_critical, a.fuel.critical)) {
                once("fuel:" + a.fuel.speech_key);
                speech_say(a.fuel.speech, a.fuel.critical ? SpeechPriority::High : SpeechPriority::Normal, "fuel", 60);
                g_fuel_key = a.fuel.speech_key;
            }
        } else if (!a.fuel.active) {
            g_fuel_key.clear();
            g_fuel_was_critical = false;
        }
        if (a.fuel.active) remind(a.fuel.target, "fuel", "Time to refuel.");
    }
    if (cfg.say_sleep) {
        if (a.rest.active && a.rest.speech_key != g_rest_key && !a.rest.speech.empty() &&
            !g_said.count("rest:" + a.rest.speech_key)) {
            if (may_repeat(g_rest_said, g_rest_was_critical, a.rest.critical)) {
                once("rest:" + a.rest.speech_key);
                speech_say(a.rest.speech, a.rest.critical ? SpeechPriority::High : SpeechPriority::Normal, "rest", 60);
                g_rest_key = a.rest.speech_key;
            }
        } else if (!a.rest.active) {
            g_rest_key.clear();
            g_rest_was_critical = false;
        }
        if (a.rest.active) remind(a.rest.target, "rest", "Time for a rest.");
    }
    if (!v.valid) return;

    std::unordered_map<std::string, Upcoming> borders_now;
    for (const auto& it : v.ahead) {
        if ((it.kinds & RouteStop::Ferry) && cfg.say_ferry) {
            const std::string id = place_id("ferry", it.x, it.z);
            if (it.km <= 10 && it.km > 4 && once(id + ":10"))
                speech_say("Ferry crossing in " + spoken_km(it.km) + ".", SpeechPriority::Normal, "ferry", 20);
            if (secs(it.km) <= 15 && it.km > 0.3 && once(id + ":near"))
                speech_say("The ferry is " + spoken_km(it.km) + " ahead.", SpeechPriority::Normal, "ferry", 8);
        }
        if ((it.kinds & RouteStop::Border) && cfg.say_border) {
            const std::string id = place_id("border", it.x, it.z);
            borders_now[id] = {it.label, it.detail, it.km};
            if (it.km <= cfg.border_km && it.km > 0.5 && once(id + ":ahead"))
                speech_say("In " + spoken_km(it.km) + " you'll cross into " + spoken_country(it.label) + ".",
                           SpeechPriority::Normal, "border", 30);
        }
    }
    // A border that was just ahead and now isn't: we crossed it.
    if (cfg.say_border)
        for (const auto& [id, b] : g_borders_ahead)
            if (!borders_now.count(id) && b.km < 2.0 && once(id + ":welcome"))
                speech_say("Welcome to " + spoken_country(b.label) + ". " + spoken_limits(b.detail),
                           SpeechPriority::Normal, "border", 30);
    g_borders_ahead.swap(borders_now);

    // Exits, forks, turns and roundabouts. Only when our route is the one the GPS is on - a
    // wrong "take the exit" is worse than none.
    if (cfg.say_lanes && v.matches_gps && std::fabs(v.gps_ratio - 1.0) < 0.1) {
        const RouteView::Item* m1 = nullptr;
        const RouteView::Item* m2 = nullptr;
        for (const auto& it : v.ahead) {
            if (!(it.kinds & RouteStop::Maneuver) || it.km < 0.05) continue;
            if (!m1) m1 = &it;
            else { m2 = &it; break; }
        }
        if (m1) {
            const Turn& t = m1->turn;
            const std::string id = place_id("turn", m1->x, m1->z);
            const std::string what = spoken_turn(t.turn, t.flags, t.exit_no);
            const std::string lanes = spoken_lanes(t.lanes, t.mask, t.lht);
            // Timed in real seconds: motorway exits and forks get an early call (~15 s, time to
            // change lanes) and a close one (~6 s); city turns only the close one. The voice takes
            // about a second to start, so say the distance you'll be at by then.
            const bool early = (t.turn == 1 || t.turn == 11) && t.lanes >= 2;
            const double s = secs(m1->km);
            const double spoken_at = std::max(0.1, m1->km - m1->km / std::max(1.0, s));
            if (!what.empty() && early && s <= 16 && s > 9 && once(id + ":early"))
                log_info("turn call (early): junction at %.0f m on the route, %.1f km / %.0f s ahead", m1->at_m, m1->km, s),
                speech_say("In " + spoken_km(spoken_at) + ", " + what + "." + (lanes.empty() ? "" : " " + lanes),
                           SpeechPriority::Normal, "turn", 4);
            if (!what.empty() && s <= 6.5 && s > 1.5 && once(id + ":near")) {
                log_info("turn call (near): junction at %.0f m on the route, %.1f km / %.0f s ahead", m1->at_m, m1->km, s);
                std::string text = "In " + spoken_km(spoken_at) + ", " + what + ".";
                if (m2 && secs(m2->km - m1->km) < 4) {
                    const std::string then = spoken_turn(m2->turn.turn, m2->turn.flags, m2->turn.exit_no);
                    if (!then.empty()) text += " Then " + then + ".";
                    once(place_id("turn", m2->x, m2->z) + ":near");  // said together: don't repeat it
                }
                if (!lanes.empty()) text += " " + lanes;
                speech_say(text, SpeechPriority::High, "turn", 3);  // time-critical: ahead of chatter
            }
        }
    }

    if (cfg.say_destination && v.remaining_km <= cfg.destination_km && v.remaining_km > 0.5 && once("destination"))
        speech_say(spoken_destination(v.dest_label) + " is " + spoken_km(v.remaining_km) + " ahead.",
                   SpeechPriority::Normal, "destination", 30);
}
