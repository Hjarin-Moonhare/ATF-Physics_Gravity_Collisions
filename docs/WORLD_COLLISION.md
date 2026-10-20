# World collision

The goal is for A-Body's breasts (and later the belly, glutes and thighs) to collide with the world both ways:
- walls and doors push them;
- they knock small loose items over.

It has to be stable, so the game keeps every Havok lifetime decision.

The game facts it rests on, with the hard risks, are in [research/HAVOK_WORLD.md](research/HAVOK_WORLD.md).

## The design

1. **Proxy bodies live in A-Body's skeleton NIF.**
   - Each is a small keyframed Havok body on a helper node, `ABody_Proxy_Breast_L` / `_R` under the Pecs, with its
     own one-body physics system.
   - The system is cloned from a game NIF's one-body system and patched: keyframed, a 7.5 cm near-sphere, layer 62.
     Its motion properties don't matter: the game gives the bodies its own (100 m/s, 200 rad/s).
2. **The game owns their lifetime.**
   - They enter and leave the physics world with the character's 3D, so cell changes, fast travel, ships, saves and
     death stay the game's job.
   - TF Physics never creates, adds, removes or frees a body.
3. **TF Physics steers them each frame** with the game's own "keyframe a collision object" function (ID 125392).
4. **Layer 62** will touch only small loose items (layers 4, 55, 19, 25) once a COLL record and its overrides exist.
   Until then it collides with nothing.
5. **The world pushes back** through closest-point queries per soft group, inside the existing push caps.

## What the code does now (logging build)

- **The layer.** The game puts skeleton bodies on its ragdoll layer (8) when it adds them. So every A-Body body's
  proxies are set back to layer 62 plus "collide with nothing", through the collision object's filter setter (vtable
  slot 63, ID 125374). The setting is sent again if the game takes it back, at most every 0.5 s per body.
- **Steering.** Your character's two proxies are steered to the breast centres every frame. The keyframe function gets
  the step time in seconds, `[bhkWorld+0x7E4]`, as the game's own node update passes it.
- **Warps.** Havok holds a body to its motion's own speed limit, while the keyframe function only warps above the
  motion-properties table's limit. So TF Physics reads both. When the speed a step needs is over
  `[World] fProxyMaxSpeed` or the body's own limit, it warps the proxy itself: the keyframe call with a 1 ns step,
  which takes the game's warp path (placed, velocity 0).
- **One steer per shared motion.** The game appears to put both proxies on one Havok motion. Steering each of them
  would make the second steer undo the first, so a shared motion is steered once a frame, through the left proxy, and
  the right one rides on it (`[World] bSteerOnce`).
- **The log** counts layer sets, steers, warps and skipped steers. It logs both speed limits and each proxy's motion
  (with the motion's centre of mass) whenever they change, how far behind each proxy is before each steer (six bands),
  and how far the C_Chest ragdoll body is from its bone.
- **Where it stands.** Steering keeps the proxies within a few tens of centimetres of the breasts while moving.
  Steering once per shared motion is in testing.

## Stages

| Stage | What | State |
|---|---|---|
| W0 | the proxy data in the skeletons, the exe checks | done |
| W1 | lifetime: the game adds and removes the proxies | done: added for every A-Body body |
| W1b | layer 62 at runtime, steering | built, in testing |
| W2 | items, your character only (a test plugin with layer 62's COLL record and the overrides of 4/55/19/25) | next |
| W3 | push-back, measured only (queries) | |
| W4 | push-back on, plus the held gun as a shape | |
| W5 | NPCs near you (layers switched by distance) | |
| W6 | belly, glutes and thighs: 7 proxies per character | |
| W7 | release: the COLL records in A-Body's plugin, on by default, with an INI switch | |

## Settings (`[World]`, logging build only)

| Key | Meaning |
|---|---|
| `bProxies` | watch the proxies (read only) and log their lifetime |
| `bProxyLayer` | set every A-Body body's proxies back to layer 62 |
| `bProxyInert` | and "collide with nothing" (bit 14) |
| `bSteer` | steer your character's proxies every frame |
| `fProxyMaxSpeed` | above this speed (m/s), a proxy is warped instead of moved |
| `bSteerOnce` | when both proxies share one motion, steer it once a frame, through the left proxy |
