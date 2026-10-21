# Changelog

Releases are the builds A-Body ships.

## Development (this source)

- **Even steps** for the tuning levels: Minimal x0.25, Lower x0.5, Realistic, Low x1.25, Medium x1.5, High x2, Max x2.5
  and Ultra x3 of Realistic's movement, each a preset's springs with the swing that makes its number (`kLevelSwing`).
  A level's swing no longer moves the damping.
- **`bMatchLevel`:** a part at the level's full strength (`kLevelMatch`), where the presets give it a smaller share.
- **`fTravelLimit`:** each part's farthest travel, x its Max physics + Max extra jiggle travel (`kPartTravelLimit`).
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
