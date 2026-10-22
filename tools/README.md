# Tools

Run everything from the package folder, the one with `build.bat`.

| Tool | What it does | Needs |
|---|---|---|
| `vcvars.bat` | Puts the 64-bit MSVC tools on PATH for every build and test script. | Visual Studio 2022 or newer, or its Build Tools |
| `package_build.py` | Zips the logging build and its INI for a tester. | Python 3, a built `build\TFPhysics.dll` |
| `re\` | Reads `Starfield.exe` from disk: Address Library IDs, disassembly, vtables, callers. See `re\README.md`. | Python 3.10+, capstone, numpy, the game |
| The measuring scripts | Measure A-Body, and write the data headers, the test poses and the dent cases. See "Where the numbers come from" below. | A-Body's build tools |

## vcvars.bat

Every `.bat` in the package starts with it:

```
call tools\vcvars.bat || exit /b 1
```

It has no `setlocal`, so the PATH it sets stays for the script that called it. In order:

1. If `cl.exe` is already on PATH (a Developer Command Prompt), it does nothing.
2. Otherwise it asks `vswhere.exe` (in `%ProgramFiles(x86)%\Microsoft Visual Studio\Installer`) for the newest Visual
   Studio or Build Tools with the x64 C++ tools, and runs that copy's `VC\Auxiliary\Build\vcvars64.bat`.
3. If there's none, it says what to install and exits with 1, so the script stops before building.

If it can't find a copy you have, open "x64 Native Tools Command Prompt for VS" and run the scripts from there.

## package_build.py

Zips the logging build for a tester:

```
build.bat
python tools\package_build.py test1
```

That writes `dist\TFPhysics_test1.zip`, laid out like the game's `Data` folder:

```
SFSE\Plugins\TFPhysics.dll    from build\TFPhysics.dll
SFSE\Plugins\TFPhysics.ini    from config\TFPhysics.ini
```

- The name is up to you: `test1`, `armfix`, anything a file name can hold.
- It prints the size and the first 16 hex digits of the SHA-256 of the DLL, the INI and the zip, so two builds can be
  told apart.
- It never overwrites. A zip of that name already in `dist\` stops it, and so does a missing DLL (run `build.bat`
  first).
- It packs the logging build only. A release build is `build\release\TFPhysics.dll` or
  `build\release_col\TFPhysics.dll`, with an INI from `config\release\Fixed` or `config\release\Variable`.

A tester installs the zip over A-Body like any mod (with a mod manager, or unzipped into `Data`), plays, and sends back
`Documents\My Games\Starfield\SFSE\Logs\TFPhysics.log`.

## re\

Small read-only tools for `Starfield.exe`: what an Address Library ID points at, a function's code, a class's vtables,
who calls a function, and which code uses a string. Each one is described in `re\README.md`.

## Where the numbers come from

These scripts measured the numbers in `src\collision_data.h`, `src\lift_data.h`, `src\body_shape_data.h` and
`src\preset_data.h`, and wrote the test poses in `tests\cases\` and the dent cases in `tests\cases_dent\`. They
show how each number was made, and they rebuild the files when A-Body's mesh, skeleton or physics presets change.
On A-Body's data they write the shipped files again, line for line.

**What they need.** They read A-Body's mesh, skeleton and presets through A-Body's own build tools, which aren't part
of this package:

| Variable | The folder | Used by |
|---|---|---|
| `ABODY_TOOLS` | A-Body's Python build tools: `v06.py`, `p3_collision.py`, `p3_physics.py`, `p3_spring_envelope.py`, `p3_spring_sim.py`, `poses.py`, and what they import | every script but `gen_body_shape_data.py` and `dent_model.py` |
| `ABODY_AUDIT` | A-Body's build tree, with the physics preset and jiggle tables | `gen_body_shape_data.py`, `gen_preset_data.py` |
| `ABODY_BUILDS` | A-Body's builds folder, with its three dent rigs (`A-Body_DentRig_A`, `_B`, `_C`) | `tests\export_dent_cases.py` (or the three rig folders as its arguments) |

`natural_rest.py` also reads `reports\phase3\physics\collision\sizes.json` from the build tree that A-Body's `v06.py`
points at. They need Python 3 with numpy (A-Body's tools also need scipy), and each one takes a second or two.

**Running them,** in this order (`natural_rest.py` reads what `glute_sizes.py` writes):

```
set ABODY_TOOLS=C:\path\to\A-Body\tools
set ABODY_AUDIT=C:\path\to\A-Body
set ABODY_BUILDS=C:\path\to\A-Body\builds
python tools\glute_sizes.py
python tools\gen_collision_data.py
python tests\export_collision_cases.py
python tests\export_dent_cases.py
python tools\gen_body_shape_data.py
python tools\gen_preset_data.py
```

Then build, and run `tests\run_tests.bat`: `collide_test` checks the plugin, with the new header, against the new
poses, and `dent_test` against the new dent cases. The scripts write Windows line ends; `.gitattributes` (`* text=auto`) evens those out, so git shows no change
when the numbers are the same.

### glute_sizes.py: tools\data\sizes_glute.json

Measures the eight glute soft bones (`TF_Glute_Upper`, `_Lower`, `_Inner` and `_Outer`, each side) on A-Body's mesh at
rest, by the rules A-Body's `p3_collision.py` used for the breast, belly and inner-thigh bones:

- **Radius:** how far the bone's own skin reaches, as the 75th percentile of the distance from the bone to the vertices
  it moves most. A bone that moves fewer than 20 vertices most (Upper and Outer move 4 to 7) takes every vertex it
  moves by 0.25 or more instead.
- **Rest distances,** in the bind pose: to each limb capsule (upper arm, forearm, hand and thigh, both sides), and to
  the other side's four glute bones. `natural_rest.py` measures the capsule ones again against the trimmed capsules.

It prints one line per bone:

```
TF_Glute_Upper_L     7 vertices dominated (p75  6.4 cm),  47 at weight >= 0.25 (p75  5.6 cm) -> radius  5.6 cm
```

### natural_rest.py

The distance below which nothing pushes, for every soft bone and limb capsule: the closest the two come in the bind
pose or in a natural standing pose. A-Body's bind pose holds the arms 42° out from the body, but the game's idle lets
them hang at the sides, so bind-pose distances alone would push the belly at rest. The standing poses:

- the arms hanging 3°, 6° or 10° from straight down, with the elbows bent 10° or 25°;
- the legs together, each thigh turned 4° in.

The next two scripts import it, so the header and the test poses use the same numbers. It also holds the settings they
share:

| Name | What it is |
|---|---|
| `SOFT` | The soft groups and their bones: breast, belly and inner thigh from `p3_collision.py`, plus the glutes. Each group lists the limbs that push it. |
| `MAX_PUSH` | The most each group is pushed: breast 5 cm, belly 3, inner thigh 3, glute 2.5. These are the INI's `fMaxPush*Cm` defaults. |
| `PUSH_SPREAD` | The most two pushes of one part may differ: breast 2 cm, belly 4, inner thigh 5, glute 3.5. Each is the furthest one soft bone moved alone, along any COM axis either way with the rest of its part at rest, with the skin of A-Body's mesh smooth: no triangle turned more than 60° from its rest normal, no edge bent more than 30° past its rest bend, no skin through itself (the navel's own 1 to 3 mm triangles left out: any move of the belly bends them). The whole part moved together stays smooth to breast 5 cm, belly 4, inner thigh 4, glute 3. |
| `TRIM` | How far down its limb a capsule starts, as a share of the limb's length (0 = at the joint): upper arm 0.5, thigh 0.4. The joint end sits inside the shoulder or pelvis. Set `TF_TRIM_UPPERARM` or `TF_TRIM_THIGH` to try another value. |

Run by itself, it lists what the standing poses changed:

```
python tools\natural_rest.py
186 distances are closer in a natural pose; 6 of them inside the spheres' reach:
  TF_Abdomen_R|forearm|R                   bind  30.2 ->   8.5 cm (spheres touch at 12.5)  arms hang 3 swing +0 elbow 25
