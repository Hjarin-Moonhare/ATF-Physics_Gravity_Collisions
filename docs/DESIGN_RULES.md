# Design rules

These are the rules that keep TF Physics stable in game. Each one comes with how things break without it, and what
the code does instead.

## Game addresses and checks

**1. Every game address is an Address Library ID, checked at start-up.**
- Breaks: a game patch moves code, so a hard-coded address now points into the middle of something else. The plugin
  patches the wrong bytes and the game crashes, or worse, keeps running with corrupted state.
- Fix: `kAddrRef` / `kWorldAddrRef` hold IDs (plus an offset inside a function). At start-up every function, call site
  and vtable slot used is compared against the bytes it was read from. On any mismatch nothing is patched, and the log
  says which check failed.

**2. Rip-relative operands are checked by where they land, not by their bytes.**
- Breaks: a patch moves code without changing it. The 4 bytes of every `call rel32` or `lea [rip+x]` change anyway,
  so a byte check fails on a game that would have worked.
- Fix: work out the operand's target and compare it with the address found by ID (`RelTarget`).

## Reading game memory

**3. Anything the game can free is read with a guarded copy.**
- Breaks: a node found a few frames ago belongs to a character that just unloaded (a cell change, a door). Reading it
  directly crashes the game.
- Fix: `SafeCopy` (a `memcpy` inside SEH) and `ClassAt` (a vtable check done the same way). A bad pointer just
  returns false.

**4. Never ask Windows about memory in anything that runs every update.**
- Breaks: `VirtualQuery` costs about the size of the region it reports, because the kernel walks the pages. On a game
  heap that's milliseconds per call: 16 ms on a 2 GB region. One call per character update is enough to make the game
  stutter badly.
- Fix: `Readable` / `SafeProbe` / `VtableAt` only at start-up and during discovery. Every repeating path uses guarded
  copies. The tests count Windows memory checks per update, and the count must be 0.

**5. Read game structures the game's way.**
- Breaks: scanning a child list without its count reads past the end, and doing that through Windows checks costs
  tens of milliseconds for every new body: a hitch each time characters load in.
- Fix: an NiNode's children are an array at +0x138 with a 16-bit count at +0x142, as the game's own code reads them.
  Read exactly `count` entries, with guarded copies.

**6. Keep nothing across frames that the game can free.**
- Breaks: a body ID or world pointer kept from an earlier frame is reused after the game freed it.
- Fix: look everything up again each frame: node → collision object → body → world.

## Writing bones

**7. Write both copies, before the frame's compose.**
- Breaks: writing only the node's local transform does nothing for bones the rig maps. Writing too late shows a frame
  behind, or gets overwritten.
- Fix: `WriteTranslation` writes the node's local translation and the rig's slot, as the game's own setter does. It
  runs from the modifier hook, before `BGSModelNode::Update` composes the frame.

**8. Add on top of the springs, and remember what was added.**
- Breaks: a settled spring skips its write for a frame, so the node still holds last frame's offset. Adding this
  frame's offset on top doubles it.
- Fix: `BoneState` keeps what was written and added per node. The base is the node minus the plugin's last offset
  (`BaseOf`, `AddOnTop`).

**9. Spring travel is held at its 60 fps size.**
- Breaks: the game caps a spring's offset at MaxDist × the sub-step time, so the same spring travels half as far at
  120 fps and twice as far at 30.
- Fix: before each update, MaxDist is rescaled to original × n / (60 · dt), so the cap is the 60 fps size at any
  frame rate.

## Shapes and pushes

**10. Push only past the natural-pose distance.**
- Breaks: capsules that start at the shoulder or hip joint overlap the breasts or belly in a normal standing pose, so
  the bones are pushed all the time.
- Fix: limb capsules start part-way down the limb (`kColliderStart`). Each soft-bone and limb pair has a rest
  distance taken from natural standing poses, and a contact only pushes past it.

**11. Every push is capped.**
- Breaks: another body's shape arrives deep inside at once (lag, a teleport, a crowd), and the bone is thrown far out.
- Fix: caps per group (breast 5 cm, belly 3, inner thigh 3, glute 2.5), plus `fMaxPushOtherCm` (3 cm) for pushes from
  other bodies.

**12. Bodies share copied numbers only.**
- Breaks: a pointer into another character's skeleton outlives that character. And after a hitch, an old position
  pushed forward by its speed can land metres away.
- Fix: the shared table holds copied shapes, centre, velocity and time, behind a sequence number. Readers skip an entry
  being written, shift it forward by velocity × its age, and pass over anything older than two frames.

**13. The gravity shapes are held to the skin's stretch.**
- Breaks: small or thin breasts stretch more for the same offset, so the high gravity settings overstretch the skin.
- Fix: each body's shape ceilings are scaled by 1 / its strain (the stretch guard). The strain comes from the body
  triangle and the breast sliders.

## Threads and time

**14. State is per body, lookups take no lock, and no lock is held across a game call.**
- Breaks: the hook runs on about 11 of the game's job threads at once. A global lock makes them wait for each other,
  and a lock held across a game call can deadlock.
