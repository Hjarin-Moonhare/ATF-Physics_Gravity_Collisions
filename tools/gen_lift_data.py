"""Writes src/lift_data.h (and tools/data/lift_data.json, the same numbers for the design model): what the plugin needs
to lift a breast over the limbs below it.
  Breast skin samples, per side: the skin of the body mesh the collisions were sized on (A-Body 1.5.5's breast skin
    lies on it to 0.01 mm, the breast sliders too) whose breast bones' summed weight is at least 0.1, one sample
    per 1 cm cell; each with its rest position in C_Chest's frame, its position change per unit of the sliders that
    move breast skin (the body triangle and the TF breast sliders), and its weights for the four TF_Breast bones (each
    with its children: the nipple goes with Lower) and the Pecs anchor.
  The four bones' and the anchor's rest positions in C_Chest's frame, and the chest's up axis in its own frame.
  Tight limb shapes (tools/limb_shapes.py): the upper arm (from half way down), the forearm, the palm's four bones and
    every finger joint to the fingertip, each with the radius that holds 95% of the skin nearest to it (the body's arms,
    the vanilla hands)."""
import json
import os
import sys

import numpy as np

AUDIT_TOOLS = os.environ.get("ABODY_TOOLS", "abody_tools")
sys.path.insert(0, AUDIT_TOOLS)
import v06  # noqa: E402
import r1_verify as rv  # noqa: E402
from sfbody import mdat_io  # noqa: E402
from p3_spring_envelope import Envelope  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import limb_shapes  # noqa: E402

ROOT = os.path.dirname(HERE)
MORPHS = ("Thin", "Strong", "Overweight", "TF_BreastsLarge", "TF_BreastsSmall", "TF_BreastsPerky", "TF_BreastsGravity",
          "TF_BreastsTogether", "TF_BreastsApart")
PARTS = ("Upper", "Lower", "Inner", "Outer")
WMIN = 0.1
CELL = 0.01
GROUP = 0.03   # samples are kept in groups of 3 cm cells, each with a bound, so a limb tests the groups first


class _Scene:   # what limb_shapes.LimbShapes reads
    def __init__(self, env):
        self.env, self.body, self.base = env, env.body, env.base
        self.hands = v06.load_hands()
        self.Pb = None