```

Only the ones inside the spheres' reach change anything. Contact starts at the smaller of this distance and the two
radii added up, with the soft sphere at its measured radius × 1.1 (the INI's `fSoftScale`).

### gen_collision_data.py: src\collision_data.h

Writes the collision header from `sizes.json`, `sizes_glute.json` and `natural_rest.py`'s distances:

- the four limb capsules: the bones each one runs between, its radius, and where it starts (`TRIM`);
- the hand's tip in the wrist's frame and the wrist in the forearm's frame, used when the finger or wrist bone isn't
  found;
- per group: the most it's pushed, the most two of its pushes may differ, and which capsules it meets (none, both
  sides, its own side or the other side);
- per soft bone (24 of them): its group, side and radius, its rest distance to each capsule, and up to four soft bones
  of the other side that it meets, with their rest distances.

It prints the soft-bone count and the two fallback offsets, in cm.

### gen_lift_data.py: src\lift_data.h and data\lift_data.json

The breast lift's data: A-Body's breast skin (the summed weight of a breast's four bones at least 0.1), one sample per
1 cm cell, in 3 cm groups; per sample its rest position in `C_Chest`'s frame, its change per unit of the sliders that
move breast skin (Thin, Strong, Overweight and the TF breast sliders) and its weights (the four bones with their
children, and the Pecs anchor); per group its mean and the bound of its members. Also the bones' and the anchor's
rest positions, the chest's up axis, and the tight arm and hand shapes from `limb_shapes.py` (each with the radius
that holds 95% of the skin nearest to it). `lift_model.py` is the same lift for the design model.

### ..\tests\export_collision_cases.py: tests\cases\*.txt

Poses A-Body's skeleton 13 ways, and writes what `collide_test.cpp` checks the plugin against: `rest`, `arms_forward`,
`arms_crossed`, `sitting`, `deep_squat`, `sprint_stride`, `arms_sides`, `arms_hanging`, `hands_on_glutes`,
`hands_on_glutes_firm`, and from `arm_poses.py` `folded`, `clasped` and `across`. The expected pushes are worked out here, not by the plugin's code: the same contact rule, half
for soft against soft, capped per group, and a part's pushes coupled the same way.

Each file, in metres:

```
N <node> <parent> <local x y z> <rotation: 9 numbers> <world x y z>
E <soft bone> <push x y z>
```

- `N`, one per node: its position in its parent's frame, its world rotation as the game stores it (each row is one of
  the node's axes), and its world position. `-` means no parent. Spaces in names become `_`.
- `E`, one per soft bone: the push the design expects.

It prints each pose's node count and largest push:

```
arms_crossed   218 nodes, largest designed push 5.00 cm
```

### dent_model.py

The dents' design model, in numpy: `load()` reads a `DentRig.bin` (version 1 or 2), and `solve_region()` /
`solve_region2()` run one region's solve on the bones' world transforms with the same steps as
`src\dents_core.h` (the second also returns the tilts); `box_qp()` is the version 2 fit's exact solve,
`tilt_matrix()` a tilt's turn, and `dent_world()` places the dent bones with their moves and turns. `dent_test`
holds the plugin to it through the cases `export_dent_cases.py` writes. It needs only numpy.

### ..\tests\export_dent_cases.py: tests\cases_dent\*.txt

Copies A-Body's three dent rigs to `tests\data\`, and for each rig takes nine of the poses `export_collision_cases.py`
makes (`rest`, `arms_crossed`, `arms_forward`, `hands_on_glutes`, `hands_on_glutes_firm`, `sitting`, `folded`,
`clasped`, `across`), moves each soft bone and everything under it by the design's push, and writes what
`dent_test.cpp` checks: per pose, at the base sliders and with the breast, butt and thigh sliders at 0.5, every rig
bone's world transform and the dent bones' moves (and on rig C their tilts) the model gives. It prints each case's
contacts and deepest dent.

### gen_body_shape_data.py: src\body_shape_data.h

For each of A-Body's 50 spring bones, over all 15 installer settings (5 physics presets × 3 extra-jiggle levels):

- **its part,** by name: breast, butt, belly, thigh, hip, calf or arm. The rest (flanks, lower back, lats, pelvis,
  perineum) are torso, and only follow the body.
- **its largest `MaxDist`** on each COM axis (side, fore/aft, vertical): a body's scaled swing never travels further.
- **its lowest damping** on each axis: a body's damping never drops lower.

So no body shape moves more than a Max physics + Max jiggle install already does. It reads
`%ABODY_AUDIT%\reports\phase3\physics\presets\<preset>.json` and `...\jiggle\<preset>_<level>.json`, and stops if
their bone lists differ. It prints the count per part, and whether Max + Max is the largest on every bone (on A-Body it
is):

```
wrote ...\src\body_shape_data.h: 50 bones {'breast': 8, 'butt': 8, 'belly': 4, 'thigh': 10, 'hip': 2, 'calf': 4, 'arm': 4, 'torso': 10}; Max + Max is the largest on every bone: True
```

### gen_preset_data.py: src\preset_data.h

For each of A-Body's 50 spring bones, the springs of every installer setting (5 physics presets × 3 extra-jiggle
levels), per COM axis as the record stores them: strength, damping ratio and MaxDist. It also lists the levels a part
can be given, Minimal to Ultra, with the swing that gives each its strength: it runs every table on A-Body's spring
model (`p3_spring_sim.py`: walk, run, jump landing, sprint start/stop, strafe reversal at 60 fps), sets each level's
swing on the breasts and butt, and writes each part's factor to reach the level too (`kLevelMatch`). It reads the
same tables as `gen_body_shape_data.py` and stops if their bone lists differ.

The plugin matches each spring record against these values to find the installed setting, and writes a level's
springs into the record when a tuning file gives a part that level.

### FreeMorph.bin and tests\data\freemorph_vectors.bin

`FreeMorph.bin` is body data: A-Body's build tools make it for each body geometry from its mesh, its bones and its
sliders (the breasts' zones, a volume of graded layers down to the chest wall per breast, linear elastic flesh under
stiffer skin, and each skin node's response to a unit force at every node within 8 cm, held as int16 with one scale
per node; the limb shapes fitted to the game's hands and A-Body's arms). A-Body ships it in
`Data\SFSE\Plugins\TFPhysics\` for each skin. Its layout is in `docs\BODY_INTEGRATION.md`. The test vectors are
the design model's solve of a grab on A-Body 2.0's Nude / Tan lines file, from the same bone transforms the C++ reads.

## Another body

The same measurements, made on that body's own mesh and skeleton. See `docs\BODY_INTEGRATION.md`.
