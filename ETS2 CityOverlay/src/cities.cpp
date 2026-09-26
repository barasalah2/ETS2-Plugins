#include "cities.h"
#include "csv.h"
#include "log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

// A stale dataset position is replaced by a learned one if they are further apart than this.
static const double kRelearnDistance = 3000.0;
// Extra margin around the current city's areas so the name doesn't flicker at the boundary.
static const double kStayMargin = 250.0;

static double num(const std::string& s) { return strtod(s.c_str(), nullptr); }

static bool inside(const CityArea& a, double x, double z, double margin)
{
    return x >= a.x1 - margin && x <= a.x2 + margin && z >= a.z1 - margin && z <= a.z2 + margin;
}

void CityDb::load(const std::wstring& dir)
{
    dir_ = dir;
    cities_.clear();
    index_.clear();
    learned_.clear();

    // cities.csv: id,name,country,x,z
    auto rows = read_csv(dir + L"\\cities.csv");
    for (size_t i = 1; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.size() < 5 || r[0].empty()) continue;
        City c;
        c.id = r[0]; c.name = r[1]; c.country = r[2];
        c.x = num(r[3]); c.z = num(r[4]);
        c.from_dataset = true;
        index_[c.id] = cities_.size();
        cities_.push_back(std::move(c));
    }

    // city_areas.csv: id,x1,z1,x2,z2 (one row per boundary rectangle)
    rows = read_csv(dir + L"\\city_areas.csv");
    for (size_t i = 1; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.size() < 5) continue;
        auto it = index_.find(r[0]);
        if (it == index_.end()) continue;
        CityArea a{num(r[1]), num(r[2]), num(r[3]), num(r[4])};
        if (a.x1 > a.x2) std::swap(a.x1, a.x2);
        if (a.z1 > a.z2) std::swap(a.z1, a.z2);
        cities_[it->second].areas.push_back(a);
    }

    // learned.csv: id,name,x,z,count (x/z are the mean of all recorded points)
    rows = read_csv(dir + L"\\learned.csv");
    for (size_t i = 1; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (r.size() < 5 || r[0].empty()) continue;
        Learned l;
        l.name = r[1];
        l.count = std::max(1, atoi(r[4].c_str()));
        l.sx = num(r[2]) * l.count;
        l.sz = num(r[3]) * l.count;
        learned_[r[0]] = l;
        apply_learned(r[0], l);
    }
}

size_t CityDb::area_count() const
{
    size_t n = 0;
    for (const auto& c : cities_) n += c.areas.size();
    return n;
}

void CityDb::apply_learned(const std::string& id, const Learned& l)
{
    const double x = l.sx / l.count, z = l.sz / l.count;
    auto it = index_.find(id);
    if (it == index_.end()) {
        City c;
        c.id = id;
        c.name = l.name.empty() ? id : l.name;
        c.x = x; c.z = z;
        index_[id] = cities_.size();
        cities_.push_back(std::move(c));
        return;
    }
    City& c = cities_[it->second];
    if (!c.from_dataset) {
        if (!l.name.empty()) c.name = l.name;
        c.x = x; c.z = z;
    } else if (c.areas.empty() && std::hypot(c.x - x, c.z - z) > kRelearnDistance) {
        // Map boundaries are authoritative; only point-only dataset entries get corrected.
        c.x = x; c.z = z;
    }
}

void CityDb::learn(const std::string& id, const std::string& name, double x, double z)
{
    if (id.empty()) return;
    Learned& l = learned_[id];
    if (!name.empty()) l.name = name;
    l.sx += x; l.sz += z; l.count++;
    apply_learned(id, l);
    save_learned();
    log_info("learned city '%s' (%s) at x=%.0f z=%.0f (samples=%d)", id.c_str(), name.c_str(), x, z, l.count);
}

void CityDb::save_learned() const
{
    FILE* f = _wfopen((dir_ + L"\\learned.csv").c_str(), L"wb");
    if (!f) { log_warn("cannot write learned.csv"); return; }
    fputs("id,name,x,z,count\n", f);
    for (const auto& [id, l] : learned_)
        fprintf(f, "%s,%s,%.1f,%.1f,%d\n", csv_escape(id).c_str(), csv_escape(l.name).c_str(),
                l.sx / l.count, l.sz / l.count, l.count);
    fclose(f);
}

Location CityDb::locate(double x, double z, const std::string& current_id,
                        double in_radius, double near_radius) const
{
    auto make = [&](const City& c, Location::Kind kind) {
        Location loc;
        loc.kind = kind;
        loc.id = c.id; loc.name = c.name; loc.country = c.country;
        loc.distance = std::hypot(c.x - x, c.z - z);
        return loc;
    };
    auto is_in = [&](const City& c, bool current) {
        if (c.areas.empty())
            return std::hypot(c.x - x, c.z - z) <= in_radius * (current ? 1.25 : 1.0);
        for (const auto& a : c.areas)
            if (inside(a, x, z, current ? kStayMargin : 0.0)) return true;
        return false;
    };

    // Stay in the current city while still (roughly) inside it.
    if (!current_id.empty()) {
        auto it = index_.find(current_id);
        if (it != index_.end() && is_in(cities_[it->second], true))
            return make(cities_[it->second], Location::InCity);
    }

    const City* in_best = nullptr;
    const City* near_best = nullptr;
    double in_d = 1e300, near_d = 1e300;
    for (const auto& c : cities_) {
        const double d = std::hypot(c.x - x, c.z - z);
        if (d < near_d) { near_d = d; near_best = &c; }
        if (d < in_d && is_in(c, false)) { in_d = d; in_best = &c; }
    }
    if (in_best) return make(*in_best, Location::InCity);
    if (near_best && near_d <= near_radius) return make(*near_best, Location::Near);
    return {};
}
