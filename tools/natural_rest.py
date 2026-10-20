"""The plugin's "no push closer than this" distances: for every soft bone and collider (and soft-soft pair), the
closest they come in the bind pose AND in natural standing poses.  The bind pose alone has the arms 42 deg
out from the body; the game's idle hangs them at the sides, so bind-pose distances alone would push the
belly.  Shared by gen_collision_data.py (the plugin's header) and tests/export_collision_cases.py (the expected
pushes), so both use the same numbers.
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.environ.get("ABODY_TOOLS", "abody_tools"))
import v06  # noqa: E402
import p3_collision as pc  # noqa: E402
from p3_spring_envelope import SKEL  # noqa: E402

DOWN, FWD = (0, 0, -1), (0, 1, 0)
SIZES = os.path.join(v06.AUDIT, r"reports\phase3\physics\collision\sizes.json")
# The glutes, measured by glute_sizes.py into their own file (sizes.json stays as it was measured).
GLUTE_SIZES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "sizes_glute.json")
# Every soft group the plugin knows: p3_collision's three, plus the glutes: the hands and forearms of either arm.  Not
# the thighs: a leg swung back 10 deg (walking) already pushed the lower glute 1.8 cm and stretched the skin 38% while
# no skin passed through itself; the fold under the buttock is the skinning's job.
SOFT = dict(pc.SOFT)
SOFT["glute"] = ([f"TF_Glute_{b}_{{s}}" for b in ("Upper", "Lower", "Inner", "Outer")], {"forearm": "B", "hand": "B"})
# glutes 2.5 cm: hands resting on them pushed 2.5 cm at a 59% peak stretch, inside the approved back slump's 62%
MAX_PUSH = {"breast": 0.05, "belly": 0.03, "innerthigh": 0.03, "glute": 0.025}
# Each capsule starts this far down its limb (0 = at the joint).  The joint end sits inside the shoulder or pelvis:
# in game it touched the outer breast and the belly in ordinary standing and walking.
TRIM = {"upperarm": float(os.environ.get("TF_TRIM_UPPERARM", 0.5)), "thigh": float(os.environ.get("TF_TRIM_THIGH", 0.4))}


def segment(world, name, side):
    """A collider's capsule ends, trimmed."""
    a, b, _ = pc.SEGMENTS[name]
    A, B = world[f"{side}_{a}"][:3, 3], world[f"{side}_{b}"][:3, 3]
    return A + (B - A) * TRIM.get(name, 0.0), B


def angle_from_down(world, a, b):
    d = world[b][:3, 3] - world[a][:3, 3]
    return float(np.degrees(np.arccos(-d[2] / np.linalg.norm(d))))


def natural_poses(skel, base, swings=(0,), strides=(0,)):
    """name -> pose ops: arms hanging 3-10 deg out with a slight elbow bend, and the legs together: standing still.
    Walking swings (25 deg) are left out by default: a swing back brings the upper arm as close to the breast as
    aiming forward does, and allowing it lets most of the arms-forward overlap back in (2.6 -> 12.9 cm2)."""
    poses = {}
    for hang in (3, 6, 10):
        for swing in swings:
            for elbow in (10, 25):
                ops = []
                for s in "LR":
                    ops.append(v06.toward(skel, f"{s}_Biceps", f"{s}_Forearm", DOWN,
                                          angle_from_down(base, f"{s}_Biceps", f"{s}_Forearm") - hang))
                    if swing:   # about the body's side-to-side axis: forward (+) or back (-)
                        ops.append((f"{s}_Biceps", np.array([1.0, 0.0, 0.0]) * (-1 if swing > 0 else 1), abs(swing)))
                    ops.append(v06.toward(skel, f"{s}_Forearm", f"{s}_Wrist", FWD, elbow))
                poses[f"arms hang {hang} swing {swing:+d} elbow {elbow}"] = ops
    for stride in strides:
        ops = []
        for s, other in (("L", "R"), ("R", "L")):
            inward = np.sign(base[f"{other}_Thigh"][0, 3] - base[f"{s}_Thigh"][0, 3])
            ops.append(v06.toward(skel, f"{s}_Thigh", f"{s}_Calf", (inward, 0, 0), 4))   # legs together
            if stride:
                ops.append((f"{s}_Thigh", np.array([1.0, 0.0, 0.0]) * (1 if s == "L" else -1), stride))
        poses[f"legs together stride {stride}"] = ops
    return poses


def hands_on_glutes(skel, base, back=30, inward=20):
    """A test pose (not used for the rest distances): both hands on the glutes, the arm hanging straight down, swung
    back, the forearm turned in toward the middle."""
    t = lambda *a: v06.toward(skel, *a)  # noqa: E731
    return [op for s, inn in (("L", (1, 0, 0)), ("R", (-1, 0, 0))) for op in (
        t(f"{s}_Biceps", f"{s}_Forearm", DOWN, angle_from_down(base, f"{s}_Biceps", f"{s}_Forearm")),
        ("swing", f"{s}_Biceps", f"{s}_Forearm", (0, -1, 0), back),
        ("swing", f"{s}_Forearm", f"{s}_Wrist", inn, inward))]


def rest_distances(verbose=False):
    """sizes.json's rest distances, each lowered to the closest it gets in the natural poses."""
    sizes = json.load(open(SIZES))
    glute = json.load(open(GLUTE_SIZES))
    sizes["soft_radius_m"].update(glute["soft_radius_m"])
    sizes["rest_distance_m"].update(glute["rest_distance_m"])
    rest = dict(sizes["rest_distance_m"])
    skel = v06.load_skeleton(SKEL)
    base = v06.bind_rest(skel, v06.load_vanilla_body())
    for key in rest:   # bind-pose distances to the trimmed capsules
        parts = key.split("|")
        if len(parts) == 3:
            n, name, cs = parts
            c = base[n][:3, 3]
            rest[key] = float(np.linalg.norm(c - pc.closest_on_segment(*segment(base, name, cs), c)))
    lowered = {}
    for pname, ops in natural_poses(skel, base).items():
        world = v06.pose(skel, ops, base)
        for key in rest:
            parts = key.split("|")
            if len(parts) != 3:
                continue   # soft against soft: the poses don't move one breast against the other
            n, name, cs = parts
            c = world[n][:3, 3]
            d = float(np.linalg.norm(c - pc.closest_on_segment(*segment(world, name, cs), c)))
            if d < rest[key]:
                lowered[key] = (rest[key] if key not in lowered else lowered[key][0], d, pname)
                rest[key] = d
    if verbose:
        touch = {}
        for key, (was, now, pname) in sorted(lowered.items(), key=lambda kv: kv[1][1] - kv[1][0]):
            n, name, cs = key.split("|")
            r = sizes["soft_radius_m"][n] * 1.1 + sizes["collider_radius_m"][name]
            if now < r:   # only these change the plugin's behaviour (contact = min(r + R, rest))
                touch[key] = (was, now, r, pname)
        print(f"{len(lowered)} distances are closer in a natural pose; {len(touch)} of them inside the spheres' reach:")
        for key, (was, now, r, pname) in list(touch.items())[:20]:
            print(f"  {key:40s} bind {was * 100:5.1f} -> {now * 100:5.1f} cm (spheres touch at {r * 100:4.1f})  {pname}")
    return sizes, rest


if __name__ == "__main__":
    rest_distances(verbose=True)
