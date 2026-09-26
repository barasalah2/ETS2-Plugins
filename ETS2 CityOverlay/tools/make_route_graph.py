"""Pack the road graph into data/route_graph.bin for the plugin's route engine.

Inputs come from the truckermudgeon/maps tools run against your own game install:
  parser:    npx tsx packages/clis/parser/index.ts -i "<ETS2 folder>" -o out
  generator: npx tsx packages/clis/generator/index.ts graph -m europe -i out -o graphout

  python tools/make_route_graph.py --graph C:\\tmmaps\\graphout\\europe-graph.json --map-out C:\\tmmaps\\out
      [--maneuvers C:\\tmmaps\\graphout\\europe-maneuvers.csv]   (from C:\\tmmaps\\export-maneuvers.ts)

maneuvers.bin (for lane and exit warnings), version 1:
  header   magic "ECMV", u32 version, u32 count
  records  u32 from_node, u32 to_node, u8 turn, u8 roundabout exit, u8 flags, u8 lanes, u16 lane mask,
           u8 choices (ways out of the junction from this entry), u8 0
           sorted by (from_node, to_node). turn: 1-4 left (slight..U-turn), 11-14 right, 21-28 roundabout.
           flags: 1 merge, 2 roundabout, 4 exit off a road that carries on. Lane mask bit 0 = lane
           closest to the centre divider.

File layout (little-endian), version 2:
  header   magic "ECRG", u32 version, u32 nodes, u32 edges, u32 service_areas, u32 companies,
           u32 countries, u32 string_bytes, f32 max_speed (map m per s, for the A* heuristic)
  nodes    f32 x, f32 z                      per node
  offsets  u32                               per state + 1 (state = node*2 + dir, dir 0=forward 1=backward)
  edges    u32 to_state, f32 distance_m, f32 duration_s, u32 flags (1 = ferry/train, 2 = one-lane road)
  areas    u32 node, u32 flags (1 = fuel, 2 = parking/sleep, 4 = repair service)
  companies u32 node, u32 name offset ("city.company" in the string table)
  countries u32 id, u32 name offset, u8 truck limit km/h on motorway / expressway / local road, u8 0
  node countries  u8 forward-side country id, u8 backward-side country id   per node (0 = unknown)
  strings  zero-terminated UTF-8
"""
import argparse
import csv
import json
import math
import pathlib
import struct

