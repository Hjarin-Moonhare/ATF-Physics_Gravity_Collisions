# The Havok world: facts

Read from Starfield 1.16.244, `Starfield.esm`, the vanilla and A-Body skeleton NIFs and the game's mesh archives,
with the tools in `tools\re\`. **Confirmed in game** marks what TF Physics' logging build saw in play. IDs are Address
Library IDs. Offsets are in bytes. For an ID's RVA in the installed game: `python tools\re\addrlib.py <ID>`.

## 1. From a character to its physics world

| Step | Read | Evidence |
|---|---|---|
| Character → cell | `actor+0xB0` | ID 63431 +0x10: `mov rcx,[rcx+0xB0]` |
| Interior or not | `cell+0x48` bit 0 | ID 63431 +0x1C: `test byte [rcx+0x48],1` |
| Interior: the cell's own `bhkWorld` | the ExtraHavok entry (extra type 1) in the cell's extra list at `cell+0x50`, field `+0x18` | ID 44831 +0x6 (type 1) and +0x12, through the get-by-type ID 37878 |
| Exterior: one shared `bhkWorldM` | `**ID 938080` (a pointer to a pointer) | ID 63431 +0x2D, +0x34 |
| `bhkWorld` → `hknpBSWorld` | `bhkWorld+0x50` | ID 125025 +0x4DC |
| `hknpBSWorld` → `bhkWorld` | `hknpBSWorld+0x9B0` | ID 125025 +0x459 |

- 217 game functions inline this path. ID 63431 is the shortest; foot IK (ID 104442) and furniture checks (ID 101738)
  use it too.
- The exterior world is made once at start-up: ID 62828 calls ID 62787, which builds the `bhkWorldM` (vtable ID
  471430), and stores its holder in ID 938080 at +0x177.
- Space has its own `BSGalaxy::Havok::Physics::bhkSpaceWorld` (vtable ID 470774).
- `bhkWorld`: vtable ID 471072, constructor ID 124980, Havok set-up ID 125025.
- `hknpBSWorld` is an `hknpWorld`: 0xC20 bytes, constructor ID 125059 (it calls the `hknpWorld` constructor ID 160508),
  vtables at +0x00 (ID 471046), +0x18 (ID 471050) and +0x20 (ID 471048).
- **Units:** node world positions are Havok positions. The scale constant (data ID 361100) is (1, 1, 1), so both are
  metres, and the game copies `NiPoint3` positions into its queries unscaled (pick-data set-up ID 43094).

## 2. The world object

**The interface at +0x20** (vtable ID 471048; Havok's functions take `this` = world + 0x20):

| Slot | ID | Gives |
|---|---|---|
| 2 (+0x10) | 125221 | the world's collision filter, the global `bhkCollisionFilter` |
| 3 (+0x18) | 125220 | `[this+0xA80]`, the object every query puts at `query+0x00` |
| 6 (+0x30) | 125224 | `[this+0xA38]`: the motion-properties table at world+0xA58 |
| 11 (+0x58) | 125222 | whether a body ID is valid |
| 14 (+0x70) | 125204 | the `hknpBody*` for a body ID |
| 26 (+0xD0) | 104837 | a motion, from the array at `[this+0x168]` (world+0x188) |

**Bodies:** an array at `world+0x38`, 0xB0 bytes each, the count at `+0x40`.

| Offset | Field |
|---|---|
| +0x00 | rotation rows |
| +0x30 | position. A second part sits at +0x40; ID 161979 adds the two. |
| +0x50 | full body ID, with its serial (the low 24 bits index the array) |
| +0x54 | flags: STATIC 1, DYNAMIC 2, KEYFRAMED 4, ACTIVE 8. A live body has bit 0 or 1 set; a keyframed one reads 0x0E. Foot IK skips flag 0x10, and the projectile line-of-sight collector skips mask 0x5. |
| +0x80 | its motion's index |
| +0x88 | the flag ID 125101 checks before writing a body directly |
| +0x8A | material ID |
| +0x8C | collision filter info (section 9) |
| +0x98 | owner. From a hit body to its reference: owner → vfunc +0x138 → +0x10 → ID 63285 (IDs 56927, 101737) |

**Motions:** an array at `world+0x188`, 0x80 bytes each, the count at `+0x190`.

| Offset | Field |
|---|---|
| +0x10 | centre of mass (3 doubles) |
| +0x58 | motion-properties index (16 bits) |
| +0x5F | non-zero = valid |
| +0x60 | linear velocity, with the motion's own speed limit in w |
| +0x70 | angular velocity (local), with its own limit in w |

**Motion properties:** the table at `world+0xA58`: entries at +0x40, 0x70 bytes each, the count at +0x48.

| Offset | Field |
|---|---|
| +0x08 | gravity factor |
| +0x0C | time factor |
| +0x10 / +0x14 | max linear / angular speed |
| +0x18 / +0x1C | damping |

**Other fields:**
- `+0x9B0` the `bhkWorld`;
- `+0x9C0` Havok's `m_simulationStage` (section 5);
- `+0xB08` the step lock, and `+0xB20` the world lock (section 6);
- `+0xB60` the command queue (section 3).

`bhkWorld+0x7E4` holds the seconds the next physics update simulates: ID 125014 writes it as the leftover at +0x7DC
plus the frame's time. In game it reads about 0.017.

## 3. Changing bodies: the command queue

Every bhk change to a body goes through a world-level wrapper. Each one first asks whether the body can be written now.
If it can, the wrapper writes at once, holding the world lock shared (ID 123862). If not, it queues a command:

| Wrapper | ID | Asks | Queues |
|---|---|---|---|
| SetBodyTransform | 125070 | ID 125101 | `bhkSetBodyTransformCmd` (ID 126157) |
| SetBodyVelocity | 125073 | ID 125100 | `bhkSetBodyVelocityCmd` (ID 126158) |
| ApplyHardKeyframe | 125080 | ID 125101 | `bhkApplyHardKeyframe` (ID 126179) |
| SetMotionType | 125083 | ID 125100 | `bhkSetBodyMotionTypeCmd` (ID 126167) |

- ID 125101 checks the body ID and the body's flag at +0x88.
- SetBodyVelocity's direct path calls Havok's `setBodyVelocity` (ID 160867). A non-zero velocity also activates the
  body (ID 125063).
- **More commands.** RTTI lists 50 of them as `bhkCommandQueue::...::Ubhk<Name>`, among them:
  - `bhkAddBodiesCommand`: queued by ID 126190, called from ID 125678 (adding a physics system to a world) and from IDs
    125103, 122412, 125980;
  - `bhkSetBodyFilterInfo`, `...Layer` and `...Group`: IDs 126168, 126169 and 126170, called from the collision
    object's slots (IDs 125374, 125375, 125376);
  - `bhkReleasePhysicsSystemCommand` (ID 126196, from 125677) and `bhkReleaseCollisionObjectCommand` (ID 126197, from
    42784): freeing goes through the queue too.
- **The queue always defers.** Its constructor (ID 126102) sets its defer flag (+0x1D8) to 1, and nothing else in the
  bhk code (IDs 124950-126400) writes it.
- **Any thread can queue:** commands go in with atomic operations.
- **The flush** is inside `bhkWorld::Update` (ID 125011), before the step: `ProcessCmdBuffers`, with tasks from ID 125062
  and the work in `bhkCommandQueue::ProcessCommands` (ID 126156).
- **Havok's own guard.** Its `checkNotInSimulation` assert (string ID 396773) sits only in world-changing code: ten
  functions, such as the command handlers `CMD_SET_BODY_GRAVITY_FACTOR` (ID 126119),
  `CMD_SET_BODY_MOTION_PROPERTY_FLAGS` (ID 126120) and `CMD_SET_BODY_SURFACE_VELOCITY` (ID 126139). No query function
  checks the stage.

## 4. Keyframing

**Keyframe a collision object, with a warp** (ID 125392): `(co, float transform[16], float stepSeconds, bool* warped)`.
- **The transform:** the rotation rows with w = 0, then the position with w = 1, as the game's node update (ID 125360)
  builds it.
- **The step:** `[bhkWorld+0x7E4]`, in seconds, not 1/dt (confirmed in game).
- **What it does:**
  1. it holds the world lock shared (ID 43095) while it works;
  2. it works out the velocity that reaches the target in one step: the gap × 1/step (ID 161979);
  3. under both of the motion-properties entry's limits (+0x10, +0x14), it sets that velocity (SetBodyVelocity, ID
     125073);
  4. over either one, it sets the transform (SetBodyTransform, ID 125070) with velocity 0, and sets `*warped`.
- **No world, or a step ≤ 0:** it returns false and changes nothing.
- **Users:** the node update (ID 125360), for keyframed bodies on scene-graph nodes: doors, lifts, platforms.

**Plain hard keyframe** (ID 125393): always ApplyHardKeyframe (ID 125080), with no speed check. Used by ID 120007 (space
objects) and ID 124725.

**Two speed limits:**
- Havok's `setBodyVelocity` (ID 160867) holds every velocity to the motion's own limits, the w lanes at motion +0x60
  and +0x70.
- The keyframe function warps only above the motion-properties entry's limits.
- A body that needs more speed than its motion's own limit, but less than the entry's, neither keeps up nor warps: it
  trails its target (confirmed in game).

**A warp on demand:** with a step of 1 ns, any visible gap needs more speed than any entry allows, so the keyframe
function takes its warp path: the body is placed, with velocity 0 (confirmed in game).

## 5. The physics update

- **One job:** `bhkWorld::Update` (ID 125011) runs as one node of the frame's job graph. That's job function ID 97742,
  created in the graph builder ID 97790 at +0x5F7, in another group than the actor-update job (ID 97777, created at
  +0xD97).
- **Also outside the frame:** ID 125015 reaches it from ID 98652, which handles `LoadGame`, and from ID 50113, a cell
  visitor. So physics also runs around loads, before characters have been updated.
- **What it does, per world:**
  1. takes the step lock (ID 123819 at +0x602) and the world lock shared (ID 123862 at +0x60E);
  2. processes the queued commands (`ProcessCmdBuffers`, +0x8A9);
  3. runs the "before" listeners (ID 125147, +0xA22);
  4. runs the step (ID 125013, +0xA4F): collide tasks (vtable ID 471056), listeners, solve tasks (vtable ID 471060),
     listeners, particles;
  5. runs the "after" listeners (ID 125148, +0xAD4);
  6. releases the world lock (+0xED3) and the step lock (+0xF15).
- **Phases,** by their profiler names: BeforeWholePhysicsUpdate; per step BeforeAnyPhysicsStep,
  BetweenPhysicsCollideAndSolve and AfterAnyPhysicsStep; then AfterWholePhysicsUpdate.
- **The stage** at `hknpBSWorld+0x9C0`:

  | Value | Stage | Written by |
  |---|---|---|
  | 1 | DONE | ID 160586; the constructor too (ID 160508 +0x3E8) |
  | 2, 4 | PRE_COLLIDE, COLLIDE | ID 160587 |
  | 8 | POST_COLLIDE | ID 160585 |
  | 0x10, 0x20 | PRE_SOLVE, SOLVE | ID 160588 |
  | 0x40 | POST_SOLVE | IDs 161707, 161745 |

  DONE and POST_SOLVE are named in Havok's own asserts; the other names follow the bit order.
- **Step listeners:** `bhkIWorldStepListener` (vtable ID 429779) has a destructor and 10 callbacks.
  - Add one with ID 125044 `(bhkWorld*, listener*)`: it takes the world's lock at +0x38 and adds the listener if it's
    missing. Remove one with ID 125045.
  - The list lives in the world (+0x18 / +0x20), so it goes away with the world.
  - The game's own: the gravity modifier (vtable ID 454316), the water gravity modifier (429745), space asteroids
    (449415), the character proxy manager (471074), the character rigid-body manager (471082) and the ragdoll
    penetration manager (471787).
  - The gravity service adds one static listener (data ID 829892) to a body's world (ID 104715 → ID 125044) and never
    removes it.
- **World events:** `BGSHavokWorldCreatedEvent` (source vtable ID 413863) is sent when a world is made; two game
  services listen to it. There is no event when a world is destroyed.

## 6. Locks and threads

| Lock | Taken by | Effect |
|---|---|---|
| World lock, shared (`+0xB20`, ID 123862) | every game query, through ID 43095 or inline; `bhkWorld::Update` for the whole update | readers share it: queries don't block each other or the step |
| World lock, exclusive (ID 123863) | 5 places: ID 62770 +0xA18, ID 90508 +0x33, ID 98387 +0x53E, ID 102642 +0x1B9A, ID 125029 +0x14 | excludes readers |
| Step lock (`+0xB08`, ID 123819) | `bhkWorld::Update` for the whole update; also ID 39597 | exclusive and recursive |

- **`BSReadWriteLock`** is 8 bytes: the writer's thread ID at +0, the state at +4 (readers in 0xFFF, waiting writers in
  0x3FF000, the write depth in 0xFFC00000). It uses `GetCurrentThreadId` (ID 223108), `WaitOnAddress` (ID 223586) and
  `WakeByAddressSingle` / `All` (IDs 223583 / 223584).
- **The read-lock helper** ID 43095 `(out, hknpBSWorld*, mode)` locks for reading, unless a thread-local override is set
  (`TLS+0x1D1` set and `TLS+0x1DC` = 2). Only ID 124948 sets it, around Havok's task phases: from the step (ID 125013,
  directly and through ID 124959) and from the task runner (ID 124975, through ID 124970). Threads running physics
  tasks don't lock.
- **The unlock** is inline: `old = atomic add(lock+4, −1)`; when `(old & 0xFFC00FFF) == 1` and `old & 0x3FF000`, it
  calls `WakeByAddressSingle(lock+4)`.
- **Where the bone-modifier hook runs:** in the actor-update job, on BSJobs2 worker threads (`TNodeJob<void(*)()>`,
  vtable ID 449294), several at once. The thread-local override is never set there.
  - NPCs: ID 97777 → 97776 → 103230 → worker tasks ID 103222 → Actor vfunc 0xE4 (ID 100687) → the modifier loop at
    +0x69D (site B).
  - The player: ID 97776 → 103212 → 102695 → PlayerCharacter vfunc 0x13E (ID 102655) → the loop at +0x1035 (site C).
- **The game's own queries right next to the hook:**
  - NPCs: ID 100687 +0x606 calls ID 102423 → 121092 → 125764 (the shared lock, closest points, then a shape cast)
    before the hook.
  - The player: the same at ID 102655 +0x100F before it, and ID 102779 → `bhkWorld::CastRay` (ID 125018, the shared
    lock at +0xE8) at +0x11EA after it.
  - Foot IK casts rays from the animation graph (`BGSFootIkImplementation`, vtable ID 451658: ID 104442 → 104439,
    through ID 125018).

## 7. How bodies enter and leave the world

- **Adding:** `TaskQueueInterface::QueueHavokAdd`'s task (ID 99986) calls AddHavokSubtree (ID 125021). It walks the
  model's nodes (children at node+0x138, a 16-bit count at +0x142). For each collision object it calls the add callback
  (the pointer at data ID 810372, which holds ID 63426). The callback:
  1. calls the object's add-to-world slot (+0x1E8, ID 125368), which reaches ID 125678 and queues the add;
  2. gives every body added by that call one system group (filter info bits 16-31). Bodies in one group don't collide
     with each other.
- **The group is never 0 after this add:** it is the call's group, the layer's fixed group (`[filter+0xB4+layer×4]`), or
  a new number from the counter at `[filter+0x30]`, which skips 0.
- **The biped-part filter** is optional: `FilterBipedPartOnAdd` (vtable ID 451248) compares filter-info bits 8-12.
- **Removing:** RemoveHavokSubtree (ID 125022), through the callback at data ID 810373. It has 13 callers: references,
  actors and cells.
- **Characters go the same way.** Papyrus `ForceAddRagdollToWorld` / `ForceRemoveRagdollFromWorld` (ID 118204) take the
  reference's cell world (interior: `cell+0x50` → ID 44831; exterior: ID 938080) and call AddHavokSubtree or
  RemoveHavokSubtree on the reference's whole 3D (`ref` slot +0x560). Any collision object in a character's 3D enters
  and leaves the world with it.

**Bodies in skeleton data (confirmed in game):**
- Collision objects on A-Body's skeleton nodes are added for every A-Body body, in the same world and group as the
  ragdoll.
- The add puts them on layer 8 (`L_BIPED`), whatever their data says.
- A filter set afterwards through the object's SetCollisionFilterInfo (slot 63, +0x1F8, ID 125374; queued as
  `bhkSetBodyFilterInfo`) holds.
- They get the game's motion properties, not the ones in their NIF data: entry 17 of the table's 80, with 100 m/s,
  200 rad/s, gravity factor 1 and damping 0 / 0.1. Their motions' own limits are 100 m/s and 200 rad/s too.
- Two of them, in two separate one-body systems under `L_Pecs` and `R_Pecs`, report the same motion index.
- The living character's `C_Chest` ragdoll body is in the world, and keyframed.

## 8. What moves a body on a node

- **Scene-graph nodes:** `NiAVObject` UpdateWorldData (slot 79, ID 147243) calls the node's collision-object Update (ID
  125360) when its update data's flag 0x800 is clear. A keyframed body is keyframed to the node (ID 125392); a dynamic
  body's node takes the body's transform.
- **Skeleton bones:** the rig compose (ID 147429, called from `BGSModelNode::Update`, ID 48634) writes each bone's world
  transform straight into the node (+0x80..+0xBF). It doesn't call UpdateWorldData, so no collision update runs.
- **The model's physics component:** every `BGSModelNode` has a `bhkNPModelNodeComponent` at +0x70 (vtable ID 400536),
  made in its constructor (ID 48628).
  - Its RegisterNode (ID 125800) lists each rig bone's body, except those on layer 30.
  - Its Update (ID 125797) runs inside the compose. It teleports the listed bodies to their bones (a queued
    SetBodyTransform) only when the update flags hold 0x4000.
- **0x4000 comes from three Update calls only:** cell attach (ID 57089), the SetOrientation console command (ID 69142)
  and photo mode (ID 92043). The per-frame character update passes 0x1 or 0x11 (set at `0x15cbbce` for the root Update
  at `0x15cbbe5`, in ID 91213).
- **So a body on a skeleton bone is placed with its character, then left where it is:** the game doesn't move it frame
  by frame (confirmed in game).

## 9. The collision filter and layers

**The filter:** the `bhkCollisionFilter` singleton (data ID 937689, vtable ID 471311). Its layer matrix is 64 rows of 64
bits at +0x1B8: the row is a layer, the bit the other layer (read by ID 125618). 64 layers is the limit.

**Filter info bits:** 0-6 the layer; 8-12 the ragdoll part; 14 collide with nothing; 15 the part-group rule; 16-31 the
system group.

**The pair rules** (ID 125616), in order:
1. bit 14 on either body: no collision;
2. group 0 on either body: collision, whatever the layers;
3. different groups: the layer matrix decides (`and rax,[rdi+r10*8+0x1B8]` at +0x64), with a special case for layer 30
   with bit 15;
4. the same group: no collision, except by the ragdoll rules (bit 15 on both, bones that aren't neighbours, the index in
   bits 8-12) and the biped-part table at `filter+0x34`.

**The COLL records:**
- The loader (ID 64716) takes a record's BNAM index when it is under 64 and not set before, and writes only that
  layer's own row, from its CNAM. It doesn't mirror the row into the others. The game toggles pairs in both rows at
  runtime elsewhere (ID 125986).
- `Starfield.esm` has 62 COLL records, indices 0-61 with no gaps, and their CNAM lists are symmetric. Indices 62 and 63
  are free: their rows are 0.
- BNAM is the runtime layer number. The world filter treats layer 30 (CHARCONTROLLER) and the biped layers 8, 32 and 33
  specially, as the records name them.

| # | Layer | # | Layer | # | Layer |
|---|---|---|---|---|---|
| 0 | UNIDENTIFIED | 21 | ACOUSTIC_SPACE | 42 | PATHINGPICK |
| 1 | STATIC | 22 | ACTORZONE | 43 | SHIELD |
| 2 | ANIMSTATIC | 23 | PROJECTILEZONE | 44 | FXCOLLIDER |
| 3 | TRANSPARENT | 24 | GASTRAP | 45 | SPELLEXPLOSION |
| 4 | CLUTTER | 25 | SHELLCASING | 46 | DROPPINGPICK |
| 5 | Weapon | 26 | TRANSPARENT_SMALL | 47 | DEADACTORZONE |
| 6 | PROJECTILE | 27 | INVISIBLE_WALL | 48 | TRIGGER_FALLINGTRAP |
| 7 | SPELL | 28 | TRANSPARENT_SMALL_ANIM | 49 | NAVCUT |
| 8 | *BIPED* | 29 | CLUTTER_LARGE | 50 | CRITTER |
| 9 | TREES | 30 | *CHARCONTROLLER* | 51 | SPELLTRIGGER |
| 10 | PROPS | 31 | STAIRHELPER | 52 | *LIVING_AND_DEAD_ACTORS* |
| 11 | WATER | 32 | *DEADBIP* | 53 | DETECTION |
| 12 | TRIGGER | 33 | *BIPED_NO_CC* | 54 | TRAP_TRIGGER |
| 13 | TERRAIN | 34 | AVOIDBOX | 55 | CLUTTER_NOCUT |
| 14 | TRAP | 35 | COLLISIONBOX | 56 | *CHARBUMPER* |
| 15 | NONCOLLIDABLE | 36 | CAMERASPHERE | 57 | PARTICLE_COLLIDER |
| 16 | CLOUDTRAP | 37 | VOLUME | 58 | PARTICLE_ONLY |
| 17 | GROUND | 38 | CONEPROJECTILE | 59 | AUDIO_OCCLUSION |
| 18 | PORTAL | 39 | Camera | 60 | VEHICLECHASSIS |
| 19 | DEBRIS_SMALL | 40 | ITEMPICKER | 61 | Camera_Only |
| 20 | DEBRIS_LARGE | 41 | LOS | | |

*Italic*: the character layers (the living, the dead, the ragdoll, the character controller).

**Who meets loose items** (from the CNAM lists):

| Layer | Clutter (4, 29, 55) | Static world | Also |
|---|---|---|---|
| 4, 29, 55 (clutter) | yes | yes | |
| 8 `L_BIPED` (the living ragdoll) | no | yes | |
| 30 `L_CHARCONTROLLER` (the character capsule) | yes | yes | 20, 5 and 10; not 19 or 25 |
| 32 `L_DEADBIP` (dead ragdolls) | yes | yes | |
| 33 `L_BIPED_NO_CC` | yes | no | 5, 6, 7, 10, 30, 38, 39 (the camera), 45, 57, 60 |
| 62, 63 | no record: they meet nothing but group-0 bodies (pair rule 2) | | |

**The query filter** `hknpBSCustomCollisionFilter` (vtable ID 395152, 35 references). Its checks are ID 125624 (shape
against shape) and ID 125623 (against a body); ID 125622 filters body pairs.

| Offset | As the game writes it |
|---|---|
| +0x00 | vtable |
| +0x08 | qword 0xFFFFFFFF |
| +0x10 | qword 1 |
| +0x18 | byte 4 |
| +0x20 | the layer mask (64 bits) |
| +0x28 | filter info: the group in the high 16 bits |
| +0x2C | flag byte (0 also skips bit-14 bodies) |

How it decides: a target with bit 14 is skipped when the flag is 0; group 0 always hits; the group at +0x28 is skipped;
anything else hits when the mask has its layer (`bt rax,rdx` at ID 125624 +0x4C). `bhkWorld::CastRay` (ID 125018)
builds this filter when its pick data carries a mask, and uses the world filter otherwise (+0x1B0..+0x206).

**Masks the game uses:**

| Where | Mask | Layers |
|---|---|---|
| Foot IK ray, ID 104439 +0x8D | `0x1C02200E`, plus one optional bit from global ID 883698 | STATIC, ANIMSTATIC, TRANSPARENT, TERRAIN, GROUND, TRANSPARENT_SMALL, INVISIBLE_WALL, TRANSPARENT_SMALL_ANIM |
| Workshop snap, ID 56927 | `0x84008006` | STATIC, ANIMSTATIC, NONCOLLIDABLE, TRANSPARENT_SMALL, STAIRHELPER |
| Placement box, ID 39597 | layer 34's matrix row, plus bit 29 | STATIC, ANIMSTATIC, TRANSPARENT, TREES, TERRAIN, DEBRIS_LARGE, TRANSPARENT_SMALL(_ANIM), CLUTTER_LARGE, STAIRHELPER |

**An actor's group:** ID 101035 returns an actor's filter info (group 6 when it has none). Foot IK puts
`(group << 16) | 30` in its ray (ID 104439 +0x65). GetCollisionFilterInfo (ID 125373) reads a collision object's: the
body's +0x8C, under the shared lock.

## 10. Collision objects

A node's collision object is at `node+0x110`. `bhkNPCollisionObject` (vtable ID 471137):

| Slot | ID | What it does |
|---|---|---|
| +0x138 | 37610 | cast to bhkNPCollisionObject |
| +0x1C0 | 125360 | update from its node |
| +0x1D8 | 125418 | SetMotionType, through ID 125083 |
| +0x1E8 | 125368 | add to world |
| +0x1F8 | 125374 | SetCollisionFilterInfo (slot 63) |
| +0x200 | 125375 | SetLayer |
| +0x208 | 125376 | SetGroup |

GetBodyId is ID 125371, GetCollisionFilterInfo ID 125373.

**From a collision object to its body and world,** the way ID 125373 reads it (confirmed in game):

| Read | Gives |
|---|---|
| `co+0x20` | its physics system |
| system `+0x20` | the system's instance; null when it isn't in a world |
| `co+0x28` | the body's index in the system |
| instance `+0x30` / `+0x38` | the body IDs and their count |
| instance `+0x20` | the `hknpBSWorld` + 0x20 |

## 11. Queries

| Bethesda wrapper (`this` = the world) | Havok function (`this` = world + 0x20) | Arguments after `this` |
|---|---|---|
| GetClosestPoints, ID 125113 | ID 161504 | the query, the query shape's `hkTransform`, the collector |
| CastShape, ID 125111 | ID 161503 | the query, the orientation, the collector, the start collector |
| CastRay, ID 125110 | ID 161502 | the query, the collector |
| QueryAabb, ID 125109 | ID 161506 | the query, the collector |
| QueryPoint, ID 125112 | ID 161508 | the query, the collector |

- The wrappers only start and stop a Havok timer and forward the call. Their names come from the timer strings, such as
  `TthknpBSWorld::GetClosestPoints` (ID 470957).
- None of them takes a lock: their callers take the shared lock first (ID 43095).
- `hknpWorld::getClosestPoints` (ID 161504) calls the broadphase directly: `[world+0x4F0]` slot 20, which for
  `hknpSimdTreeBroadPhase` (vtable ID 359545) is ID 184214. That walks the tree, filters each candidate body and runs
  the narrow phase per body.
- **The game's users:** four functions call ID 125113, and seven more inline it, among them:
  - workshop snapping (ID 56927): a sphere, the custom filter and an `hknpUniqueBodyIdHitCollector`, the shared lock
    at +0x2ED, the query at +0x414;
  - a placement box (ID 39597): an 8-point hull (ID 160304), the custom filter and an `hknpAnyHitCollector`. It takes
    the step lock (+0x4EB) and the shared lock (+0x577); the query is at +0x62E;
  - ID 125764, called from the actor update: closest points (+0x36D), then a shape cast (+0x7EF), under the shared lock
    (ID 43095 at +0x85);
  - IDs 97048, 102781, 104996 and 135824.

**The query** (`hknpClosestPointsQuery`, 0x40 bytes, 16-byte aligned), as every caller fills it:

| Offset | Value | Evidence |
|---|---|---|
| +0x00 | the object from interface slot 3 (ID 125220) | ID 56927 +0x35D |
| +0x08 | the collision filter (the custom filter) | ID 184214 +0x161 |
| +0x10 | uint16 material ID 0xFFFF | every caller |
| +0x14 | uint32 query filter info 0xFFFFFFFF: the filter judges by the target's info | every caller |
| +0x18 | uint64 user data 0 | every caller |
| +0x20 | the word 1 | ID 43094 +0x27 |
| +0x21 | broadphase layers to skip: 0 | ID 184214 +0x1BC |
| +0x22 | broadphase layers to include: 0xFB | ID 184214 +0x1C2 |
| +0x28 | the query shape | ID 184214 +0x117 |
| +0x30 | float maximum distance, clamped to the collector's threshold (`collector+0x10`) | ID 184214 +0x159, +0x16B |
| +0x38 | qword 0 | ID 56927 |

**The sphere:**
- An `hknpInplaceShapeBuffer<0xB0>` on the stack: vtable ID 299516, +0x08 the shape (0), +0x10 a pointer to +0x20,
  +0x18 the capacity 0xB0, then 0xB0 bytes of storage.
- `ID 160356(buffer, &centre, radius in xmm2)` builds an `hknpSphereShape` (constructor ID 160346, vtable ID 299471) in
  that storage and returns it. It is never added to the world.
- The game's clean-up (ID 39597): the buffer's vtable set to `hknpShapeBuffer` (ID 299507), then the shape's vfunc 1
  called with 0.
- The game puts the centre at (0,0,0) and the position in the transform.

**The transform** (`hkTransform`, 64 bytes, aligned): the rows `(1,0,0,0) (0,1,0,0) (0,0,1,0)`, then the translation
(x, y, z, 0). The game copies the identity from globals ID 777942 and ID 777944, and writes the translation at +0x30 (ID
56927 +0xB4..+0xEB).

**The collector** (`hknpAllHitsCollector`, 0x4B0 bytes, aligned; 96 references in 57 functions):
- constructor ID 39679 (rcx = the buffer; 10 results inline);
- slot 1 the destructor (ID 39676, which frees the array if it grew past 10), slot 2 reset (ID 39678), slot 3 the hits
  (ID 36279), slot 4 add a hit (ID 160279: it copies 0x70 bytes, then sets the hit count);
- fields: +0x08 a hint byte (0x40), +0x0C the hit count, +0x10 the early-out threshold (default from global ID 777947),
  +0x20 a flag byte, +0x30 the results, +0x38 the size, +0x3C the capacity.
- The collector base is 0x30 bytes: vtable entry 0 is ID 156189 (Havok's type getter, shared by every collector), entry
  5 is ID 36961.

**A result** (`hknpCollisionResult`, 0x70 bytes):

| Offset | Field | Evidence |
|---|---|---|
| +0x00 | the point | |
| +0x10 | the normal | ID 125764 +0x41F..+0x47F |
| +0x20 | the distance (for closest points; a fraction for rays) | ID 125764 +0x4DA; ID 107147 +0xA |
| +0x48 | the hit body's ID | ID 56927 +0x549; ID 101737 +0x34; ID 104433 +0x67 |
| +0x5C | the hit shape's filter info (layer = `& 0x7F`) | ID 101737 +0x1A..+0x25; ID 107147 +0x2F |
| +0x68 | the query type: 3 closest points, 4 point query, 1 ray | ID 125764 +0x3AD; ID 104433 +0xA |

Batched ray queries exist too: `hknpBatchQueryUtil` (ID 160997), which foot IK uses.

## 12. Contact events

- The game listens to Havok's events through `hkSignal2<hknpEvent, hknpEventHandlerInput>` members:
  `PhysicsContactListenerInstance` (the animation graph's PhysicsContactListener node, vtable ID 468147),
  `HazardTargetFinderHavok`, `FOCollisionListener`, `LoadedAreaBound` and the water manager.
- The events are raised during the step, on Havok's worker threads: by the event creators for manifolds (ID 300282) and
  contact impulses (ID 300266), and by `hknpFireSignalTask`.
- A subscription belongs to a body or a world.

## 13. Physics data in skeletons and worn items

Havok data in NIFs is `TAG0`, SDK 2019.2.

- **A collision object block** is 14 bytes: the target node, u16 flags (0x0080 on all), the physics-system block and
  the body index. One system with N bodies, plus N collision objects on N nodes, is the native pattern.
- **The vanilla skeleton** has 22 collision objects in two systems: the ragdoll's 20 bodies and the controller
  system's 2.
  - **The ragdoll** (`hknpRagdollData`, in a `bhkRagdollSystem`, vtable ID 471328): 20 bodies and 19 constraints, on
    COM, C_Spine, C_Hips, C_Spine2, C_Chest, C_Neck, C_Head, C_BackPack and each side's thigh, calf, foot, biceps,
    forearm and wrist. All 20 are DYNAMIC, on layer 8, with max speeds of 100 m/s and 200 rad/s. They are driven by the
    ragdoll and animation-graph code (`AnimationRagdollDrive`, `hknpRagdollKeyFrameHierarchyUtility`, the
    `AddRagdollHandler` and `RemoveRagdollHandler` graph events), not by the node update.
  - **The controller system:** two STATIC capsules on layer 30, CharacterBumper (radius 224 mm) and
    CharacterController (radius 306 mm, from 0.31 to 1.45 m up).
  - The ragdoll capsules' shapes carry the user data 0x54D67FA5.
- **No soft-tissue bone carries a body,** in vanilla or in A-Body (none of A-Body's 57 added soft bones). The nearest
  is A-Body's C_Chest capsule (radius 158.8 mm): the Pecs sit 60 mm inside it and the nipples on its surface. A-Body's
  skeleton keeps all 24 vanilla physics blocks.
- **Worn items:** only spacesuit backpacks (34 of them) and 2 robot skins carry bodies; no outfit, hat or helmet does.
  Their dropped `_go` models carry 1 DYNAMIC body each, on layer 4 (`L_CLUTTER`).
  - A backpack adds a small ragdoll: a `C_BackPack_Twin` body copying the skeleton's C_BackPack sphere, plus its own
    body hung from it by a ragdoll constraint, both DYNAMIC on layer 8.
  - Creature, robot and turret skins carry their whole ragdoll the same way, through `COM_Twin`. The exe holds the
    strings `COM_Twin`, `ParentBoneName` and `bUseTwinConstraintGroup:Havok`.
- **A one-body system:** `meshes\clothes\spacesuit_ranger_01\spacesuit_ranger_backpack_01_m.nif` (in
  `Starfield - MeshesPatch.ba2`) holds a `bhkPhysicsSystem` with one DYNAMIC capsule (radius 142.9 mm) and its own
  motion properties, with no twin and no constraint.
- **In a body's cinfo** (192 bytes): the motion type at byte 40 (2 DYNAMIC, 1 KEYFRAMED, from the exe's own Havok
  reflection) and the filter info at byte 16.

## 14. How the game pushes things

- **The character capsule** (`bhkCharProxyController` / `hknpCharacterProxy`, layer 30) pushes clutter with a limited
  force: Havok's proxy has `characterStrength` and `characterMass`.
- **Held items (the Z key)** hang on a force-limited spring on a dynamic body: `hknpBSMouseSpringAction` (vtable ID
  471525), tuned by `fZKeyMaxForce`, `fZKeySpringElasticity`, `fZKeySpringDamping`, `fZKeyMaxTranslationSpeed` and
  `fZKeyMaxContactMassRatio`. The item pushes other items, and walls stop it.
- **Animated objects** (doors, lifts) are keyframed bodies on scene-graph nodes, moved by ID 125392: by velocity under
  the max speed, by a warp over it.
- **Seated actors** get special treatment: a collector named `hknpBSIgnorePoseBoundForActorsInFurnitureCollector`
  (vtable ID 451877, add-hit ID 101737).
- **Physics damage:** the `fPhysicsDamage*` settings let fast items hurt the characters they hit.

## 15. The held gun

- **Where it hangs:** the skeleton's `WEAPON` bone, a child of `C_Chest` at (−0.280, 0.068, −0.482) in the chest's
  frame. Its children are `R_HandIk` and `L_HandIk`. `WeaponLeft` is another child of `C_Chest`. The animation moves the
  gun with `WEAPON`, and the hands are pulled to the IK targets on it.
- **The attach node:** Actor vfunc 281 (slot 0x8C8, ID 101299) returns an equip slot's attach-node name. It defaults to
  the global `BSFixedString` "Weapon" (ID 888911, set up by ID 2901); ID 49314 chooses between "Weapon" and "WeaponLeft"
  (ID 888912, set up by ID 2902). `BSFixedString` names compare without case, so "Weapon" is the `WEAPON` bone.
- **The gun model:** its NIF root is named `WEAPON` too (`maelstrom.nif`, `rattler.nif`). Modular parts (barrels,
  magazines, sights) are separate NIFs attached through connect points.
- **Its size each frame:**

  | Field | Offset | Evidence |
  |---|---|---|
  | the whole subtree's bound (world centre, radius) | `node+0x100` / `+0x10C` | ID 147191 +0x68..+0x90; merged by ID 147579 |
  | each BSGeometry's NIF box: centre | `geo+0x130` | BSGeometry LoadBinary (ID 147282, vtable ID 498008 slot 42) +0x166, read when the stream is 0x8A or newer |
  | the box's half-extents | `geo+0x140` | ID 147282 +0x172 |
  | the bounding sphere (centre, radius) | `geo+0x150` / `+0x15C` | ID 147282 +0x12F / +0x145 |
  | the world transform | `+0x80` rotation rows, `+0xB0` translation, `+0xBC` scale | as for any node |

- **Real boxes** (centre; half-extents, in metres): the Maelstrom's body (0, 0.151, 0.055); (0.037, 0.190, 0.081), and
  its fluted barrel (0, 0.104, 0); (0.016, 0.104, 0.016). The long axis is +Y, forward.
- A rifle body's subtree sphere is about 0.2 m. `Maelstrom_Decals` has a 0.68 m sphere, before its 0.26 scale.
- `NiNode::UpdateWorldBound` (ID 147191) leaves a node's bound alone when bit 13 of `node+0x118` is set.
- The first-person gun is on the separate first-person skeleton.

## Hard risks

1. **Never take the step lock** (`hknpBSWorld+0xB08`) from a game job. A worker waiting on it while the step's own
   tasks need workers can stall the frame.
2. **The world lock doesn't keep queries out of the step.** The step holds it shared, and processes the queued
   commands under it too. The order of the actor-update job and the physics job comes from the job graph (ID 97790),
   and its link helpers (IDs 97796-97803, 97808, 97810, 97811, 46127) aren't decoded.
3. **A world can disappear without warning.** There is no "world destroyed" event: a save load, leaving a worldspace or
   a ship cell unloading ends a world. A body ID or world pointer kept across frames then means nothing, and removing a
   body from a dead world is a use-after-free.
4. **Bodies a plugin adds itself need all of the game's lifetime work.** Each add call gets a new system group (ID
   63426), so such a body pushes its own character's capsule on any layer that meets layer 30. It must leave the world
   before the world goes, and follow its character through every change of world.
5. **Contact-event subscriptions are tied to bodies.** The game destroys bodies on cell changes, saves and deaths
   without telling anyone, so a subscription left on a dead body crashes. The handlers run inside the step, on Havok's
   worker threads.
6. **Group 0 collides with everything,** whatever the layers, and passes the query filter whatever its mask. Only a body
   added outside the game's add can have group 0.
7. **COLL rows aren't mirrored.** A mod that overrides a COLL record writes only that record's row, so a new layer's bit
   in the other rows drops out, and the pair collides one way only.
8. **Physics also runs around loads** (`LoadGame`, a cell visitor), before characters are updated. Code inside the
   physics update can't count on this frame's character state.
9. **Only the queued wrappers write a body in a world safely.** A direct write while the world steps races the step.
10. **Two speed limits.** A body that needs more speed than its motion's own limit, but less than its
    motion-properties entry's, neither keeps up nor warps: it trails its target.
11. **The game changes motion types:** 28 game functions set them, some for a whole subtree (death, scripts,
    furniture).
12. **A body moved by velocity pushes items at that speed.** Fast steering launches clutter, and fast items hurt the
    characters they hit (`fPhysicsDamage*`).
13. **Layer 33 (`L_BIPED_NO_CC`)** meets projectiles, spells (6, 7, 38, 45), the camera (39) and character capsules
    (30): a body on it gets shot, nudges the camera and shoves passers-by.
14. **Struct offsets aren't in the Address Library.** `actor+0xB0`, `world+0xB20`, `node+0x110`, `bhkWorld+0x7E4` and
    the rest need a byte check where they're read, or a fresh look after each patch.
