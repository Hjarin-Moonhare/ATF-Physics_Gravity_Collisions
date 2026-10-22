# How TF Physics works

Everything is in `src/plugin.cpp`. Its comments give the game layouts each piece reads, with the 1.16.244 RVA next to
each Address Library ID. This page is the map.

## Start-up (`SFSEPlugin_Load`)

1. **The log opens** (logging build only).
2. **The INI is read** (`LoadSettings`). Every value is clamped, NaN-safe. Then the body's dent rig, if it ships one
   (`LoadDentRig`: `DentRig.bin` in `Data\SFSE\Plugins\TFPhysics\`), and its FreeMorph data (`LoadFreeMorph`:
   `FreeMorph.bin` in the same folder).
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
   - `FmResolveAddresses` (with FreeMorph data): the MorphDriver instance and geometry classes by RTTI name, the
     engine's buffer update (its first 44 bytes, compared in 16-byte pieces; through another plugin's jump if one is
     there), the morph table's reads in the weight writer, MorphDB's targets holder, and the per-body morph copies
     setting. The writes stay off unless every one reads as expected.
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
   - gravity goes into each spring's velocity (the part's `[Springs]` key, or its `fSag`), and each spring's share of
     its part's gravity shape is worked out;
   - damping follows the body shape and the tuning;
   - a zero frame time (the game paused) steps nothing: each spring's rest point for the pose is worked out instead.
4. **The game's own loop.** The springs and correctives run.
5. `PostUpdate`:
   - the breast anchors follow the chest's world tilt, with the dead zone, lying, side and face-down detection, the
     thigh check and the per-bone shapes on top of the springs. Each shape takes its level: one set for every body
     or for the shape is its own largest, and the installed one is held at `[Gravity] fMaxBack` .. `fMaxDown`;
   - `ScaleJiggle` scales each spring's swing by the body shape and the tuning; on a paused frame `PausedSettle` puts
     every soft bone at its rest point instead;
   - `AddSettle` puts the other parts' gravity shapes on their bones;
   - `Collide` pushes the soft bones, then `Dents` dents the skin an arm, a hand or a leg still presses (a body with
     a dent rig, see Dents below);
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
| Dent bodies (64) | `COM` | the body's dent bones and the bones they read, found once by name; each region's last solve (moves and turns) and the bones it read | a mutex to find or add a body; a body is updated on one thread |
| Masses (256) | anchor node | each breast's and glute's mass state | one mutex |
| Spring goals (4096) | spring instance | its goal's last world position, whether it's a body spring, a glute's side | the masses' mutex |
| FreeMorph (1) | the player's `COM` | the data's bones found under it, the solve's state (on the worker), the newest records | a mutex per hand-over: the job, the result |
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
- A part's pushes are coupled (`CouplePushes`, `[Collision] bCouplePushes`): no two of its bones' pushes differ by
  more than the part's spread (`kGroupPushSpread`: breast 2 cm, belly 4, inner thigh 5, glute 3.5). A bone with no
  contact of its own follows the pushed ones. Of two pushed bones, the one the move carries further along its own push
  follows the other; in a squeeze (the move would carry both, or take both back against their pushes) they give way
  evenly. Four passes over the part's pairs, then the group's cap again. The belly's, inner thighs' and glutes'
  spreads are wider than their caps, so there it only acts in a squeeze.
- A breast's sphere, cap and spread are x its size (`[Collision] bBreastSize`): 1 + 0.5 Large - 0.5 Small, 0.5 to 1.6.
- A breast is lifted over the arms below it (`LiftBreasts`, `[Collision] bBreastLift`), when a forearm or a hand
  comes within 25 cm of it. Its skin samples (`lift_data.h`: about 460 a side in 3 cm groups, each group with a bound
  so a shape tests the groups first) are placed from the chest, the Pecs anchor and the four bones, with the sliders'
  changes and the bones' sideways and fore-aft pushes. Tight shapes of the arm and hand (21 a side: the upper arm from
  half way down, the forearm, four palm bones, every finger and thumb joint) come from the bones. Along the chest's up
  axis a sample inside a lifter with the limb below needs a lift; a forearm or a level upper arm above caps it. The
  four lifts are solved by projections (80 rounds) within the breast's cap and spread.
- Breast against breast uses the positions without the plugin's gravity offsets: lying on a side moves one breast
  toward the other on purpose.

**Other bodies.** They push through the shared table, capped at `fMaxPushOtherCm` (3 cm) × the body's scale. Dead
bodies are kept up to `fDeadMaxAgeMs` old while they lie still.

## Dents

A body can ship a dent rig ([BODY_INTEGRATION.md](BODY_INTEGRATION.md)): dent bones in its skin, each riding on a
bone of the body and carrying that bone's skin weight in its region (a breast, a glute, a thigh), and `DentRig.bin`
with each region's skin samples, its dent bones and the arm, hand and leg shapes. `src\dents_core.h` holds the
file's reader and the solve, with no game types; `tools\dent_model.py` is the same solve as the design model.

**Per body.** `Dents` runs at the end of `Collide`, on a body whose dent bones and the bones they read were all found
by name under `COM` (in any case; looked for again every 2 s until they are, and checked by name pointer on every
update). The soft bones' world positions are where this frame's springs and pushes put them, and a bone that hangs
on a soft bone (a nipple) moves with it. Up to 64 bodies with a dent rig are kept; an entry unused for 2 s is reused.

**Per region, each update:**
1. The skin samples are placed: each sample's rest position with the sliders' changes, skinned by its bones; its
   normal turned by its strongest bone.
2. The arm, hand and leg shapes the region uses (capsules between bone origins; a fingertip as a tip along the last
   joint's line) are tested only when one comes near the region's bounds.
3. A sample inside a shape needs the inward move along its normal that takes it out (bisection, up to the rig's
   largest need).
4. The wanted dent is the largest need x a smooth bump of the distance to each contact, out to the rig's fall-off,
   smoothed over the samples' neighbours with each contact kept at its need.
5. Only the dent bones under the wanted dent and one ring of their neighbours move. Their inward moves are the least
   squares fit to the wanted dent, with a smoothing term over the bones' lattice, each between 0 and its largest
   move (projected Gauss-Seidel, until no move changes by 0.1 µm). Then a sample dented past its want by over 2 mm
   weighs x4 and a contact short by over 1 mm x2, for up to 4 rounds.
6. A dent bone's local translation is its rest plus its move along its inward line, written to the node and the
   rig's copy like the other bones.

**Version 2 regions** (`TFDENT02`, as the region's flags say):
- **One shared field:** the unknowns are the values of the field's control points (samples of the region); every
  dent bone, on whatever bone it rides, takes its move from the field at its rest place (the points' Wendland
  bumps, normalised), so a vertex dents the same whatever bones it hangs on. The fit is smoothed over the field's
  edges, and each guard round is solved exactly (`BoxQP`: the primal active-set method, a Cholesky solve on the
  free values, warm-started from the round before), with the guard's settings from the file.
- **Tilt:** a dent bone also turns by the slope of the dent around it: omega = g x i, g the gradient of its lattice
  neighbours' moves in its tangent plane at rest, i its inward line, turned into its parent's frame; the angle is
  atan |omega|. The turn moves the skin around the bone by the dent's slope and tilts its normals with it; the fit
  includes that move. The turn and the translation are written together (`WriteTurned`: the node's rows at
  +0x40, the rig slot's rows at +0x00). `[Dents] bTilt` switches it off.

**The write check** (logging build): at the start of a body's dents, each dent bone written last frame with a dent
over 2 mm is compared with where its parent's world and the local written put it, and a turned one's axes with
its parent's axes and the local axes written (both from the same compose). The 5 s log counts the ones where
written, the ones back at rest and the turns that came out the other way round.

**Over time.** A dent follows its solve within `[Dents] fRelaxTime` (a share of 1 - exp(-dt / fRelaxTime) per
frame), its depth x `[Dents] fScale`. A region none of whose bones moved more than 0.3 mm since its last solve, at
the same sliders, keeps that solve, so a still pose costs no solve. A dent bone whose dent has ended is written back
to its rest once. On A-Body's rigs `dent_test` measures about 120 µs per region solve, and about 270 µs per
solve with a contact; a version 2 region with a contact takes longer (its fit has up to 10 rounds).

## Breast and glute masses (`breast_mass.h`)

`bm::Step` is one damped mass per breast and per glute, in a frame of three axes (the chest's for a breast: X along the
spine, Y forward, Z to the left; COM's for a glute: up, forward, left), stepped in 1/240 s sub-steps. It is driven by
the anchor's acceleration alone: standing still or moving at a steady speed it rests. Each axis has its own frequency
and damping ratio, a soft limit on each side (25 times stiffer past it) and a hard stop at 1.35 times the soft limit.
A jump past its acceleration limit, or the anchor over 3 m from where its velocity puts it, starts the mass again at
rest. After the game's update, `BreastMass` adds the breast's offset to the anchor's local position with the gravity
offset, and `GluteMass` adds the glute's to `L_Butt` / `R_Butt` (in their parent's frame), each x the part's jiggle
level and `fScale`. The states sit in a table of 256 slots keyed by the anchor node, under one mutex.

## Relative damping

The game's spring solver damps a spring's offset against its bone's motion in the world, so moving at a steady speed v
leaves a lag of c v / S (c its damping, S its stiffness), and soft springs ride at their travel cap. Before the game's
update, `RelativeDamping` works out each body spring's goal (its parent's world transform x its rest local) and that
goal's velocity from the last update, and adds c x that velocity (in COM's axes, the spring's own frame) to the
spring's offset velocity (`bm::RelativeKick`). The damping then acts on the motion relative to the body. Only springs
whose target is `TF_*` or `*_Nipple` take it. A goal that moved over 5 m in a frame (a cut, a teleport) is skipped.
The goals sit in a table of 4096 slots keyed by the spring instance; the same pass finds the glute anchors (the
`TF_Glute_*` springs' parent).

## FreeMorph (`freemorph_core.h`)

1. **Binding.** The body's MorphDriver record for the free morph (driver `COM`, MaxAnimationDistance -1) is on its
   naked body's addon; the game binds it to the body geometry that has the morph and copies its weight (instance
   +0x28) into the geometry's performance-morph row every frame. `Collect` finds it among the modifiers (by the
   instance's class and the morph name its data holds).
2. **Every frame, for the player's body** (from `Collide`, after the pushes): `FmUpdate` holds the weight at 1, finds
   the morph's targets buffer from the geometry (`FmResolve`: the geometry's morph row, the morph table's column, the
   row's handle, its holder, the buffer, its size, usage, stride and count all checked against the data), finds the
   data's bones under `COM` once (by name, and the soft bone above each), and hands their world transforms, with this
   frame's spring and push moves, and the sliders to the worker thread.
3. **The worker** (`FmWorker`) runs `fm::Frame`: per breast a limb comes near, the nodes, the limb shapes, the
   candidates, then up to 12 Gauss-Seidel sweeps warm-started from the last frame; it writes the records (`fm::Records`:
   the f16 position delta, the vertex colour, the normal and tangent turned as the deformed skin turns them) and posts
   them. A frame it is still busy with is skipped; a still pose settles to one sweep.
4. **The upload.** The update thread takes the newest finished records once and writes all of them with the engine's
   own buffer update (staged, then copied to the GPU by its upload service). A write that can't happen yet stays due.
5. **Modes** (`[FreeMorph] iModeKey`, none by default): contact, a 1 cm dent going round the left breast, the file's own
   records, off (weight 0).

Costs measured in game: the solve 0.25 to 1 ms on its thread while the hands move (5 ms at most), about 0.2 ms once a
hold has settled; 10 to 12 µs a frame on the update thread.

## Build variants

| Define | `kDiag` | `kCollisions` | What's in it |
|---|---|---|---|
| none (`build.bat`) | true | true | everything: log, statistics, samples, collisions, dents, world collision |
| `TFP_RELEASE` | false | false | gravity, springs, body shape, the masses, the relative damping |
| `TFP_RELEASE` + `TFP_RELEASE_COLLISIONS` | false | true | the above + the collisions, also between bodies, the dents and FreeMorph (the build A-Body 2.0 ships) |

The release builds also force the world collision and the snapshots off, whatever the INI says. The tuning files work
in every build.

## The log (logging build)

`Documents\My Games\Starfield\SFSE\Logs\TFPhysics.log` starts with every check's result. Then, every 5 s:
- calls per site and the plugin's own time (ms per second of play, µs per update), split by part;
- springs held, gravity applied, body-shape scaling;
- collisions: bodies checked, pushes, contacts by pair with the deepest, other bodies and the shared table;
- dents: body updates with a dent rig and their cost, how many were dented, the deepest dent, dent bone move and
  turn, regions solved and regions kept as they were, and the write check;
- the breast and glute masses (steps, the largest offset per axis) and the relative damping (springs kicked, the
  largest steady lag taken off);
- FreeMorph: the mode, the weight and record writes and their cost, the solves on the worker thread (time, frames
  skipped), each breast's contacts, held and sliding points, press and rise, the buffer, and any write refused, with
  why;
- one gravity sample;
- the world-collision lines.

[ENGINE_NOTES.md](ENGINE_NOTES.md) has what's known about the game structures behind all this.
