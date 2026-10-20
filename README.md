# TF Physics

TF Physics is the SFSE plugin that runs A-Body's body physics in Starfield. It hooks the game's bone-modifier pass,
where the game runs each character's springs (SpringBone) and pose correctives (PoseDeformer), and adds what those
can't do by themselves.

It's one C++20 source file with no dependencies: no CommonLibSF, no SFSE headers, no CBPC code. Every game address
comes from the Address Library, and everything the plugin relies on is byte-checked at start-up. If anything doesn't
match, it patches nothing and says why in its log.

## What it does

- Breast gravity from the chest's real tilt: standing, bent over, lying on the back, a side or the front, on all
  fours. Each pose has its own shape: slump, side spread, face-down squash, teardrop.
- Every A-Body spring held to its 60 fps travel at any frame rate.
- Gravity inside the breast, belly and butt springs.
- Breast size, the body triangle (thin / muscular / heavy) and the part sliders scale jiggle and gravity per part, with
  a stretch guard so small or thin breasts don't overstretch.
- Tuning files: each of the 8 parts (breasts, butt, belly, thighs, hips, calves, arms, torso) can take a level in
  even steps of Realistic's movement (Minimal x0.25, Lower x0.5, Realistic, Low x1.25, Medium x1.5, High x2, Max x2.5,
  Ultra x3), its own travel limit, swing, per direction and per bone, settle, bounce speed and sag. Factors per
  character, per outfit and for the planet's gravity go on top. The files are read again while the game runs. See
  [docs/TUNING.md](docs/TUNING.md).
- The gravity shapes' largest levels are settings, and the breast gravity level can be set for every body.
- Collisions:
  - upper arms, forearms, hands and thighs push the breasts;
  - forearms, hands and thighs push the belly;
  - forearms and hands push the glutes;
  - the breasts push each other, and other characters' bodies push too.
- World collision (logging build only): two Havok proxy bodies per A-Body body, steered to the breasts every frame.
  They collide with nothing yet. See [docs/WORLD_COLLISION.md](docs/WORLD_COLLISION.md).

## Status

- TF Physics 1.3 ships in A-Body 1.5. It's the `build_release_collisions.bat` build.
- This source is 1.3 plus the tuning files (every build) and the world-collision work (logging build).
- It was read from Starfield 1.16.244. SFSE loads it on any game version that has an Address Library database, and
  the start-up checks decide whether it runs.

## Requirements

- SFSE 0.2.21 or newer, and Address Library for SFSE Plugins.
- A body with TF Physics' bones and modifiers. Today that's A-Body, which ships the DLL and its INI. See
  [docs/BODY_INTEGRATION.md](docs/BODY_INTEGRATION.md).

## Building

Visual Studio 2022 or newer, or its Build Tools, with "Desktop development with C++".

