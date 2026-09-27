#include "route.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <queue>

static_assert(sizeof(RouteGraph::Edge) == 16, "Edge must match route_graph.bin");
static_assert(sizeof(RouteGraph::Maneuver) == 16, "Maneuver must match maneuvers.bin");

static const double kPi = 3.14159265358979323846;
static const double kCell = 250.0;         // map m, spatial index cell
static const double kSnapRadius = 60.0;    // map m: how far off a road's centre line the truck may be
static const double kAreaSearch = 250.0;   // map m: service areas this close to the route are checked
static const double kDetourSearch = 1500;  // map m: how far the detour searches may look
static const double kFuelSearch = 1000.0;  // map m: fuel stations this close to the route are checked
static const float  kInf = std::numeric_limits<float>::infinity();

static int64_t cell_key(int cx, int cz) { return ((int64_t)cx << 32) ^ (int64_t)(uint32_t)cz; }
static int cell_of(double v) { return (int)std::floor(v / kCell); }

// --- graph ----------------------------------------------------------------------------------

template <class F> void RouteGraph::nodes_near(double px, double pz, double r, F&& f) const
{
    for (int cx = cell_of(px - r); cx <= cell_of(px + r); ++cx)
        for (int cz = cell_of(pz - r); cz <= cell_of(pz + r); ++cz) {
            auto it = node_grid_.find(cell_key(cx, cz));
            if (it != node_grid_.end())
                for (uint32_t n : it->second) f(n);
        }
}

template <class F> void RouteGraph::areas_near(double px, double pz, double r, F&& f) const
{
    for (int cx = cell_of(px - r); cx <= cell_of(px + r); ++cx)
        for (int cz = cell_of(pz - r); cz <= cell_of(pz + r); ++cz) {
            auto it = area_grid_.find(cell_key(cx, cz));
            if (it != area_grid_.end())
                for (uint32_t a : it->second) f(a);
        }
}

bool RouteGraph::load(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    struct Header
    {
        char magic[4];
        uint32_t version, nodes, edges, areas, companies, countries, string_bytes;
        float max_speed;
    } h{};
    struct CountryRecord { uint32_t id, name; uint8_t motorway, expressway, local, pad; };
    static_assert(sizeof(CountryRecord) == 12, "CountryRecord must match route_graph.bin");
    auto read = [&](void* p, size_t bytes) { return bytes == 0 || fread(p, 1, bytes, f) == bytes; };
    bool ok = read(&h, sizeof(h)) && memcmp(h.magic, "ECRG", 4) == 0 && h.version == 2;
    std::vector<uint32_t> areas, comps;
    std::vector<CountryRecord> country_records;
    std::vector<char> strings;
    if (ok) {
        xz.resize((size_t)h.nodes * 2);
        offsets.resize((size_t)h.nodes * 2 + 1);
        edges.resize(h.edges);
        areas.resize((size_t)h.areas * 2);
        comps.resize((size_t)h.companies * 2);
        country_records.resize(h.countries);
        node_country.resize((size_t)h.nodes * 2);
        strings.resize(h.string_bytes);
        ok = read(xz.data(), xz.size() * 4) && read(offsets.data(), offsets.size() * 4) &&
             read(edges.data(), edges.size() * sizeof(Edge)) && read(areas.data(), areas.size() * 4) &&
             read(comps.data(), comps.size() * 4) &&
             read(country_records.data(), country_records.size() * sizeof(CountryRecord)) &&
             read(node_country.data(), node_country.size()) && read(strings.data(), strings.size());
    }
    fclose(f);
    ok = ok && offsets.back() == edges.size() && (strings.empty() || strings.back() == 0);
    for (size_t i = 0; ok && i + 1 < offsets.size(); ++i) ok = offsets[i] <= offsets[i + 1];
    for (size_t i = 0; ok && i < edges.size(); ++i) ok = edges[i].to < h.nodes * 2;
    if (!ok) {
        xz.clear(); offsets.clear(); edges.clear();
        return false;
    }

    max_speed = h.max_speed > 1.0f ? h.max_speed : 28.0f;
    for (uint32_t i = 0; i < h.areas; ++i) {
        if (areas[i * 2] >= h.nodes) continue;
        area_of_node[areas[i * 2]] = (uint32_t)area_node.size();
        area_node.push_back(areas[i * 2]);
        area_flags.push_back(areas[i * 2 + 1]);
    }
    for (uint32_t i = 0; i < h.companies; ++i)
        if (comps[i * 2] < h.nodes && comps[i * 2 + 1] < strings.size())
            companies[std::string(&strings[comps[i * 2 + 1]])].push_back(comps[i * 2]);
    for (const auto& cr : country_records)
        if (cr.id < 256 && cr.name < strings.size())
            countries[(uint8_t)cr.id] = {std::string(&strings[cr.name]), cr.motorway, cr.expressway, cr.local};
    for (uint32_t n = 0; n < h.nodes; ++n)
        node_grid_[cell_key(cell_of(x(n)), cell_of(z(n)))].push_back(n);
    for (uint32_t a = 0; a < area_node.size(); ++a)
        area_grid_[cell_key(cell_of(x(area_node[a])), cell_of(z(area_node[a])))].push_back(a);
    return true;
}

