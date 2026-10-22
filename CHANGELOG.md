# Changelog

Releases are the builds A-Body ships.

## 2.1 (A-Body 2.0)

- **FreeMorph, hand contact:** the player's own hands and arms press into the breasts. The body carries one free
  performance morph (`TFFREE`, a record for every vertex of both breasts) driven by one MorphDriver record that the
  game never weighs (MaxAnimationDistance -1); TF Physics holds its weight at 1 and rewrites its records every frame
  through the engine's own buffer update. What it writes is a real-time flesh contact (`src\freemorph_core.h`): each
  breast's skin nodes skinned from their bones and the sliders, the hand and arm shapes (round cones on the finger
  bones and the arms, rounded boxes on the palms, blended into one smooth surface), and projected Gauss-Seidel on the
  nodes' contact forces with the breast's precomputed response (Green's functions of its volume down to the chest
  wall, 3x3 per node pair within 8 cm). A node a limb holds keeps to its point on it while its sideways force stays
  under the grip (a spring of 100 N/m, at most 1 cm of drag at a firm press), then slides; a press is capped softly
  from a third to a half of the tissue's depth. The solve runs on a worker thread of its own, one frame behind the
  bones; a settled hold takes one sweep. The data is `FreeMorph.bin` in `Data\SFSE\Plugins\TFPhysics\`. Every
  address it uses is checked at start-up, and the writes stay off unless all of them read as expected. In contact
  the breasts answer the player's own arms through the free morph alone (`[FreeMorph] bOwnPushes`). `[FreeMorph]`.
- **Breast mass:** each whole breast is one damped mass on its anchor (`L_Pecs`, `R_Pecs`), in the chest's axes (3.0,
  2.8 and 2.0 Hz; damping ratios 0.45, 0.40, 0.35), driven only by the anchor's acceleration, with lopsided soft limits
  (2 cm up, 3 down, 1.2 forward, 1.5 back, 2 out, 1.2 in; 25 times stiffer past them, a hard stop at 1.35 times), in
  1/240 s sub-steps. Its offset goes on the anchor's local position, so the springs, the tips and the nipple under it
  ride on it. `[BreastMass] bEnabled`, `fScale`.
- **Glute mass:** the same on `L_Butt` and `R_Butt` (found as the `TF_Glute` springs' parent), in the body's axes:
  3.0, 2.6 and 2.2 Hz, damping ratios 0.60, 0.55, 0.40, soft limits 1.6 cm up, 2.4 down, 1 fore and aft, 1.2 out,
  0.8 in. `[GluteMass] bEnabled`, `fScale`.
- **Relative damping:** before the game's update, each body spring's offset velocity gets + c x its goal's velocity in
  COM's axes, so the game's damping (on the bone's motion in the world) acts on the motion relative to the body. A
  steady speed no longer leaves the c v / S lag. Only the body's own springs (`TF_*`, `*_Nipple`); hair and outfit
  springs keep the game's damping. `[Springs] bRelativeDamping`.
- **Cuts and teleports:** a mass starts again at rest when its anchor lands over 3 m from where its velocity puts it,
  and the relative damping skips a spring whose goal moved over 5 m in a frame, so a fall at any speed stays smooth.

## 1.5 (A-Body 1.9)


- **Dent rig version 2:** a region can dent all of the skin an arm presses, not one bone's share of it: dent bones
  on every bone that skin hangs on, each moved by one shared field per region (control points on its skin with
  Wendland bumps, smoothed over the field's edges). Each guard round's fit is solved exactly (the primal active-set
  method on the box-bounded least squares), contacts cleared first, with the guard settings from the file. A dent
  bone also turns with the slope of the dent around it, so the skin's lighting shows the dent; the fit includes
  what the turn moves, and the turn is written with the translation (node and rig copy). `[Dents] bTilt` (on unless
  set to 0). The log checks last frame's writes: each dent bone where its parent's world and the local written put
  it, and each turn as written. Version 1 rigs work as before.
- **Dents:** on a body that ships dent bones and a dent rig (`DentRig.bin` in `Data\SFSE\Plugins\TFPhysics\`),
  an arm, a hand or a leg still pressing into a breast, a glute or a thigh after the pushes dents its skin
  (`src\dents_core.h`). A dent bone rides on a bone of the body and carries that bone's skin weight in its region,
  so with no contact the skin moves exactly as before. Per region, the rig's skin samples are placed from their bones
  and the sliders; a sample inside an arm, hand or leg shape needs the inward move that takes it out, up to the rig's
  limit; the wanted dent fades out around the contacts, and the dent bones' inward moves are its smoothed fit, never
  outward and within each bone's largest move, with a guard against overshoot. A dent follows its contact within
  `[Dents] fRelaxTime` (0.06 s), x `[Dents] fScale` (0 to 3); a region none of whose bones moved more than 0.3 mm
  keeps its last dents. `[Dents] bEnabled` (on unless set to 0). In the logging build and the release build with
  collisions.
- **Even steps** for the tuning levels: Minimal x0.25, Lower x0.5, Realistic, Low x1.25, Medium x1.5, High x2, Max x2.5
  and Ultra x3 of Realistic's movement, each a preset's springs with the swing that makes its number (`kLevelSwing`).
  A level's swing no longer moves the damping.
- **`bMatchLevel`:** a part at the level's full strength (`kLevelMatch`), where the presets give it a smaller share.
- **`fTravelLimit`:** each part's farthest travel, x its Max physics + Max extra jiggle travel (`kPartTravelLimit`).
- **Breasts lifted over the arms below them:** crossed arms, a hand underneath. Each breast's skin (`lift_data.h`:
  samples of A-Body's breast skin in `C_Chest`'s frame, with what the breast sliders and the body triangle do to
  them) sits where the chest, the Pecs anchor and the four `TF_Breast` bones put it each frame. A sample inside a
  forearm, a palm bone or a finger or thumb joint (tight shapes, 95% of the skin inside, + 3 mm) with the limb below
  it needs the lift that takes it out over the limb; a forearm, or an upper arm within ~20 degrees of level, above
  a sample caps its rise (0 when it presses on it from above). The four bones' lifts are the least ones that meet
  every need and cap (80 rounds of projections), within the breast's cap and spread; a lifted bone's vertical push is
  its lift, never below its own rise. Only when a forearm or a hand comes within 25 cm of a breast.
  `[Collision] bBreastLift` (on unless set to 0).
- **Breast size in the collisions:** a breast's collision sphere, cap and spread x 1 + 0.5 Large - 0.5 Small (0.5 to
  1.6). `[Collision] bBreastSize` (on unless set to 0). `TF_BreastsPerky` and `TF_BreastsApart` are read too.
- **A part pushed as one soft mass:** the collision pushes of the soft bones of a breast, the belly, a glute and an
  inner thigh are coupled. No two of a part's pushes differ by more than the furthest one bone moves alone with the
  skin smooth, no hard crease and no sharp edge (`kGroupPushSpread`: breast 2 cm, belly 4, inner thigh 5, glute 3.5).
  A bone with no contact of its own follows the pushed ones. Of two pushed bones, the one the move carries further
  along its own push follows the other; when the move would carry both, or take both back against their pushes (a
  squeeze), they give way evenly. Every push stays inside its group's cap. The belly's, inner thighs' and glutes'
  spreads are wider than their caps, so there it only acts in a squeeze. `[Collision] bCouplePushes` (on unless set
  to 0).
- **A paused game (photo mode):** a zero frame time steps nothing. With `[Springs] bSettleWhenPaused` (on unless set
  to 0) every soft bone sits at its rest point for the pose, the larger of its spring's steady sag and its part's
  gravity shape, in place of the bounce its spring holds; the breast gravity shapes and the collisions go on top.
- **Gravity shapes for the other parts:** lying down or on all fours, the soft bones of the butt, belly, thighs, hips,
  calves, upper arms and torso settle toward the floor past what their springs allow, up to the settle each part's
  skin takes cleanly (`kPartSettleLimit`), x `[Gravity] fGravityShape` or a part's `fGravityShape` (0 to 2; off unless
  set). The shape tops up the springs' own steady sag, so the settle is the same with any springs.
- **Breast gravity levels:**
  - `[Gravity] fLevel` sets every body's breast gravity level in place of the installed one. A tuning file's
    `[Breasts] fGravityLevel` replaces it while the game runs, and `fGravityBack`, `fGravitySide`, `fGravityHang` and
    `fGravityDown` set one shape each;
  - a level set this way is what a normal or bigger breast gets, and its own largest level. A smaller or thinner
    breast gets it x its breast size or its stretch guard's scale, the lower of the two;
  - the installed level's largest levels are `[Gravity] fMaxBack`, `fMaxSide`, `fMaxHang` and `fMaxDown`, and
    `[Size] fMaxLevel` goes up to 8.
- **Spring gravity in all 8 parts:** `[Springs] fSpringGravityHips`, `fSpringGravityCalves`, `fSpringGravityArms` and
  `fSpringGravityTorso` join the breasts, butt, belly and thighs (0 unless set).
- **Tuning files** (`Data\SFSE\Plugins\TFPhysics\*.ini`, every build; [docs/TUNING.md](docs/TUNING.md)):
  - each of the 8 parts takes any physics preset's springs at the installed extra jiggle, Lower or Minimal
    (Realistic's with half and a quarter of the swing), or Off;
  - per part: the swing, per direction and per bone, settle, bounce speed and sag;
  - factors per character (`[Player]`, `[NPC <ID>]`), per outfit (`[Outfit]`, by biped slots or form ID) and for the
    planet's gravity (`[Tuning] bPlanetGravity`);
  - read again within 2 s of a change (`[Tuning] bReload`);
  - `tests\tune_dump.cpp` prints how they're read.
- **World collision** (logging build only):
  - the proxies' layer is set back to 62 at runtime;
  - your character's proxies are steered every frame with the game's keyframe function;
  - above `[World] fProxyMaxSpeed` (20 m/s) or their own speed limit, TF Physics warps them itself, and both limits
    are logged;
  - when both proxies share one Havok motion, it's steered once a frame (`[World] bSteerOnce`). The log shows each
    proxy's motion, how far behind it is before each steer, and how far the C_Chest ragdoll body is from its bone;
  - the speed is worked out for the time the physics simulates (its whole substeps), and which frame's update that is
    is measured from the moves (`[World] iSteerTime`). The log shows the physics timer and the order;
  - a motion the game has made dynamic (a ragdoll) is never steered.
- **Collisions with dead bodies** (logging build; off in the releases).

## 1.3 (A-Body 1.5)

- Collisions between bodies in the release (`TFP_RELEASE_COLLISIONS`):
  - hands, forearms, upper arms and thighs against the breasts, belly and glutes;
  - breast against breast;
  - other characters' bodies, through the shared table, capped at 3 cm.

  Dead bodies are left out.
- Only guarded copies in anything that runs every update.
- Child lists read the game's way (a 16-bit count at +0x142, the array at +0x138), so new bodies load without hitches.

## 1.2

- Every game address comes from the Address Library, so a game patch that only moves code no longer turns the plugin
  off. The Address Library is now required.

## 1.1 (A-Body 1.1.1)

- **Body-shape physics:** the body triangle (thin / muscular / heavy) and the part sliders scale each part's jiggle and
  gravity. Muscular is firmer, thin lowers both, heavy raises both. Used by A-Body's Variable presets.
- **A stretch guard:** small or thin breasts are held to the skin stretch the base body has at the same setting.

## 1.0 (A-Body 1.0.5)

- Breast gravity from the chest's real tilt, with per-pose shapes: back slump, side spread, face-down squash and
  bent-forward teardrop. A thigh check tells lying flat from all fours.
- Every A-Body spring held to its 60 fps travel at any frame rate.
- Gravity inside the breast, belly and butt springs.
- Breast gravity scaled by the breast size sliders.