def main():
    env = Envelope()
    body, base = env.body, env.base
    _, md, _, _, _ = rv.load_build()
    idx = {b: j for j, b in enumerate(body.bones)}
    W0 = v06.skin(body, base)
    C = base["C_Chest"]
    Rc, pc = C[:3, :3], C[:3, 3]
    to_chest = lambda p: (p - pc) @ Rc   # world (rest) -> chest frame (rows of p)  # noqa: E731
    deltas = {}
    for m in MORPHS:
        P = mdat_io.apply_morphs(md, body.P, {m: 1.0}).astype(np.float64)
        deltas[m] = (v06.skin(body, base, P) - W0) @ Rc   # the change per unit of the slider, in the chest frame
    out = {"morphs": list(MORPHS), "parts": list(PARTS), "cell_m": CELL, "wmin": WMIN, "sides": {}}
    for s in "LR":
        bones = [f"TF_Breast_{p}_{s}" for p in PARTS]
        Wb = np.stack([env.Wd[:, [idx[k] for k in env.subtree(b) if k in idx]].sum(1) for b in bones], 1)
        wp = env.Wd[:, idx[f"{s}_Pecs"]]
        cand = np.flatnonzero(Wb.sum(1) >= WMIN)
        # one vertex per welded position, then one per 1 cm cell (the one nearest the cell's middle)
        _, first = np.unique(env.key[cand], return_index=True)
        cand = cand[np.sort(first)]
        P = to_chest(W0[cand])
        cell = np.floor(P / CELL).astype(np.int64)
        keys, inv = np.unique(cell, axis=0, return_inverse=True)
        mid = (keys + 0.5) * CELL
        pick = []
        for c in range(len(keys)):
            m = np.flatnonzero(inv.ravel() == c)
            pick.append(m[np.argmin(np.linalg.norm(P[m] - mid[c], axis=1))])
        sel = cand[np.array(sorted(pick))]
        Ps = to_chest(W0[sel])
        gk = np.floor(Ps / GROUP).astype(np.int64)
        order = np.lexsort((gk[:, 2], gk[:, 1], gk[:, 0]))   # the samples in group order
        sel, gk = sel[order], gk[order]
        side = {"samples": [], "bone_rest": [to_chest(base[b][:3, 3]).tolist() for b in bones],
                "pecs_rest": to_chest(base[f"{s}_Pecs"][:3, 3]).tolist()}
        for v in sel:
            side["samples"].append({"p": to_chest(W0[v]).round(6).tolist(),
                                    "d": [deltas[m][v].round(6).tolist() for m in MORPHS],
                                    "w": Wb[v].round(5).tolist(), "wp": round(float(wp[v]), 5)})
        # the groups: first sample, count, and what bounds them (from the rounded numbers, as the plugin has them)
        side["groups"] = []
        smp = side["samples"]
        _, starts = np.unique(gk, axis=0, return_index=True)
        starts = sorted(starts.tolist()) + [len(sel)]
        for a, b in zip(starts[:-1], starts[1:]):
            P = np.array([smp[k]["p"] for k in range(a, b)])
            D = np.array([smp[k]["d"] for k in range(a, b)])
            Wg = np.array([smp[k]["w"] for k in range(a, b)])
            wpg = np.array([smp[k]["wp"] for k in range(a, b)])
            c, dbar, wbar, wpbar = P.mean(0), D.mean(0), Wg.mean(0), float(wpg.mean())
            side["groups"].append({"first": a, "count": b - a, "c": c.round(6).tolist(), "d": dbar.round(6).tolist(),
                                   "dm": (np.linalg.norm(D - dbar, axis=2).max(0) + 1e-6).round(6).tolist(),
                                   "w": wbar.round(6).tolist(), "wp": round(wpbar, 6),
                                   "r0": round(float(np.linalg.norm(P - c, axis=1).max()) + 1e-6, 6),
                                   "dw": round(float((np.abs(Wg - wbar).sum(1) + np.abs(wpg - wpbar)).max()) + 1e-6, 6)})
        out["sides"][s] = side
        print(f"{s}: {len(cand)} breast skin vertices (welded) -> {len(sel)} samples in {len(side['groups'])} groups, "
              f"part weight median {np.median(Wb[sel].sum(1)):.2f}")
    out["up"] = (Rc.T @ np.array([0.0, 0.0, 1.0])).round(6).tolist()
    ls = limb_shapes.LimbShapes(_Scene(env))
    caps = []
    for c in limb_shapes.chains("L"):
        name, a, b, _ = c
        caps.append({"name": name, "a": a[2:], "b": b[2:] if b else None,
                     "parent": (limb_shapes._parent_of(a)[2:] if b is None else None),
                     "tip": round(ls.tip.get(("L", name), 0.0), 5),
                     "radius": [round(ls.radius[("L", name)], 5), round(ls.radius[("R", name)], 5)],
                     "trim": limb_shapes.UPPER_TRIM if name == "upperarm" else 0.0})
    out["limbs"] = caps
    json.dump(out, open(os.path.join(HERE, "data", "lift_data.json"), "w"), indent=1)

    # ---- the header
    hb = sorted({c["a"] for c in caps} | {c["b"] for c in caps if c["b"]} | {c["parent"] for c in caps if c["parent"]})
    L = ["// Generated by tools/gen_lift_data.py from A-Body's body and the vanilla hands; do not edit by hand.", "#pragma once",
         "", "// breast skin samples: rest position in C_Chest's frame (m), change per unit of each slider, weights",
         f"constexpr int kLiftMorphs = {len(MORPHS)};",
         "constexpr const char* kLiftMorphName[kLiftMorphs] = {" + ", ".join(f'"{m}"' for m in MORPHS) + "};",
         "struct LiftSample { float p[3]; float d[kLiftMorphs][3]; float w[4]; float wPecs; };",
         "// groups of samples (3 cm cells): their mean rest position, slider changes and weights, how far a member can be",
         "// from the mean (r0 + sum |slider| x dm + dw x the largest bone move), first sample and count",
         "struct LiftGroup { int first, count; float c[3]; float d[kLiftMorphs][3]; float dm[kLiftMorphs]; float w[4]; float wPecs; float r0, dw; };",
         "// the TF_Breast bones in the order the weights use (per side), and the chest's up axis in its own frame",
         "constexpr const char* kLiftPart[4] = {" + ", ".join(f'"{p}"' for p in PARTS) + "};",
         "constexpr float kLiftUp[3] = {" + ", ".join(f"{x:.6f}f" for x in out["up"]) + "};"]
    for s in "LR":
        side = out["sides"][s]
        L.append(f"constexpr float kLiftBoneRest{s}[4][3] = {{" + ", ".join("{" + ", ".join(f"{x:.6f}f" for x in p) + "}"
                                                                            for p in side["bone_rest"]) + "};")
        L.append(f"constexpr float kLiftPecsRest{s}[3] = {{" + ", ".join(f"{x:.6f}f" for x in side["pecs_rest"]) + "};")
        L.append(f"constexpr int kLiftSamples{s} = {len(side['samples'])};")
        L.append(f"constexpr LiftSample kLiftSample{s}[kLiftSamples{s}] = {{")
        for smp in side["samples"]:
            L.append("    {{" + ", ".join(f"{x:.6f}f" for x in smp["p"]) + "}, {" +
                     ", ".join("{" + ", ".join(f"{x:.6f}f" for x in d) + "}" for d in smp["d"]) + "}, {" +
                     ", ".join(f"{x:.5f}f" for x in smp["w"]) + f"}}, {smp['wp']:.5f}f}},")
        L.append("};")
        L.append(f"constexpr int kLiftGroups{s} = {len(side['groups'])};")
        L.append(f"constexpr LiftGroup kLiftGroup{s}[kLiftGroups{s}] = {{")
        for g in side["groups"]:
            L.append(f"    {{{g['first']}, {g['count']}, {{" + ", ".join(f"{x:.6f}f" for x in g["c"]) + "}, {" +
                     ", ".join("{" + ", ".join(f"{x:.6f}f" for x in d) + "}" for d in g["d"]) + "}, {" +
                     ", ".join(f"{x:.6f}f" for x in g["dm"]) + "}, {" + ", ".join(f"{x:.6f}f" for x in g["w"]) +
                     f"}}, {g['wp']:.6f}f, {g['r0']:.6f}f, {g['dw']:.6f}f}},")
        L.append("};")
    L += ["", "// tight limb shapes, per side: from bone a to bone b, or a tip: from a along parent -> a for tip (m)",
          f"constexpr int kLiftHandBones = {len(hb)};",
          "constexpr const char* kLiftHandBone[kLiftHandBones] = {" + ", ".join(f'"{b}"' for b in hb) + "};   // without the L_/R_",
          "struct LiftLimb { const char* name; int a; int b; int parent; float tip; float radius[2]; float trim; };",
          f"constexpr int kNumLiftLimbs = {len(caps)};", "constexpr LiftLimb kLiftLimb[kNumLiftLimbs] = {"]
    for c in caps:
        L.append(f'    {{"{c["name"]}", {hb.index(c["a"])}, {hb.index(c["b"]) if c["b"] else -1}, '
                 f'{hb.index(c["parent"]) if c["parent"] else -1}, {c["tip"]:.5f}f, {{{c["radius"][0]:.5f}f, '
                 f'{c["radius"][1]:.5f}f}}, {c["trim"]:.2f}f}},')
    L.append("};")
    p = os.path.join(ROOT, "src", "lift_data.h")
    open(p, "w").write("\n".join(L) + "\n")
    print("wrote", p, f"({os.path.getsize(p):,} bytes);", len(caps), "limb shapes per side;", len(hb), "arm and hand bones")


if __name__ == "__main__":
    main()
