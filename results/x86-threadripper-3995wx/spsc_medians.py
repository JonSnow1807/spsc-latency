"""Median-of-3 table from spsc_bench raw output. Values are printed exactly as
they appear in the raw files (M items/s from the bench's summary.md per-trial
table, latencies from measurements.csv); medians are selections, no arithmetic."""
import csv, re, sys
d = sys.argv[1]
rows = list(csv.DictReader(open(f"{d}/measurements.csv")))
# M items/s strings as printed by the bench in summary.md
mps = {}
for line in open(f"{d}/summary.md"):
    m = re.match(r"\| (\w+) \| (\d+) \| (\d+) \| (\d+) \| ([0-9.]+) \|", line)
    if m:
        mps[m.groups()[:4]] = m.group(5)
assert len(mps) == len(rows), (len(mps), len(rows))
def key(r): return (r["implementation"], r["capacity"], r["element_bytes"], r["trial"])
for r in rows:
    # cross-check summary.md string against csv items_per_second
    assert abs(float(mps[key(r)]) - float(r["items_per_second"]) / 1e6) < 0.0006, key(r)
    r["mps"] = mps[key(r)]
order = ["naive", "acqrel", "padded", "cached"]
caps = sorted({int(r["capacity"]) for r in rows})
cols = [("mps", "M items/s"), ("p50_ns", "p50 ns"), ("p99_ns", "p99 ns"), ("p999_ns", "p99.9 ns")]
def med(vals):
    s = sorted(vals, key=float); assert len(s) == 3; return s[1]
print("| Design | Capacity | Bytes | " + " | ".join(f"median {c[1]}" for c in cols) + " | " + " | ".join(f"trials {c[1]} (t1, t2, t3)" for c in cols) + " |")
print("|---|---:|---:|" + "---:|" * len(cols) + "---|" * len(cols))
for b in ["8", "64"]:
    for c in caps:
        for impl in order:
            g = sorted([r for r in rows if r["implementation"] == impl and r["capacity"] == str(c) and r["element_bytes"] == b], key=lambda r: int(r["trial"]))
            assert [r["trial"] for r in g] == ["1", "2", "3"], (impl, c, b)
            meds = [med([r[k] for r in g]) for k, _ in cols]
            trials = [", ".join(r[k] for r in g) for k, _ in cols]
            print(f"| {impl} | {c} | {b} | " + " | ".join(meds) + " | " + " | ".join(trials) + " |")
