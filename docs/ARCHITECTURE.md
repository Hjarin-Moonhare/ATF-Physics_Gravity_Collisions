# How TF Physics works

Everything is in `src/plugin.cpp`. Its comments give the game layouts each piece reads, with the 1.16.244 RVA next to
each Address Library ID. This page is the map.

## Start-up (`SFSEPlugin_Load`)

1. **The log opens** (logging build only).
2. **The INI is read** (`LoadSettings`). Every value is clamped, NaN-safe.
3. **Game addresses resolve** from the Address Library database the running game would use
   (`ReadAddressLibrary`, `ResolveAddresses`). There are 25 core entries in `kAddrRef`, each an ID plus an offset for
   a place inside a function. Optional ones live in `kWorldAddrRef`; when one of those fails, only that feature turns
   off.
4. **Checks.** Each one runs on the game in memory:
   - `CheckGame`: the first bytes of every function used, each call site and the register blocks around it, and the
     modifier classes' update slots. Rip-relative operands are checked by where they land, never as bytes, since they
     change whenever code moves.
   - `CheckSizeReading`: the morph-table code.
   - `CheckDeadReading`: `Actor::IsDead`, slot 269 of both character vtables.
   - `ResolveTuneAddresses`: the three reads behind the tuning files (the player's gravity scale, a form's ID, what a
     character wears), each by the bytes of the game function that reads it. A read whose check fails is off.
   - `ResolveWorldAddresses` (logging build, `[World]` on): the collision object and world classes by RTTI name, the
     two Havok-side functions it calls, and the getters of the motion tables.
5. **The tuning files are read** (`LoadTuning`, [TUNING.md](TUNING.md)).
6. **The hooks go in** (`InstallHooks`), only if everything passed. Otherwise nothing is patched and the log says why.

## The hook

The game updates a character's bone modifiers in one loop: ID 54894, RVA `0x872770`. It walks the character's
`BoneModifierExtra` list and calls each instance's update (vtable slot 1) when its driver is near the camera, or its
reset (slot 2) when it isn't. Three call sites call that loop:

| Site | Where (ID + offset) | Who |
|---|---|---|
| A | 83224 + 0x774 | never seen firing |
| B | 100687 + 0x69D (`Actor` vfunc 0xE4) | NPCs; up to ~9,500 calls a second in crowds |
| C | 102655 + 0x1035 (`PlayerCharacter` vfunc 0x13E) | the player, about once per frame |

