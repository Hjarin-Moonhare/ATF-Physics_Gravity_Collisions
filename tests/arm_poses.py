"""Arm poses for the collision tests, built with a two-bone arm solve (the wrist put at a target, the elbow bent toward
a pole): folded (the forearms crossed under the breasts), clasped (the hands clasped at the upper chest, the forearms up
along the breasts) and across (one forearm over the waist, the other up over the opposite breast).  x is the
character's right, y forward, z up (m); the skeleton's rest pose is the starting point."""
import numpy as np

import v06


def arm_ik(skel, world, s, target, pole):
    """The pose with the s arm's wrist at target, its elbow bent toward pole (a world direction)."""
    S, E, W = (world[f"{s}_{b}"][:3, 3] for b in ("Biceps", "Forearm", "Wrist"))
    l1, l2 = np.linalg.norm(E - S), np.linalg.norm(W - E)
    d = np.asarray(target, float) - S
    D = float(np.clip(np.linalg.norm(d), abs(l1 - l2) + 1e-3, l1 + l2 - 1e-3))
    u = d / np.linalg.norm(d)
    a = (l1 * l1 - l2 * l2 + D * D) / (2 * D)
    h = np.sqrt(max(l1 * l1 - a * a, 0.0))
    p = np.asarray(pole, float) - np.dot(pole, u) * u
    p /= np.linalg.norm(p)
    En = S + a * u + h * p
    Wn = S + D * u

    def angle(x, y):
        return float(np.degrees(np.arccos(np.clip(np.dot(x, y) / (np.linalg.norm(x) * np.linalg.norm(y)), -1, 1))))

    w1 = v06.pose(skel, [("swing", f"{s}_Biceps", f"{s}_Forearm", En - S, angle(E - S, En - S))], world)
    E1, W1 = w1[f"{s}_Forearm"][:3, 3], w1[f"{s}_Wrist"][:3, 3]
    return v06.pose(skel, [("swing", f"{s}_Forearm", f"{s}_Wrist", Wn - E1, angle(W1 - E1, Wn - E1))], w1)


def three(skel, base):
    """{name: world} for folded, clasped and across."""
    out = {}
    w = arm_ik(skel, base, "L", (0.07, 0.135, 1.195), (-1, -0.2, -0.4))
    out["folded"] = arm_ik(skel, w, "R", (-0.07, 0.175, 1.225), (1, -0.2, -0.4))
    w = arm_ik(skel, base, "L", (-0.02, 0.16, 1.40), (-1, -0.2, -0.6))
    out["clasped"] = arm_ik(skel, w, "R", (0.02, 0.16, 1.40), (1, -0.2, -0.6))
    w = arm_ik(skel, base, "L", (0.09, 0.135, 1.10), (-1, -0.3, -0.5))
    out["across"] = arm_ik(skel, w, "R", (-0.12, 0.16, 1.33), (1, -0.2, -0.6))
    return out
