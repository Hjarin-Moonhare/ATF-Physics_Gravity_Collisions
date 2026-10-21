"""Test cases for the plugin's collisions (tests/collide_test.cpp): the skeleton in a few poses, as the game holds it
(local translation, world rotation rows = axes, world position), and the pushes the design gives
(A-Body's p3_collision.py rules, sizes from sizes.json, soft size x1.1, per-group caps, the bones of one
part coupled as the plugin's CouplePushes does).
"""
import json
import os
import sys

import numpy as np

AUDIT_TOOLS = os.environ.get("ABODY_TOOLS", "abody_tools")
sys.path.insert(0, AUDIT_TOOLS)
import v06  # noqa: E402
import poses  # noqa: E402
import p3_collision as pc  # noqa: E402
from p3_spring_envelope import SKEL  # noqa: E402

SCALE = 1.1
POSES = ("rest", "arms_forward", "arms_crossed", "sitting", "deep_squat", "sprint_stride", "arms_sides", "arms_hanging",
         "hands_on_glutes", "hands_on_glutes_firm")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "cases")
os.makedirs(OUT, exist_ok=True)

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import natural_rest  # noqa: E402

sizes, rest_d = natural_rest.rest_distances()   # the same distances as the plugin's header
SOFT, CAP = natural_rest.SOFT, natural_rest.MAX_PUSH   # p3_collision's groups + the glutes, and their caps
seg_r, soft_r = sizes["collider_radius_m"], sizes["soft_radius_m"]
skel = v06.load_skeleton(SKEL)
base = v06.bind_rest(skel, v06.load_vanilla_body())
soft = []
for g, (bones, _) in SOFT.items():
    for b in bones:
        soft += [(g, b.format(s=s), s) for s in "LR"] if "{s}" in b else [(g, b, None)]


def pushes(world):
    out = {}
    for g, n, s in soft:
        c = world[n][:3, 3]
        r = soft_r[n] * SCALE
        total = np.zeros(3)
        for name, (a, b, _) in pc.SEGMENTS.items():
            rule = SOFT[g][1].get(name)
            for cs in "LR":
                if rule is None or not (rule == "B" or s is None or (rule == "S") == (s == cs)):
                    continue
                P = pc.closest_on_segment(*natural_rest.segment(world, name, cs), c)   # trimmed, as the plugin
                d = np.linalg.norm(c - P)
                contact = min(r + seg_r[name], rest_d[f"{n}|{name}|{cs}"])
                if 1e-6 < d < contact:
                    total += (c - P) / d * (contact - d)
        for g2, m, s2 in soft:
            key = f"{n}|{m}"
            if key in rest_d:
                P = world[m][:3, 3]
                d = np.linalg.norm(c - P)
                contact = min(r + soft_r[m] * SCALE, rest_d[key])
                if 1e-6 < d < contact:
                    total += (c - P) / d * (contact - d) * 0.5
        L = np.linalg.norm(total)
        out[n] = total * (CAP[g] / L) if L > CAP[g] else total
    return couple(out)


def couple(out):
    """The bones of one part push as one soft mass (the plugin's CouplePushes): no two differ by more than the
    part's spread.  A bone with no contact of its own follows the pushed ones; of two pushed bones, the one the move
    carries further along its own push follows the other; when it would carry both, or take both back against their
    pushes (a squeeze), they give way evenly."""
    contact = {n: np.linalg.norm(p) >= 1e-4 for n, p in out.items()}
    for g in SOFT:
        for side in (None, "L", "R"):
            m = [n for g2, n, s in soft if g2 == g and s == side]
            if len(m) < 2:
                continue
            S = natural_rest.PUSH_SPREAD[g]
            for _ in range(4):
                for a in range(len(m)):
                    for b in range(a + 1, len(m)):
                        i, j = m[a], m[b]
                        d = out[i] - out[j]
                        L = np.linalg.norm(d)
                        if L <= S:
                            continue
                        corr = d * ((L - S) / L)
                        wi = 0.5
                        if contact[i] != contact[j]:
                            wi = 0.0 if contact[i] else 1.0
                        elif contact[i]:
                            ci, cj = float(d @ out[i]) <= 0.0, float(d @ out[j]) >= 0.0
                            if ci != cj:
                                wi = 1.0 if ci else 0.0
                        out[i] = out[i] - corr * wi
                        out[j] = out[j] + corr * (1.0 - wi)
            for n in m:
                L = np.linalg.norm(out[n])
                if L > CAP[g]:
                    out[n] = out[n] * (CAP[g] / L)
    return out


def token(name):   # one whitespace-free word per name (camera nodes have spaces; none of them collide)
    return name.replace(" ", "_") or "<empty>"


P = poses.build(skel)
# arms lowered 32 deg (as the gravity design tools test), and hanging at the sides as in a standing idle
P["arms_sides"] = [v06.toward(skel, f"{s}_Biceps", f"{s}_Forearm", (0, 0, -1), 32) for s in "LR"]
P["arms_hanging"] = [op for s in "LR" for op in (
    v06.toward(skel, f"{s}_Biceps", f"{s}_Forearm", (0, 0, -1), natural_rest.angle_from_down(base, f"{s}_Biceps", f"{s}_Forearm") - 6),
    v06.toward(skel, f"{s}_Forearm", f"{s}_Wrist", (0, 1, 0), 10))]
P["hands_on_glutes"] = natural_rest.hands_on_glutes(skel, base, 30, 20)        # the glute group
P["hands_on_glutes_firm"] = natural_rest.hands_on_glutes(skel, base, 30, 28)
for pname in POSES:
    world = base if pname == "rest" else v06.pose(skel, P[pname], base)
    lines = []
    for name in world:
        W = world[name]
        par = skel[name]["parent"]
        if par in world:
            Wp = world[par]
            local = Wp[:3, :3].T @ (W[:3, 3] - Wp[:3, 3])
        else:
            par, local = "-", W[:3, 3]
        rows = [W[:3, k] for k in range(3)]   # the game's rows are the axes
        nums = list(local) + [x for r in rows for x in r] + list(W[:3, 3])
        lines.append(f"N {token(name)} {token(par)} " + " ".join(f"{x:.7f}" for x in nums))
    for n, p in pushes(world).items():
        lines.append(f"E {n} " + " ".join(f"{x:.7f}" for x in p))
    open(os.path.join(OUT, f"{pname}.txt"), "w").write("\n".join(lines) + "\n")
    big = max(np.linalg.norm(p) for p in pushes(world).values())
    print(f"{pname:14s} {len(world)} nodes, largest designed push {big * 100:.2f} cm")
