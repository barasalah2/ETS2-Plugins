#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// World coordinates are the game's x/z plane (y is height), in meters.
struct CityArea
{
    double x1, z1, x2, z2;  // axis-aligned rectangle, x1<=x2, z1<=z2
};

struct City
{
    std::string id;       // game token, e.g. "berlin" (same as the SDK's destination.city.id)
    std::string name;     // display name, UTF-8
    std::string country;  // display name, UTF-8 (may be empty)
    double x = 0, z = 0;  // city centre
    std::vector<CityArea> areas;  // city boundary rectangles from the map; empty = use radius
    bool from_dataset = false;    // came from cities.csv (vs. learned only)
};

struct Location
{
    enum Kind { None, InCity, Near } kind = None;
    std::string id, name, country;
    double distance = 0;  // to the city centre, world meters
    double km = 0;        // the same in the game's km (filled in by the plugin)
};

class CityDb
{
public:
    // Loads cities.csv + city_areas.csv (optional) + learned.csv (optional) from dir.
    void load(const std::wstring& dir);
    size_t size() const { return cities_.size(); }
    size_t area_count() const;
    size_t learned_count() const { return learned_.size(); }
    const std::vector<City>& all() const { return cities_; }

    // current_id is the city we were in last time; used for hysteresis at the boundary.
    Location locate(double x, double z, const std::string& current_id,
                    double in_radius, double near_radius) const;

    // Records a position the game says belongs to city `id` (e.g. where a job was delivered).
    // Adds unknown cities and corrects stale positions. Persists to learned.csv.
    void learn(const std::string& id, const std::string& name, double x, double z);

private:
    struct Learned { std::string name; double sx = 0, sz = 0; int count = 0; };

    void apply_learned(const std::string& id, const Learned& l);
    void save_learned() const;

    std::vector<City> cities_;
    std::unordered_map<std::string, size_t> index_;  // id -> cities_ index
    std::unordered_map<std::string, Learned> learned_;
    std::wstring dir_;
};
