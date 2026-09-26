#include "pois.h"
#include "csv.h"

#include <cmath>
#include <cstdlib>

static const double kPi = 3.14159265358979323846;

// Relative bearing of (dx, dz) seen from a truck with SDK heading h, in degrees, +right.
static float relative_bearing(double dx, double dz, double heading)
{
    // SDK: angle counter-clockwise from north (-Z), so a direction's own angle is atan2(-dx, -dz).
    const double target = std::atan2(-dx, -dz);
    double rel = target - heading * 2.0 * kPi;  // + = counter-clockwise = left
    while (rel > kPi) rel -= 2.0 * kPi;
    while (rel < -kPi) rel += 2.0 * kPi;
    return (float)(-rel * 180.0 / kPi);         // flip so + = right
}

float bearing_to(double x, double z, double heading, double tx, double tz)
{
    return relative_bearing(tx - x, tz - z, heading);
}

void PoiDb::load(const std::wstring& dir)
{
    pois_.clear();
    // pois.csv: kind,x,z,label   (kind = fuel | rest)
    auto rows = read_csv(dir + L"\\pois.csv");
    for (size_t i = 1; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.size() < 4) continue;
        PoiKind kind;
        if (r[0] == "fuel") kind = PoiKind::Fuel;
        else if (r[0] == "rest") kind = PoiKind::Rest;
        else continue;
        pois_.push_back({kind, strtod(r[1].c_str(), nullptr), strtod(r[2].c_str(), nullptr), r[3]});
    }
}

size_t PoiDb::count(PoiKind kind) const
{
    size_t n = 0;
    for (const auto& p : pois_) n += p.kind == kind;
    return n;
}

Target PoiDb::find_ahead(PoiKind kind, double x, double z, double heading, double cone_deg,
                         double max_dist, const Target& current) const
{
    auto make = [&](const Poi& p) {
        Target t;
        t.valid = true;
        t.x = p.x; t.z = p.z;
        t.label = p.label;
        t.distance = std::hypot(p.x - x, p.z - z);
        t.rel_deg = relative_bearing(p.x - x, p.z - z, heading);
        return t;
    };

    // Keep the current target until it falls well behind us (e.g. we drove past it).
    if (current.valid) {
        for (const auto& p : pois_) {
            if (p.kind != kind || p.x != current.x || p.z != current.z) continue;
            Target t = make(p);
            if (std::fabs(t.rel_deg) <= 110.0f && t.distance <= max_dist) return t;
            break;
        }
    }

    const Poi* ahead = nullptr;
    const Poi* any = nullptr;
    double ahead_d = max_dist, any_d = 1e300;
    for (const auto& p : pois_) {
        if (p.kind != kind) continue;
        const double d = std::hypot(p.x - x, p.z - z);
        if (d < any_d) { any_d = d; any = &p; }
        if (d < ahead_d && std::fabs(relative_bearing(p.x - x, p.z - z, heading)) <= cone_deg) {
            ahead_d = d;
            ahead = &p;
        }
    }
    if (ahead) return make(*ahead);
    if (any) return make(*any);
    return {};
}

Target PoiDb::find_after(PoiKind kind, double x, double z, double heading, double cone_deg,
                         double max_dist, const Target& next, double min_sep) const
{
    if (!next.valid) return {};
    const Poi* best = nullptr;
    double best_d = max_dist;
    for (const auto& p : pois_) {
        if (p.kind != kind) continue;
        const double d = std::hypot(p.x - x, p.z - z);
        if (d <= next.distance || d >= best_d) continue;
        if (std::hypot(p.x - next.x, p.z - next.z) < min_sep) continue;
        if (std::fabs(relative_bearing(p.x - x, p.z - z, heading)) > cone_deg) continue;
        best_d = d;
        best = &p;
    }
    if (!best) return {};
    Target t;
    t.valid = true;
    t.x = best->x; t.z = best->z;
    t.label = best->label;
    t.distance = best_d;
    t.rel_deg = relative_bearing(best->x - x, best->z - z, heading);
    return t;
}
