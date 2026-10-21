"""The plugin's breast lift (Collide's LiftBreasts) as the design model, on tools/data/lift_data.json (written by
gen_lift_data.py): the same samples, shapes and steps, so a test can hold the plugin to it.

Per side: each breast skin sample sits at C_Chest x (rest + the sliders' changes) x the body's scale, plus the Pecs
anchor's move x its anchor weight and each TF_Breast bone's move (its offset from its rest, with the sideways and
fore-aft part of its collision push) x its weight.  Along the chest's up axis, a sample inside a lifter (a forearm, a
palm bone, a finger or thumb joint; its radius + MARGIN) with the limb below it (the way up out is the shorter) needs
that lift; a forearm, or an upper arm within ~20 degrees of level, above a sample caps how far it may rise (0 when it
already presses on it from above).  The four bones' lifts h are the least ones meeting every need and cap (projections,
ITERS rounds), each within the breast cap and any two within the breast spread.  A bone with a lift gets
max(lift, its push's own rise) as its vertical push, the others keep theirs.
"""
import json
import os

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = json.load(open(os.path.join(HERE, "data", "lift_data.json")))
MARGIN = 0.003
HMAX = 0.25      # how far a vertex inside a limb looks for the way out (m)
STEPS = 16       # golden-section and bisection steps (0.004 mm over 25 cm)
ITERS = 80
RAISED = 0.35
LIFTERS = ("forearm", "palm", "index", "middle", "ring", "pinky", "thumb")


def seg_dist(q, A, B):
    AB = B - A
    t = min(max(float((q - A) @ AB) / max(float(AB @ AB), 1e-12), 0.0), 1.0)
    return float(np.linalg.norm(q - (A + t * AB)))


def exits(v, u, A, B, rr, reach):
    """Along v + h u: (inside now, the shortest h > 0 out going up, going down) for a vertex inside the capsule;
    for one outside, (False, the first h in (0, reach] that enters it or inf, None).  The distance from the line to
    the capsule's axis segment is convex in h: golden-section for its least value, then bisection for the edges."""
    f = lambda h: seg_dist(v + h * u, A, B) - rr  # noqa: E731
    if f(0.0) < 0:
        def edge(lo, hi):   # f(lo) < 0 <= f(hi)
            for _ in range(STEPS):
                mid = (lo + hi) / 2
                if f(mid) < 0:
                    lo = mid
                else:
                    hi = mid
            return hi
        up = edge(0.0, HMAX) if f(HMAX) >= 0 else HMAX
        dn = -edge(0.0, -HMAX) if f(-HMAX) >= 0 else HMAX
        return True, up, dn
    a, b = 0.0, reach
    g = (np.sqrt(5) - 1) / 2
    c, d = b - g * (b - a), a + g * (b - a)
    fc, fd = f(c), f(d)
    for _ in range(STEPS):
        if fc < fd:
            b, d, fd = d, c, fc
            c = b - g * (b - a)
            fc = f(c)
        else:
            a, c, fc = c, d, fd
            d = a + g * (b - a)
            fd = f(d)
    hm = (a + b) / 2
    if f(hm) >= 0:
        return False, np.inf, None
    lo, hi = 0.0, hm   # f(lo) >= 0 > f(hi)
    for _ in range(STEPS):
        mid = (lo + hi) / 2
        if f(mid) < 0:
            hi = mid
        else:
            lo = mid
    return False, hi, None


def limb_segments(world, side, scale=1.0):
    """[(name, A, B, radius)] of one arm, as the plugin builds them from the bone positions."""
    out = []
    for c in DATA["limbs"]:
        A = world[f"{side}_{c['a']}"][:3, 3]
        if c["b"]:
            B = world[f"{side}_{c['b']}"][:3, 3]
        else:
            P = world[f"{side}_{c['parent']}"][:3, 3]
            u = A - P
            B = A + u / max(np.linalg.norm(u), 1e-9) * c["tip"] * scale
        if c["trim"]:
            A = A + (B - A) * c["trim"]
        out.append((c["name"], A, B, c["radius"][0 if side == "L" else 1] * scale))
    return out


