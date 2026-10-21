# The frame and bone writes: facts

Read from Starfield 1.16.244 with the tools in `tools\re\`, and from OSF's public source (ozooma10's `osf-animation`,
`osf-cbpc` and CommonLibSF fork: facts only, no code). **Confirmed in game** marks what TF Physics' logging build saw
in play. IDs are Address Library IDs; addresses are 1.16.244 RVAs. `docs\ENGINE_NOTES.md` has the layouts.

## 1. One character's update, in order

In the per-actor update (`0x15cb6e0`, ID 91213):

1. **The bone-modifier pass.** The player's runs in PlayerCharacter vfunc 0x13E (ID 102655), which calls the modifier
   loop (ID 54894) at +0x1035: site C. An NPC's runs in Actor vfunc 0xE4 (ID 100687), at +0x69D: site B. The loop runs
   each modifier's update: the springs (SpringBone) and the pose correctives (PoseDeformer). A third caller, ID 83224 at
   +0x774 (site A), has no character to hand over.
2. **The actor's 3D update:** at `0x15cbbe5`, `NiAVObject::Update` (vfunc 0x49) on the actor's 3D root, with the update
   flags 0x1 or 0x11.
3. **The compose:** `BSFadeNode::Update` (`0x2419b00`) calls the fade node's `BGSModelNode` (+0x180) vfunc 2 with
   `&fadeNode->local`. That is `BGSModelNode::Update` (ID 48634): its rig compose (ID 147429) turns the rig's local
   transforms into world transforms and commits them.

- Nothing writes the rig between the modifier pass and the compose, and both come before skinning. A rig write in or
  right after step 1 shows that same frame.
- OSF hooks `BGSModelNode::Update` and writes at the start of step 3.
- `BGSModelNode::Update` also runs for skeletons whose springs didn't run that frame (AI-frozen actors, per OSF), and for
  every skeleton that isn't an actor's.
- **The character can't be reached from its 3D root:** nothing within 0x300 bytes of it points back. The loop's callers
  hold the character in callee-saved registers: site C in `rsi`, around its get-3D call (ID 102735); site B in `r12`,
  around a virtual get-3D (`call [rax+0x558]`).
- **Threads:** in play the loop runs on about 11 job threads. In menus (the looks menu) only site B runs, on the main
  thread (confirmed in game).

## 2. Where bone writes survive

- **The rig's copy is what counts.** The compose reads `rig->local`. The node's own local transform (`node+0x40`
  rotation rows, `node+0x70` translation) is a mirror that other code reads.
- **Writes inside `AnimationManager::Update`** (slot 4) are overwritten afterwards by the snapshot applier (vfunc 7)
  (from OSF's source).
- **A node's local transform alone** does nothing for a bone the rig maps (from OSF's source).
- Some bones live only in the rig's flattened table, with no node under COM: on A-Body's skeleton, the wrists and
  `Middle1`.

## 3. The game's setters

| Setter | ID | What it writes |
|---|---|---|
| Set local translation `(NiAVObject* node, const float t[3])` | 147432 | `node+0x70..0x7B`. Then it locks the rig at `node+0x20` (reference count +1), finds the bone's slot by name (ID 37355), writes `rig->local->data[slot]` at +0x30, and releases the rig (ID 68279). |
| Set local rotation `(NiAVObject* node, const NiMatrix3*)` | 147431 | the same two copies, for the rotation rows |

- The SpringBone update writes through the translation setter. The PoseDeformer update (`0x874990`, ID 54933) does the
  same steps inline.
- A name-map slot of 0x3FF means the rig has no such bone.

## 4. Springs and correctives

- **On A-Body, the game's own SpringBones drive the `TF_*` bones:** the body's bone-modifier records are live, not inert
  editor data (confirmed in game).
- **A spring's rest** (the +0x30 block, translation at +0x60) comes from the skeleton's reference-pose table, looked up
  by bone name (`0x873010`), never from the live node. So an offset written to a bone never ends up in a spring's rest,
  even when an outfit rebuilds the modifier list.
- **At rest:** a spring writes nothing while its offset and its velocity are both within 0.0005 (0.5 mm) on every axis.
- **The solver** is RK4, with the constants 2 and 1/6.
- **Damping** (instance +0xC0) is per instance and per axis, 2 · √strength · ratio, set only by the constructor
  (`0x877650`).
- **Spring chains on A-Body:** only `TF_PelvicSoft_L`, `TF_PelvicSoft_R` and `TF_Perineum` hang under another spring
  bone (`TF_PelvicSoftRoot`), and all four are in one part.

## 5. Address Library

- Every address TF Physics uses moved between 1.16.236, 1.16.242 and 1.16.244, while its ID stayed: the modifier loop
  was at `0x873000`, then `0x873070`, and is now at `0x872770`.

## 6. Part bones and bone limits

- **Part bones:** an armor addon's NIF (a body's skin is one) can bring its own bones: NiNodes with a
  `ParentBoneName` string extra data. The attach (ID 125302, through 125303) puts each under the named bone of the
  character's skeleton, matching the name in any case, when the part's root has BSX bit 0x10000. The root's flag
  0x20000000, the child order and `sgoKeep` are not read.
- **Bones per skinned shape:** at most 255. The skinning shaders keep the count in 8 bits, with a 256-slot bone
  buffer; a count over 255 wraps without an error. The largest of the game's own meshes has 143 (the Va'ruun Admin
  cape).
- **Weights per vertex:** at most 8 (`shaderspc_dx12_105.fxp`, the game's one shader pack). A shape with 65,536
  vertices or more isn't skinned.

## Hard risks

1. **A write inside `AnimationManager::Update` is lost:** the snapshot applier rewrites the rig locals after it.
2. **A write to the node's local transform alone doesn't move a mapped bone.** The game's setters write both copies.
3. **`BGSModelNode::Update` runs where no springs ran:** for AI-frozen actors and for skeletons that aren't actors'.
4. **The 3D root doesn't lead back to its character.** The character has to come from the caller.
5. **A PoseDeformer sets its target's local position outright,** so a SpringBone on the same bone loses its result.
6. **A spring's travel cap is MaxDist × the sub-step time,** so the same record moves twice as far at 30 fps as at 60.
7. **Every patch moves the code.** A raw RVA breaks with each one; an ID survives, but struct offsets aren't covered by
   the Address Library.
8. **A shape with more than 143 bones may not draw right:** the code that packs a shape's bones for the shaders is
   unread, and the game's own meshes stop at 143.
9. **A body's part bones may not attach:** the attach was read from the exe. The plugin dents only a body whose dent
   bones were all found, so a missing one leaves that body undented.
