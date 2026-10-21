"""The plugin's dents (Dents in plugin.cpp) as the design model, on a body's DentRig.bin (written by A-Body's
abody_dent_rig.py): the same samples, shapes and steps, so a test can hold the plugin to it.

A dent bone rides on a bone (its parent) and carries that bone's skin weight in its region, so it moves its skin
only by its own offset.  Per region, each frame:
  1. every sample's place: its rest (mesh space) with the body's sliders' changes, skinned by its bones (world x
     mesh-to-bone), its normal turned by its strongest bone;
  2. the arm, hand and leg shapes the region uses (capsules between bone origins, a tip along its parent's line,
     the start cut off) near the region's box;
  3. a sample inside a shape (radius + margin, not flagged, its dent share at least 0.05) needs the inward move e
     along its normal that takes it out (bisection, at most E_MAX);
  4. the wanted dent t = the largest e x bump(d / FALL) of the samples in contact within FALL (at least e on a
     contact), smoothed TSM rounds over the samples' neighbours (contacts kept at their e);
  5. the dent bones under the wanted dent and one ring of their lattice neighbours (the rest stay at 0) take inward
     moves D (along each one's inward line, turned by its parent) that minimise |G D - t|^2 + LAM_S |L D|^2 (G = a
     sample's share of a bone x the cosine of its normal and the bone's line, L the lattice's weighted edges), each
     within [0, D_MAX] (projected Gauss-Seidel, at most SWEEPS, until no move changes by SETTLED); then a sample dented
     more than its wanted dent + 2 mm weighs x4, a contact left short by more than 1 mm x2, and again (ROUNDS);
  6. each dent bone's local translation = its rest + D x its inward line (parent frame).
"""
import struct

import numpy as np

SWEEPS = 60
ROUNDS = 4
SETTLED = 1e-7      # a sweep that changes no move by more than this (m) ends the sweeps
BISECT = 18
NEIGHBOUR = 0.015   # samples closer than this are neighbours for the smoothing


def _str(b, o):
    n = struct.unpack_from("<H", b, o)[0]
    return b[o + 2:o + 2 + n].decode("latin1"), o + 2 + n


def wendland(x):
    x = np.clip(x, 0.0, 1.0)
    return (1 - x) ** 4 * (4 * x + 1)


def load(path):
    b = open(path, "rb").read()
    assert b[:8] == b"TFDENT01", "not a dent rig file"
    o = 8
    rig = {"bones": [], "binds": {}, "morphs": [], "colliders": [], "regions": []}
    nb = struct.unpack_from("<I", b, o)[0]
    o += 4
    for _ in range(nb):
        name, o = _str(b, o)
        has = b[o]
        o += 1
        rig["bones"].append(name)
        if has:
            v = struct.unpack_from("<12f", b, o)
            o += 48
            M = np.eye(4)
            M[:3, :3] = np.array(v[:9]).reshape(3, 3)
            M[:3, 3] = v[9:12]
            rig["binds"][name] = M
    nm = struct.unpack_from("<I", b, o)[0]
    o += 4
    for _ in range(nm):
        name, o = _str(b, o)
        rig["morphs"].append(name)
    nc = struct.unpack_from("<I", b, o)[0]
    o += 4
    for _ in range(nc):
        name, o = _str(b, o)
        side, kind, a, bb, tp, tip, rad, start = struct.unpack_from("<BBHhhfff", b, o)
        o += struct.calcsize("<BBHhhfff")
        rig["colliders"].append(dict(name=name, side=chr(side), kind=kind, a=rig["bones"][a],
                                     b=rig["bones"][bb] if bb >= 0 else None, tip_parent=rig["bones"][tp] if tp >= 0 else None,
                                     tip=tip, radius=rad, start=start))
    nr = struct.unpack_from("<I", b, o)[0]
    o += 4
    for _ in range(nr):
        name, o = _str(b, o)
        kind, side = struct.unpack_from("<BB", b, o)
        o += 2
        margin, e_max, fall, d_max, lam_s, tsm, top = struct.unpack_from("<fffffIf", b, o)
        o += struct.calcsize("<fffffIf")
        R = dict(name=name, kind=kind, side=chr(side), margin=margin, e_max=e_max, fall=fall, d_max=d_max, lam_s=lam_s,
                 tsm=tsm, dent=[], edges=[], samples=[], use=[])
        nd = struct.unpack_from("<I", b, o)[0]
        o += 4
        for _ in range(nd):
            dn, o = _str(b, o)
            v = struct.unpack_from("<H3f3f", b, o)
            o += struct.calcsize("<H3f3f")
            R["dent"].append(dict(name=dn, parent=rig["bones"][v[0]], rest=np.array(v[1:4]), inward=np.array(v[4:7])))
        ne = struct.unpack_from("<I", b, o)[0]
        o += 4
        for _ in range(ne):
            R["edges"].append(struct.unpack_from("<HHf", b, o))
            o += 8
        ns = struct.unpack_from("<I", b, o)[0]
        o += 4
        P, N, share, flags, lbs, dw, mo = [], [], [], [], [], [], []
        for _ in range(ns):
            v = struct.unpack_from("<3f3ffB", b, o)
            o += struct.calcsize("<3f3ffB")
            P.append(v[0:3])
            N.append(v[3:6])
            share.append(v[6])
            flags.append(v[7])
            n1 = b[o]
            o += 1
            lbs.append([struct.unpack_from("<Hf", b, o + 6 * i) for i in range(n1)])
            o += 6 * n1
            n2 = b[o]
            o += 1
            dw.append([struct.unpack_from("<Hf", b, o + 6 * i) for i in range(n2)])
            o += 6 * n2
            n3 = b[o]
            o += 1
            mo.append([struct.unpack_from("<H3f", b, o + 14 * i) for i in range(n3)])
            o += 14 * n3
        nu = struct.unpack_from("<I", b, o)[0]
        o += 4
        R["use"] = list(struct.unpack_from(f"<{nu}H", b, o))
        o += 2 * nu
        R.update(P=np.array(P), N=np.array(N), share=np.array(share), flags=np.array(flags), lbs=lbs, dw=dw, mo=mo)
        # dense forms
        nd = len(R["dent"])
        C = np.zeros((ns, nd))
        for k, row in enumerate(dw):
            for j, c in row:
                C[k, j] = c
        R["C"] = C
        Lap = np.zeros((nd, nd))
        for i, j, w in R["edges"]:
            Lap[i, i] += w
            Lap[j, j] += w
            Lap[i, j] -= w
            Lap[j, i] -= w
        R["Lap"] = Lap
        d = np.linalg.norm(R["P"][:, None, :] - R["P"][None, :, :], axis=2)
        R["nbr"] = [np.flatnonzero((d[k] < NEIGHBOUR) & (np.arange(ns) != k)) for k in range(ns)]
        rig["regions"].append(R)
    assert o == len(b), (o, len(b))
    return rig


