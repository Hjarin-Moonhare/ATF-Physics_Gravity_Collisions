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
MaxDist per COM axis, from `src\preset_data.h`, at the extra-jiggle level the body's records were installed with.

| Level | Springs | Swing |
|---|---|---|
| `Minimal` | Realistic | x0.25 |
| `Lower` | Realistic | x0.5 |
| `Realistic`, `Low`, `Medium`, `High`, `Max` | that preset | x1 |
| `Off` | as installed | x0, and no sag |
| `Preset` (also `Installed`, `Default`) | in a part's section: `[All]`'s level; in `[All]`: as installed | x1 |

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
| `fSwing` | 0 to 3 | x the part's swing |
| `fUpDown`, `fSideToSide`, `fFrontBack` | 0 to 3 | x the swing on one COM axis: vertical, side, fore/aft |
| `fSettle` | 0.25 to 4 | x the damping ratio: above 1 it settles sooner, below 1 it wobbles longer |
| `fBounceSpeed` | 0.5 to 2 | x the springs' frequency (the strength x its square) |
| `fSag` | 0 to 0.3 | the gravity inside the part's springs, in g. Left out: `[Springs]` in TFPhysics.ini (breasts, belly, butt, thighs), else none |
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

- **The swing** = the body's jiggle factor (body shape, character, outfit) x the level's swing x `fSwing` x the bone's
  factor x the axis factor.
- **Above 1,** the swing stops at the part's ceiling: the travel of Max physics with Max extra jiggle, over the
  spring's travel as tuned.
- **No sag** goes into an axis whose swing is under 0.05.
- **The damping ratio** = the level's x the jiggle coupling (`[BodyShape] fJiggleDamping`) x `fSettle`. It never
  drops below the release's lowest for that bone, and never goes above critical.

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
