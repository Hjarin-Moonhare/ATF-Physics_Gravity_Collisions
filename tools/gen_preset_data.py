"""Writes src/preset_data.h: for each of the body's 50 TF_ spring bones, the springs every installer setting gives it
(5 physics presets x 3 extra-jiggle levels: the preset and jiggle tables in A-Body's build tree,
reports/phase3/physics/presets and jiggle, the ones the A-Body plugins were built from): strength, damping ratio and MaxDist per COM axis (side, fore/aft, vertical), as
the record stores them.

The plugin finds which setting a body's record was built with (its values match one of these exactly) and can give a
part the springs of any other preset: the game reads strength and MaxDist from the record on every update, and the
plugin writes the damping per instance.

The levels a part can be given: a spring preset and a swing on top of it, so that the part moves the level's strength
against Realistic (Minimal x0.25, Lower x0.5, Realistic x1, Low x1.25, Medium x1.5, High x2, Max x2.5, Ultra x3).  A
preset's own strength is measured on the game's spring model (A-Body's p3_spring_sim.py: walk, run, jump landing,
sprint start/stop and strafe reversal at 60 fps): per motion, the largest offset of a part's bones against
Realistic's, averaged over the motions.  The level's swing is set on the breasts and butt (their mean, extra jiggle
None); kLevelMatch brings every other part to the level's strength too, per extra-jiggle level, for a part tuned with
bMatchLevel=1 (the presets give the belly, thighs, hips, calves, arms and torso a smaller share at the higher levels).
"""
import json
import os
import sys

sys.path.insert(0, os.environ.get("ABODY_TOOLS", "abody_tools"))
import numpy as np  # noqa: E402
import p3_spring_sim as sim  # noqa: E402

AUDIT = os.environ.get("ABODY_AUDIT", "abody_audit")
PRESETS = ("Realistic", "Low", "Medium", "High", "Max")
JIGGLE = ("None", "Normal", "Max")
LEVELS = [("Minimal", 0, 0.25), ("Lower", 0, 0.5), ("Realistic", 0, 1.0), ("Low", 1, 1.25), ("Medium", 2, 1.5),
          ("High", 3, 2.0), ("Max", 4, 2.5), ("Ultra", 4, 3.0)]   # name, spring preset, strength against Realistic
# the parts in body_shape_data.h's order, by their bones' names (the rest of the TF_ bones are torso)
PARTS = [("breast", ("TF_Breast",)), ("butt", ("TF_Glute",)), ("belly", ("TF_Abdomen",)), ("thigh", ("TF_Thigh",)),
         ("hip", ("TF_Hip",)), ("calf", ("TF_Calf",)), ("arm", ("TF_UpperArm",)),
         ("torso", ("TF_Flank", "TF_LowerBack", "TF_Lat", "TF_Pelvic", "TF_Perineum"))]
CALIBRATE = ("breast", "butt")
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src", "preset_data.h")


def table(preset, level):
    if level == "None":
        path = os.path.join(AUDIT, r"reports\phase3\physics\presets", f"{preset}.json")
    else:
        path = os.path.join(AUDIT, r"reports\phase3\physics\jiggle", f"{preset}_{level}.json")
    return path, json.load(open(path))


def peaks(tbl):
    """Per part and motion: the largest offset of the part's bones (m), on the game's spring model at 60 fps."""
    sp = sim.Springs(tbl)
    dt = 1.0 / 60
    out = {}
    for m in sim.MOTIONS:
        _, P = sim.motion(m, dt)
        X, _ = sp.run(P, dt)
        mag = np.linalg.norm(X, axis=2)
        for part, pre in PARTS:
            idx = [i for i, n in enumerate(sp.names) if n.startswith(pre)]
            out[part, m] = float(mag[:, idx].max())
    return out


def strengths(tables):
    """swing[level], match[level][extra jiggle][part], and each preset's own strength per extra jiggle and part."""
    pk = {(p, j): peaks(tables[(p, j)][1]) for p in PRESETS for j in JIGGLE}

    def ratio(p, j, part):   # the mean over the motions of the part's peak against Realistic's (same extra jiggle)
        r = [pk[p, j][part, m] / pk["Realistic", j][part, m] for m in sim.MOTIONS if pk["Realistic", j][part, m] > 1e-5]
        return float(np.mean(r))

    own = {(p, j, part): ratio(p, j, part) for p in PRESETS for j in JIGGLE for part, _ in PARTS}
    swing, match = [], []
    for _, s, target in LEVELS:
        w = target / float(np.mean([own[PRESETS[s], "None", part] for part in CALIBRATE]))
        swing.append(w)
        match.append([[target / (w * own[PRESETS[s], j, part]) for part, _ in PARTS] for j in JIGGLE])
    return swing, match, own