def lift(world, push, sliders, size=1.0, scale=1.0, cap=0.05, spread=0.02):
    """push: {bone: world push} of the soft bones (the coupled sphere pushes); returns them with the breasts lifted.
    world: bone -> 4x4 (with the soft bones where the frame has them); sliders: {morph name: weight}."""
    out = dict(push)
    C = world["C_Chest"]
    Rc, pc = C[:3, :3], C[:3, 3]
    up = Rc @ np.array(DATA["up"])
    up /= np.linalg.norm(up)
    mw = np.array([sliders.get(m, 0.0) for m in DATA["morphs"]])
    segs = [(s, *x) for s in "LR" for x in limb_segments(world, s, scale)]
    cap_b, spread_b = cap * size * scale, spread * size * scale
    for s in "LR":
        side = DATA["sides"][s]
        bones = [f"TF_Breast_{p}_{s}" for p in DATA["parts"]]
        rest = [pc + scale * (Rc @ np.array(r)) for r in side["bone_rest"]]
        d_b = [world[b][:3, 3] - r for b, r in zip(bones, rest)]
        d_p = world[f"{s}_Pecs"][:3, 3] - (pc + scale * (Rc @ np.array(side["pecs_rest"])))
        pb = [out.get(b, np.zeros(3)) for b in bones]
        horiz = [p - (p @ up) * up for p in pb]
        base_h = [float(p @ up) for p in pb]
        smp = side["samples"]
        P = np.array([x["p"] for x in smp]) + np.einsum("m,kmj->kj", mw, np.array([x["d"] for x in smp]))
        Wv = np.array([x["w"] for x in smp])
        wp = np.array([x["wp"] for x in smp])
        V = pc + scale * (P @ Rc.T) + wp[:, None] * d_p + sum(Wv[:, [a]] * (d_b[a] + horiz[a]) for a in range(4))
        lo = V.min(0)
        hi = V.max(0)
        needs, ceil = [], np.full(len(V), np.inf)
        for side_c, name, A, B, R in segs:
            rr = R + MARGIN * scale
            if np.any(np.minimum(A, B) - rr > hi + cap_b) or np.any(np.maximum(A, B) + rr < lo - cap_b):
                continue   # nowhere near this breast
            axis = (B - A) / max(np.linalg.norm(B - A), 1e-9)
            lifter = name.startswith(LIFTERS)
            holds = name == "forearm" or (name == "upperarm" and abs(float(axis @ up)) < RAISED)
            if not (lifter or holds):
                continue
            for k in range(len(V)):
                dk = seg_dist(V[k], A, B)
                if dk >= rr + (cap_b if holds else 0.0):   # only a holder looks further: as far as a lift can go
                    continue
                inside, a, b = exits(V[k], up, A, B, rr, cap_b)
                if inside:
                    if a <= b:
                        if lifter:
                            needs.append((k, a))
                    elif holds:
                        ceil[k] = 0.0
                elif holds and a < ceil[k]:
                    ceil[k] = a
        h = np.zeros(4)
        if needs:
            capped = np.flatnonzero(np.isfinite(ceil))
            for _ in range(ITERS):
                for k, need in needs:
                    w = Wv[k]
                    cur = float(w @ h)
                    if cur < need:
                        h += w * ((need - cur) / float(w @ w))
                for k in capped:
                    w = Wv[k]
                    cur = float(w @ h)
                    if cur > ceil[k]:
                        h += w * ((ceil[k] - cur) / float(w @ w))
                h = np.clip(h, -cap_b, cap_b)
                for a in range(4):
                    for b in range(a + 1, 4):
                        dd = h[a] - h[b]
                        if abs(dd) > spread_b:
                            corr = (abs(dd) - spread_b) / 2 * np.sign(dd)
                            h[a] -= corr
                            h[b] += corr
        for a, b in enumerate(bones):
            hb = max(h[a], base_h[a]) if h[a] > 1e-6 else base_h[a]
            out[b] = horiz[a] + hb * up
    return out
