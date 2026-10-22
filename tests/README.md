# Tests

Every test includes `src\plugin.cpp` itself and runs the plugin's real code on fake game objects laid out the way the
game lays them out. None of them needs the game. Each `.bat` builds its tests with MSVC (through `tools\vcvars.bat`)
and runs them from the package folder. Run them from cmd or PowerShell: `run_analyze.bat` uses Windows' `find`, and
under Git Bash `find` is the Unix one.

| Script | Builds and runs | Output in | Time |
|---|---|---|---|
| `run_tests.bat [game folder]` | `collide_test`, `gravity_test`, `shape_test`, `tune_dump` (on `config\TFPhysics.ini`), `dent_test`, `breast_mass_test`, `freemorph_test` (with `data\FreeMorph.bin`), `addrlib_test` | `build\tests` | about 40 s |
| `run_stress.bat` | `stress_test`, as the logging build | `build\tests` | about 15 s |
| `run_stress_release_collisions.bat` | `stress_test`, as the release with collisions | `build\tests_relcol` | about 15 s |
| `run_ini_check_release_collisions.bat` | `ini_check_release_collisions`, on both release INIs | `build\ini_check` | about 5 s |
| `run_analyze.bat` | nothing: MSVC `/analyze` on the plugin | `build\analyze.txt` | about 10 s |
| `run_benchmarks.bat` | `fault_cost`, `vq_bench` | `build\tests` | about 10 s |

**Reading the output.** Each case prints one line: what it is, what it found, then `OK` or `FAIL`. A suite ends with
`all cases passed` or `SOME CASES FAILED`, and exits with 1 when a case failed. `run_tests.bat` stops at the first
suite that fails.

```
arms_crossed                                             8 bones pushed, largest 5.00 cm, worst difference 0.003 cm (TF_Breast_Outer_R)  OK
```

CMake runs the same programs as `run_tests.bat` and `run_stress.bat`: `ctest --test-dir build/cmake -C Release` (see
the main README).

## collide_test.cpp

Collisions, on A-Body's skeleton in the 13 poses in `cases\`. In each pose, every soft bone's push must match the
design's push in the file to 0.01 cm (0.3 cm when the wrist and finger bones are missing and the plugin falls back to
the rest pose).

- **Every child-list layout the plugin can meet:** pointer + count, a small array in the node, a linked list, a
  pointer to 16-byte entries, and the game's own (a pointer at +0x138, a 16-bit count at +0x142). Each one runs every
  pose, plus:
  - the breast anchor rebuilt from the chest;
  - no wrist or finger bones;
  - three frames with some springs skipping (the pushes hold and nothing drifts), then collisions switched off
    (nothing is left behind);
  - lying on the left side at x2 gravity, where nothing may push.
- **A foreign node first** in the forearm and thigh-twist child lists.
- **The game's layout with no Windows checks:** learning it, the bone search and the collisions make 0 memory checks.
- **Collisions against the gravity shapes** at x2, in each shape with the arms at rest, at the sides and hanging.
  These lines print the pushes and have no `OK`.
- **Arm poses that lift the breasts:** `folded` (the forearms crossed under the breasts), `clasped` (the hands
  clasped at the upper chest) and `across` (one forearm over the waist, the other up over a breast), held to the
  design's pushes with the lift (`tools/lift_model.py`) like the other poses; a breast x1.25 pushed further but inside
  its cap, and `across` lifted further with the lift than without it.