**The thunks.**
- `InstallHooks` allocates one page within ±2 GB of the sites and writes three 17-byte thunks.
- Each thunk copies the caller's character register into `r8`: `mov r8, r12` for B, `mov r8, rsi` for C, `xor r8d,
  r8d` for A. Both registers are callee-saved and hold the character there.
- It then jumps to `Hooked<Site>(owner, referenceNode, character)`.
- Each site's `call rel32` is then pointed at its thunk.
- `owner` is the character's 3D root.

**`Hooked<Site>`, per call:**

0. `MaybeReloadTuning`: every 2 s the first thread through checks the tuning files, and reads them again on a change.
1. `Collect`: walk the modifier list (`BoneModifierExtra` +0x28, instance +0x08 next).
   - A body is "ours" when the list holds TF Physics' face-up gravity correctives: PoseDeformers on `L_Pecs` /
     `R_Pecs` driven by `COM`, recognised by their record values (`IsOurFaceUpRecord`).
   - Its springs are the SpringBones driven by the same `COM`.
2. `BodyFactorsOf`: the body's size and shape, from its NPC record (`actor+0x98`). It reads the morph-weight table at
   `TESNPC+0x448` and the body triangle at `+0x350`; for the tuning, the character's form IDs, what it wears and the
   player's gravity scale. Cached per body and read again every 2 s.
3. `PreUpdate`, before the game's own update:
   - a tuned part's springs (strength, MaxDist) go into its records, with the instance's sub-step rate (a level's swing
     stays out of the damping);
   - each spring's MaxDist is set so its cap is the 60 fps size;
   - gravity goes into each spring's velocity;
   - damping follows the body shape and the tuning.
4. **The game's own loop.** The springs and correctives run.
5. `PostUpdate`:
   - the breast anchors follow the chest's world tilt, with the dead zone, lying, side and face-down detection, the
     thigh check and the per-bone shapes on top of the springs, each shape held at its largest level
     (`[Gravity] fMaxBack` .. `fMaxDown`);
   - `ScaleJiggle` scales each spring's swing by the body shape and the tuning;
   - `Collide` pushes the soft bones;
   - `AddOnTopAll` writes the per-bone results;
   - `Share` publishes this body's shapes for the bodies near it.
6. **Logging build only:** `WatchProxies` (the world collision) and `MaybeLog`. Every 5 s the log thread writes what
   happened.

A body that isn't ours only runs `ShareOtherBody`. It lends its limbs and torso to A-Body bodies within 3 m, and nothing
of its own is changed.

## Writing bone positions

**Where.** A modifier's result has to land in two places: the node's local translation (`node+0x70`) and the rig's copy
of it (the skeleton's flattened bone table). That's what the game's own setter does (ID 147432, `0x2be9ed0`).
`WriteTranslation` / `WriteRigCopy` do the same: take a rig reference, find the bone, write the 64-byte slot's
translation, release the reference.

**When.** The plugin's writes land after the modifier loop and before `BGSModelNode::Update` (ID 48634) composes local
into world for that frame, so they show the same frame.

**On top of the springs.** `BaseOf` takes what the spring wrote this frame, and `AddOnTop` adds the plugin's offset on
top. `BoneState` remembers what was written and added per node, so a frame where the spring skips (no write while the
offset and velocity are under 0.5 mm) doesn't double the offset.

## State and threads

**Threads.** In play the hook runs on about 11 of the game's job threads at once. In menus, only site B runs, on the
main thread. All state is per body, keyed by game pointers:

| Table | Key | Holds | Locking |
|---|---|---|---|
| `g_springInfo` (2048) | `SpringBoneData*` | original springs, the installer setting they match, part, side, bone, body-shape ceilings, damping floors | reads lock-free; adding takes a mutex; an entry is published whole |
| Bone states (16384) | node | written / added offsets | `BoneLock`, held briefly |
| Body nodes | `COM` | every bone the collisions use, found once by name | a mutex on insert; checked again by name pointer |
| Factors | 3D root | size, shape and stretch-guard factors | 4-way cache |
| Other bodies (128) | `COM` | copied shapes, centre, velocity, time | seqlock: readers skip odd or stale entries and shift by velocity × age |
| `g_tunes` (2) | none | what the tuning files say | two copies: a reload fills the one not in use, then switches (`g_tuneNow`, release / acquire); one reload at a time (`g_tuneBusy`) |

**Counters** are relaxed atomics. The log is a queue drained by its own thread, with the file kept open, so a worker
thread never waits on disk.

## Reading game memory

**When an object might not be what it was.**
- A node found frames ago, another character's body, anything Havok-side: read with `SafeCopy`, a `memcpy` inside SEH.
  A bad pointer gives `false`, not a crash.
- `ClassAt` checks a vtable the same way.
- Objects the game handed to this call (the modifier list, its instances) are read directly.

**Never in anything that runs every update.**
- `Readable` asks Windows (`VirtualQuery`, with a per-thread region cache). It's only for discovery: the first look at
  a layout, a load-time check.
- `VirtualQuery`'s cost grows with the region it reports, because the kernel walks the pages: 1 MB about 1 µs, 64 MB
  65 µs, 2 GB 16 ms. Calling it on every update makes the game stutter. `tests\vq_bench.cpp` measures it.

**Child lists.** An NiNode's children are an array at `+0x138` (8-byte entries) with a 16-bit count at `+0x142`
(`kGameChildLayout`). New layouts are found once on the breast anchor by `DiscoverLayout`.

## Collisions

**Shapes.**
- Soft bones are spheres (`collision_data.h`: 24 bones in 4 groups: breast, belly, inner thigh, glute).
- Limbs are capsules (upper arm, forearm, hand, thigh), trimmed so they start part-way down the upper arm and thigh.
  At their joint ends they gave false contacts.
- Every body also shares its torso, calves and feet.

**Rules.**
- A contact pushes only past the pair's natural-pose distance, so nothing moves at rest.
- Pushes are capped per group (breast 5 cm, belly 3, inner thigh 3, glute 2.5).
- Breast against breast uses the positions without the plugin's gravity offsets: lying on a side moves one breast
  toward the other on purpose.

**Other bodies.** They push through the shared table, capped at `fMaxPushOtherCm` (3 cm) × the body's scale. Dead
bodies are kept up to `fDeadMaxAgeMs` old while they lie still.

## Build variants

| Define | `kDiag` | `kCollisions` | What's in it |
|---|---|---|---|
| none (`build.bat`) | true | true | everything: log, statistics, samples, collisions, world collision |
| `TFP_RELEASE` | false | false | gravity, springs, body shape |
| `TFP_RELEASE` + `TFP_RELEASE_COLLISIONS` | false | true | the above + collisions between bodies (A-Body 1.5) |

The release builds also force the world collision and the snapshots off, whatever the INI says. The tuning files work
in every build.

## The log (logging build)

`Documents\My Games\Starfield\SFSE\Logs\TFPhysics.log` starts with every check's result. Then, every 5 s:
- calls per site and the plugin's own time (ms per second of play, µs per update), split by part;
- springs held, gravity applied, body-shape scaling;
- collisions: bodies checked, pushes, contacts by pair with the deepest, other bodies and the shared table;
- one gravity sample;
- the world-collision lines.

[ENGINE_NOTES.md](ENGINE_NOTES.md) has what's known about the game structures behind all this.
