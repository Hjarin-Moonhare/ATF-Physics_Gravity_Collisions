# Working on TF Physics

Read [docs/DESIGN_RULES.md](docs/DESIGN_RULES.md) first. It lists what keeps the plugin stable, and what breaks when a
rule is skipped.

## Code that goes in

- **Your own code, or code you have permission to use.**
  - No CBPC code: its license doesn't allow porting it to another game without permission.
  - No CommonLibSF, and no new dependencies unless there's no way around it. That keeps it one self-contained file
    that builds with nothing installed.
- **Every new game address** is an Address Library ID in `kAddrRef` or `kWorldAddrRef`, with the 1.16.244 RVA in a
  comment, and gets a start-up check. A failed check switches the plugin, or just that feature, off and logs why.
- **Diagnostics go behind `kDiag`.** A new world feature stays off in the release builds.
- **New settings** get an INI key, with a clamp (NaN-safe) and a comment in `config\TFPhysics.ini`.

## Before sending a change

1. `tests\run_tests.bat` passes. With `STARFIELD_DIR` set, the real-exe checks pass too.
2. `tests\run_stress.bat` passes, with 0 memory checks per update.
3. `tests\run_analyze.bat` shows no new warnings (30 today).
4. `build.bat`, `build_release.bat` and `build_release_collisions.bat` all build with no warnings at /W4.
5. **New behaviour gets a test.**
   - `collide_test.cpp` builds fake game nodes, and has stand-ins for the game's Havok functions.
   - `gravity_test.cpp` and `shape_test.cpp` work out the expected numbers from the design, not from the plugin's
     code.
6. **For anything that touches the game,** run the logging build in game and read its log.

## Style

- Comments say what the game does and why the code does what it does, with units (m, cm, s, degrees) and the offsets
  read.
- Log lines are plain sentences with numbers, so a tester can read them.
