"""Tight limb shapes for skin-level collisions: capsules for the upper arm (from part-way down, as the plugin trims it),
the forearm, the palm's four bones (wrist to the index, middle and ring knuckles, cup to the pinky knuckle) and every
finger joint to the fingertip, each sized so a share (default 95%) of the skin nearest to it is inside; fitted on the
rest pose (the body's arms, the vanilla hands).  segments(world, side) gives each capsule's ends in a pose.
"""
import numpy as np

UPPER_TRIM = 0.5   # the upper arm's capsule starts this far down (the joint end sits inside the shoulder)
FINGERS = (("Index", "Wrist"), ("Middle", "Wrist"), ("Ring", "Wrist"), ("Pinky", "Cup"))


def chains(side):
    """(name, bone A, bone B or None for a tip, mesh) per capsule; a tip capsule runs from A along its parent->A."""
    s = side
    out = [("upperarm", f"{s}_Biceps", f"{s}_Forearm", "body"), ("forearm", f"{s}_Forearm", f"{s}_Wrist", "body")]
    for f, root in FINGERS:
        out.append((f"palm_{f.lower()}", f"{s}_{root}", f"{s}_{f}", "hands"))
        out += [(f"{f.lower()}0", f"{s}_{f}", f"{s}_{f}1", "hands"), (f"{f.lower()}1", f"{s}_{f}1", f"{s}_{f}2", "hands"),
                (f"{f.lower()}2", f"{s}_{f}2", None, "hands")]
    out += [("thumb0", f"{s}_Thumb", f"{s}_Thumb1", "hands"), ("thumb1", f"{s}_Thumb1", f"{s}_Thumb2", "hands"),
            ("thumb2", f"{s}_Thumb2", None, "hands")]
    return out


def _parent_of(name):
    side, b = name[:2], name[2:]
    for f, root in FINGERS + (("Thumb", "Wrist"),):
        if b == f + "2":
            return side + f + "1"
    return None


class LimbShapes:
    def __init__(self, scene, share=0.95):
        sc = scene
        self.tip = {}     # tip capsule: length (m) along parent->bone
        self.radius = {}
        W0 = {"body": v06_skin(sc.body, sc.base, sc.Pb), "hands": v06_skin(sc.hands, sc.base)}
        Wd = {"body": sc.env.Wd, "hands": dense(sc.hands)}
        names = {"body": sc.body.bones, "hands": sc.hands.bones}
        for side in "LR":
            caps = chains(side)
            for mesh in ("body", "hands"):
                mine = [c for c in caps if c[3] == mesh]
                if mesh == "body":
                    keys = ("Biceps", "Forearm", "Wrist", "Elbow", "Deltoid", "UpperArm")
                    cols = [j for j, n in enumerate(names[mesh]) if (n.startswith(side + "_") and any(k in n for k in keys)) or
                            (n.startswith("TF_UpperArm") and n.endswith("_" + side))]
                    sel = np.flatnonzero(Wd[mesh][:, cols].sum(1) > 0.5)
                else:
                    cols = [j for j, n in enumerate(names[mesh]) if n.startswith(side + "_")]
                    sel = np.flatnonzero(Wd[mesh][:, cols].sum(1) > 0.5)
                P = W0[mesh][sel]
                # tips: as long as the farthest skin dominated by the last joint
                for name, a, b, _ in mine:
                    if b is None:
                        j = names[mesh].index(a)
                        dom = sel[np.argmax(Wd[mesh][sel], 1) == j]
                        A = sc.base[a][:3, 3]
                        u = A - sc.base[_parent_of(a)][:3, 3]
                        u /= np.linalg.norm(u)
                        self.tip[(side, name)] = float(max(((W0[mesh][dom] - A) @ u).max(), 0.005)) if len(dom) else 0.01
                ends = [self.ends(sc.base, side, c) for c in mine]
                dom = np.array(names[mesh])[np.argmax(Wd[mesh][sel], 1)]
                for k, c in enumerate(mine):
                    own = self.own_bones(side, c)
                    group = [i for i, c2 in enumerate(mine) if self.own_bones(side, c2) == own]   # palm bones share
                    m = np.isin(dom, list(own))
                    if not m.any():
                        self.radius[(side, c[0])] = 0.01
                        continue
                    Q = P[m]
                    A, B = ends[k]
                    AB = B - A
                    t = ((Q - A) @ AB) / max(float(AB @ AB), 1e-12)
                    d = seg_dist(Q, A, B)
                    if len(group) > 1:   # skin shared by several capsules: each takes the skin nearest to it
                        Dg = np.stack([seg_dist(Q, *ends[i]) for i in group], 1)
                        m2 = np.array(group)[np.argmin(Dg, 1)] == k
                    else:
                        m2 = np.ones(len(Q), bool)
                    keep = m2 & (t > -0.05) & (t < 1.05)
                    self.radius[(side, c[0])] = float(np.percentile(d[keep], share * 100)) if keep.any() else 0.01

    @staticmethod
    def own_bones(side, c):
        """The bones whose skin a capsule stands for."""
        name, a, b, _ = c
        s = side
        if name == "upperarm":
            return frozenset({f"{s}_Biceps", f"{s}_Biceps_Twist", f"{s}_Biceps_Twist1", f"TF_UpperArm_Inner_{s}",
                              f"TF_UpperArm_Back_{s}"})
        if name == "forearm":
            return frozenset({f"{s}_Forearm", f"{s}_Elbow", f"{s}_Wrist_Twist", f"{s}_Wrist_Twist1", f"{s}_Wrist_Twist2"})
        if name.startswith("palm_"):
            return frozenset({f"{s}_Wrist", f"{s}_Cup"})
        return frozenset({a})

    def ends(self, world, side, c):
        name, a, b, _ = c
        A = world[a][:3, 3]
        if b is None:
            u = A - world[_parent_of(a)][:3, 3]
            u /= np.linalg.norm(u)
            B = A + u * self.tip[(side, name)]
        else:
            B = world[b][:3, 3]
        if name == "upperarm":
            A = A + (B - A) * UPPER_TRIM
        return A, B

    def segments(self, world, side=None):
        """[(side, name, A, B, radius)] for both arms (or one)."""
        out = []
        for s in ("L", "R") if side is None else (side,):
            for c in chains(s):
                A, B = self.ends(world, s, c)
                out.append((s, c[0], A, B, self.radius[(s, c[0])]))
        return out


def seg_dist(P, A, B):
    AB = B - A
    t = np.clip(((P - A) @ AB) / max(float(AB @ AB), 1e-12), 0, 1)
    return np.linalg.norm(P - (A + t[:, None] * AB), axis=1)


def v06_skin(mesh, world, P=None):
    import v06
    return v06.skin(mesh, world, P)


def dense(mesh):
    import v06
    return v06.dense_weights(mesh)