- Fix: per-body tables keyed by game pointers. Entries are published whole, so lookups need no lock. `BoneLock` is held
  only briefly, and rig writes happen outside it.

**15. No file work on game threads.**
- Breaks: opening and closing the log for each line on a worker thread costs seconds at load-in, when the most lines
  are written.
- Fix: `Log()` only queues the line. A writer thread keeps the file open, and a full queue drops lines (and counts
  them) rather than make a game thread wait.

## Settings and builds

**16. Every INI value is clamped, NaN-safe.**
- Breaks: an out-of-range value, or the text `nan`, divides by zero or pushes the wrong way.
- Fix: every value goes through a clamp written as `!(v >= lo) ? lo : ...`, which also catches NaN.

**17. Release builds compile test features out.**
- Breaks: a world-collision or logging feature left switched on in an INI would run in a public release.
- Fix: `kDiag` and `kCollisions` are compile-time constants. Release builds force those features off whatever the INI
  says.

## Havok (world collision)

**18. The game owns every Havok lifetime.**
- Breaks: a body the plugin creates itself gets no notice when its world is destroyed (a cell change, fast travel), so
  it's left dangling and the game crashes later.
- Fix: the proxy bodies live in the skeleton's own data, so the game adds and removes them with the character.
  TF Physics never creates, adds, removes or frees a body.

**19. Bodies are only written through the game's queued wrappers.**
- Breaks: writing a body while the physics world steps corrupts the step.
- Fix: the layer goes through the collision object's filter setter (vtable slot 63), the motion through the keyframe
  function (ID 125392). These are the game's own wrappers: they take the world's lock, and queue the change for the
  game's flush before the next step whenever the world can't take it right away.

**20. The game puts skeleton bodies on its ragdoll layer: set it back.**
- Breaks: on layer 8 the proxies bump into characters and catch shots.
- Fix: set layer 62 plus "collide with nothing" at runtime for every A-Body body. Re-send it if the game takes it
  back, at most every 0.5 s per body, since the change is queued.

**21. The keyframe function takes a step time in seconds.**
- Breaks: passing a rate (1/dt), or throwing away values that don't look like a rate, means nothing ever moves.
- Fix: pass `[bhkWorld+0x7E4]`, the seconds the next physics update simulates, as the game's own node update does.

**22. A body has two speed limits: warp before the lower one.**
- Breaks: Havok's velocity setter holds a body to its motion's own limit, while the keyframe function only warps
  above the motion-properties table's limit. When the two differ, a fast-moving proxy neither keeps up nor warps, and
  falls metres behind.
- Fix: read both limits. When the speed a step needs is over `[World] fProxyMaxSpeed` or the body's own limit, warp it:
  the keyframe call with a 1 ns step, which takes the game's warp path (placed, velocity 0).

## Tuning

**23. A shared spring record holds only what every body gives it.**
- Breaks: every body on an armour addon shares its spring records, so a per-body value written there (a character's
  or an outfit's factor) would give each body the springs of whichever body wrote last.
- Fix: a record only gets a part's level and bounce speed, the same for every body. The per-body factors (body shape,
  character, outfit, planet) change only the swing and the damping, which are per instance.

**24. A tuned record stays on the tuned path.**
- Breaks: with a tuning taken out, a record keeps the last springs written into it, and its instances keep their
  damping and sub-step rate.
- Fix: a record once tuned is written on every update from its springs as tuned. Untuned, those are the installed
  values, bit for bit.

**25. A level's swing stays out of the damping.**
- Breaks: the extra swing that gives a level its number would also loosen its damping, so the level would move more
  than its number and settle later than its preset.
- Fix: a level is its preset's springs as they are, with the swing on top. Only the body's factors, `fSwing` and
  `fSettle` move the damping.

**26. A gravity level that is set is used as set.**
- Breaks: held to the installed level's largest levels, a level set for every body or for one shape never shows above
  them. Multiplied by the breast size, it shows on bigger breasts as a higher level than the one set.
- Fix: a set level is its own largest level. Only a body it would stretch more than a normal one gets less: x its
  breast size or its stretch guard's scale, the lower of the two, when that is below 1.

**27. A part's gravity shape tops up its springs' own sag.**
- Breaks: a fixed settle on top of the springs' sag goes past the skin's limit on springs with long travel and stops
  short on springs with little.
- Fix: per spring and COM axis, the shape is the rest of the way from the spring's steady sag, as its swing shows it,
  to the part's settle limit. A bone of a part with its shape on gets an offset every frame, a zero one too, so last
  frame's comes off when the body stands up.

**28. A paused frame steps nothing.**
- Breaks: with no time passing the game's springs hold their last bounce, so a pose changed while paused shows a
  frozen bounce, and anything stepped by the frame time stands still or divides by zero.
- Fix: a zero frame time writes no spring, record, damping or velocity. Each soft bone is put at its rest point for
  the pose, from values that need no time, in place of the held bounce; the springs go on from their own state when
  the game runs again.
