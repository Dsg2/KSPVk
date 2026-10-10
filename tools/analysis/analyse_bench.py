"""Summarise a KSPVk.Probe bench folder into Markdown tables (render-share breakdown per scene).

usage: python analyse_bench.py <bench dir> [out.md]
"""
import json, os, sys

def load(path):
    pairs = json.load(open(path, encoding="utf-8"), object_pairs_hook=lambda p: p)
    d = {}
    for k, v in pairs:
        if k == "cameras" and "camera_timing" not in d and v and dict(v[0]).get("cpu_ms_per_frame") is not None:
            k = "camera_timing"  # pre-rename builds wrote timing and scene dump under the same key
        d.setdefault(k, v)
    return d

def dd(v): return [dict(x) for x in v] if isinstance(v, list) else dict(v)

def main():
    root = sys.argv[1]
    steps = []
    order = [l.split()[0] for l in open(os.path.join(root, "summary.txt")) if l.strip() and not l.startswith(("step", "#"))]
    for name in order:
        p = os.path.join(root, name + ".json")
        if os.path.exists(p): steps.append((name, load(p)))
    out = ["| scene | fps | frame ms (p50/p95) | render CPU ms | scripts ms | physics ms | render share of CPU frame | GPU busy ms (span) | GPU-bound? | visible slots / renderers | tris visible |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    cams = []
    for name, j in steps:
        c = dict(j["categories_ms_per_frame"])
        fm = j["frame_ms_mean"]; rc = c.get("render_cpu", 0); gs = j.get("gpu_span_ms_mean")
        rt = dict(j.get("renderer_totals", []))
        tris = sum(dict(r).get("visible_tris", 0) for r in j.get("renderers_by_type", []))
        out.append(f"| {name} | {1000/fm:.0f} | {j['frame_ms_p50']:.2f} / {j['frame_ms_p95']:.2f} | {rc:.2f} | "
                   f"{c.get('scripts',0)+c.get('scripts_fixed',0):.2f} | {c.get('physics',0):.2f} | {100*rc/fm:.0f}% | "
                   f"{gs:.2f} | {'yes' if gs and gs > 0.9*fm else 'no'} | {rt.get('visible_material_slots','?')} / {rt.get('renderers','?')} | {tris/1e3:.0f}k |")
        for ct in dd(j.get("camera_timing", [])):
            cams.append((name, ct))
    out += ["", "| scene | camera | CPU ms/frame | GPU-timeline ms/frame |", "|---|---|---|---|"]
    out += [f"| {s} | {c['name']} | {c['cpu_ms_per_frame']:.2f} | {c['gpu_timeline_ms_per_frame']:.2f} |" for s, c in cams if c["cpu_ms_per_frame"] >= 0.05]
    out += ["", "| scene | top shaders by visible slots (count) |", "|---|---|"]
    for name, j in steps:
        sh = sorted(dd(j.get("renderers_by_shader", [])), key=lambda r: -r.get("visible", 0))[:6]
        out.append(f"| {name} | " + ", ".join(f"{r['key']} ({r.get('visible',0)})" for r in sh) + " |")
    text = "\n".join(out) + "\n"
    if len(sys.argv) > 2: open(sys.argv[2], "w", encoding="utf-8").write(text)
    print(text)

main()
