#pragma once

#include <string>
#include <utility>
#include <vector>

// Places the truck can refuel or sleep, from the game's map data (pois.csv).
enum class PoiKind { Fuel, Rest };

struct Poi
{
    PoiKind kind;
    double x, z;         // world position, meters
    std::string label;   // e.g. "Fuel station", "Truck stop", "Rest area"
};

struct Target
{
    bool valid = false;
    double distance = 0;  // straight-line world meters
    double km = 0;        // the same distance in the game's km (as the GPS/odometer count it)
    float rel_deg = 0;    // bearing relative to the truck: 0 = dead ahead, +90 = right, -90 = left
    std::string label;
    double x = 0, z = 0;
    bool on_route = false;  // km is the distance along your job route, not a straight line
    bool by_road = false;   // km is the road distance to it (found by the road search), not on the route
    double at_m = -1;       // for route targets: where on the route (map meters from its start)
};

// Bearing of (tx, tz) seen from a truck at (x, z) with SDK heading: 0 = ahead, +90 = right.
float bearing_to(double x, double z, double heading, double tx, double tz);

class PoiDb
{
public:
    void load(const std::wstring& dir);
    size_t count(PoiKind kind) const;

    // Nearest place of `kind` within +-cone_deg of the truck's heading and max_dist meters.
    // Falls back to the nearest one in any direction. `heading` is the SDK value (0..1, 0 = north,
    // counter-clockwise). `current` keeps the previous pick while it is still roughly ahead, so the
    // target doesn't jump between two candidates on every bend.
    Target find_ahead(PoiKind kind, double x, double z, double heading, double cone_deg,
                      double max_dist, const Target& current) const;

    // The nearest place of `kind` ahead that is further away than `next` and more than min_sep
    // world meters from it (so the station on the other side of the same motorway doesn't count).
    // This is where you'd have to get to if you skipped `next`.
    Target find_after(PoiKind kind, double x, double z, double heading, double cone_deg,
                      double max_dist, const Target& next, double min_sep) const;

    std::vector<std::pair<double, double>> positions(PoiKind kind) const
    {
        std::vector<std::pair<double, double>> out;
        for (const auto& p : pois_)
            if (p.kind == kind) out.push_back({p.x, p.z});
        return out;
    }

private:
    std::vector<Poi> pois_;
};