def collider_segments(rig, world):
    """(A, B, radius, collider) per collider, in world, from the bones' world transforms."""
    out = []
    for c in rig["colliders"]:
        if c["a"] not in world:
            continue
        A = world[c["a"]][:3, 3]
        if c["b"] is not None:
            if c["b"] not in world:
                continue
            Bp = world[c["b"]][:3, 3]
        else:
            if c["tip_parent"] not in world:
                continue
            u = A - world[c["tip_parent"]][:3, 3]
            u = u / max(np.linalg.norm(u), 1e-9)
            Bp = A + u * c["tip"]
        A = A + (Bp - A) * c["start"]
        out.append((A, Bp, c["radius"], c))
    return out


def seg_dist(Q, A, B):
    AB = B - A
    t = np.clip(((Q - A) @ AB) / max(float(AB @ AB), 1e-12), 0, 1)
    return np.linalg.norm(Q - (A + t[:, None] * AB), axis=1)


def place(rig, R, world, morph):
    """Every sample's world place and normal."""
    mi = {n: i for i, n in enumerate(rig["morphs"])}
    w_m = np.zeros(len(rig["morphs"]))
    for n, w in (morph or {}).items():
        if n in mi:
            w_m[mi[n]] = w
    X = np.zeros((len(R["P"]), 3))
    Nw = np.zeros((len(R["P"]), 3))
    for k in range(len(R["P"])):
        p = R["P"][k].copy()
        for mk, *dl in R["mo"][k]:
            if w_m[mk] != 0.0:
                p += w_m[mk] * np.array(dl)
        x = np.zeros(3)
        best, bw = None, -1.0
        for bi, w in R["lbs"][k]:
            name = rig["bones"][bi]
            T = world[name] @ rig["binds"][name]
            x += w * (T[:3, :3] @ p + T[:3, 3])
            if w > bw:
                best, bw = T, w
        X[k] = x
        n = best[:3, :3] @ R["N"][k]
        Nw[k] = n / max(np.linalg.norm(n), 1e-12)
    return X, Nw