bool RouteGraph::load_maneuvers(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    struct { char magic[4]; uint32_t version, count; } h{};
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.magic, "ECMV", 4) == 0 && h.version == 1;
    if (ok) {
        maneuvers.resize(h.count);
        ok = h.count == 0 || fread(maneuvers.data(), sizeof(Maneuver), h.count, f) == h.count;
    }
    fclose(f);
    const uint32_t n = (uint32_t)(xz.size() / 2);
    for (size_t i = 0; ok && i < maneuvers.size(); ++i) ok = maneuvers[i].from < n && maneuvers[i].to < n;
    if (!ok) maneuvers.clear();
    return ok;
}

const RouteGraph::Maneuver* RouteGraph::maneuver(uint32_t from_node, uint32_t to_node) const
{
    auto it = std::lower_bound(maneuvers.begin(), maneuvers.end(), std::make_pair(from_node, to_node),
                               [](const Maneuver& m, const std::pair<uint32_t, uint32_t>& k) {
                                   return m.from != k.first ? m.from < k.first : m.to < k.second;
                               });
    return it != maneuvers.end() && it->from == from_node && it->to == to_node ? &*it : nullptr;
}

RouteGraph::Snap RouteGraph::snap(double px, double pz, double heading, double radius) const
{
    // SDK heading: 0 = north (-Z), counter-clockwise, so forward = (-sin, -cos).
    const double fx = -std::sin(heading * 2 * kPi), fz = -std::cos(heading * 2 * kPi);
    Snap best;
    double best_score = 1e300;
    // Edges are short (median ~40 m) but some run for hundreds of meters, so look further for
    // their start nodes than the distance we accept from the road itself.
    nodes_near(px, pz, radius + 600.0, [&](uint32_t n) {
        for (uint32_t d = 0; d < 2; ++d) {
            const uint32_t s = n * 2 + d;
            for (uint32_t e = offsets[s]; e < offsets[s + 1]; ++e) {
                const Edge& E = edges[e];
                if (E.flags & kFerry) continue;
                const uint32_t m = E.to / 2;
                const double ax = x(n), az = z(n), dx = x(m) - ax, dz = z(m) - az;
                const double len2 = dx * dx + dz * dz;
                if (len2 < 0.01) continue;
                const double len = std::sqrt(len2);
                const double c = (dx * fx + dz * fz) / len;  // 1 = pointing where the truck points
                if (c < 0.5) continue;
                const double t = std::clamp(((px - ax) * dx + (pz - az) * dz) / len2, 0.0, 1.0);
                const double dist = std::hypot(px - (ax + t * dx), pz - (az + t * dz));
                const double score = dist + (1.0 - c) * 60.0;
                if (dist <= radius && score < best_score) {
                    best_score = score;
                    best.ok = true;
                    best.edge = e;
                    best.t = t;
                    best.dist = dist;
                }
            }
        }
    });
    return best;
}

// --- cities along the route -----------------------------------------------------------------

struct CitySnapshot
{
    std::vector<City> cities;
    std::unordered_map<int64_t, std::vector<uint32_t>> grid;  // 1 km cells -> city index
    double radius = 1800;                                     // for cities without boundary areas

    static int64_t key(double x, double z)
    {
        return cell_key((int)std::floor(x / 1000.0), (int)std::floor(z / 1000.0));
    }

    void build()
    {
        for (uint32_t i = 0; i < cities.size(); ++i) {
            const City& c = cities[i];
            auto cover = [&](double x1, double z1, double x2, double z2) {
                for (int cx = (int)std::floor(x1 / 1000); cx <= (int)std::floor(x2 / 1000); ++cx)
                    for (int cz = (int)std::floor(z1 / 1000); cz <= (int)std::floor(z2 / 1000); ++cz)
                        grid[cell_key(cx, cz)].push_back(i);
            };
            if (c.areas.empty()) cover(c.x - radius, c.z - radius, c.x + radius, c.z + radius);
            for (const auto& a : c.areas) cover(a.x1, a.z1, a.x2, a.z2);
        }
    }

    int find(double x, double z) const
    {
        auto it = grid.find(key(x, z));
        if (it == grid.end()) return -1;
        for (uint32_t i : it->second) {
            const City& c = cities[i];
            if (c.areas.empty()) {
                if (std::hypot(c.x - x, c.z - z) <= radius) return (int)i;
                continue;
            }
            for (const auto& a : c.areas)
                if (x >= a.x1 && x <= a.x2 && z >= a.z1 && z <= a.z2) return (int)i;
        }
        return -1;
    }
};

// --- engine ---------------------------------------------------------------------------------