| Script | Output | What it is |
|---|---|---|
| `build.bat` | `build\TFPhysics.dll` | Logging build: a log every 5 s, collisions, world collision. Goes with `config\TFPhysics.ini`. |
| `build_release.bat` | `build\release\TFPhysics.dll` | Release: gravity, springs, body shape. |
| `build_release_collisions.bat` | `build\release_col\TFPhysics.dll` | Release with collisions (what A-Body 1.5 ships). Goes with `config\release\`. |

The scripts find Visual Studio through vswhere, or use a Developer Command Prompt you already have open. CMake works
too, and puts the three DLLs in `build\cmake\dll_logging`, `dll_release` and `dll_release_col`:

```
cmake -S . -B build/cmake -A x64
cmake --build build/cmake --config Release
ctest --test-dir build/cmake -C Release --output-on-failure
```

## Testing

None of the tests need the game ([tests/README.md](tests/README.md) has the details):
- **`tests\run_tests.bat`:**
  - collisions on A-Body's skeleton in ten poses, other and dead bodies;
  - the world proxies, against stand-ins for the game's Havok functions;
  - gravity and body shape;
  - the tuning files: each level against the preset table, Ultra, the match, the travel limits, back to the
    installed springs bit for bit, the multipliers, characters, outfits, the planet, the files read again;
  - the gravity shapes' largest levels from the INI, and a breast gravity level for every body;
  - the Address Library reader, and every start-up check on a fake game image.

  If it finds the game (through `STARFIELD_DIR`, or the usual Steam folders), it also runs the checks on the real
  `Starfield.exe`, read only.
- **`tests\tune_dump.exe <TFPhysics.ini>`** (built by `run_tests.bat`): how a TFPhysics.ini and its tuning files
  are read, with the plugin's own reader.
- **`tests\run_stress.bat`:** many bodies on 11 threads, the memory guards, and the cost per body.
- **`tests\run_analyze.bat`:** MSVC `/analyze`. 30 known warnings.

## Trying a build in game

1. Install A-Body.
2. Replace `Data\SFSE\Plugins\TFPhysics.dll` with yours. For the logging build, also replace `TFPhysics.ini` with
   `config\TFPhysics.ini`. Tuning files go in `Data\SFSE\Plugins\TFPhysics\` ([docs/TUNING.md](docs/TUNING.md)).
3. Play, then read `Documents\My Games\Starfield\SFSE\Logs\TFPhysics.log`.

## Layout

| Folder | What's in it |
|---|---|
| `src\` | `plugin.cpp`, plus three generated data headers: collision sizes, body-shape limits and the physics presets' springs. |
| `tests\` | The offline tests, A-Body's skeleton in ten poses (`tests\cases\`), and the scripts that run them. |
| `config\` | The logging build's INI, and A-Body 1.5's release INIs (Fixed and Variable presets). |
| `docs\` | The design rules, how it works, the engine notes, the body requirements, the tuning files and world collision. `docs\research\` holds the game facts behind them, with the hard risks. |
| `tools\` | `vcvars.bat` for the scripts, `package_build.py`, `re\` (read-only tools for `Starfield.exe`), and the scripts that measured the data headers (they need A-Body's own build tools). See `tools\README.md`. |

## How it works, in short

1. The game updates each character's bone modifiers in one function, called from three places. The plugin points
   those calls at small thunks that pass the character along and call the original in between.
2. **Before the game's update,** for an A-Body body:
   - each spring's travel limit is set to its 60 fps size;
   - a tuned part's springs go into its spring records;
   - gravity is added to the springs' velocities;
   - damping follows the body shape and the tuning.
3. **After it:**
   - the breast anchors follow the chest's tilt, with per-bone shapes;
   - the jiggle is scaled per part (body shape and tuning);
   - collisions push the soft bones.

   Writes go where the game's own setter writes: the node's local transform and the rig's copy.
4. **Every other character** only lends its shapes to the A-Body bodies near it.

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the details, and [docs/ENGINE_NOTES.md](docs/ENGINE_NOTES.md) has the
game structures behind them.

## Contributing

- [docs/DESIGN_RULES.md](docs/DESIGN_RULES.md) lists the rules that keep the plugin stable.
- [CONTRIBUTING.md](CONTRIBUTING.md) covers how to check a change.
- **Help is welcome on:**
  - world collision: items, push-back, NPCs;
  - a data-driven body definition, so other bodies can use TF Physics. For example, ones built on SF Extended
    Skeleton.
- **Known limits:** crossed arms clip through, sitting doesn't move the belly, and deep squats are only partly handled.

## Thanks

- **SFSE**, and **Address Library for SFSE Plugins**.
- **OSF**, whose source showed where animation writes land, and which ones the game overwrites.
- Starfield and Havok belong to their owners. TF Physics calls the game's own functions; it contains no Havok code.

## License

TF Physics is Copyright (c) 2026 TF_Skeleton, under the MIT License (the LICENSE file).