def solve_region(rig, R, world, morph, scale=1.0, stats=None):
    """The dent bones' inward moves D (m) for one region; scale: the dents' size (e_max and d_max)."""
    nd = len(R["dent"])
    D = np.zeros(nd)
    segs = [collider_segments(rig, world)[i] for i in R["use"]] if R["use"] else []
    if not segs:
        return D
    X, Nw = place(rig, R, world, morph)
    lo, hi = X.min(0), X.max(0)
    near = [s for s in segs if not ((np.minimum(s[0], s[1]) - s[2] - R["margin"] > hi).any() or
                                    (np.maximum(s[0], s[1]) + s[2] + R["margin"] < lo).any())]
    if not near:
        return D
    e_max = R["e_max"] * scale
    e = np.zeros(len(X))
    ok = (R["flags"] & 1 == 0) & (R["share"] >= 0.05)
    for A, Bp, rad, c in near:
        rr = rad + R["margin"]
        ins = np.flatnonzero((seg_dist(X, A, Bp) < rr) & ok)
        if not len(ins):
            continue
        Q, n = X[ins], Nw[ins]
        lo_ = np.zeros(len(ins))
        hi_ = np.full(len(ins), e_max)
        deep = seg_dist(Q - hi_[:, None] * n, A, Bp) < rr
        for _ in range(BISECT):
            mid = (lo_ + hi_) / 2
            m = seg_dist(Q - mid[:, None] * n, A, Bp) < rr
            lo_ = np.where(m, mid, lo_)
            hi_ = np.where(m, hi_, mid)
        e[ins] = np.maximum(e[ins], np.where(deep, e_max, hi_))
    con = np.flatnonzero(e > 0)
    if stats is not None:
        stats["contacts"] = int(len(con))
    if not len(con):
        return D
    t = np.zeros(len(X))
    for c in con:
        d = np.linalg.norm(X - X[c], axis=1)
        t = np.maximum(t, e[c] * wendland(d / R["fall"]))
    t[con] = np.maximum(t[con], e[con])
    floor = np.zeros(len(X))
    floor[con] = e[con]
    for _ in range(R["tsm"]):
        t = np.array([0.5 * t[k] + 0.5 * t[nb].mean() if len(nb) else t[k] for k, nb in enumerate(R["nbr"])])
        t = np.maximum(t, floor)
    nj = np.array([world[dd["parent"]][:3, :3] @ dd["inward"] for dd in R["dent"]])
    nj /= np.maximum(np.linalg.norm(nj, axis=1, keepdims=True), 1e-12)
    G = R["C"] * (-(Nw @ nj.T))   # a dent bone's inward move, along the sample's inward normal
    # the dent bones that take part: under the wanted dent, and one ring of their lattice neighbours
    act = (R["C"][t > 0] > 0).any(0)
    act2 = act.copy()
    for i, j, _w in R["edges"]:
        if act[i]:
            act2[j] = True
        if act[j]:
            act2[i] = True
    ia = np.flatnonzero(act2)
    fs = np.flatnonzero((R["C"][:, ia] > 0).any(1))
    Ga = G[np.ix_(fs, ia)]
    La = R["Lap"][np.ix_(ia, ia)]
    w = np.ones(len(fs))
    Da = np.zeros(len(ia))
    d_max = R["d_max"] * scale
    tf = t[fs]
    con_in = np.searchsorted(fs, con)
    for _ in range(ROUNDS):
        M = (Ga * w[:, None]).T @ Ga + R["lam_s"] * La + 1e-6 * np.eye(len(ia))
        rhs = (Ga * w[:, None]).T @ tf
        for _ in range(SWEEPS):
            change = 0.0
            for x in range(len(ia)):
                if M[x, x] <= 0:
                    continue
                r_ = rhs[x] - M[x] @ Da + M[x, x] * Da[x]
                v = min(max(r_ / M[x, x], 0.0), d_max)
                change = max(change, abs(v - Da[x]))
                Da[x] = v
            if change < SETTLED:
                break
        got = Ga @ Da
        over = np.flatnonzero(got > tf + 0.002)
        short = con_in[got[con_in] < e[con] - 0.001]
        if not len(over) and not len(short):
            break
        w[over] *= 4.0
        w[short] *= 2.0
    D[ia] = Da
    if stats is not None:
        got_all = G @ D
        stats.update(need_cm=round(float(e.max() * 100), 2), dent_cm=round(float(got_all.max() * 100), 2),
                     left_cm=round(float(np.maximum(e[con] - got_all[con], 0).max() * 100), 2),
                     bone_cm=round(float(D.max() * 100), 2))
    return D


def solve(rig, world, morph=None, scale=1.0):
    """{dent bone name: local translation offset from its rest (parent frame, m)}, {region: stats}."""
    out, st = {}, {}
    for R in rig["regions"]:
        s = {}
        D = solve_region(rig, R, world, morph, scale, s)
        st[R["name"]] = s
        for dd, d in zip(R["dent"], D):
            out[dd["name"]] = d * dd["inward"]
    return out, st


def dent_world(rig, world, offsets):
    """World transforms of the dent bones: parent world x (rest + offset)."""
    out = {}
    for R in rig["regions"]:
        for dd in R["dent"]:
            L = np.eye(4)
            L[:3, 3] = dd["rest"] + offsets.get(dd["name"], np.zeros(3))
            out[dd["name"]] = world[dd["parent"]] @ L
    return out