bool RouteEngine::init(const std::wstring& dir)
{
    dir_ = dir;
    const ULONGLONG t0 = GetTickCount64();
    if (!graph_.load(dir + L"\\route_graph.bin")) {
        log_warn("route_graph.bin missing or invalid; route features disabled");
        return false;
    }
    if (!graph_.load_maneuvers(dir + L"\\maneuvers.bin"))
        log_warn("maneuvers.bin missing or invalid; no lane and exit warnings");
    log_info("road graph: %zu nodes, %zu edges, %zu service areas, %zu companies, %zu countries, "
             "%zu junction maneuvers (%llu ms)",
             graph_.xz.size() / 2, graph_.edges.size(), graph_.area_node.size(), graph_.companies.size(),
             graph_.countries.size(), graph_.maneuvers.size(), GetTickCount64() - t0);
    quit_ = false;
    thread_ = std::thread(&RouteEngine::worker, this);
    return true;
}

void RouteEngine::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
}

void RouteEngine::set_cities(const std::vector<City>& cities, double radius)
{
    auto snap = std::make_shared<CitySnapshot>();
    snap->cities = cities;
    snap->radius = radius;
    snap->build();
    std::lock_guard<std::mutex> lock(mutex_);
    cities_ = snap;
}

void RouteEngine::request(const RouteRequest& r)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_ = r;
        pending_ = true;
    }
    cv_.notify_one();
}

std::shared_ptr<const Route> RouteEngine::latest()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
}

void RouteEngine::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.reset();
    pending_ = false;
    ++generation_;  // a route still being built for the old job is thrown away
}

void RouteEngine::set_fuel_points(const std::vector<std::pair<double, double>>& points)
{
    std::unordered_map<uint32_t, bool> nodes;
    for (uint32_t a = 0; a < graph_.area_node.size(); ++a)
        if (graph_.area_flags[a] & RouteGraph::kFuel)
            nodes[graph_.area_node[a]] = (graph_.area_flags[a] & RouteGraph::kParking) != 0;
    size_t snapped = 0;
    for (const auto& [px, pz] : points) {
        // The nearest road node that has somewhere to go.
        double best = 150.0;
        uint32_t best_node = UINT32_MAX;
        graph_.nodes_near(px, pz, best, [&](uint32_t n) {
            if (graph_.offsets[n * 2] == graph_.offsets[n * 2 + 2]) return;  // no edges either way
            const double d = std::hypot(graph_.x(n) - px, graph_.z(n) - pz);
            if (d < best) { best = d; best_node = n; }
        });
        if (best_node != UINT32_MAX && !nodes.count(best_node)) {
            nodes[best_node] = false;
            ++snapped;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    fuel_nodes_.swap(nodes);
    log_info("nearest-fuel search: %zu fuel places (%zu town stations added from the map)", fuel_nodes_.size(), snapped);
}

void RouteEngine::request_nearest_fuel(double x, double z, double heading, double max_m)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fuel_x_ = x; fuel_z_ = z; fuel_heading_ = heading; fuel_max_m_ = max_m;
        pending_fuel_ = true;
    }
    cv_.notify_one();
}

NearestPlace RouteEngine::nearest_fuel()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nearest_fuel_;
}

// Dijkstra by road distance from the truck to the first fuel node. Runs on the worker thread;
// fuel_nodes_ is only replaced under the mutex before searches start, so reading it here is safe.
NearestPlace RouteEngine::find_nearest_fuel(double x, double z, double heading, double max_m)
{
    NearestPlace out;
    out.stamp = GetTickCount64();
    const RouteGraph& g = graph_;
    const RouteGraph::Snap sn = g.snap(x, z, heading, kSnapRadius);
    if (!sn.ok) return out;
    const RouteGraph::Edge& first = g.edges[sn.edge];
    const size_t states = g.offsets.size() - 1;
    std::vector<float> cost(states, kInf);
    using QE = std::pair<float, uint32_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    cost[first.to] = (float)((1.0 - sn.t) * first.dist);
    open.push({cost[first.to], first.to});
    while (!open.empty()) {
        const auto [c, s] = open.top();
        open.pop();
        if (c > cost[s]) continue;
        auto hit = fuel_nodes_.find(s / 2);
        if (hit != fuel_nodes_.end()) {
            out.valid = true;
            out.dist_m = c;
            out.x = g.x(s / 2);
            out.z = g.z(s / 2);
            out.label = hit->second ? "Truck stop" : "Fuel station";
            return out;
        }
        for (uint32_t e = g.offsets[s]; e < g.offsets[s + 1]; ++e) {
            const auto& E = g.edges[e];
            if (E.flags & RouteGraph::kFerry) continue;  // a ferry won't get you fuel in time
            const float nc = c + E.dist;
            if (nc < cost[E.to] && nc <= max_m) {
                cost[E.to] = nc;
                open.push({nc, E.to});
            }
        }
    }
    return out;  // nothing within max_m
}