OUT = pathlib.Path(__file__).resolve().parent.parent / "data" / "route_graph.bin"
OUT_MANEUVERS = OUT.with_name("maneuvers.bin")
DIRS = {"forward": 0, "backward": 1}
AREA_FLAGS = {"gas_ico": 1, "parking_ico": 2, "service_ico": 4}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--graph", required=True, help="europe-graph.json from the generator")
    ap.add_argument("--map-out", required=True, help="parser output dir (europe-nodes.json, europe-companies.json)")
    ap.add_argument("--maneuvers", help="europe-maneuvers.csv from export-maneuvers.ts")
    args = ap.parse_args()
    map_out = pathlib.Path(args.map_out)

    data = json.loads(pathlib.Path(args.graph).read_text(encoding="utf-8"))
    graph = {uid: nb for uid, nb in data["graph"]}

    # Every node that is a key or an edge target needs an index and a position.
    wanted = set(graph)
    for nb in graph.values():
        for d in DIRS:
            for e in nb.get(d, []):
                wanted.add(e["nodeUid"])
    pos, node_country = {}, {}
    for n in json.loads((map_out / "europe-nodes.json").read_text(encoding="utf-8")):
        if n["uid"] in wanted:
            pos[n["uid"]] = (n["x"], n["y"])
            node_country[n["uid"]] = (n.get("forwardCountryId", 0) & 0xFF, n.get("backwardCountryId", 0) & 0xFF)
    uids = sorted(u for u in wanted if u in pos)
    index = {u: i for i, u in enumerate(uids)}
    print(f"nodes: {len(uids)} ({len(wanted) - len(uids)} referenced without a position, dropped)")

    offsets, edges = [0], []
    max_speed = 0.0
    for u in uids:
        nb = graph.get(u, {})
        for d in DIRS:
            for e in nb.get(d, []):
                to = index.get(e["nodeUid"])
                if to is None:
                    continue
                flags = (1 if e.get("isFerry") else 0) | (2 if e.get("isOneLaneRoad") else 0)
                dist, dur = float(e["distance"]), float(e["duration"])
                if not e.get("isFerry") and dur > 0:
                    max_speed = max(max_speed, dist / dur)
                edges.append((to * 2 + DIRS[e["direction"]], dist, dur, flags))
            offsets.append(len(edges))
    print(f"edges: {len(edges)}, max road speed {max_speed * 3.6:.0f} km/h (map units)")

    areas = []
    for uid, sa in data.get("serviceAreas", []):
        flags = 0
        for f in sa["facilities"]:
            flags |= AREA_FLAGS.get(f, 0)
        if flags and uid in index:
            areas.append((index[uid], flags))
    print(f"service areas: {len(areas)} "
          f"(fuel {sum(1 for _, f in areas if f & 1)}, parking {sum(1 for _, f in areas if f & 2)})")

    # Companies: routing target node. A company node missing from the graph falls back to the
    # nearest graph node (companies are prefab entrances, always right next to a road).
    graph_nodes = [u for u in uids if u in graph]
    grid = {}
    for u in graph_nodes:
        x, z = pos[u]
        grid.setdefault((int(x // 500), int(z // 500)), []).append(u)

    def nearest(x, z):
        best, bd = None, 1e18
        cx, cz = int(x // 500), int(z // 500)
        for gx in range(cx - 1, cx + 2):
            for gz in range(cz - 1, cz + 2):
                for u in grid.get((gx, gz), []):
                    d = math.hypot(pos[u][0] - x, pos[u][1] - z)
                    if d < bd:
                        best, bd = u, d
        return best, bd

    strings = bytearray()
    companies = []
    fallback = 0
    for c in json.loads((map_out / "europe-companies.json").read_text(encoding="utf-8")):
        node = c.get("nodeUid")
        if node not in graph:
            node, d = nearest(c["x"], c["y"])
            if node is None or d > 300:
                continue
            fallback += 1
        companies.append((index[node], len(strings)))
        strings += f'{c["cityToken"]}.{c["token"]}'.encode("utf-8") + b"\0"
    print(f"companies: {len(companies)} ({fallback} snapped to the nearest road node)")

    countries = []
    for c in json.loads((map_out / "europe-countries.json").read_text(encoding="utf-8")):
        limits = c.get("truckSpeedLimits", {})
        lim = lambda k: int((limits.get(k) or {}).get("limit", 0)) & 0xFF
        countries.append((c["id"], len(strings), lim("motorway"), lim("expressway"), lim("localRoad")))
        strings += c["name"].encode("utf-8") + b"\0"
    print(f"countries: {len(countries)}; nodes with a country: {sum(1 for u in uids if any(node_country[u]))}")

    with OUT.open("wb") as f:
        f.write(struct.pack("<4s7If", b"ECRG", 2, len(uids), len(edges), len(areas), len(companies),
                            len(countries), len(strings), max_speed))
        for u in uids:
            f.write(struct.pack("<2f", *pos[u]))
        f.write(struct.pack(f"<{len(offsets)}I", *offsets))
        for e in edges:
            f.write(struct.pack("<IffI", *e))
        for a in areas:
            f.write(struct.pack("<2I", *a))
        for c in companies:
            f.write(struct.pack("<2I", *c))
        for c in countries:
            f.write(struct.pack("<2I4B", c[0], c[1], c[2], c[3], c[4], 0))
        for u in uids:
            f.write(struct.pack("<2B", *node_country[u]))
        f.write(strings)
    print(f"wrote {OUT} ({OUT.stat().st_size / 1e6:.1f} MB)")

    if args.maneuvers:
        # Only junction passages the route engine can actually take: an edge from -> to must exist.
        edge_pairs = set()
        for u in uids:
            nb = graph.get(u, {})
            for d in DIRS:
                for e in nb.get(d, []):
                    if e["nodeUid"] in index:
                        edge_pairs.add((index[u], index[e["nodeUid"]]))
        records = []
        with open(args.maneuvers, encoding="utf-8") as f:
            for row in csv.DictReader(f):
                turn = int(row["dir"])
                if turn == 0:  # straight through: nothing to announce
                    continue
                a, b = index.get(row["start"]), index.get(row["end"])
                if a is None or b is None or (a, b) not in edge_pairs:
                    continue
                records.append((a, b, turn, int(row["exit_no"]) & 0xFF, int(row["flags"]) & 0xFF,
                                int(row["lanes"]) & 0xFF, int(row["mask"]) & 0xFFFF, int(row["choices"]) & 0xFF))
        records.sort()
        with OUT_MANEUVERS.open("wb") as f:
            f.write(struct.pack("<4s2I", b"ECMV", 1, len(records)))
            for r in records:
                f.write(struct.pack("<2I4BH2B", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], 0))
        print(f"wrote {OUT_MANEUVERS}: {len(records)} junction maneuvers")


if __name__ == "__main__":
    main()