def main():
    tables = {(p, j): table(p, j) for p in PRESETS for j in JIGGLE}
    bones = sorted(tables[("Realistic", "None")][1])
    for (p, j), (path, t) in tables.items():
        assert sorted(t) == bones, path
        for b in bones:
            for key in ("strength", "damping", "travel_cm"):   # "travel_cm" holds the record's MaxDist
                assert len(t[b][key]) == 3 and all(x >= 0 for x in t[b][key]), (path, b, key)
    def num(x):   # a C++ float literal: always with a point (55.0f, not 55f)
        s = f"{x:.6g}"
        return (s if any(c in s for c in ".e") else s + ".0") + "f"

    vec = lambda v: "{" + ", ".join(num(x) for x in v) + "}"  # noqa: E731
    swing, match, own = strengths(tables)
    r4 = lambda x: round(x, 4)  # noqa: E731
    lines = ["// Generated by tools/gen_preset_data.py from A-Body's preset and jiggle tables; do not edit by hand.",
             "#pragma once", "",
             f"constexpr int kNumSpringPresets = {len(PRESETS)};   // " + ", ".join(PRESETS),
             f"constexpr int kNumExtraJiggle = {len(JIGGLE)};   // the extra-jiggle levels: " + ", ".join(JIGGLE),
             "constexpr const char* kSpringPresetName[kNumSpringPresets] = {" + ", ".join(f'"{p}"' for p in PRESETS) + "};",
             "constexpr const char* kExtraJiggleName[kNumExtraJiggle] = {" + ", ".join(f'"{j}"' for j in JIGGLE) + "};", "",
             "// the levels a part can be given, lowest first: a spring preset, the swing on top of it, and the strength that",
             "// gives the breasts and butt against Realistic (their movement on the game's spring model: the largest offset",
             "// per motion, walk, run, jump landing, sprint start/stop and strafe reversal at 60 fps, averaged)",
             "enum PhysicsLevel { " + ", ".join(f"kLevel{n}" for n, _, _ in LEVELS) + ", kNumLevels };",
             "constexpr const char* kLevelName[kNumLevels] = {" + ", ".join(f'"{n}"' for n, _, _ in LEVELS) + "};",
             "constexpr int kLevelSprings[kNumLevels] = {" + ", ".join(str(s) for _, s, _ in LEVELS) + "};",
             "constexpr float kLevelSwing[kNumLevels] = {" + ", ".join(num(r4(w)) for w in swing) + "};",
             "constexpr float kLevelStrength[kNumLevels] = {" + ", ".join(num(t) for _, _, t in LEVELS) + "};",
             "// a part's swing x this when it is tuned to match the level (bMatchLevel=1): its own movement at the level",
             "// brought to the level's strength, per installed extra-jiggle level; parts in body_shape_data.h's order (" +
             ", ".join(p for p, _ in PARTS) + ")",
             f"constexpr int kNumMatchParts = {len(PARTS)};",
             "constexpr float kLevelMatch[kNumLevels][kNumExtraJiggle][kNumMatchParts] = {"]
    for li, (name, _, _) in enumerate(LEVELS):
        lines.append("    {" + ", ".join("{" + ", ".join(num(r4(x)) for x in match[li][j]) + "}" for j in range(len(JIGGLE))) +
                     "},   // " + name)
    lines += ["};", "",
              "// one installer setting's spring for one bone, per COM axis (side, fore/aft, vertical), as the record stores it",
             "struct PresetSpring { float strength[3], damping[3], maxDist[3]; };",
             "struct PresetBoneDef { const char* name; PresetSpring s[kNumSpringPresets][kNumExtraJiggle]; };",
             f"constexpr int kNumPresetBones = {len(bones)};", "constexpr PresetBoneDef kPresetBones[kNumPresetBones] = {"]
    for b in bones:
        lines.append(f'    {{"{b}", {{')
        for p in PRESETS:
            cells = []
            for j in JIGGLE:
                e = tables[(p, j)][1][b]
                cells.append("{" + vec(e["strength"]) + ", " + vec(e["damping"]) + ", " + vec(e["travel_cm"]) + "}")
            lines.append("        {" + ", ".join(cells) + "},")
        lines.append("    }},")
    lines.append("};")
    open(OUT, "w", newline="\n").write("\n".join(lines) + "\n")
    # what differs between the settings, per bone: a match must be unambiguous
    clash = 0
    for b in bones:
        seen = {}
        for p in PRESETS:
            for j in JIGGLE:
                e = tables[(p, j)][1][b]
                key = tuple(e["strength"] + e["damping"] + e["travel_cm"])
                if key in seen and seen[key] != p:
                    clash += 1
                seen.setdefault(key, p)
    print(f"wrote {OUT}: {len(bones)} bones x {len(PRESETS)} presets x {len(JIGGLE)} extra-jiggle levels; "
          f"{clash} bone/setting pairs whose springs match another preset's")
    print("each preset's own strength against Realistic (extra jiggle None): " + "; ".join(
        f"{p} " + " ".join(f"{part} x{own[p, 'None', part]:.2f}" for part, _ in PARTS) for p in PRESETS[1:]))
    print("level swings: " + ", ".join(f"{n} x{w:.4f}" for (n, _, _), w in zip(LEVELS, swing)))
    print("match factors at Max, extra jiggle None / Max: " + "; ".join(
        f"{part} x{match[6][0][k]:.3f} / x{match[6][2][k]:.3f}" for k, (part, _) in enumerate(PARTS)))


if __name__ == "__main__":
    main()
