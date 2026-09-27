#include "rules.h"
#include "csv.h"
#include "log.h"
#include "speech.h"
#include "spoken.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

static const double kPriceStep = 0.08;  // a price difference below this isn't worth a word

void RulesMonitor::load_prices(const std::wstring& dir)
{
    for (const auto& row : read_csv(dir + L"\\countries.csv"))
        if (row.size() >= 3 && row[0] != "name") {
            const double p = atof(row[2].c_str());
            if (p > 0) prices_[row[0]] = p;
        }
}

double RulesMonitor::price(const std::string& country) const
{
    auto it = prices_.find(country);
    return it == prices_.end() ? 0.0 : it->second;
}

static int percent(double from, double to) { return (int)std::lround(std::fabs(to - from) / from * 100.0); }

std::string RulesMonitor::border_tag(const std::string& from, const std::string& to) const
{
    const double p0 = price(from), p1 = price(to);
    if (p0 <= 0 || p1 <= 0 || std::fabs(p1 - p0) / p0 < kPriceStep) return {};
    return std::string("diesel ") + (p1 < p0 ? "-" : "+") + std::to_string(percent(p0, p1)) + "%";
}

// "Speed limit 60" the moment it drops while you're too fast; optionally a warning while you
// stay over it. Speed cameras fine from 5 km/h over the limit.
void RulesMonitor::speed(const Config& cfg, const RulesInputs& in, unsigned long long now)
{
    const float limit = in.limit_kmh;
    if (limit < 1) {
        last_limit_ = 0;
        over_since_ = 0;
        return;
    }
    // A new limit counts once it has held for a moment: it flickers at junctions and slip roads.
    if (std::fabs(limit - pending_limit_) > 0.5f) {
        pending_limit_ = limit;
        pending_since_ = now;
    }
    if (now - pending_since_ < 1500) return;
    const bool dropped = last_limit_ > 0 && limit < last_limit_ - 4;
    last_limit_ = limit;
    if (cfg.say_speed_limit && dropped && in.speed_kmh > limit + 4 && now - last_limit_call_ > 4000) {
        last_limit_call_ = now;
        speech_say("Speed limit " + std::to_string((int)std::lround(limit)) + ".", SpeechPriority::Normal, "limit", 5);
    }
    if (in.speed_kmh >= limit + 5) {
        if (!over_since_) over_since_ = now;
    } else {
        over_since_ = 0;
    }
    if (cfg.speeding_warning && over_since_ && now - over_since_ > 6000 && now - last_speeding_call_ > 90000) {
        last_speeding_call_ = now;
        speech_say("You're over the limit. It's " + std::to_string((int)std::lround(limit)) + " here.",
                   SpeechPriority::Normal, "limit", 6);
    }
}

// Headlights off while driving. Low beams on all the time avoids every headlight fine, so one
// calm reminder now and then; sooner in the rain (wipers on) and at night.
void RulesMonitor::lights(const Config& cfg, const RulesInputs& in, unsigned long long now)
{
    if (!cfg.lights_reminder || !in.have_lights) return;
    const bool on = in.low_beam || in.high_beam;
    if (on || in.speed_kmh < 20) {
        lights_off_since_ = 0;
        if (on) lights_rain_said_ = false;
        return;
    }
    if (!lights_off_since_) lights_off_since_ = now;
    const bool night = in.game_hour >= 0 && (in.game_hour >= 20 || in.game_hour < 6);
    const unsigned long long off_for = now - lights_off_since_;
    if (in.wipers && !lights_rain_said_ && off_for > 5000) {
        lights_rain_said_ = true;
        last_lights_call_ = now;
        speech_say("It's raining and your headlights are off. Low beams are required in the rain.",
                   SpeechPriority::Normal, "lights", 20);
        return;
    }
    // By day it's said once a session (lights off can be fine then); at night every 10 minutes.
    const bool due = night ? (!last_lights_call_ || now - last_lights_call_ > 10 * 60000ULL) : !lights_day_said_;
    if (off_for > 20000 && due) {
        last_lights_call_ = now;
        if (!night) lights_day_said_ = true;
        speech_say(night ? "Your headlights are off, and it's dark. Switch on the low beams to avoid a fine."
                         : "Your headlights are off. Keeping the low beams on avoids a fine in rain, in tunnels "
                           "and after dark.",
                   SpeechPriority::Normal, "lights", 20);
    }
}

// Diesel across the next border: a lot cheaper and your fuel will last to a station there, or a
// lot dearer and there's still a station before it. Said once per border per job.
void RulesMonitor::fuel_prices(const RulesInputs& in, const RouteView& r)
{
    if (in.fuel_warning || in.country.empty()) return;
    const RouteView::Item* border = nullptr;
    for (const auto& it : r.ahead)
        if ((it.kinds & RouteStop::Border) && it.km > 10) { border = &it; break; }
    if (!border || border->km > 150) return;
    const double p0 = price(in.country), p1 = price(border->label);
    if (p0 <= 0 || p1 <= 0 || std::fabs(p1 - p0) / p0 < kPriceStep) return;
    const std::string key = border->label + ":" + std::to_string((int)(border->x / 200)) + ":" +
                            std::to_string((int)(border->z / 200));
    if (borders_said_.count(key)) return;

    const std::string where = spoken_country(border->label);
    const int pct = percent(p0, p1);
    std::string text;
    if (p1 < p0) {
        // Cheaper ahead: only worth waiting if the tank reaches a station on the other side.
        const RouteView::Item* after = nullptr;
        for (const auto& it : r.ahead)
            if ((it.kinds & RouteStop::Fuel) && it.km > border->km) { after = &it; break; }
        if (!after || in.range_km < after->km + 60) return;
        text = "Diesel is about " + std::to_string(pct) + " percent cheaper in " + where +
               ". Your fuel will last to the first station after the border, in " + spoken_km(after->km) + ".";
    } else {
        // Dearer ahead: worth filling up before the border, if the tank isn't nearly full and there's
        // a station on this side and a fair way to go on the other.
        if (fuel_max_ > 0 && in.fuel_l > fuel_max_ * 0.8f) return;
        const RouteView::Item* before = nullptr;
        for (const auto& it : r.ahead)
            if ((it.kinds & RouteStop::Fuel) && it.km > 1 && it.km < border->km) before = &it;
        if (!before || r.remaining_km - border->km < 100) return;
        text = "Diesel is about " + std::to_string(pct) + " percent dearer in " + where +
               ". The last station before the border is in " + spoken_km(before->km) + ".";
    }
    borders_said_.insert(key);
    log_info("fuel prices: %s %.3f, %s %.3f", in.country.c_str(), p0, border->label.c_str(), p1);
    speech_say(text, SpeechPriority::Normal, "fuelprice", 60);
}

void RulesMonitor::update(const Config& cfg, const RulesInputs& in, const RouteView* route, const std::string& target)
{
    const unsigned long long now = GetTickCount64();
    if (target != target_) {
        target_ = target;
        borders_said_.clear();
    }
    fuel_max_ = std::max(fuel_max_, in.fuel_l);
    if (!cfg.announce) return;
    speed(cfg, in, now);
    lights(cfg, in, now);
    if (cfg.say_fuel_prices && route && route->valid) fuel_prices(in, *route);
}
