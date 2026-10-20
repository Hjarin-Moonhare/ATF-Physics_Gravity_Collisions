# What I read from Starfield.exe

Everything here was read from Starfield 1.16.244 with the tools in `tools\re\`, and most of it was confirmed in game.
Addresses are RVAs in 1.16.244, with the Address Library ID beside them; use the ID, the RVA moves with every patch.
Offsets are in bytes. Where something is still unknown, it says so.

## 1. Address Library and SFSE

- **The database.** `Data\SFSE\Plugins\versionlib-<major>-<minor>-<build>-0.bin`, format 5: a 0x60-byte header
  (format, game version ×4, name[64], pointer size 8, ID count at +0x5C), then one `uint32` RVA per ID. The ID is the
  index.
- **Every address TF Physics uses moved** between 1.16.236, .242 and .244, so raw RVAs break with each patch.
- **Version independence (SFSE 0.2.21, `PluginManager.cpp`).** A plugin counts as version independent when
  `addressIndependence & (Signatures 1<<0 | AddressLibraryV2 1<<2)` and `structureIndependence & (NoStructs 1<<0 |
  1.14.70 layout 1<<3)`.
  - With any Address Library bit set, SFSE refuses the plugin ("address library needs to be updated") when the
    database for the running game is missing.

## 2. Bone modifiers

**The modifier list.**
- `0x872550` (ID 54892): 3D root → `BoneModifierExtra`, or null. The list starts at +0x28.
- An instance:

  | Offset | Field |
  |---|---|
  | +0x00 | vtable |
  | +0x08 | next |
  | +0x10 | driver node |
  | +0x18 | target node |
  | +0x20 | data record |

**The per-character loop** is `0x872770` (ID 54894). For each instance, it calls update (vtable slot 1) if the driver
is within the record's max animation distance of a global reference node, probably the camera. Otherwise it calls
reset (slot 2).

**How a character's list is built:**
- `0x8725f0` gets or creates the extra on a 3D (+0x28 list, +0x30 count). `0x8717c0` adds one modifier, and skips one
  whose data is already there.
- First the race's per-sex list is added, always, whatever is worn: race +0x5C8 / +0x5D0, 16 bytes per sex, with the
  sex in NPC +0xF0 bit 0.
- Then every model in `actor+0x210` adds its own list. An armour addon's BNAM is stored per sex at ARMA +0x200 +
  sex × 16.
- **Confirmed in game:** a BNAM list is built only from BMODs in the same plugin. A PoseDeformer listed on an armour
  addon does run.

### SpringBone

**Classes.** `SpringBoneInstance` vtable `0x4b512e8` (ID 405175); update = slot 1, `0x877910` (ID 54979). The
constructor is `0x877650`.

**What the solver does:**
- **Frames.** Every per-axis value (Strength, Damp, Scale, MaxDist) acts in the driver's frame, and an empty driver
  means the bone's parent. For a COM driver that's X side, Y fore/aft, Z vertical.
- **Values.**
  - Strength is stiffness: f = √S / 2π Hz.
  - Damp is a damping ratio: c = 2 · Damp · √S.
  - Scale multiplies the velocity once per sub-step (1 = no change).
- **Steps.** RK4, with sub-steps = max(1, ⌈dt · 0.63662 · √(max S)⌉), so soft springs take one sub-step per frame.
- **The cap.** MaxDist clamps the offset per axis to MaxDist × the sub-step time. MaxDist 1 is 1.67 cm at 60 fps,
  3.3 cm at 30, 0.7 cm at 144: the look depends on the frame rate. TF Physics rescales MaxDist each update so the cap
  is the 60 fps size.
- **Damping and motion.** Damping acts on absolute motion, so a steady speed v leaves a lag of 2 · Damp · v / √S. Soft
  springs sit pinned at their cap while walking or running.
- **No write when settled.** Nothing is written while |offset| and |velocity| are both ≤ 0.5 mm.
- **Missing bones.** A spring whose target or driver bone is missing is never created.
- **Defaults** (in the data's constructor): max animation distance 20, Strength 500, Damp 1, Scale 0.25, MaxDist 1000.
- **What is read when.** The update reads Strength, Scale and MaxDist from the shared record on every update. The
  constructor caches the damping per instance (`+0xC0` = 2 · √S · ratio) and the sub-step rate (`+0xD0` = 0.63662 ·
  √(largest S)), so a record's new Strength needs both worked out again for every instance on it.

**Layouts.** `SpringBoneData` holds 3 floats each at:

| Offset | Field |
|---|---|
| +0x20 | Strength |
| +0x30 | damping ratio |
| +0x40 | Scale |
| +0x50 | MaxDist |

The instance:

| Offset | Field |
|---|---|
| +0x30 | rest local transform (4 rows, translation at +0x60): the goal, in the target's parent frame. It writes rest − offset. |
| +0x70 | the simulated world transform |
| +0xB0 | the offset's velocity (driver frame) |
| +0xC0 | damping per axis, set by the constructor |
| +0xD0 | the sub-step rate |
| +0xD4 | reset flag |

- The rest comes from the skeleton's reference-pose table by bone name (`0x873010`), never from the live node.
- **The frame time** the springs use: global ID 936960, loaded at ID 54979 + 0xA9.

### PoseDeformer

**Classes.** `PoseDeformerInstance` vtable `0x4b50ea0` (ID 405151); update = slot 1, `0x874990` (ID 54933); weight =
slot 4.

**What it does:**
- **The weight** comes from the angle between the driver's local axis and Vector. The Axis byte is 0/1/2 for X/Y/Z,
  and Vector is given in the driver's **parent** frame. The weight is 1 inside Inner degrees, falling linearly to 0 at
  Outer.
- **The result.** The target's local position = rest + w · Position (metres), its rotation = rest Euler + w · Angle
  (degrees), and its scale = 1 + w · Scale.
- **It sets the local position outright,** so never put a SpringBone on the same bone.
- **Several on one bone add up.** The first one each frame starts from the cached rest, and later ones read the
  current local. Position and rotation add; scale doesn't (the rows are re-normalised, and the last one wins).
- **Rotations.** The engine keeps rotation rows = the bone's axes. In column terms that's R = Rx(−a) Ry(−b) Rz(−c):
  Angle X turns the target about its parent's X axis.

**Layout.** The data:

| Offset | Field |
|---|---|
| +0x20 | Vector (3 floats) |
| +0x2C | Position (3 floats) |
| +0x50 | Inner (degrees) |
| +0x54 | Outer (degrees) |
| +0x58 | Axis byte |

The instance's rest local translation is at +0x28.

## 3. Where bone writes land

**The frame order per character,** in `0x15cb6e0` (ID 91213):
1. The modifier pass: `PlayerCharacter` vfunc 0x13E (ID 102655) or `Actor` vfunc 0xE4 (ID 100687).
2. The loop `0x872770`.
3. The 3D root's `NiAVObject::Update` (vfunc 0x49).
4. `BSFadeNode::Update` `0x2419b00`.
5. `BGSModelNode::Update` `0x64e750` (ID 48634), which composes the rig's local transforms into world ones and commits
   them.

Anything written in or right after step 2 shows that frame.

**The game's setter** writes both copies: `0x2be9ed0` (ID 147432) for translation, `0x2be9e10` (ID 147431) for
rotation. It writes `node+0x70` and the rig's local slot +0x30.

**The rig** (`BGSModelNode::Rig`, `node+0x20`):

| Offset | Field |
|---|---|
| +0x00 | reference count |
| +0x08 | local buffer: data at +0x10, 0x40 bytes per bone, translation at +0x30 |
| +0x10 | world |
| +0x18 | previous world |
| +0x28 | parents |
| +0x38 | order |
| +0x48 | name map: entries at +0x58, end index at +0x60 |

- A name-map entry is 24 bytes: a BSFixedString key, then a value at +8. The slot is value & 0x3FF; 0x3FF means none.
- Lookup: `0x320ef0` (ID 37355). Release: `0xcec8a0` (ID 68279).
- Some bones are in the rig's flattened table only, with no node under COM. On A-Body's skeleton that's the wrists and
  `Middle1`.

**From OSF's source:** writes inside `AnimationManager::Update` (slot 4) get overwritten by the snapshot applier
(vfunc 7), and writing only the node's local does nothing for bones the rig maps.

## 4. Nodes

`NiAVObject`:

| Offset | Field |
|---|---|
| +0x10 | name (BSFixedString) |
| +0x20 | rig |
| +0x38 | parent |
| +0x40 | local rotation (3 rows × 16 bytes, rows = axes) |
| +0x70 | local translation |
| +0x80 | world rotation |
| +0xB0 | world translation |
| +0xBC | world scale |
| +0x110 | collision object |

**`NiNode` children:** an array pointer at +0x138 (8-byte entries; skip nulls) and a 16-bit count at +0x142. Read from
`NiNode::UpdateWorldBound` (ID 147191). It is **not** a {pointer, uint32 count} pair anywhere in 0x40-0x300.

**Vtables:** `NiNode` `0x4eefb60`, `BSFadeNode` `0x4d6a920`, `BSGeometry` `0x4ef0380`. Check a class at runtime
through vtable[-1] → the CompleteObjectLocator → the type name.

## 5. Characters

- **Which character an update is for.** The loop's first argument is the character's 3D root, and the root doesn't
  point back to its character (nothing within 0x300 bytes). The callers hold it in callee-saved registers:
  - site C has it in `rsi`, around its `call 0x1a3ec60` (get-3D, ID 102735);
  - site B has it in `r12`, around a virtual get-3D (`call [rax+0x558]`).
- **Character → NPC record:** `actor+0x98`. Vtables: `Actor` `0x4cb9248` (ID 451614), `PlayerCharacter` `0x4cbc448`
  (ID 452447), `TESNPC` `0x4bd5a90` (ID 420893).
- **Morph-target weights** (the body sliders that sfee writes, BMPN/BMPV) live in a table at `TESNPC+0x448`:
  - entries at +0x18, capacity (u64) at +0x20, used = [+0x20] − [+0x28];
  - an entry is 24 bytes: a BSFixedString name, the weight (float) at +8, and a slot marker (int32, −1 = empty) at
    +0x10;
  - the writer is `0xce7990` (ID 68207): it clamps weights to 0..1, quantises them to 8 bits and erases zeros. It's
    called from `TESNPC::Load` (vtable slot 17, `0xcd2160`, ID 68102).
- **The body triangle** is Thin, Strong and Overweight at `TESNPC+0x350 / +0x354 / +0x358` (MWGT).
  - `0x34ed40` (ID 37982) builds the body's morph-weight list from the table plus these three by name (the name table
    is ID 409163).
  - (0,0,0) is the base body. Vanilla NPCs use (0,0,0) or (⅓,⅓,⅓). The three are independent (they sum to 0..2).
- **Life state** = (`dword actor+0xF8` >> 17) & 0xF, as `Actor::IsDead` reads it: vtable slot 269 of `Actor` and
  `PlayerCharacter`, `0x18e4540`. Dead = 1 (dying), 2 (dead), 5 (recycled), 7 (an essential character down).
- **Form ID:** `form+0x28` (the Papyrus GetFormID native, ID 117985: `mov eax, [r8+0x28]`).
- **The player's gravity scale,** with its modifiers: `player+0x1138` (GetPlayerGravityScale, ID 65694).
- **What a character wears** (WornCoversBipedSlot, ID 65296): the inventory at `actor+0xA0`, its lock at `+0xA8`, its
  count at `+0x28` and its entries at `+0x30`, 0x28 bytes each: the item at `+0x00`, worn when the byte at `+0x20` has
  any of bits 0-2. An armour (form type 0x22 at `+0x2E`) keeps its biped slots in the 64 bits at `+0x208`.
  HumanRace's slot names: 3 BODY, 5 Backpack, 35 SS BODY, 36 SS Helmet, 37 SS Backpack. Vanilla clothes are one
  piece on 3 (often with 5); a spacesuit's body is on 35.
- **Cell:** `actor+0xB0`. Interior = cell +0x48 bit 0. The exterior world's bhkWorld: ID 938080 is a pointer to a
  pointer.

## 6. Havok (hknp, with Bethesda's bhk layer)

**Units.** Node world positions are Havok positions: the scale constant (data ID 361100) is (1, 1, 1), metres.

**From a node to its body:**

| Read | Gives |
|---|---|
| `node+0x110` | `bhkNPCollisionObject` (vtable ID 471137) |
| co +0x20 → +0x20 | the system's instance |
| co +0x28 | the body's index in it |
| instance +0x30 / +0x38 | the body IDs and their count |
| instance +0x20 | `hknpBSWorld` + 0x20; null = not in a world |

`hknpBSWorld` has vtables at +0x00 (ID 471046), +0x18 (ID 471050) and +0x20 (ID 471048). `world+0x9B0` is the
`bhkWorld`.

**Bodies:** an array at `world+0x38`, 0xB0 bytes each, with the count at +0x40.

| Offset | Field |
|---|---|
| +0x00-0x2F | rotation rows |
| +0x30 | position. Havok adds a second part at +0x40 in doubles; it's tiny in practice. |
| +0x50 | full ID, with its serial |
| +0x54 | flags: STATIC 1, DYNAMIC 2, KEYFRAMED 4, ACTIVE 8. A body counts only with bit 0 or 1 set, so a keyframed one is 0x0E. |
| +0x80 | the motion's index |
| +0x8C | collision filter info: layer in bits 0-6, bit 14 = collide with nothing, group in bits 16-31 |

**Motions:** an array at `world+0x188`, 0x80 bytes each, count at +0x190 (the getter is slot 26 of the +0x20 vtable).

| Offset | Field |
|---|---|
| +0x10 | centre of mass (doubles) |
| +0x58 | motion-properties index (16 bits) |
| +0x5F | non-zero = valid |
| +0x60 | linear velocity, with the motion's **own** speed limit in w |
| +0x70 | angular velocity (local), with its limit in w |

**Motion properties:** a table at `world+0xA58` (slot 6 of that vtable, ID 125224): entries at +0x40, 0x70 bytes each,
count at +0x48.

| Offset | Field |
|---|---|
| +0x08 | gravity factor |
| +0x0C | time factor |
| +0x10 / +0x14 | max linear / angular speed |
| +0x18 / +0x1C | damping |

**Two speed limits.**
- Havok's `setBodyVelocity` (ID 160867) holds every velocity to the motion's own limit (the w lanes).
- The keyframe function (below) decides to warp from the table entry's limits.
- When the two differ, a body can neither keep up nor warp. A-Body's proxies get the game's 100 m/s for both.

**Changing bodies.**
- **The command queue.** Every bhk change to a body in a world goes through the command queue at `world+0xB60`, which
  is flushed in `bhkWorld::Update` (ID 125011) before the step (ID 125013).
- **Wrappers:** SetBodyTransform 125070, SetBodyVelocity 125073, ApplyHardKeyframe 125080, SetMotionType 125083.
  SetBodyTransform and SetBodyVelocity write at once when the body's byte at +0x88 is clear (ID 125101; for a
  velocity, ID 125100 asks every body on the motion, the ring through +0x84), holding only the shared world lock, and
  queue otherwise (IDs 126157, 126158); a last byte argument queues the change whatever the flag. A non-zero velocity
  also activates the body (ID 125063).
- **Keyframe with warp** (ID 125392): `(co, float transform[16], float step seconds, bool* warped)`.
  - The transform is the rotation rows with w = 0, then the position with w = 1, as the game's node update (ID 125360)
    builds it.
  - The step the game passes is `[bhkWorld+0x7E4]`, in seconds (**not** 1/dt): the leftover plus the frame's time,
    from the physics timer (below). With fixed substeps the update simulates only its whole substeps, so a body
    keyframed with it covers part of its gap each update.
  - It computes the velocity that arrives in one step (ID 161979, gap × 1/step). Under the table entry's limits it
    sets that velocity; over them it sets the transform with velocity 0 and reports a warp.
  - With no world, or a step ≤ 0, it does nothing.
- **The physics timer** (ID 125014 `(bhkWorld*, frame time)`): `+0x7CA` fixed substeps (a byte), `+0x7CC` the
  frame's time, `+0x7D4` the substep, `+0x7DC` the leftover after this update, `+0x7E4` the leftover before it plus
  the frame's time, `+0x7E8` this update's substeps: the whole ones that fit in `+0x7E4`, at most `+0x800`. With fixed
  substeps the leftover is `+0x7E4 - n x substep`. For the main worlds ID 99406 runs it (through IDs 99403 and
  124990) before ID 99392 builds the frame's job graph; ID 91213 runs it for the menus' 3D worlds.
- **Plain hard keyframe** (ID 125393): no speed check.
- **SetCollisionFilterInfo(co, u32)** (ID 125374) = slot 63 (+0x1F8) of the collision object's vtable. In a world it
  queues `bhkSetBodyFilterInfo` (ID 126168).

**Bodies in skeleton data.**
- Collision objects on skeleton nodes are added with the character's 3D (AddHavokSubtree ID 125021, removed with
  125022), in the same world and group as the ragdoll.
- **But** the add forces layer 8 (`L_BIPED`) on them, and nothing moves them each frame. They're placed again only on
  cell attach: update flag 0x4000, in `bhkNPModelNodeComponent` (ID 125797).
- **Their motion properties are the game's,** not the ones in the NIF's Havok data. A-Body's proxies get the
  entry for their world (16 or 17 of 80 outside, 9 of 16 in an interior): 100 m/s and 200 rad/s, gravity factor 1,
  damping 0 / 0.1. Their motions' own limits are 100 / 200 too.
- **Both proxies are on the `C_Chest` ragdoll body's motion:** two separate one-body systems, one motion for the
  three, its centre of mass about 15 cm from each.
- The living character's `C_Chest` ragdoll body is in the world, and keyframed.
- The vanilla ragdoll has 20 dynamic bodies on layer 8, with max speeds of 100 m/s and 200 rad/s.

**Layers and the filter.**
- The filter is global ID 937689: a layer matrix of 64 × u64 at +0x1B8.
- The COLL loader (ID 64716) writes only its own row, so a new layer needs both sides set.
- The pair filter (ID 125616) lets **group 0 on either body collide with anything**, whatever the layers.
- Layers 62 and 63 are free. `L_BIPED` (8) never hits clutter (4, 29, 55). `L_BIPED_NO_CC` (33) hits clutter but not
  static.

**Locks.** The world lock is at `world+0xB20` (shared ID 123862, exclusive 123863). The step lock is at +0xB08 (ID
123819): never take it from a hook.

**Queries** (for one-way world collision): `hknpBSWorld::GetClosestPoints` (ID 125113) with a stack sphere (ID
160356) and an all-hits collector (ID 39679). The game itself runs such queries right next to the modifier hook. The
details are in `research\HAVOK_WORLD.md`.

## 7. Cost and threads

- **Threads.** In play the modifier loop runs on about 11 job threads. In menus (the looks menu), only site B runs, on
  the main thread.
- **VirtualQuery** costs about the size of the region it reports, because the kernel walks the pages: 1 MB about 1 µs,
  64 MB 65 µs, 512 MB 0.6 ms, 2 GB 16 ms. One VirtualQuery per update is enough to make the game stutter.
- **A guarded read that faults** (SEH) costs microseconds, and more when a crash logger has a vectored handler
  installed (`tests\fault_cost.cpp`).
- **In a busy city area,** with about a dozen A-Body bodies and a hundred other characters updating, TF Physics'
  logging build spends 55-64 ms per second of play: 5-7 µs per hook call, 11-14 µs per body's collisions.

## Still unknown

- Whether the physics job runs before or after the actor-update job in a frame, or alongside it. The world-collision
  steering measures it from the moves.
- Why the game puts the proxies on the `C_Chest` ragdoll body's motion: whether it does that for every keyframed body
  under a skeleton.