void RouteEngine::worker()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [&] { return quit_ || pending_ || pending_fuel_; });
        if (quit_) break;
        if (pending_fuel_) {
            pending_fuel_ = false;
            const double fx = fuel_x_, fz = fuel_z_, fh = fuel_heading_, fm = fuel_max_m_;
            lock.unlock();
            const NearestPlace found = find_nearest_fuel(fx, fz, fh, fm);
            lock.lock();
            nearest_fuel_ = found;
            if (!pending_) continue;
        }
        const RouteRequest rq = request_;
        pending_ = false;
        const auto cities = cities_;
        const uint64_t generation = generation_;
        lock.unlock();

        static const CitySnapshot no_cities;
        const ULONGLONG t0 = GetTickCount64();
        std::shared_ptr<Route> r = build(rq, cities ? *cities : no_cities);
        const ULONGLONG ms = GetTickCount64() - t0;

        lock.lock();
        if (r && generation == generation_) {
            r->id = next_id_++;
            latest_ = r;
            int fuel = 0, rest = 0, city = 0, border = 0, turns = 0;
            for (const auto& s : r->stops) {
                fuel += (s.kinds & RouteStop::Fuel) != 0;
                rest += (s.kinds & RouteStop::Rest) != 0;
                city += (s.kinds & RouteStop::City) != 0;
                border += (s.kinds & RouteStop::Border) != 0;
                turns += (s.kinds & RouteStop::Maneuver) != 0;
            }
            log_info("route to '%s': %.1f map km, %.0f min at truck limits (real time), %zu points, "
                     "%d fuel / %d sleep stops, %d cities, %d borders, %d junction maneuvers, built in %llu ms",
                     rq.target_key.c_str(), r->length_m() / 1000.0, r->cum_dur.back() / 60.0, r->px.size(),
                     fuel, rest, city, border, turns, ms);
        }
    }
}

// Bounded Dijkstra by distance over a small neighbourhood. Sources are (state, origin tag, cost);
// `done(state)` returns true for a goal. Returns the goal state's cost and the origin tag it came from.
struct LocalSearch
{
    const RouteGraph& g;
    double limit;

    template <class Done>
    bool run(const std::vector<std::pair<uint32_t, int>>& sources, Done&& done, double& cost_out,
             int& origin_out, uint32_t& goal_out) const
    {
        std::unordered_map<uint32_t, std::pair<float, int>> best;  // state -> (cost, origin)
        using QE = std::pair<float, uint32_t>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
        for (const auto& [s, origin] : sources) {
            best[s] = {0.0f, origin};
            open.push({0.0f, s});
        }
        while (!open.empty()) {
            const auto [c, s] = open.top();
            open.pop();
            const auto entry = best[s];  // copy: inserting below may rehash the map
            const int origin = entry.second;
            if (c > entry.first) continue;
            if (done(s, origin)) {
                cost_out = c;
                origin_out = origin;
                goal_out = s;
                return true;
            }
            for (uint32_t e = g.offsets[s]; e < g.offsets[s + 1]; ++e) {
                const auto& E = g.edges[e];
                if (E.flags & RouteGraph::kFerry) continue;
                const float nc = c + E.dist;
                if (nc > limit) continue;
                auto it = best.find(E.to);
                if (it == best.end() || nc < it->second.first) {
                    best[E.to] = {nc, origin};
                    open.push({nc, E.to});
                }
            }
        }
        return false;
    }
};

