"""Test cases for the plugin's dents (tests/dent_test.cpp): a body's dent rig (tests/data/DentRig_<rig>.bin, copied
from A-Body's build) and, per pose, every rig bone's world transform (the collision cases' skeleton with the designed
pushes on the soft bones, tests/export_collision_cases.py) with the dents the design model gives (tools/dent_model.py):
per region its dent bones' moves (D) and, when the region tilts, their tilts (W, 3 per dent bone, parent frame).
Usage: python tests/export_dent_cases.py [rig dir A] [rig dir B] [rig dir C]
"""
import os
import shutil
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))
import export_collision_cases as ecc  # noqa: E402
import dent_model  # noqa: E402

ABODY_BUILDS = os.environ.get("ABODY_BUILDS", "abody_builds")
RIGS = {"A": os.path.join(ABODY_BUILDS, "A-Body_DentRig_A"), "B": os.path.join(ABODY_BUILDS, "A-Body_DentRig_B"),
        "C": os.path.join(ABODY_BUILDS, "A-Body_DentRig_C")}
POSES = ("rest", "arms_crossed", "arms_forward", "hands_on_glutes", "hands_on_glutes_firm", "sitting", "folded",
         "clasped", "across") + ecc.MOD_POSES
PRIVATE = set(ecc.MOD_POSES)   # made from another author's pose mod: kept in the development tree only
DATA = os.path.join(HERE, "data")
OUT = os.path.join(HERE, "cases_dent")
MORPHS = {"": {}, "_large": {"TF_BreastsLarge": 0.5, "TF_ButtLarge": 0.5, "TF_ThighsLarge": 0.5}}


def pushed(world):
    """The skeleton with the designed pushes: each soft bone and everything under it moved by its push."""
    p = ecc.pushes(world)
    out = {k: v.copy() for k, v in world.items()}
    kids = {}
    for n in world:
        kids.setdefault(ecc.skel[n]["parent"], []).append(n)
    for n, d in p.items():
        stack = [n]
        while stack:
            k = stack.pop()
            out[k][:3, 3] += d
            stack += kids.get(k, [])
    return out


def xf_line(i, name, M):
    """B <index> <name> <have> ax0 ax1 ax2 (the columns, unit) pos scale"""
    if M is None:
        return f"B {i} {name} 0"
    s = float(np.linalg.norm(M[:3, 0]))
    ax = [M[:3, k] / s for k in range(3)]
    nums = [x for a in ax for x in a] + list(M[:3, 3]) + [s]
    return f"B {i} {name} 1 " + " ".join(f"{x:.8f}" for x in nums)


def main():
    for tag, arg in zip("ABC", sys.argv[1:]):
        RIGS[tag] = arg
    os.makedirs(DATA, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    for tag, d in RIGS.items():
        src = os.path.join(d, "DentRig.bin")
        dst = os.path.join(DATA, f"DentRig_{tag}.bin")
        shutil.copyfile(src, dst)
        rig = dent_model.load(dst)
        for pname in POSES:
            world = pushed(ecc.world_of(pname))
            for mtag, morph in MORPHS.items():
                lines = [f"rig {tag}", "scale 1"] + [f"morph {k} {v}" for k, v in morph.items()]
                for i, name in enumerate(rig["bones"]):
                    lines.append(xf_line(i, name, world.get(name)))
                st_all = {}
                for R in rig["regions"]:
                    st = {}
                    D, W = dent_model.solve_region2(rig, R, world, morph, 1.0, st)
                    st_all[R["name"]] = st
                    lines.append(f"D {R['name']} {len(D)} " + " ".join(f"{x:.8f}" for x in D))
                    if R["rflags"] & 1:
                        lines.append(f"W {R['name']} {len(D)} " + " ".join(f"{x:.8f}" for x in W.reshape(-1)))
                name = f"{tag}_{pname}{mtag}.txt"
                open(os.path.join(OUT, name), "w").write("\n".join(lines) + "\n")
                deep = max([s.get("dent_cm", 0.0) for s in st_all.values()] or [0.0])
                print(f"{name:34s} contacts {sum(s.get('contacts', 0) for s in st_all.values()):4d}, deepest dent {deep:.2f} cm",
                      flush=True)


if __name__ == "__main__":
    main()
