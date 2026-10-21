# Tests

Every test includes `src\plugin.cpp` itself and runs the plugin's real code on fake game objects laid out the way the
game lays them out. None of them needs the game. Each `.bat` builds its tests with MSVC (through `tools\vcvars.bat`)
and runs them from the package folder. Run them from cmd or PowerShell: `run_analyze.bat` uses Windows' `find`, and
under Git Bash `find` is the Unix one.

| Script | Builds and runs | Output in | Time |
|---|---|---|---|
| `run_tests.bat [game folder]` | `collide_test`, `gravity_test`, `shape_test`, `tune_dump` (on `config\TFPhysics.ini`), `addrlib_test` | `build\tests` | about 30 s |
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

Collisions, on A-Body's skeleton in the ten poses in `cases\`. In each pose, every soft bone's push must match the
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
  only.

It looks for the game in `run_tests.bat`'s first argument, then in `STARFIELD_DIR`, then in the usual Steam folders.
Without the game those cases print `SKIPPED`; they don't fail.

```
tests\run_tests.bat "C:\Games\Starfield"
```

## stress_test.cpp

The plugin's whole per-frame path on many fake bodies:

- **Cost per body-update, one thread:** bodies 3 m apart (about 5 µs) and 24 bodies in one spot (about 20 µs).
- **11 threads for 4 s:** no deadlock, and 0 Windows memory checks in every update.
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
OK
```

The two INIs differ only in `bodyShape`: 0 in Fixed, 1 in Variable. The world features stay at 0 in a release.

## run_analyze.bat

Runs MSVC `/analyze` with all warnings on the plugin, writes `build\analyze.txt` and prints the warning count. Nothing
is built. The 30 it reports are known:

- 22 are C4865 in the Windows SDK's own headers (enum types under `/Wall`);
- 6 are C5219, integers turned into floating point in the log's statistics and the sub-step count;
- 2 are C4365, `int` to `size_t` in the child-list discovery's log.

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