std::shared_ptr<Route> RouteEngine::build(const RouteRequest& rq, const CitySnapshot& cities)
{
    const RouteGraph& g = graph_;
    auto target_it = g.companies.find(rq.target_key);
    if (target_it == g.companies.end()) {
        log_warn("route: '%s' is not in the road graph", rq.target_key.c_str());
        return nullptr;
    }
    const std::vector<uint32_t>& targets = target_it->second;

    const RouteGraph::Snap sn = g.snap(rq.x, rq.z, rq.heading, kSnapRadius);
    if (!sn.ok) {
        if (snap_fail_logged_ != rq.target_key)
            log_info("route: truck isn't on a known road yet (x=%.0f z=%.0f), will keep trying", rq.x, rq.z);
        snap_fail_logged_ = rq.target_key;
        return nullptr;
    }
    snap_fail_logged_.clear();
    const RouteGraph::Edge& first = g.edges[sn.edge];
    const uint32_t start = first.to;

    // A* on travel time (the game's default "fastest" route).
    const size_t states = g.offsets.size() - 1;
    std::vector<float> cost(states, kInf);
    std::vector<int32_t> via(states, -1), prev(states, -1);
    std::vector<uint8_t> closed(states, 0);
    auto h = [&](uint32_t s) {
        double best = 1e300;
        for (uint32_t t : targets) best = std::min(best, (double)std::hypot(g.x(s / 2) - g.x(t), g.z(s / 2) - g.z(t)));
        return (float)(best / g.max_speed);
    };
    using QE = std::pair<float, uint32_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    cost[start] = (float)((1.0 - sn.t) * first.dur);
    open.push({cost[start] + h(start), start});
    int64_t goal = -1;
    while (!open.empty()) {
        const uint32_t s = open.top().second;
        open.pop();
        if (closed[s]) continue;
        closed[s] = 1;
        if (std::find(targets.begin(), targets.end(), s / 2) != targets.end()) {
            goal = s;
            break;
        }
        for (uint32_t e = g.offsets[s]; e < g.offsets[s + 1]; ++e) {
            const auto& E = g.edges[e];
            const float nc = cost[s] + E.dur;
            if (nc < cost[E.to]) {
                cost[E.to] = nc;
                via[E.to] = (int32_t)e;
                prev[E.to] = (int32_t)s;
                open.push({nc + h(E.to), E.to});
            }
        }
    }
    if (goal < 0) {
        log_warn("route: no road connection to '%s'", rq.target_key.c_str());
        return nullptr;
    }
    std::vector<uint32_t> path;
    for (int64_t s = goal; s != start && via[s] >= 0 && path.size() < states; s = prev[s])
        path.push_back((uint32_t)via[s]);
    std::reverse(path.begin(), path.end());

    // Route line with cumulative distance/time, split into city and open-road parts.
    auto r = std::make_shared<Route>();
    r->target_key = rq.target_key;
    r->dest_label = rq.dest_label;
    std::vector<uint32_t> pstate;  // graph state per point (point 0 is the truck itself)
    auto add_point = [&](double px, double pz, uint32_t state, double dm, double dsec) {
        const bool in_city = !r->px.empty() &&
                             cities.find((px + r->px.back()) * 0.5, (pz + r->pz.back()) * 0.5) >= 0;
        const double m = r->cum_m.empty() ? 0.0 : r->cum_m.back();
        const double cm = r->cum_city_m.empty() ? 0.0 : r->cum_city_m.back();
        const double d = r->cum_dur.empty() ? 0.0 : r->cum_dur.back();
        const double cd = r->cum_city_dur.empty() ? 0.0 : r->cum_city_dur.back();
        r->px.push_back((float)px);
        r->pz.push_back((float)pz);
        r->cum_m.push_back(m + dm);
        r->cum_city_m.push_back(cm + (in_city ? dm : 0.0));
        r->cum_dur.push_back(d + dsec);
        r->cum_city_dur.push_back(cd + (in_city ? dsec : 0.0));
        pstate.push_back(state);
    };
    add_point(rq.x, rq.z, UINT32_MAX, 0, 0);
    add_point(g.x(start / 2), g.z(start / 2), start, (1.0 - sn.t) * first.dist, (1.0 - sn.t) * first.dur);
    for (uint32_t e : path) {
        const auto& E = g.edges[e];
        if (E.flags & RouteGraph::kFerry) {
            RouteStop st;
            st.kinds = RouteStop::Ferry;
            st.at_m = r->cum_m.back();
            st.label = "Ferry / train";
            st.x = r->px.back();
            st.z = r->pz.back();
            r->stops.push_back(st);
        }
        add_point(g.x(E.to / 2), g.z(E.to / 2), E.to, E.dist, E.dur);
    }

    // Cities the route passes through (not the one we start in).
    const int start_city = cities.find(rq.x, rq.z);
    int last_city = start_city;
    for (size_t i = 1; i < r->px.size(); ++i) {
        const int c = cities.find(r->px[i], r->pz[i]);
        if (c >= 0 && c != last_city) {
            bool seen = false;  // the route may clip a city's edge twice
            for (const auto& s : r->stops) seen |= (s.kinds & RouteStop::City) && s.label == cities.cities[c].name;
            if (!seen) {
                RouteStop st;
                st.kinds = RouteStop::City;
                st.at_m = r->cum_m[i];
                st.label = cities.cities[c].name;
                st.x = (float)cities.cities[c].x;
                st.z = (float)cities.cities[c].z;
                r->stops.push_back(st);
            }
        }
        if (c >= 0) last_city = c;
    }

    // Border crossings. A change only counts if the new country holds for the next few nodes, so a
    // single oddly-tagged node near a border doesn't invent a crossing.
    {
        uint8_t current = 0;
        for (size_t i = 1; i < pstate.size() && !current; ++i) current = g.country_after(pstate[i]);
        if (auto start = g.countries.find(current); start != g.countries.end()) r->start_country = start->second.name;
        for (size_t i = 1; i < pstate.size(); ++i) {
            const uint8_t c = g.country_after(pstate[i]);
            if (!c || c == current) continue;
            int agree = 0, seen = 0;
            for (size_t k = i + 1; k < pstate.size() && seen < 3; ++k) {
                const uint8_t n = g.country_after(pstate[k]);
                if (!n) continue;
                ++seen;
                agree += n == c;
            }
            if (seen && agree < seen) continue;
            current = c;
            auto it = g.countries.find(c);
            if (it == g.countries.end()) continue;
            RouteStop st;
            st.kinds = RouteStop::Border;
            st.at_m = r->cum_m[i];
            st.label = it->second.name;
            char limits[32];  // truck limits "motorway,local" for the announcement
            snprintf(limits, sizeof(limits), "%d,%d", it->second.motorway, it->second.local);
            st.detail = limits;
            st.x = r->px[i];
            st.z = r->pz[i];
            r->stops.push_back(st);
        }
    }

    // Junction maneuvers (exits, forks, turns, roundabouts) for the lane and exit warnings. A junction
    // with only one way out isn't a decision, and merging onto a road needs no instruction.
    {
        uint8_t country = 0;
        for (size_t i = 1; i + 1 < pstate.size(); ++i) {
            if (const uint8_t c = g.country_after(pstate[i])) country = c;
            const RouteGraph::Maneuver* m = g.maneuver(pstate[i] / 2, pstate[i + 1] / 2);
            if (!m || m->choices < 2 || (m->flags & 1)) continue;
            RouteStop st;
            st.kinds = RouteStop::Maneuver;
            st.at_m = r->cum_m[i];
            st.x = r->px[i];
            st.z = r->pz[i];
            auto cn = g.countries.find(country);
            st.turn = {m->turn, m->exit_no, m->flags, m->lanes, m->mask,
                       cn != g.countries.end() && cn->second.name == "United Kingdom"};
            r->stops.push_back(st);
        }
    }

    // Fuel stations and places to sleep you can reach with a short detour from the route.
    // A station on the other side of a motorway fails this: getting there and back needs a U-turn.
    // Parking is looked for next to the route; fuel further out (~1 km of map, ~19 game km), because
    // in sparse regions - northern Finland - the only fuel is in a town off the main road.
    std::unordered_map<uint32_t, uint32_t> index_of_state;  // route state -> point index
    for (uint32_t i = 1; i < pstate.size(); ++i) index_of_state.emplace(pstate[i], i);
    std::unordered_map<int64_t, std::vector<uint32_t>> route_cells;  // 500 m cells -> point index
    for (uint32_t i = 1; i < r->px.size(); ++i)
        route_cells[cell_key((int)std::floor(r->px[i] / 500.0), (int)std::floor(r->pz[i] / 500.0))].push_back(i);
    auto closest_point = [&](double x, double z, double radius, double& best, uint32_t& idx) {
        best = radius;
        idx = 0;
        const int cx = (int)std::floor(x / 500.0), cz = (int)std::floor(z / 500.0);
        const int reach = (int)std::ceil(radius / 500.0);
        for (int gx = cx - reach; gx <= cx + reach; ++gx)
            for (int gz = cz - reach; gz <= cz + reach; ++gz) {
                auto it = route_cells.find(cell_key(gx, gz));
                if (it == route_cells.end()) continue;
                for (uint32_t i : it->second) {
                    const double d = std::hypot(r->px[i] - x, r->pz[i] - z);
                    if (d < best) { best = d; idx = i; }
                }
            }
        return idx != 0;
    };
    struct Candidate { uint32_t flags; uint32_t point; };
    std::unordered_map<uint32_t, Candidate> candidates;  // node -> what it offers, nearest route point
    for (uint32_t a = 0; a < g.area_node.size(); ++a) {
        if (!(g.area_flags[a] & RouteGraph::kParking) || (g.area_flags[a] & RouteGraph::kFuel)) continue;
        double d;
        uint32_t i;
        if (closest_point(g.x(g.area_node[a]), g.z(g.area_node[a]), kAreaSearch, d, i))
            candidates[g.area_node[a]] = {RouteGraph::kParking, i};
    }
    for (const auto& [node, truck_stop] : fuel_nodes_) {
        double d;
        uint32_t i;
        if (closest_point(g.x(node), g.z(node), kFuelSearch, d, i))
            candidates[node] = {RouteGraph::kFuel | (truck_stop ? RouteGraph::kParking : 0u), i};
    }
    const LocalSearch near_search{g, kDetourSearch}, far_search{g, kDetourSearch * 2.5};
    const size_t last = r->px.size() - 1;
    for (const auto& [node, cand] : candidates) {
        const uint32_t flags = cand.flags;
        const bool fuel = (flags & RouteGraph::kFuel) != 0;
        const LocalSearch& search = fuel ? far_search : near_search;
        double at_m = -1, detour = 0;
        auto on_route = index_of_state.find(node * 2);
        if (on_route == index_of_state.end()) on_route = index_of_state.find(node * 2 + 1);
        if (on_route != index_of_state.end()) {
            at_m = r->cum_m[on_route->second];
        } else {
            const uint32_t ic = cand.point;
            const uint32_t back = fuel ? 40 : 12, ahead = fuel ? 5 : 2;
            std::vector<std::pair<uint32_t, int>> from_route;
            for (uint32_t j = ic > back ? ic - back : 1; j <= std::min<size_t>(last, ic + ahead); ++j)
                from_route.push_back({pstate[j], (int)j});
            double d1 = 0, d2 = 0;
            int j1 = -1, j2 = -1;
            uint32_t goal_state = 0;
            if (!search.run(from_route, [&](uint32_t s, int) { return s / 2 == node; }, d1, j1, goal_state))
                continue;
            // Back onto the route after the exit point. Some service areas can only be left in
            // one direction in the graph, so try both.
            std::vector<std::pair<uint32_t, int>> from_area = {{node * 2, 0}, {node * 2 + 1, 0}};
            if (!search.run(from_area,
                            [&](uint32_t s, int) {
                                auto it = index_of_state.find(s);
                                return it != index_of_state.end() && (int)it->second > j1;
                            },
                            d2, j2, goal_state))
                continue;
            j2 = (int)index_of_state[goal_state];
            detour = std::max(0.0, d1 + d2 - (r->cum_m[j2] - r->cum_m[j1]));
            if (detour > (fuel ? rq.max_fuel_detour_m : rq.max_detour_m)) continue;
            at_m = r->cum_m[j1];
        }
        RouteStop st;
        st.kinds = (uint8_t)((fuel ? RouteStop::Fuel : 0) | ((flags & RouteGraph::kParking) ? RouteStop::Rest : 0));
        st.at_m = at_m;
        st.detour_m = detour;
        st.label = st.kinds == (RouteStop::Fuel | RouteStop::Rest) ? "Truck stop"
                 : st.kinds == RouteStop::Fuel                     ? "Fuel station"
                                                                   : "Parking";
        st.x = g.x(node);
        st.z = g.z(node);
        r->stops.push_back(st);
    }

    // A station and a truck stop sharing one exit (e.g. an Autohof reachable from both sides) are
    // one stop for the driver: merge places that you'd leave the route for at the same point.
    std::stable_sort(r->stops.begin(), r->stops.end(),
                     [](const RouteStop& a, const RouteStop& b) { return a.at_m < b.at_m; });
    std::vector<RouteStop> merged;
    const uint8_t place = RouteStop::Fuel | RouteStop::Rest;
    for (const RouteStop& st : r->stops) {
        RouteStop* prev = nullptr;
        for (auto it = merged.rbegin(); it != merged.rend() && st.at_m - it->at_m < 250.0; ++it)
            if ((it->kinds & place) && (st.kinds & place) && std::hypot(it->x - st.x, it->z - st.z) < 600.0) {
                prev = &*it;
                break;
            }
        if (!prev) {
            merged.push_back(st);
            continue;
        }
        prev->kinds |= st.kinds;
        prev->detour_m = std::min(prev->detour_m, st.detour_m);
        prev->label = (prev->kinds & place) == place ? "Truck stop" : prev->label;
    }
    r->stops.swap(merged);

    RouteStop dest;
    dest.kinds = RouteStop::Destination;
    dest.at_m = r->length_m();
    dest.label = rq.dest_label;
    dest.x = r->px.back();
    dest.z = r->pz.back();
    r->stops.push_back(dest);
    std::stable_sort(r->stops.begin(), r->stops.end(),
                     [](const RouteStop& a, const RouteStop& b) { return a.at_m < b.at_m; });

    if (GetEnvironmentVariableW(L"ETS2_CITY_OVERLAY_DEBUG", nullptr, 0) > 0) {
        // For tools/test_host: the route line and stops, so it can drive along them.
        if (FILE* f = _wfopen((dir_ + L"\\route_debug.csv").c_str(), L"wb")) {
            fputs("type,x,z,at_m,kinds,label\n", f);
            for (size_t i = 0; i < r->px.size(); ++i)
                fprintf(f, "p,%.2f,%.2f,%.1f,0,\n", r->px[i], r->pz[i], r->cum_m[i]);
            for (const auto& s : r->stops)
                fprintf(f, "s,%.2f,%.2f,%.1f,%d,%s%s\n", s.x, s.z, s.at_m, s.kinds, s.label.c_str(),
                        (s.kinds & RouteStop::Maneuver) ? ("turn " + std::to_string(s.turn.turn) + " lanes " +
                                                           std::to_string(s.turn.lanes) + " mask " +
                                                           std::to_string(s.turn.mask)).c_str()
                                                        : "");
            fclose(f);
        }
    }
    return r;
}

