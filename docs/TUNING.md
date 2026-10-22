# Tuning files

TF Physics reads every `*.ini` file in `Data\SFSE\Plugins\TFPhysics\` (the folder next to `TFPhysics.ini`) when the
game starts: in name order, up to 64 files. With `[Tuning] bReload=1` it checks them every 2 s and reads them all
again when one changed, so a file can be edited while the game runs.

- A key left out keeps what an earlier file set, and a section can be split over files.
- Every value is clamped to its range; text that isn't a number reads as the low end.
- A value stands alone on its line. Windows' INI reader keeps a comment after a value as part of the value, so
  `sLevel=Lower ; half` names no level.
- `tests\tune_dump.exe <path to TFPhysics.ini>` prints how a TFPhysics.ini and its tuning files are read, with the
  plugin's own reader ([../tests/README.md](../tests/README.md)).

## Levels

A level gives a part a physics preset's springs, plus a swing on top. The springs are strength, damping ratio and
MaxDist per COM axis, from `src\preset_data.h`, at the extra-jiggle level the body's records were installed with. The
swing makes the level's strength: how far the breasts and butt move against Realistic, on the game's spring model
(the largest offset per motion, walk, run, jump landing, sprint start/stop and strafe reversal at 60 fps, averaged).

| Level | Springs | Strength (breasts and butt) |
|---|---|---|
| `Minimal` | Realistic | x0.25 |
| `Lower` | Realistic | x0.5 |
| `Realistic` | Realistic | x1 |
| `Low`, `Medium`, `High`, `Max` | that preset | x1.25, x1.5, x2, x2.5 |
| `Ultra` | Max | x3 |
| `Off` | as installed | x0, and no sag |
| `Preset` (also `Installed`, `Default`) | in a part's section: `[All]`'s level; in `[All]`: as installed | as installed |

- **The other parts.** The presets give the belly, thighs, hips, calves, arms and torso a smaller share at the higher
  levels. `bMatchLevel=1` brings a part to the level's strength too (`kLevelMatch`, per extra-jiggle level).
- **Damping.** A level's swing is not in the damping: the damping is the preset's own, moved only by the body's
  factors, `fSwing` and `fSettle`.

- **Which setting is installed.** At load, each spring record is matched against the table. Strength, damping and
  MaxDist must agree to within 1e-4 (relative, above 1); the first preset and extra-jiggle level that fits is the
  installed one.
- **A level's springs.** A part at a level gets that level's springs, whatever setting is installed:
  - strength (`+0x20`) and MaxDist (`+0x50`) go into the shared spring record, which the game reads on every update;
  - the instance's sub-step rate goes to `+0xD0` (0.63662 · √(largest strength), as the constructor sets it);
  - its damping (`+0xC0`) is worked out from the level's ratio every frame.
- **Other springs.** A record that matches no setting (another body's springs) keeps its own. A level then changes
  only its swing.
- **Taking a tuning out.** A record that was tuned once stays on that path, so with the tuning taken out the installed
  values come back bit for bit.

## Sections

### [All]

| Key | What it does |
|---|---|
| `sLevel` | the level of every part whose own section gives none, or gives `Preset` |

### [Breasts] [Butt] [Belly] [Thighs] [Hips] [Calves] [Arms] [Torso]

| Key | Range | What it does |
|---|---|---|
| `sLevel` | a level | the part's level; `Preset` = `[All]`'s |
| `bMatchLevel` | 0 or 1 | 1 = the level's full strength for this part |
| `fTravelLimit` | 0.5 to 3 | the farthest the part travels, x its Max physics + Max extra jiggle travel; left out: `kPartTravelLimit` |
| `fGravityLevel` | 0 to 8 | `[Breasts]` only: every body's breast gravity level in every shape, over `[Gravity] fLevel` and the installed level; 0 = not set ([Breast gravity levels](#breast-gravity-levels)) |
| `fGravityBack`, `fGravitySide`, `fGravityHang`, `fGravityDown` | 0 to 8 | `[Breasts]` only: the level of one shape (lying on the back, on a side, bent forward and on all fours, face down), over every other level; 0 = not set |
| `fGravityShape` | 0 to 2 | every part but the breasts: the part's gravity shape, x the settle its skin takes cleanly; left out: `[Gravity] fGravityShape` ([Gravity shapes of the other parts](#gravity-shapes-of-the-other-parts)) |
| `fSwing` | 0 to 3 | x the part's swing |
| `fUpDown`, `fSideToSide`, `fFrontBack` | 0 to 3 | x the swing on one COM axis: vertical, side, fore/aft |
| `fSettle` | 0.25 to 4 | x the damping ratio: above 1 it settles sooner, below 1 it wobbles longer |
| `fBounceSpeed` | 0.5 to 2 | x the springs' frequency (the strength x its square) |
| `fSag` | 0 to 0.3 | the gravity inside the part's springs, in g. Left out: the part's key in TFPhysics.ini's `[Springs]` ([Spring gravity](#spring-gravity-tfphysicsini)) |
| a bone's name | 0 to 3 | x that bone's swing; only the part's own bones count |

The bones of each part:

| Part | Bones (`_L` and `_R` each, unless named) |
|---|---|
| Breasts | `TF_Breast_Inner`, `TF_Breast_Lower`, `TF_Breast_Outer`, `TF_Breast_Upper` |
| Butt | `TF_Glute_Inner`, `TF_Glute_Lower`, `TF_Glute_Outer`, `TF_Glute_Upper` |
| Belly | `TF_Abdomen_L`, `TF_Abdomen_R`, `TF_Abdomen_Lower`, `TF_Abdomen_Upper` |
| Thighs | `TF_Thigh_Front`, `TF_Thigh_InnerLower`, `TF_Thigh_InnerUpper`, `TF_Thigh_Outer`, `TF_Thigh_Rear` |
| Hips | `TF_Hip_Outer` |
| Calves | `TF_Calf_Lateral`, `TF_Calf_Medial` |
| Arms | `TF_UpperArm_Back`, `TF_UpperArm_Inner` |
| Torso | `TF_Flank`, `TF_Lat`, `TF_LowerBack`, and `TF_PelvicSoftRoot`, `TF_PelvicSoft_L`, `TF_PelvicSoft_R`, `TF_Perineum` |

### [Player], [NPC <form ID>]

- **Keys:** `fAll`, then `fBreasts`, `fButt`, `fBelly`, `fThighs`, `fHips`, `fCalves`, `fArms`, `fTorso` (0 to 3).
  Each is x that character's jiggle in one part; `fAll` counts for every part without its own key.
- **Which character:** `[NPC 00012345]` names one by its reference's form ID or its base record's, in hex as the
  console shows it. With several sections for one character, the last one counts.
- At most 64 character sections.

### [Outfit <name>]

| Key | What it does |
|---|---|
| `sSlots` | biped slots, a comma list (0 to 63): the section counts for every worn armour on any of them |
| `sForm` | one armour's form ID (hex): the section counts for that piece, in place of any slot section |
| `fAll`, `fBreasts` .. `fTorso` | x the jiggle of a body wearing it (0 to 3) |

- A body gets, per part, the lowest factor of all the pieces it wears.
- At most 24 outfit sections.
- Starfield's slots for outfits, by HumanRace's biped names:
  - 3 = BODY: vanilla clothes are one piece there, often with 5 = Backpack;
  - 35 = a spacesuit's body, 36 = its helmet, 37 = its backpack.

### [Tuning] in TFPhysics.ini

| Key | Default | What it does |
|---|---|---|
| `bReload` | 1 | check the files every 2 s and read them again on a change; 0 = read when the game starts only |
| `bPlanetGravity` | 1 | every gravity factor x clamp(1 + `fPlanetGravity` x (scale − 1), 0, 3), from the player's gravity scale |
| `fPlanetGravity` | 1 | how much of the planet's difference from 1 g counts (0 to 1) |
| `bOutfits` | 1 | the `[Outfit]` sections count |

`[Tuning]` is part of what a reload checks: its keys are read again with the files. `bReload` itself is read when the
game starts.

## A spring's swing and damping

- **The swing** = the body's jiggle factor (body shape, character, outfit) x the level's swing (x `kLevelMatch` with
  `bMatchLevel`) x `fSwing` x the bone's factor x the axis factor.
- **Every axis** stops at the part's ceiling: the travel of Max physics with Max extra jiggle x the part's travel
  limit, over the spring's travel as tuned. A limit below the travel holds the swing below 1 too.
- **No sag** goes into an axis whose swing is under 0.05.
- **The damping ratio** = the level's x the jiggle coupling (`[BodyShape] fJiggleDamping`, on the body's factors
  and `fSwing`) x `fSettle`. It never drops below the release's lowest for that bone, and never goes above critical.

## The reads behind it

| What | Where | Checked at start-up against |
|---|---|---|
| the player's gravity scale, with its modifiers | `player+0x1138` | GetPlayerGravityScale, ID 65694 |
| a form's ID | `form+0x28` | the Papyrus GetFormID native, ID 117985 |
| what a character wears | the inventory at `actor+0xA0` | WornCoversBipedSlot, ID 65296 |

- **The inventory:** its count is at `+0x28` and its entries at `+0x30`, 0x28 bytes each. Each entry holds the item at
  `+0x00`, and it's worn when the byte at `+0x20` has any of bits 0-2. An armour (form type 0x22 at `+0x2E`) keeps its
  biped slots in the 64 bits at `+0x208`.
- **No lock.** The game holds the inventory's lock (`+0xA8`) for its own read. The plugin reads with guarded copies
  and no lock, so a list changing under it gives one wrong answer, for 2 s at most.
- **Failed checks.** A read whose check fails is off, and the log's tuning line says which passed.
- **The planet's gravity** is the player's, kept for every body (the bodies loaded are near the player).

## Breast gravity levels

Each gravity shape takes the first level that is set:

1. its own: `[Breasts] fGravityBack`, `fGravitySide`, `fGravityHang` or `fGravityDown` in a tuning file;
2. `[Breasts] fGravityLevel` in a tuning file;
3. `[Gravity] fLevel` in TFPhysics.ini;
4. the installed level (the gravity record's).

A level from 1 to 3 is what a normal or bigger breast gets, and its own largest level. A smaller or thinner breast gets
it x its breast size or its stretch guard's scale, the lower of the two. The installed level is x the breast size and
held per shape to its largest level:

| Key | Default | What it does |
|---|---|---|
| `[Gravity] fLevel` | 0 | every body's breast gravity level (0.5 to 8); 0 = the installed level |
| `[Gravity] fMaxBack` | 2.0 | the installed level's largest effective level (x breast size) lying on the back |
| `[Gravity] fMaxSide` | 2.4 | ... lying on a side |
| `[Gravity] fMaxHang` | 2.3 | ... bent forward, and on all fours |
| `[Gravity] fMaxDown` | 3.0 | ... lying face down |
| `[Size] fMaxLevel` | 3.0 | the installed level x breast size never goes above this |

The largest levels are 0.5 to 8 each, and the stretch guard scales them down for a body whose breast skin stretches
more than the base body's. On A-Body's mesh the skin first folds above x3.5 lying on the back or on a side, above x5.75
bent forward or on all fours (with the springs' own sag at its most on top), and above x6 face down.

From level x2 up the breast springs' sag is held to 1 / strain, counting the highest level any shape has.

## Spring gravity (TFPhysics.ini)

| Key | Default | Part |
|---|---|---|
| `[Springs] fSpringGravity` | 0.06 | breasts |
| `[Springs] fSpringGravityButt` | 0.04 | butt |
| `[Springs] fSpringGravityBelly` | 0.03 | belly |
| `[Springs] fSpringGravityThighs` | 0 | thighs |
| `[Springs] fSpringGravityHips` | 0 | hips |
| `[Springs] fSpringGravityCalves` | 0 | calves |
| `[Springs] fSpringGravityArms` | 0 | upper arms |
| `[Springs] fSpringGravityTorso` | 0 | torso |

Each is in g, 0 to 0.3, and a part's `fSag` in a tuning file replaces it while the game runs. A spring's steady sag
stops at 85% of its 60 fps travel on each COM axis, so more gravity past that adds nothing on that axis. On A-Body's Max
springs a full tilt reaches that point at about: breasts 0.08, butt 0.079, belly 0.041, thighs 0.044, hips 0.028,
calves 0.032, arms 0.028, torso 0.013 (the lowest over the extra jiggle levels).

## Gravity shapes of the other parts

Lying down, on a side or on all fours, the soft bones of the butt, belly, thighs, hips, calves, upper arms and torso
settle toward the floor past their springs' travel. Per spring and per COM axis, the shape is the rest of the way from
the spring's own steady sag (as its swing shows it) to

    level x kPartSettleLimit[part] x 85% of the bone's Max physics + Max extra jiggle travel (60 fps size) x the change of gravity

where the change of gravity is the one the springs' sag uses (each axis held at 1 g) and the level is the part's
`fGravityShape` in a tuning file, else `[Gravity] fGravityShape` (default 0: off), x the body's gravity factor up to 1
(a thinner body, or a lighter planet, settles less). It goes on after the game's update, like the breasts' shapes, in
the bone's parent frame. With `[Gravity] bEnabled=0` the parts' shapes are off too.

`kPartSettleLimit` (x 85% of the Max + Max travel), on A-Body's mesh: the last step before the skin folds over or
passes through itself, one part at a time, on all fours, on the back and on a side:

| Part | Limit | First break above it |
|---|---|---|
| Butt | 1.5 | the crotch, on all fours |
| Belly | 0.7 | inside the navel, on all fours and on a side |
| Thighs | 1.75 | the hips and pubis |
| Hips | 2.5 | the arms (at the hips), lying on the back |
| Calves | 2 | behind the knees, on all fours |
| Arms | 1.75 | the shoulders and armpits, on a side |
| Torso | 2.25 | the waist and back |

At these limits the skin itself moves up to about 2 to 4 cm.

## A paused game (TFPhysics.ini)

| Key | Default | What it does |
|---|---|---|
| `[Springs] bSettleWhenPaused` | 1 | while the game is paused (a zero frame time, as in photo mode), every soft bone sits at its rest point for the pose; 0 = the bounce its spring holds |

A spring's rest point, per COM axis, is the larger of its own steady sag (held at 85% of its 60 fps travel, as its
swing shows it) and its part's gravity shape. Nothing is stepped on a paused frame: no spring, record, damping or
velocity is written, so the springs go on from their own state when the game runs again.

## Masses, damping and hand contact (TFPhysics.ini)

| Key | Default | What it does |
|---|---|---|
| `[Springs] bRelativeDamping` | 1 | every body spring (`TF_*`, `*_Nipple`) damped against the body's motion, not the world's; 0 = the game's own damping |
| `[BreastMass] bEnabled` | 1 | each whole breast one damped mass on its anchor; 0 = the springs alone |
| `[BreastMass] fScale` | 1.0 | its swing x this (0 to 2), on top of the breasts' level |
| `[GluteMass] bEnabled` | 1 | each glute one damped mass on `L_Butt` / `R_Butt` |
| `[GluteMass] fScale` | 1.0 | its swing x this (0 to 2), on top of the butt's level |
| `[FreeMorph] bEnabled` | 1 | the hand contact on the player's breasts (a body with FreeMorph data) |
| `[FreeMorph] iModeKey` | 0 | a key that steps through the modes (a Windows key code: 0 = none, 111 = Numpad /) |
| `[FreeMorph] iStartMode` | 2 | the mode at the start: 2 contact, 1 a 1 cm dent going round the left breast, 0 the file's own records, 3 off |
| `[FreeMorph] fGrip` | -1 | how far a pressing finger drags the skin before it slides, in mm (0 to 30; -1 = the data's, 10) |
| `[FreeMorph] fHoldGive` | -1 | a held point's sideways give, in mm per newton (at least 7; -1 = the data's, 10) |
| `[FreeMorph] fCapScale` | 1.0 | the press's soft cap (a third to a half of the tissue's depth) x this (0.3 to 1.5) |
| `[FreeMorph] fDentSpeed` | 0.5 | the moving dent's turns per second (0 to 4) |
| `[FreeMorph] bOwnPushes` | 0 | 1 = in contact the arms' capsule pushes and the lift work on the breasts too; 0 = the free morph alone answers the player's own arms there |

The masses scale with the part's jiggle level (a part at `Off` has none). They are read when the game starts.
