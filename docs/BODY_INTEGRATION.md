# What a body needs for TF Physics

TF Physics doesn't simulate springs by itself. It works with the game's own bone modifiers: the springs
(SpringBone) and pose correctives (PoseDeformer) a body ships in its plugin. It changes them before the game's update,
and adds on top of them after it. So a body needs those modifiers, and the bones they drive, under the names below.
Today that body is A-Body.

## How a body is recognised

An actor counts as ours when its bone-modifier list holds TF Physics' face-up gravity corrective, one per side
(`IsOurFaceUpRecord`).

**The corrective:** a PoseDeformer with
- target `L_Pecs` or `R_Pecs` (the breast anchor), driver `COM`;
- Axis Y (byte 1), Vector (0, 0, 1), Inner 0°, Outer 90°;
- Position with Y < 0 (toward the chest) and Z ≠ 0 (the spread; +Z is the left breast).

**Its level** is −Y / 1.5 cm. That's how the installed gravity level (Real, Plus, Max) is read: the level must be
between 0.1 and 10.

**What follows from it:**
- The body's springs are the SpringBones driven by the same `COM`.
- On each side, the springs on `TF_Breast_Upper_*`, `_Lower_*`, `_Inner_*` and `_Outer_*` whose parent is the anchor
  are the four breast bones. They're taken by exact name, so an outfit's own spring under the breast is never mistaken
  for one.

An actor without the corrective is left alone. It only lends its limb and torso shapes to A-Body bodies within 3 m.

## Bones

| What | Bones (`_L` / `_R` each where it applies) |
|---|---|
| Anchors, driver | `L_Pecs`, `R_Pecs`, `COM`; `C_Chest` (the chest's tilt) |
| Breast springs | `TF_Breast_Upper`, `_Lower`, `_Inner`, `_Outer` |
| Belly springs | `TF_Abdomen_L`, `_R`, `_Upper`, `_Lower` |
| Inner thigh springs | `TF_Thigh_InnerUpper`, `_InnerLower` (their collision group is off by default) |
| Glute springs | `TF_Glute_Upper`, `_Lower`, `_Inner`, `_Outer` |
| Limbs for collisions | `Biceps` → `Forearm` → `Wrist` → `Middle1`; `Thigh` → `Calf` → `Foot` → `Toe` |
| Torso for other bodies | `C_Spine2` → `C_Neck` (the chest); the hips |
| Thigh tilt (lying vs all fours) | the thigh springs' parents, `L/R_Thigh_Twist*`, whose X axis runs down the thigh |

- Every other `TF_*` spring driven by COM still gets the 60 fps travel and its part's gravity and jiggle, by name
  prefix: `TF_Breast_`, `TF_Abdomen_`, `TF_Glute_`, `TF_Thigh_`.
- `src\body_shape_data.h` lists A-Body's 50 spring bones by exact name, with their part, their largest travel and their
  lowest damping.
- Bones are searched by name under `COM`. When the wrist or finger bone exists only in the rig's table, the hand is
  placed from the rest pose (`kWristInForearm`, `kHandTipInWrist`).

## Data measured from A-Body

The numbers are A-Body's, measured from its mesh and skeleton (the scripts are in `tools\`; see `tools\README.md`):
- **`src\collision_data.h`:**
  - each soft bone's sphere radius and group;
  - each limb capsule's radius and where it starts down the limb;
  - per group, which limbs it meets and how far it may be pushed;
  - the natural-pose distance of every soft-bone and limb pair, below which nothing pushes.
- **`src\body_shape_data.h`:** each spring's part, its largest travel (the release's Max physics with Max jiggle) and
  its lowest damping ratio.
- **`src\preset_data.h`:** each spring's strength, damping ratio and MaxDist at every installer setting (5 physics
  presets x 3 extra-jiggle levels). The plugin finds a body's installed setting from it, and the tuning files'
  levels give a part those springs ([TUNING.md](TUNING.md)).
- **The gravity shapes** in `plugin.cpp` (the back slump, side, face-down and teardrop tables): offsets for the four
  breast bones, designed on A-Body's mesh to keep the skin stretch under a limit.

## Sliders

The plugin reads the morph-target weights the body's sliders store on the NPC record, by name:

| Sliders | Use |
|---|---|
| `TF_BreastsLarge`, `TF_BreastsSmall` | breast size: gravity × (1 + 0.5 Large − 0.5 Small) |
| `TF_ButtLarge` / `Small`, `TF_HipsWide` / `Narrow`, `TF_WaistThin`, `TF_ThighsLarge` / `Slim`, `TF_CalvesLarge` / `Small`, `TF_ArmsLarge` / `Small` | part sizes for the body-shape physics |
| `TF_BreastsGravity`, `TF_BreastsTogether`, `TF_NipplesLarge`, `TF_NipplesSmall` | the stretch guard |

It also reads the body triangle (Thin / Strong / Overweight).

## World collision (test builds only)

A-Body's world-collision test skeletons add two nodes, `ABody_Proxy_Breast_L` and `_R`, under the Pecs. Each carries a
`bhkNPCollisionObject` and its own keyframed one-body Havok system. See [WORLD_COLLISION.md](WORLD_COLLISION.md).

## Supporting other bodies

What's A-Body-specific today, and what a data-driven version would need:

1. **A body definition** in place of the compiled headers. Per skeleton, it would give:
   - bone roles to names;
   - sphere radii and caps;
   - rest distances, or a rule that derives them from the skeleton's reference pose at runtime;
   - part membership and travel ceilings;
   - the springs of each of its installer settings, for the tuning levels;
   - the gravity shapes per bone.
2. **A detection rule** that isn't A-Body's corrective. For example, the definition's bones plus springs driven by
   COM, or a list of races and skeletons.
3. **Springs.** TF Physics relies on the game running the body's springs. A body with no game springs, which expects
   a plugin to simulate everything, CBPC-style, would need TF Physics to run its own spring step. That's a bigger
   change, but the hook, the write path, the collisions and the shared table would all carry over.
