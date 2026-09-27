"""Build the plugin's map data: data/cities.csv, data/city_areas.csv, data/pois.csv.

Two sources:

  python tools/make_cities_csv.py --parser-out <dir>
      Preferred. <dir> is the output of the truckermudgeon/maps parser run against your own
      game install (europe-cities.json, europe-countries.json). Gives current positions for
      every map DLC you own plus the real city boundary rectangles.

  python tools/make_cities_csv.py
      Fallback. Downloads Koenvh1's ETS2-City-Coordinate-Retriever lists (MIT). Positions
      only, base map + older DLCs (no Iberia, West Balkans, Greece, Nordic Horizons...).
      No fuel stations or rest areas (pois.csv is left empty).
"""
import argparse
import csv
import json
import math
import pathlib
import urllib.request

DATA = pathlib.Path(__file__).resolve().parent.parent / "data"

KOENVH1 = "https://raw.githubusercontent.com/Koenvh1/ETS2-City-Coordinate-Retriever/master/"
KOENVH1_FILES = [  # later files override earlier ones for the same city id
    "cities_default", "cities_east", "cities_north", "cities_fr",
    "cities_italy_map", "cities_btbs", "cities_balkan_e",
]
COUNTRY_NAMES = {  # Koenvh1 only has tokens
    "uk": "United Kingdom", "czech": "Czech Republic", "turkey": "Türkiye",
    "bosnia": "Bosnia and Herzegovina", "macedonia": "North Macedonia",
}


def from_parser(out_dir):
    out_dir = pathlib.Path(out_dir)
    countries = {c["token"]: c["name"] for c in
                 json.loads((out_dir / "europe-countries.json").read_text(encoding="utf-8"))}
    cities, areas = {}, []
    for c in json.loads((out_dir / "europe-cities.json").read_text(encoding="utf-8")):
        cid = c["token"]
        # Map area: (x, y) is the top-left corner, extending by width/height (map y = world z).
        # The city's own x/y is its first visible area's corner, so the centre is that area's middle.
        # Hidden areas carry no label on the map but are still city territory, so keep them for detection.
        cx, cz = c["x"], c["y"]
        visible = [a for a in c.get("areas", []) if not a.get("hidden")]
        if visible:
            cx = visible[0]["x"] + visible[0]["width"] / 2
            cz = visible[0]["y"] + visible[0]["height"] / 2
        cities[cid] = (c["name"], countries.get(c["countryToken"], c["countryToken"]), cx, cz)
        for a in c.get("areas", []):
            areas.append((cid, a["x"], a["y"], a["x"] + a["width"], a["y"] + a["height"]))
    return cities, areas


# Where you can sleep, by prefab folder. Garages only work if you own them, so they're left out.
REST_LABELS = {"gas_station": "Truck stop", "gas_big": "Truck stop", "gas": "Truck stop",
               "gas_small": "Truck stop", "gas_small_road": "Truck stop", "gas_parking": "Truck stop",
               "hotel": "Hotel", "rest_places": "Rest area", "parking": "Parking"}


def merge_nearby(points, radius):
    """Collapse points of the same label closer than radius into one (their mean)."""
    merged = []
    for label, x, z in points:
        for m in merged:
            if m[0] == label and math.hypot(m[1] / m[3] - x, m[2] / m[3] - z) < radius:
                m[1] += x; m[2] += z; m[3] += 1
                break
        else:
            merged.append([label, x, z, 1])
    return [(m[0], m[1] / m[3], m[2] / m[3]) for m in merged]


def pois_from_parser(out_dir):
    pois = json.loads((pathlib.Path(out_dir) / "europe-pois.json").read_text(encoding="utf-8"))
    # The parser emits one entry per pump / parking spot; group by prefab first.
    by_prefab = {}
    triggers = []
    for p in pois:
        if p.get("type") != "facility" or p.get("icon") not in ("gas_ico", "parking_ico"):
            continue
        kind = "fuel" if p["icon"] == "gas_ico" else "rest"
        path = p.get("prefabPath", "")
        folder = path.split("/")[1] if path.count("/") >= 2 else ""
        if kind == "fuel":
            label = "Fuel station"
        elif folder.startswith("garage"):
            continue
        else:
            label = REST_LABELS.get(folder, "Parking")
        if "prefabUid" in p:
            by_prefab.setdefault((kind, label, p["prefabUid"]), []).append((p["x"], p["y"]))
        else:
            triggers.append((kind, label, p["x"], p["y"]))

    # Only merge within one prefab (and loose triggers), never across prefabs: stations on the
    # two sides of a motorway are separate places and only one of them is reachable.
    rows = []
    for (kind, label, _), pts in by_prefab.items():
        rows.append((kind, sum(x for x, _ in pts) / len(pts), sum(z for _, z in pts) / len(pts), label))
    for kind in ("fuel", "rest"):
        loose = [(label, x, z) for k, label, x, z in triggers if k == kind]
        for label, x, z in merge_nearby(loose, 100.0):
            rows.append((kind, x, z, label))
    return rows


def from_koenvh1():
    cities = {}
    for name in KOENVH1_FILES:
        with urllib.request.urlopen(KOENVH1 + name + ".json") as r:
            data = json.loads(r.read().decode("utf-8"))
        for c in data["citiesList"]:
            token = c["country"].strip().lower()
            country = COUNTRY_NAMES.get(token, token.capitalize())
            cities[c["gameName"]] = (c["realName"].strip(), country, float(c["x"]), float(c["z"]))
        print(f"{name}: {len(data['citiesList'])} cities")
    return cities, []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parser-out", help="truckermudgeon/maps parser output directory")
    args = ap.parse_args()
    cities, areas = from_parser(args.parser_out) if args.parser_out else from_koenvh1()
    pois = pois_from_parser(args.parser_out) if args.parser_out else []

    DATA.mkdir(parents=True, exist_ok=True)
    with (DATA / "cities.csv").open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id", "name", "country", "x", "z"])
        for cid in sorted(cities):
            name, country, x, z = cities[cid]
            w.writerow([cid, name, country, f"{x:.1f}", f"{z:.1f}"])
    with (DATA / "city_areas.csv").open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id", "x1", "z1", "x2", "z2"])
        for a in sorted(areas):
            w.writerow([a[0]] + [f"{v:.1f}" for v in a[1:]])
    with (DATA / "pois.csv").open("w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kind", "x", "z", "label"])
        for kind, x, z, label in sorted(pois):
            w.writerow([kind, f"{x:.1f}", f"{z:.1f}", label])
    fuel = sum(1 for p in pois if p[0] == "fuel")
    print(f"wrote {len(cities)} cities, {len(areas)} boundary areas, "
          f"{fuel} fuel stations, {len(pois) - fuel} rest places to {DATA}")
    if args.parser_out:
        # Each country's diesel price in the game's economy (for "cheaper fuel across the border").
        countries = json.loads((pathlib.Path(args.parser_out) / "europe-countries.json").read_text(encoding="utf-8"))
        with (DATA / "countries.csv").open("w", encoding="utf-8", newline="") as f:
            w = csv.writer(f)
            w.writerow(["name", "code", "fuel_price"])
            for c in sorted(countries, key=lambda c: c["name"]):
                w.writerow([c["name"], c.get("code", ""), f"{c.get('fuelPrice', 0):.3f}"])
        print(f"wrote {len(countries)} countries (fuel prices) to {DATA}")


if __name__ == "__main__":
    main()