// --- tracking -------------------------------------------------------------------------------

void RouteTracker::set(std::shared_ptr<const Route> route)
{
    route_ = std::move(route);
    seg_ = 0;
    t_ = 0;
    off_since_ = 0;
    reported_ = false;
}

bool RouteTracker::update(double x, double z, double heading, float speed_kmh)
{
    if (!route_ || route_->px.size() < 2) return false;
    const Route& r = *route_;
    const double fx = -std::sin(heading * 2 * kPi), fz = -std::cos(heading * 2 * kPi);
    const size_t segments = r.px.size() - 1;
    const size_t lo = seg_ >= 3 ? seg_ - 3 : 0, hi = std::min(segments, seg_ + 80);
    double best_score = 1e300, best_dist = 1e300, best_t = t_;
    size_t best = seg_;
    for (size_t i = lo; i < hi; ++i) {
        const double ax = r.px[i], az = r.pz[i], dx = r.px[i + 1] - ax, dz = r.pz[i + 1] - az;
        const double len2 = dx * dx + dz * dz;
        const double t = len2 > 0.01 ? std::clamp(((x - ax) * dx + (z - az) * dz) / len2, 0.0, 1.0) : 0.0;
        const double dist = std::hypot(x - (ax + t * dx), z - (az + t * dz));
        const double c = len2 > 0.01 ? (dx * fx + dz * fz) / std::sqrt(len2) : 1.0;
        // Prefer segments pointing our way and not behind the one we were on.
        const double score = dist + (c < 0 ? 200.0 : 0.0) + (i < seg_ ? 5.0 : 0.0);
        if (score < best_score) {
            best_score = score;
            best_dist = dist;
            best = i;
            best_t = t;
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (best_dist <= 80.0 && best_score < 200.0) {
        seg_ = best;
        t_ = best_t;
        off_since_ = 0;
        reported_ = false;
        return false;
    }
    if (std::fabs(speed_kmh) < 5.0f) return false;  // manoeuvring at a dock or in a car park
    // Pulling into a fuel station or parking on the route is part of the plan, not leaving it.
    for (const RouteStop& st : r.stops)
        if ((st.kinds & (RouteStop::Fuel | RouteStop::Rest)) && std::hypot(st.x - x, st.z - z) < 400.0) {
            off_since_ = 0;
            return false;
        }
    if (reported_) return false;
    if (!off_since_) off_since_ = now;
    if (now - off_since_ <= 5000) return false;
    reported_ = true;
    return true;
}

RouteView RouteTracker::view(const Scales& s, float nav_distance_m, float nav_time_s) const
{
    RouteView v;
    if (!route_ || route_->px.size() < 2) return v;
    const Route& r = *route_;
    const size_t i0 = std::min(seg_, r.px.size() - 2);
    auto here = [&](const std::vector<double>& a) { return a[i0] + t_ * (a[i0 + 1] - a[i0]); };
    const double m0 = here(r.cum_m), cm0 = here(r.cum_city_m), d0 = here(r.cum_dur), cd0 = here(r.cum_city_dur);

    // Interpolated values at an arbitrary distance along the route.
    auto at = [&](double m, double& cm, double& d, double& cd) {
        size_t i = (size_t)(std::upper_bound(r.cum_m.begin(), r.cum_m.end(), m) - r.cum_m.begin());
        i = std::clamp<size_t>(i, 1, r.cum_m.size() - 1);
        const double span = r.cum_m[i] - r.cum_m[i - 1];
        const double f = span > 0 ? std::clamp((m - r.cum_m[i - 1]) / span, 0.0, 1.0) : 1.0;
        cm = r.cum_city_m[i - 1] + f * (r.cum_city_m[i] - r.cum_city_m[i - 1]);
        d = r.cum_dur[i - 1] + f * (r.cum_dur[i] - r.cum_dur[i - 1]);
        cd = r.cum_city_dur[i - 1] + f * (r.cum_city_dur[i] - r.cum_city_dur[i - 1]);
    };
    auto km = [&](double m, double cm) { return ((m - m0) - (cm - cm0)) * s.km_road + (cm - cm0) * s.km_city; };
    auto minutes = [&](double d, double cd) {
        return (((d - d0) - (cd - cd0)) * s.time_road + (cd - cd0) * s.time_city) / 60.0;
    };

    v.valid = true;
    v.route_id = r.id;
    v.progress_m = m0;
    v.dest_label = r.dest_label;
    v.country = r.start_country;
    for (const auto& st : r.stops) {
        if (st.at_m > m0) break;
        if (st.kinds & RouteStop::Border) v.country = st.label;
    }
    v.remaining_km = km(r.cum_m.back(), r.cum_city_m.back());
    v.remaining_min = minutes(r.cum_dur.back(), r.cum_city_dur.back());

    // If the game's own GPS agrees with our route, scale to its numbers so the two match exactly.
    double kd = 1.0, kt = 1.0;
    if (nav_distance_m > 100.0f && v.remaining_km > 0.1) {
        v.gps_checked = true;
        const double ratio = nav_distance_m / 1000.0 / v.remaining_km;
        v.gps_ratio = ratio;
        v.matches_gps = ratio > 0.8 && ratio < 1.25;
        if (v.matches_gps) {
            kd = ratio;
            if (nav_time_s > 0 && v.remaining_min > 0.5) {
                const double rt = nav_time_s / 60.0 / v.remaining_min;
                if (rt > 0.5 && rt < 2.0) kt = rt;
            }
        }
    }
    v.remaining_km *= kd;
    v.remaining_min *= kt;

    for (const RouteStop& st : r.stops) {
        if (st.at_m < m0 - 1.0) continue;  // already passed
        double cm, d, cd;
        at(st.at_m, cm, d, cd);
        v.ahead.push_back({st.kinds, st.label, km(st.at_m, cm) * kd, minutes(d, cd) * kt, st.at_m, st.x, st.z, st.detail,
                           st.turn, st.detour_m * s.km_road * kd});
    }
    return v;
}
