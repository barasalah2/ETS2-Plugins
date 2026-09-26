#pragma once

#include "cities.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// A junction maneuver: which way to go and which lanes lead there.
// turn: 1-4 left (slight, turn, sharp, U-turn), 11-14 right, 21-28 roundabout. flags: 2 roundabout,
// 4 an exit off a road that carries on. Lane mask bit 0 = the lane closest to the centre divider
// (the left lane, or the right lane where they drive on the left).
struct Turn
{
    uint8_t turn = 0, exit_no = 0, flags = 0, lanes = 0;
    uint16_t mask = 0;
    bool lht = false;  // left-hand traffic (UK)
};

// Road graph from the game's map data (data/route_graph.bin, see tools/make_route_graph.py).
// A state is a node plus a travel direction: state = node * 2 + (0 forward, 1 backward).
struct RouteGraph
{
    struct Edge { uint32_t to; float dist; float dur; uint32_t flags; };  // dist in map m, dur in s
    static const uint32_t kFerry = 1;
    static const uint32_t kFuel = 1, kParking = 2;  // service area flags

    bool load(const std::wstring& path);
    bool loaded() const { return !xz.empty(); }
    float x(uint32_t node) const { return xz[node * 2]; }
    float z(uint32_t node) const { return xz[node * 2 + 1]; }

    // The road the truck is on: the edge closest to (x, z) that points roughly along `heading`.
    struct Snap { bool ok = false; uint32_t edge = 0; double t = 0, dist = 0; };
    Snap snap(double x, double z, double heading, double radius) const;

    template <class F> void nodes_near(double x, double z, double r, F&& f) const;
    template <class F> void areas_near(double x, double z, double r, F&& f) const;

    std::vector<float> xz;           // per node
    std::vector<uint32_t> offsets;   // per state + 1: edges of state s are [offsets[s], offsets[s+1])
    std::vector<Edge> edges;
    std::vector<uint32_t> area_node, area_flags;
    std::unordered_map<uint32_t, uint32_t> area_of_node;
    std::unordered_map<std::string, std::vector<uint32_t>> companies;  // "city.company" -> nodes
    float max_speed = 28.0f;         // map m/s, for the A* heuristic

    struct Country { std::string name; uint8_t motorway = 0, expressway = 0, local = 0; };  // truck limits km/h
    std::unordered_map<uint8_t, Country> countries;  // by map country id
    std::vector<uint8_t> node_country;               // per node: forward-side id, backward-side id
    // The country you're in after passing a state's node in its direction (0 = unknown).
    uint8_t country_after(uint32_t state) const { return node_country.empty() ? 0 : node_country[state]; }

    // Junction maneuvers from data/maneuvers.bin, sorted by (from, to) node.
    struct Maneuver { uint32_t from, to; uint8_t turn, exit_no, flags, lanes; uint16_t mask; uint8_t choices, pad; };
    std::vector<Maneuver> maneuvers;
    bool load_maneuvers(const std::wstring& path);
    const Maneuver* maneuver(uint32_t from_node, uint32_t to_node) const;

private:
    std::unordered_map<int64_t, std::vector<uint32_t>> node_grid_, area_grid_;
};

// Something on the route: a place to refuel or sleep, a city you pass, a ferry, the destination.
struct RouteStop
{
    enum : uint8_t { Fuel = 1, Rest = 2, City = 4, Ferry = 8, Destination = 16, Border = 32, Maneuver = 64 };
    uint8_t kinds = 0;
    double at_m = 0;      // map meters from the start of the route where you'd leave it
    double detour_m = 0;  // extra map meters to visit it and get back on the route
    std::string label;
    std::string detail;   // e.g. a country's truck speed limits
    float x = 0, z = 0;
    Turn turn;            // for Maneuver stops
};

struct Route
{
    uint64_t id = 0;
    std::string target_key;  // "city.company"
    std::string dest_label;  // "Quarry, Prague"
    std::vector<float> px, pz;          // route line, starting at the truck
    std::vector<double> cum_m;          // map meters from the start, per point
    std::vector<double> cum_city_m;     // ... of which inside city areas (scaled differently)
    std::vector<double> cum_dur;        // seconds at truck speed limits, per point
    std::vector<double> cum_city_dur;
    std::vector<RouteStop> stops;       // sorted by at_m
    double length_m() const { return cum_m.empty() ? 0.0 : cum_m.back(); }
};

