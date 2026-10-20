"""Glute soft bones for the plugin's collisions: sizes and bind-pose distances, measured exactly as A-Body's
p3_collision.py measured the breast, belly and inner-thigh bones (the same mesh, the same rules), but written to their
own file (tools/data/sizes_glute.json), so sizes.json stays as it was measured.

* radius: how far the bone's own skin reaches (75th percentile of the distance from the bone to the vertices it
  dominates), as for the other soft bones; Upper and Outer dominate only 4-7 vertices, so theirs come from every
  vertex they move by a quarter or more;
* bind-pose distance to every collider (natural_rest.py recomputes them against the trimmed capsules and lowers them to
  the closest natural standing pose) and to the other side's four glute bones (soft against soft, half each).
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.environ.get("ABODY_TOOLS", "abody_tools"))
import v06  # noqa: E402
import p3_collision as pc  # noqa: E402
from p3_spring_envelope import Envelope  # noqa: E402

PARTS = ("Upper", "Lower", "Inner", "Outer")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "sizes_glute.json")


def main():
    env = Envelope()
    body, base, Wd = env.body, env.base, env.Wd
    W0 = v06.skin(body, base)
    dom = np.argmax(Wd, axis=1)
    idx = {b: j for j, b in enumerate(body.bones)}
    bones = [f"TF_Glute_{p}_{s}" for p in PARTS for s in "LR"]
    radius, rest, sample = {}, {}, {}
    for n in bones:
        sel = dom == idx[n]
        d = np.linalg.norm(W0[sel] - base[n][:3, 3], axis=1)
        wide = Wd[:, idx[n]] >= 0.25   # every vertex it moves by a quarter or more
        dw = np.linalg.norm(W0[wide] - base[n][:3, 3], axis=1)
        # Upper and Outer dominate only 4-7 vertices each: too few for a percentile, so those take the wider sample
        if sel.sum() >= 20:
            radius[n], sample[n] = float(np.percentile(d, 75)), f"{sel.sum()} dominated"
        else:
            radius[n], sample[n] = float(np.percentile(dw, 75)), f"{wide.sum()} at weight >= 0.25"
        print(f"{n:18s} {sel.sum():3d} vertices dominated (p75 {np.percentile(d, 75) * 100:4.1f} cm), "
              f"{wide.sum():3d} at weight >= 0.25 (p75 {np.percentile(dw, 75) * 100:4.1f} cm) -> radius {radius[n] * 100:4.1f} cm")
        c = base[n][:3, 3]
        for name, (a, b, _) in pc.SEGMENTS.items():
            for cs in "LR":
                A, B = base[f"{cs}_{a}"][:3, 3], base[f"{cs}_{b}"][:3, 3]
                rest[f"{n}|{name}|{cs}"] = float(np.linalg.norm(c - pc.closest_on_segment(A, B, c)))
        s = n[-1]
        for p in PARTS:
            m = f"TF_Glute_{p}_{'R' if s == 'L' else 'L'}"
            rest[f"{n}|{m}"] = float(np.linalg.norm(c - base[m][:3, 3]))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    json.dump({"soft_radius_m": radius, "rest_distance_m": rest, "radius_sample": sample,
               "note": "glute soft bones, measured like p3_collision.py (v0.7 mesh at rest); tools/glute_sizes.py"},
              open(OUT, "w"), indent=1)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