- **A part pushed as one** (`CouplePushes`, on a breast's four bones): one bone pushed 5 cm keeps its push and the
  other three follow to within the spread (2 cm); of two bones pushed the same way two spreads apart, the less
  pushed one is carried along and the other keeps its push; in a squeeze both give way evenly; pushes within the
  spread are left as they are; another part's push is left alone.
- **Switches:** gravity off (the anchors stay, the collisions still push the full 5 cm), no child list anywhere
  (nothing pushed, tried again later), the glutes off.
- **Other bodies:**
  - two bodies hugging, measured only and then pushing (the two pushes mirror each other), and the deepest contact;
  - shares more than two frames old don't count;
  - shares to skip: too old, half written, or the same character's;
  - a body 3 m away; a body that isn't A-Body, which shares its limbs and torso but no soft parts; no A-Body body
    near, so nothing is shared.
- **Dead bodies:**
  - calves and feet shared, for A-Body and other bodies;
  - a dead NPC next to the player, and the dead body's own contacts;
  - a dead body lying still keeps its share longer;
  - pushes from other bodies capped at 3 cm;
  - `bDeadBodies=0`;
  - 2,000 hook updates with 0 memory checks;
  - the life state, read the way `Actor::IsDead` reads it.
- **The world proxies:**
  - the proxy read: the full chain from the node to the body, and each way it can fail (no world, a stale body, an
    index out of range, another class, unreadable memory, no node);
  - the speed-limit read: the motion and the motion-properties table, and each way that can fail;
  - the watch on a body, and the writes: the layer, steering, warps, the layer taken back by the game;
  - following at speed: a proxy held to a lower speed limit than it needs falls behind, and the own-warp rule keeps it
    on target at 6 and 15 m/s;
  - one steer per shared motion: steered once a frame, the right proxy riding on it.
- **Measure only:** contacts counted, nothing pushed, the snapshot written.

## gravity_test.cpp

- **Every gravity shape** on both sides, against the design's offsets: standing, lying on the back, on the left and
  right sides, face down, bent forward on the feet, and on all fours against lying with the thighs flat.
- **The shape ceilings** at x2 with breast size ×1.5: back ×2.0, side ×2.4, bent forward ×2.3, face down ×3.0. Small
  breasts stay at ×1.0. With the largest levels set to ×6, the slump reaches ×3.
- **Set levels:** `[Gravity] fLevel`, a tuned `fGravityLevel` over it and a level per shape over both, each its own
  largest level: ×5 stays ×5 on a ×1.5 breast and over largest levels of ×2.0, a ×0.5 breast gets half, a stretch-guard
  scale of 0.8 gives 0.8 times (the lower of size and scale counts); a shape without its own level keeps the next one,
  down to the installed level.
- **Shapes off:** with the side shape off, only the slide; with the teardrop off, the hang at 1.2.
- **Spring gravity** per part.
- **The breast size** from the sliders on a fake character record, and ×1 without a character.
- **The modifier list:** the eight breast bones found by name, and a cloth bone under the anchor left out.
- **A full bone table:** stale bones dropped, a live one kept with its offset.
- **The readable-memory check:** bad probes turned away without an exception, bad guarded copies caught.
- **Settings:** garbage values held in range, no settings file giving the defaults, and the shipped
  `config\TFPhysics.ini` with every key read from the file (`run_tests.bat` copies it next to the test).
- **The call-site thunks** hand the character over in r8: none from site A, the character from sites B and C.

## shape_test.cpp

Body-shape physics, against numbers worked out from the design, not from the plugin's code:

- **Reading:** the body triangle and the part sliders from a fake record, the factors at the triangle's corners, its
  centre and (0,0,0), and garbage in the record (NaN, −3 and 7 read as 0, 0 and 1).
- **Without it:** the triangle reading unavailable (only the sliders act), `[BodyShape]` off, the overrides, no
  character.
- **Per spring:** its part, its ceilings (Max physics + Max jiggle over the installed preset), its damping floor.
- **Damping** per body and per instance: two actors on one record keep their own.
- **Spring gravity and jiggle** per part (the hips' and the torso's gravity from a tuning file and from `[Springs]`),
  and the swing scaled per COM axis: one ceiling per part, a spring that skips a frame, two instances on one bone, a
  neutral body left alone.
- **Gravity shapes of the other parts,** lying on the back: the settle on the hips with no spring sag (in COM's axes and
  on the bone), topping up a spring's own sag, none past it (a zero offset handed on), a tuning file's level, a thin
  and a heavy body, upside down held at 1 g, standing, `[Gravity] bEnabled=0`, and the breasts left to their own.
- **A paused frame** (a zero frame time): standing, a held bounce put back to rest with the spring left as it was;
  lying on the back, the spring's own sag and the butt's gravity shape over it; `bSettleWhenPaused=0` and a 0.3 s
  frame left alone.
- **The stretch guard:** the strain per body, the shape ceilings scaled by it, and the breast springs' gravity held.
- **Settings:** the shipped INI's `[BodyShape]` keys, and garbage `[BodyShape]` values (NaN taken as the low end).
- **Tuning each part,** written out from the design ([../docs/TUNING.md](../docs/TUNING.md)):
  - the installer setting a record was built with, found from its values, and a record no setting matches;
  - a part at a level: the level's strength and MaxDist in the record, the damping and the sub-step rate per instance,
    against the preset table;
  - back as installed: bit for bit the constructor's values;
  - another body's record at a level keeps its own springs;
  - bounce speed, settle, Lower, Minimal and Off; `[All]` with one part on its own;
  - per direction and per bone; a swing past Max held at the part's ceiling;
  - sag per part; characters, outfits (the lowest factor of the pieces worn) and the planet;
  - the files: five read in name order, garbage held in range, and read again after a change;
  - Ultra (Max's springs, its own swing, Max's damping), a part matched to its level, and a travel limit below 1.

## tune_dump.cpp

Prints how TF Physics reads a TFPhysics.ini's `[Tuning]` section and the tuning files next to it, with the plugin's
own reader, one line per setting. Part of what it prints:

```
build\tests\tune_dump.exe "C:\Games\Starfield\Data\SFSE\Plugins\TFPhysics.ini"
gravity enabled 1 level 5.0000 back 2.0000 side 2.4000 hang 2.3000 down 3.0000 maxlevel 3.0000 size 1 shapes 1.0000
springs Breasts 0.0600 Butt 0.0800 Belly 0.0400 Thighs 0.0450 Hips 0.0300 Calves 0.0320 Arms 0.0300 Torso 0.0130
files 2 All.ini, Breasts.ini
all Medium
part Breasts level Off match 0 swing 1.0000 ... sag -1.0000 limit -1.0000 gravity -1.0000 back 4.0000 ...
outfit [Spacesuit] form 00000000 slots 35 | Breasts 0.5000 Butt 0.6000 ...
```

`sag -1` means the part's sag comes from TFPhysics.ini's `[Springs]`; `gravity -1` and a shape's `-1` mean no level is
set there. A key that doesn't show its value was not read:
a typo in its name, or a comment after its value.

## dent_test.cpp

The dents (`src\dents_core.h`) against their design model (`tools\dent_model.py`), on A-Body's three dent rigs in
`data\`: `DentRig_A.bin` (50 dent bones in 6 regions: the breasts, glutes and thighs), `DentRig_B.bin` (156 in
the two breasts) and `DentRig_C.bin` (version 2: 160 in the two breasts on every bone their skin hangs on, one
shared field per breast, tilting). Each case in `cases_dent\` is one rig in one of nine collision poses with the
design's pushes, at the base sliders and with the breast, butt and thigh sliders at 0.5 (`_large`). Every dent
bone's move must match the model's to 0.5 mm, and on rig C every tilt to 0.005.

- **The rig files:** each one read; four copies cut short (4 bytes, 200 bytes, half, all but the last byte) and one
  with a wrong header turned away with a reason.
- **A missing bone:** without a dent bone's parent (`C_Chest`), its region isn't dented.
- **The depth setting:** x0 moves nothing, x2 stays within twice a dent bone's largest move.
- **The tilt's turn:** three orthonormal, right-handed axes, and to first order the identity plus the tilt's cross
  product.
- **The time** of a region's solve, and of one with a contact.

```
load                               rig A: 50 dent bones in 6 regions, 2882 samples, 48 shapes  OK
B_folded.txt                        121 skin samples in contact, deepest dent 1.01 cm, worst difference 0.000 mm  OK
```

## breast_mass_test.cpp

The masses and the relative damping (`src\breast_mass.h`) on made-up motion, in the plugin's own code:

- standing still and a steady 5 m/s: the mass at rest (no lag);
- a walk and a run (a vertical bob and a sway, then a stop over 0.15 s) at 30, 60 and 144 fps: the walk bounces 5 to
  25 mm along the spine, the run more, never at a hard stop, settled within 0.8 s of the stop;
- the glutes (`bm::GluteParams`): a walk 5 to 30 mm, the run never at a hard stop, settled;
- the run's bounce within 20% at 30 and 144 fps;
- a 5 s fall at 1 g (49 m/s) and a sudden landing, both masses, at 30, 60 and 144 fps: no frame moves more than 2 mm,
  the mass floats up in the fall, stays inside its hard stops and settles within 1.2 s of landing;
- a teleport (30 m in a frame) moves nothing;
- the relative damping on a model of the game's spring at 4 m/s: the lag c v / S without it, under a quarter of that
  with it.

```
   60 fps  walk: spine +9.5 / -9.5, fwd +0.0 / -0.0, side +2.7 / -2.7 mm, settles 0.53 s after stopping | run: spine +24.8 / -32.2, fwd +0.0 / -0.0, side +10.3 / -10.3 mm, settles 0.55 s
breast fall  60 fps: in the fall +20.3 mm (most change a frame 0.15 mm); range +22.7 / -31.5 mm; settles 0.45 s after landing
PASSED: 0 failures
```

## freemorph_test.cpp

FreeMorph's solve (`src\freemorph_core.h`) against its design model (A-Body's `freemorph.py`, the same steps in
Python) on the model's test vectors (`data\freemorph_vectors.bin`): both hands closing on the breasts over 12
frames, then held. It needs the `FreeMorph.bin` the vectors were made for, A-Body 2.0's (Nude or Tan lines skin), in
`data\`; without it `run_tests.bat` skips it. Frame by frame:

- each breast's shown displacement against the model's: exact while fewer than 20 nodes touch, the press and the rise
  within 1 mm after, no node more than 5 mm off (the grip's holds are path-dependent: float and double part ways a
  little once many nodes touch);
- the contacts and held points against the model's (at most 12 apart);
- the records after the last frame (the position within 5 mm), and the moving dent's records (the position within
  0.01 mm, the 10-bit fields within one step, the colours exact);
- the grab held on for 10 s at 60 fps: whether the solve settles, and how long a frame takes.

```
shown displacement: at most 3.769 mm off the model (breast_L 3.769, breast_R 2.856); contacts off by at most 4, held by 6; while under 20 nodes touch at most 0.0003 mm off; the press and the rise at most 0.424 mm off
records after the grab: 2691 of 3049 differ in bytes; position at most 1.0967 mm off, 10-bit fields at most 171 steps off (8562 fields), colours 0
held 10 s: 0.30 ms a frame on average, 3.89 ms at most
ALL PASSED
```

## addrlib_test.cpp

- **SFSE's view of the plugin:** it loads on any game version that has its Address Library database (address bits
  0x4, layout bits 0x8), and the database path SFSE checks.
- **The reader, on fake databases:** lookups, and each way a database can be wrong, with the message the log gets:
  missing, the old format 2, another game version, 32-bit pointers, cut short, an ID past the end, an ID with no
  address, the game running another version, an address outside the image.
- **Every start-up check, on a fake game image.** It passes as built, and with the code moved so its operands change.
  Each one-byte change to what a check reads makes that check fail:
  - a function's first bytes (54 of 54);
  - a modifier class's update slot;
  - the frame-time load (8 of 8);
  - a call site (15 of 15: not a call, or a call elsewhere, which is another mod's hook);
  - the size reading (29 of 29) and the triangle reading (20 of 20);
  - the character's register blocks, where only that path loses its character and the hooks still go in;
  - the tuning's three reads (53 of 53: the gravity scale 6, the form ID 5, the worn items 42), each change turning
    off only its own read.

  An unreadable address fails the check without a crash, and a tuning read outside the image turns all three off.
- **With the game:** the real database (every ID found, and 25 of 25 equal the known 1.16.244 RVAs), refused while
  another game version runs, the 1.16.242 and 1.16.236 databases, and every check on the real `Starfield.exe`, read
  only, FreeMorph's included: the MorphDriver instance and geometry classes, the buffer update, the morph table's
  reads, MorphDB's targets holder and the per-body copies setting.

It looks for the game in `run_tests.bat`'s first argument, then in `STARFIELD_DIR`, then in the usual Steam folders.
Without the game those cases print `SKIPPED`; they don't fail.

```
tests\run_tests.bat "C:\Games\Starfield"
```

## stress_test.cpp

The plugin's whole per-frame path on many fake bodies:

- **Cost per body-update, one thread:** bodies 3 m apart (about 5 µs) and 24 bodies in one spot (about 20 µs).
- **11 threads for 4 s:** no deadlock, and 0 Windows memory checks in every update.
- **Paused frames on 11 threads for 2 s:** no deadlock, every offset finite, and one more paused frame moves nothing.
- **The same input gives the same result:** identical bodies end identical (to 0.000001 cm), and one more frame moves
  nothing. The bodies did move, and the body shape scaled their swings.
- **Other bodies:** 24 in one spot (measured only), then pushing from many threads, with every offset finite and the
  pushes held to 3 cm.
- **Guarded reads:** a size read past an object's page, an unreadable weight table, and a node name and a child list in
  unreadable memory. None crashes.
- **The log from 11 threads:** every line queued, none dropped, all in the file.

The times are from the PC that runs it.

`run_stress_release_collisions.bat` builds the same test as the release with collisions. The release writes no log, so
its log case fails by design: it ends with `SOME CASES FAILED` and exit code 1, and every other case must pass.

## ini_check_release_collisions.cpp

Builds the release with collisions and reads `config\release\Fixed` and `config\release\Variable` with its own
settings loader, then prints what it got:

```
Fixed:
collision 1 measureOnly 0 others 1 othersMeasureOnly 0 deadBodies 0 glutes 1 innerThighs 0
softScale 1.10 maxPush 0.050 0.030 0.030 0.025 other 0.030 m
bodyShape 0 stretchGuard 1 gravity 1 | world proxies 0 layer 0 steer 0 snapshots 0
breast mass 1 x1.00, glute mass 1 x1.00, relative damping 1 | FreeMorph 1 key 0 start 2 grip -1.0 give -1.0 cap x1.00 own pushes 0
OK
```

The two INIs differ only in `bodyShape`: 0 in Fixed, 1 in Variable. The world features stay at 0 in a release; the
masses, the relative damping and FreeMorph are on, with no FreeMorph mode key.

## run_analyze.bat

Runs MSVC `/analyze` with all warnings on the plugin, writes `build\analyze.txt` and prints the warning count. Nothing
is built. The 30 it reports are known:

- 22 are C4865 in the Windows SDK's own headers (enum types under `/Wall`);
- 3 are C5219, integers turned into floating point: the log's run time, the sub-step count and the thigh tilt's
  average;
- 2 are C4365, `int` to `size_t` in the child-list discovery's log;
- 3 are C6001 in `freemorph_core.h`'s smooth union of the limb shapes: arrays filled for every shape used before
  they're read (the analysis doesn't follow the count).

## run_benchmarks.bat

Two measurements behind the plugin's memory rules:

- **`fault_cost`:** what one guarded read costs. Good memory: about 0.002 µs. Bad memory: about 1 µs, with the
  exception caught or checked first, and about 2 µs with a crash-logger-like vectored handler installed.
- **`vq_bench`:** what one VirtualQuery costs at a random address in a committed region of 1 MB, 64 MB, 512 MB and
  2 GB. The kernel walks the region's pages, so the cost grows with it: about 1 µs, 60 µs, 2 ms and 20 ms.

## The poses in cases\

`tests\export_collision_cases.py` exported them from A-Body's skeleton (it needs A-Body's build tools). Each file holds
every bone's local and world transform in a pose, plus the design's push per soft bone. `tools\README.md` has the
format and how to make them again.

## The dent cases in cases_dent\

`tests\export_dent_cases.py` wrote them from nine of the collision poses (it needs A-Body's build tools and its dent
rigs). Each file is one rig in one pose, in metres:

```
rig <A or B>
scale <the body's scale>
morph <slider> <weight>
B <index> <bone> 1 <x axis: 3 numbers> <y axis> <z axis> <position x y z> <scale>
D <region> <count> <each dent bone's move along its inward line>
W <region> <count> <each dent bone's tilt: 3 numbers, its parent's frame>
```

- `morph`: one line per slider that isn't at 0 (none at the base sliders).
- `B`: one line per rig bone, its world transform as the game stores it; `B <index> <bone> 0` when the pose has no
  such bone.
- `D`: one line per region, the moves the design model gives.
- `W`: one line per tilting region (rig C), the tilts the design model gives.