// Map meters/seconds -> the game's km/minutes. Open road and cities can scale differently.
struct Scales
{
    double km_road = 0.019, km_city = 0.019;  // game km per map meter
    double time_road = 19, time_city = 19;    // game seconds per real second
};

struct RouteRequest
{
    double x = 0, z = 0, heading = 0;
    std::string target_key, dest_label;
    double max_detour_m = 400;        // parking: at most this much extra driving, map meters
    double max_fuel_detour_m = 1050;  // fuel: further, where stations are sparse
};

struct CitySnapshot;

// The nearest fuel station you can drive to, in any direction (not only on the job route).
struct NearestPlace
{
    bool valid = false;
    double dist_m = 0;    // along the roads, map meters
    float x = 0, z = 0;
    std::string label;
    uint64_t stamp = 0;   // GetTickCount64 when found
};

// Computes routes on a background thread; the newest request wins.
class RouteEngine
{
public:
    bool init(const std::wstring& dir);
    void shutdown();
    bool available() const { return graph_.loaded(); }
    bool knows(const std::string& target_key) const { return graph_.companies.count(target_key) > 0; }

    // Snapshot used to name the cities on the route (radius: for cities without boundary data).
    void set_cities(const std::vector<City>& cities, double radius);
    void request(const RouteRequest& r);
    std::shared_ptr<const Route> latest();             // newest finished route (null if none)
    void clear();

    // Every fuel station on the map (pois.csv), snapped to the road graph, so the nearest-fuel
    // search also finds town stations that aren't motorway service areas.
    void set_fuel_points(const std::vector<std::pair<double, double>>& points);
    // Search the roads from the truck for the nearest fuel station, up to max_m map meters.
    void request_nearest_fuel(double x, double z, double heading, double max_m);
    NearestPlace nearest_fuel();

private:
    void worker();
    std::shared_ptr<Route> build(const RouteRequest& r, const CitySnapshot& cities);
    NearestPlace find_nearest_fuel(double x, double z, double heading, double max_m);

    RouteGraph graph_;
    std::wstring dir_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool quit_ = false, pending_ = false, pending_fuel_ = false;
    double fuel_x_ = 0, fuel_z_ = 0, fuel_heading_ = 0, fuel_max_m_ = 0;
    NearestPlace nearest_fuel_;
    std::unordered_map<uint32_t, bool> fuel_nodes_;  // node -> is a truck stop (fuel + sleep)
    RouteRequest request_;
    std::shared_ptr<const Route> latest_;
    std::shared_ptr<const CitySnapshot> cities_;
    uint64_t next_id_ = 1, generation_ = 0;
    std::string snap_fail_logged_;  // worker only: log "not on a road" once per destination
};

// What the warnings and the stops strip need, for where the truck is right now.
struct RouteView
{
    bool valid = false;
    uint64_t route_id = 0;
    bool gps_checked = false, matches_gps = false;  // compared with the game's own GPS distance
    double gps_ratio = 0;                           // GPS distance / ours (1 = identical)
    std::string dest_label;
    double remaining_km = 0, remaining_min = 0;
    double progress_m = 0;
    struct Item
    {
        uint8_t kinds;
        std::string label;
        double km, minutes;  // from the truck, along the route
        double at_m;
        float x, z;
        std::string detail;
        Turn turn;
        double detour_km = 0;  // extra driving to visit it and come back, game km
    };
    std::vector<Item> ahead;  // nearest first; the destination is last
};

// Follows the truck along the current route (game thread).
class RouteTracker
{
public:
    void set(std::shared_ptr<const Route> route);
    const Route* route() const { return route_.get(); }
    // Returns true (once) when the truck has clearly left the route and a new one should be computed.
    bool update(double x, double z, double heading, float speed_kmh);
    bool off_route() const { return reported_; }  // left the route and not back on it yet
    RouteView view(const Scales& s, float nav_distance_m, float nav_time_s) const;

private:
    std::shared_ptr<const Route> route_;
    size_t seg_ = 0;
    double t_ = 0;
    unsigned long long off_since_ = 0;
    bool reported_ = false;
};
