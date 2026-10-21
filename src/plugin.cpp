// TF Physics: body physics for A-Body in Starfield. An SFSE plugin, read from Starfield 1.16.244; every game address
// comes from the Address Library.
//
// Written from scratch (no CBPC code, no CommonLibSF).
//
// The game updates every actor's bone modifiers (springs, pose correctives) in one function (ID 54894), called from
// three places.  The plugin points those calls at thunks that pass the character along and call the original.
//
// An actor is A-Body's when its modifier list holds A-Body's breast-gravity correctives (PoseDeformers on L/R_Pecs,
// driver COM).  For those:
//   before the game's update  every spring's travel limit at its 60 fps size, gravity inside the springs (per part),
//                             damping by body shape
//   after it                  each breast anchor's offset from the CHEST's tilt in the world (the correctives can only
//                             read COM, so bending at the waist is invisible to them), and per-bone shapes on the four
//                             TF_Breast bones on top of their springs:
//                               lean or lie back      -> flatten and spread (back slump)
//                               lean or lie sideways  -> the side shape
//                               lean or bend forward  -> hang (teardrop); with the hips and thighs flat, press
//                             then the springs' swing scaled by body shape (the body triangle and the part sliders),
//                             held to the base body's skin stretch (the stretch guard); then collisions: upper arms,
//                             forearms, hands and thighs push the soft bones, the breasts push each other, other
//                             bodies push too
// Writes go where the game's own setter writes: the node's local translation and the skeleton rig's copy of it.
// The installed gravity level (x1, x2, x3) is read from the corrective records themselves; breast gravity also follows
// the breast size sliders (sfee's morph-target weights on the character's record).
// Every other actor lends its limb and torso shapes to the A-Body bodies near it (a shared table of copied numbers).
// Tuning files (Data\SFSE\Plugins\TFPhysics\*.ini, read again on a change) give each of the 8 parts its own preset
// level and its swing, per direction and per bone, settle, bounce speed and sag; and factors per character, per outfit
// and for the planet's gravity.
// Logging builds also keep a 5 s log and run the world-collision proxies: two keyframed Havok bodies per A-Body body
// (ABody_Proxy_Breast_L/R in the test skeletons), set to layer 62 + "collide with nothing" through the collision
// object's own filter setter, and moved to the breasts each frame with the game's keyframe-with-warp function.
//
// Settings: Data\SFSE\Plugins\TFPhysics.ini, read at game start; missing keys keep the built-in defaults.
//
// Safety: every address is looked up by ID, then every patched call site and every game function used is byte-checked
// there first; if the database or an ID is missing, or anything mismatches, the plugin changes nothing and says so in
// Documents\My Games\Starfield\SFSE\Logs\TFPhysics.log.

#include <windows.h>
#include <shlobj.h>
#include <intrin.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// Release build (build_release.bat defines TFP_RELEASE): gravity and springs only. No log file, no statistics or
// samples, no timing; collisions and other-body sharing are off whatever the INI says (they become their own mod).
#ifdef TFP_RELEASE
constexpr bool kDiag = false;
#else
constexpr bool kDiag = true;
#endif
// A-Body 1.5 (build_release_collisions.bat defines TFP_RELEASE and TFP_RELEASE_COLLISIONS): the release build with
// collisions between bodies on (own limbs and other bodies, as the INI's [Collision] says). Still no log and no world
// collision: those stay diagnostic-only.
#ifdef TFP_RELEASE_COLLISIONS
constexpr bool kCollisions = true;
#else
constexpr bool kCollisions = kDiag;
#endif

#include "collision_data.h"
#include "body_shape_data.h"
#include "preset_data.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ------------------------------------------------------------------ SFSE plugin interface (layout per sfse/PluginAPI.h)
struct SFSEInterface {
    std::uint32_t sfseVersion;
    std::uint32_t runtimeVersion;
    std::uint32_t interfaceVersion;
    void* (*QueryInterface)(std::uint32_t id);
    std::uint32_t (*GetPluginHandle)();
    const void* (*GetPluginInfo)(const char* name);
};

struct SFSEPluginVersionData {
    std::uint32_t dataVersion;
    std::uint32_t pluginVersion;
    char name[256];
    char author[256];
    std::uint32_t addressIndependence;    // how addresses are found (kAddressLibraryV2)
    std::uint32_t structureIndependence;  // which game layouts it reads (kLayout_1_14_70)
    std::uint32_t compatibleVersions[16];
    std::uint32_t seVersionRequired;
    std::uint32_t reservedNonBreaking;
    std::uint32_t reservedBreaking;
};

constexpr std::uint32_t MakeExeVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t build) {
    return (major << 24) | (minor << 16) | ((build & 0xFFF) << 4);
}
constexpr std::uint32_t kRuntime_1_16_244 = MakeExeVersion(1, 16, 244);
// SFSE loads the plugin on any game version whose Address Library database is installed (SFSE 0.2.21 checks
// the file itself and disables the plugin with "address library needs to be updated" if it is missing).  The game
// layouts read are the 1.14.70+ ones.  The version list keeps the version the plugin was read from.
constexpr std::uint32_t kAddressLibraryV2 = 1 << 2;   // SFSEPluginVersionData::kAddressIndependence_AddressLibraryV2
constexpr std::uint32_t kLayout_1_14_70 = 1 << 3;     // SFSEPluginVersionData::kStructureIndependence_1_14_70_Layout

extern "C" __declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
    1, 24, "TF Physics", "TF_Skeleton", kAddressLibraryV2, kLayout_1_14_70, {kRuntime_1_16_244, 0}, 0, 0, 0};

// ------------------------------------------------------------------ game addresses (Address Library) and layouts
// every game address is an Address Library ID, plus an offset for a place inside a function, looked up at
// load in the database for the running game, Data\SFSE\Plugins\versionlib-<major>-<minor>-<build>-0.bin (Address
// Library for SFSE Plugins; format 5: a 0x60-byte header, then one uint32 RVA per ID, the ID being the index).  A patch
// that only moves this code keeps the plugin working; one that changes it fails the checks (CheckGame,
// CheckSizeReading) and the plugin changes nothing.  Each comment holds the RVA in 1.16.244, where it was read.
enum GameAddr {
    kModifierUpdate,                      // 0x872770   per-actor bone-modifier loop
    kCallSiteA, kCallSiteB, kCallSiteC,   // 0x131f684, 0x18f1fbd, 0x1a2da05: its three callers
    kGetModifierExtra,                    // 0x872550   owner -> BoneModifierExtra (or null)
    kRigFind,                             // 0x320ef0   rig name map lookup -> index
    kRigRelease,                          // 0xcec8a0   rig reference release
    kPoseDeformerVtbl,                    // 0x4b50ea0  PoseDeformerInstance vtable
    kPoseDeformerUpdate,                  // 0x874990   its slot 1 (for the check)
    kSpringBoneVtbl,                      // 0x4b512e8  SpringBoneInstance vtable
    kSpringBoneUpdate,                    // 0x877910   its slot 1 (for the check)
    kFrameTime,                           // 0x5fbf9a8  the frame time the springs use
    kFrameTimeLoad,                       // 0x8779b9   the spring update's load of it
    kGet3D,                               // 0x1a3ec60  get the 3D, called just before site C (kSiteActor)
    kActorVtbl, kPlayerVtbl, kNpcVtbl,    // 0x4cb9248, 0x4cbc448, 0x4bd5a90: Actor, PlayerCharacter, TESNPC
    kNpcLoad,                             // 0xcd2160   TESNPC::Load, TESNPC vtable slot 17
    kMorphLea448, kMorphCapacity, kMorphEntries, kMorphWeight,   // 0xce79d2, 0xce7a91, 0xce7a9c, 0xce7ade: inside
                                                                 // the morph-table loader 0xce7990 (CheckSizeReading)
    kTriNamesLea, kTriWeightLoad,         // 0x34ed97, 0x34edc0: inside the morph-weight builder 0x34ed40
    kTriNameTable,                        // 0x4b69760  its name table (Thin, Strong, Overweight)
    kNumGameAddrs
};
struct AddrRef { std::uint32_t id, off; const char* what; };
constexpr AddrRef kAddrRef[kNumGameAddrs] = {
    {54894, 0, "modifier loop"},
    {83224, 0x774, "call site A"}, {100687, 0x69D, "call site B"}, {102655, 0x1035, "call site C"},
    {54892, 0, "get modifier extra"},
    {37355, 0, "rig find"},
    {68279, 0, "rig release"},
    {405151, 0, "PoseDeformer vtable"},
    {54933, 0, "PoseDeformer update"},
    {405175, 0, "SpringBone vtable"},
    {54979, 0, "SpringBone update"},
    {936960, 0, "frame time"},
    {54979, 0xA9, "frame-time load"},
    {102735, 0, "get 3D"},
    {451614, 0, "Actor vtable"}, {452447, 0, "PlayerCharacter vtable"}, {420893, 0, "TESNPC vtable"},
    {68102, 0, "TESNPC::Load"},
    {68207, 0x42, "morph loader +0x42"}, {68207, 0x101, "morph loader +0x101"}, {68207, 0x10C, "morph loader +0x10C"},
    {68207, 0x14E, "morph loader +0x14E"},
    {37982, 0x57, "morph-weight builder +0x57"}, {37982, 0x80, "morph-weight builder +0x80"},
    {409163, 0, "triangle name table"},
};
static_assert(kAddrRef[kNumGameAddrs - 1].id, "one ID per GameAddr");
// optional addresses for the proxy watch, resolved after the core ones; if one is missing or its class
// doesn't match, only the watch is off
enum WorldAddr { kNpCollisionObjectVtbl, kBSWorldVtbl, kExteriorWorld, kSetFilterFn, kKeyframeFn, kBSWorldVtbl20,
                 kNumWorldAddrs };
constexpr AddrRef kWorldAddrRef[kNumWorldAddrs] = {
    {471137, 0, "bhkNPCollisionObject vtable"}, {471046, 0, "hknpBSWorld vtable"}, {938080, 0, "the exterior world"},
    {125374, 0, "collision object SetCollisionFilterInfo"}, {125392, 0, "keyframe a collision object (with warp)"},
    {471048, 0, "hknpBSWorld vtable at +0x20 (bodies, motions, motion properties)"}};
// the two game functions the proxies are written through (both queue the change for the next physics step):
// SetCollisionFilterInfo(co, info) = slot 63 (+0x1F8) of the bhkNPCollisionObject vtable; keyframe(co, transform, step,
// &warped), the transform = the node's world rotation rows (w 0) + its position (w 1), as the game's own node update
// (ID 125360) builds it; the float = bhkWorld+0x7E4, the time the next physics update simulates (s), as 125360 passes it
// (written by ID 125014: the leftover at +0x7DC + the frame's time; the function hands 1/it to Havok)
using SetFilterFn = bool (*)(void* co, std::uint32_t filterInfo);
using KeyframeFn = bool (*)(void* co, const float* transform, float stepSeconds, bool* warped);   // seconds
constexpr std::uint32_t kProxyLayer = 62;
static std::uintptr_t g_waddr[kNumWorldAddrs] = {};
static bool g_proxyOk = false;
static bool g_proxyWriteOk = false;   // both functions checked (bytes, and the setter = the vtable's slot 63)
// a body's motion and its speed limits (read from 1.16.244): body+0x80 its motion's index; the world's
// motions at world+0x188, 0x80 bytes each, their count at +0x190 (as the world's motion getter, slot 26 of its vtable
// at +0x20, and Havok's velocity setter ID 160867 read them); a motion's +0x58 its motion-properties index (16 bits),
// +0x60 its linear velocity with its own speed limit in w (the setter holds every velocity to it), +0x70 its angular
// velocity with that limit in w.  The motion-properties table at world+0xA58 (slot 6 of that vtable): entries at +0x40,
// 0x70 bytes each, their count at +0x48; an entry's +0x08 gravity factor, +0x0C time factor, +0x10 / +0x14 the max
// linear / angular speeds the keyframe function warps above, +0x18 / +0x1C damping.  A body's position is +0x30 plus a
// second part at +0x40 (the keyframe function's velocity maths, ID 161979, adds the two).
static bool g_proxyMotionOk = false;   // both tables' places checked in the getters' bytes
constexpr float kWarpStep = 1e-9f;     // a step this short needs a speed over any table limit: a warp
static std::uintptr_t g_addr[kNumGameAddrs] = {};   // absolute, from ResolveAddresses at load; 0 until then
constexpr std::uint8_t kFrameTimeLoadOp[4] = {0xC5, 0x7A, 0x10, 0x15};   // vmovss xmm10,[rip+..]: the operand, CheckGame
// SpringBone instance: +0x30 rest local transform (4 rows: axes x scale, then translation at +0x60; the spring's goal,
// in its target's parent frame: it writes rest - its offset), +0x70 simulated world transform, +0xB0 velocity of its
// offset (driver frame), +0xC0 damping (2 sqrt(strength) x damping ratio, per axis; set once by the constructor
// 0x877650, per instance), +0xD0 sub-step rate.
// SpringBoneData (shared by every actor): +0x20 strength, +0x30 damping ratio, +0x40 scale, +0x50 MaxDist (3 floats each).

struct FnCheck { GameAddr at; std::uint8_t bytes[9]; };
constexpr FnCheck kFnChecks[] = {
    {kModifierUpdate, {0x48, 0x85, 0xC9, 0x0F, 0x84, 0xDF, 0x00, 0x00, 0x00}},
    {kGetModifierExtra, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC}},
    {kRigFind, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24}},
    {kRigRelease, {0x48, 0x85, 0xC9, 0x0F, 0x84, 0xA8, 0x00, 0x00, 0x00}},
    {kPoseDeformerUpdate, {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89}},
    {kSpringBoneUpdate, {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x81, 0xEC}},
};

// Bone modifier instance: +0x00 vtable, +0x08 next, +0x10 driver node, +0x18 target node, +0x20 data;
//   PoseDeformer instance: +0x28 rest local translation (3 floats).
// PoseDeformerData: +0x20 vector (3 floats, driver's parent frame), +0x2C position (metres, target's parent frame),
//   +0x50 inner deg, +0x54 outer deg, +0x58 axis byte.
// Node (NiAVObject): +0x10 name, +0x20 rig, +0x38 parent, +0x40 local rotation (3 rows x 16 bytes; rows = axes),
//   +0x70 local translation, +0x80 world rotation, +0xB0 world translation.
// Rig: +0x00 refcount, +0x08 -> (+0x10 NiTransform array, 64 bytes per slot, translation at +0x30),
//   +0x48 name map (+0x58 entries of 24 bytes, value at +8, slot = value & 0x3FF; +0x60 end index).
template <class T> static T& At(void* p, std::ptrdiff_t off) { return *reinterpret_cast<T*>(static_cast<char*>(p) + off); }
// A read that may hit a freed or foreign node: false instead of a crash.
// Is [p, p + n) committed, readable memory?  Answered from a small per-thread cache of VirtualQuery regions, so the
// usual case is a few compares, and a value that is not a pointer is turned away without an exception (each one
// costs microseconds, more with crash loggers' handlers installed).
static std::atomic<std::uint64_t> g_badReads{0}, g_caughtReads{0};   // turned away / caught anyway (freed meanwhile)
static std::atomic<std::uint64_t> g_vqCalls{0}, g_vqTicks{0};   // Readable's VirtualQuery calls, their time
static std::atomic<std::uint64_t> g_readableCalls{0};          // Readable's calls (tests: none per update)
static bool Readable(const void* p, size_t n) {
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
    if (a < 0x10000 || a + n < a || a + n > 0x00007FFFFFFF0000ull) return false;   // null page, overflow, not user space
    if constexpr (kDiag) g_readableCalls.fetch_add(1, std::memory_order_relaxed);
    struct Region { std::uintptr_t lo, hi; };
    thread_local Region cache[16];
    thread_local unsigned next = 0, last = 0;
    if (a >= cache[last].lo && a + n <= cache[last].hi) return true;   // most reads land where the last one did
    for (unsigned i = 0; i < 16; ++i)
        if (a >= cache[i].lo && a + n <= cache[i].hi) {
            last = i;
            return true;
        }
    std::uintptr_t at = a;
    while (at < a + n) {   // usually one region; a read across a region border checks the next one too
        MEMORY_BASIC_INFORMATION mbi;
        LARGE_INTEGER q0{}, q1{};
        if constexpr (kDiag) QueryPerformanceCounter(&q0);
        const SIZE_T got = VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi));
        if constexpr (kDiag) {   // in game a query can take long (the kernel measures the whole region)
            QueryPerformanceCounter(&q1);
            g_vqCalls.fetch_add(1, std::memory_order_relaxed);
            g_vqTicks.fetch_add(static_cast<std::uint64_t>(q1.QuadPart - q0.QuadPart), std::memory_order_relaxed);
        }
        if (!got || mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect;
        if ((prot & (PAGE_GUARD | PAGE_NOACCESS)) || !(prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        const std::uintptr_t lo = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
        cache[next++ & 15] = {lo, hi};
        at = hi;
    }
    return true;
}

// A read of memory that should be there (a node, a bone, a table the game keeps): it can only fail if the game freed
// it in between, so it is just guarded (2 ns).
static bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                        GetExceptionCode() == EXCEPTION_GUARD_PAGE
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        if constexpr (kDiag) g_caughtReads.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

// A read through a value that may not be a pointer at all (a class check, a candidate child list, a table key): the
// address is checked first, so it never costs an exception.
static bool SafeProbe(void* dst, const void* src, size_t n) {
    if (!Readable(src, n)) {
        if constexpr (kDiag) g_badReads.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return SafeCopy(dst, src, n);
}

// A BSFixedString's text: string pool entry, characters at +0x18; "external" entries (flag bit 2 at +0x14) forward
// to another entry at +8.
static bool PoolString(void* entry, char* out, size_t cap) {
    out[0] = 0;
    for (int i = 0; entry && i < 4; ++i) {
        std::uint8_t flags = 0;
        if (!SafeCopy(&flags, static_cast<char*>(entry) + 0x14, 1)) return false;
        if (!(flags & 2)) break;
        if (!SafeCopy(&entry, static_cast<char*>(entry) + 0x08, 8)) return false;
    }
    if (!entry) return false;
    if (SafeCopy(out, static_cast<char*>(entry) + 0x18, cap)) {   // whole buffer at once; byte by byte near a page end
        out[cap - 1] = 0;
        return true;
    }
    for (size_t i = 0; i + 1 < cap; ++i) {
        char ch;
        if (!SafeCopy(&ch, static_cast<char*>(entry) + 0x18 + i, 1)) return false;
        out[i] = ch;
        if (!ch) return true;
    }
    out[cap - 1] = 0;
    return true;
}

static bool NodeName(void* node, char* out, size_t cap) {   // a node's name: BSFixedString at +0x10
    out[0] = 0;
    void* entry = nullptr;
    if (!node || !SafeCopy(&entry, static_cast<char*>(node) + 0x10, 8)) return false;
    return PoolString(entry, out, cap);
}

using ModifierUpdate = void (*)(void* owner, void* referenceNode);
using GetModifierExtraFn = void* (*)(void* owner);
using RigFindFn = std::uint64_t (*)(void* map, void* key);
using RigReleaseFn = void (*)(void* rig);
static std::uintptr_t g_base = 0;
static ModifierUpdate g_original = nullptr;
static GetModifierExtraFn g_getExtra = nullptr;
static RigFindFn g_rigFind = nullptr;
static RigReleaseFn g_rigRelease = nullptr;

// ------------------------------------------------------------------ settings
struct Settings {
    bool enabled = true;
    float hangCm = 1.2f;          // forward hang at a full forward lean (Real; x the installed level)
    float pressCm = 0.5f;         // flatten toward the chest when lying face down (Real; x level)
    float deadZoneDeg = 10.0f;    // tilts smaller than this change nothing
    float lyingStartDeg = 35.0f;  // hip (COM) tilt where "forward" starts to mean face down on the floor
    float lyingFullDeg = 75.0f;   // ... and fully means it
    float thighFlatStartDeg = 30.0f;   // ... but only with the thighs this far from vertical (all fours: not lying)
    float thighFlatFullDeg = 60.0f;    // ... and fully this far
    float slump = 1.0f;           // lying on the back: 0 = slide the whole breast only, 1 = slump (designed), up to 1.5
    float sideSlump = 1.0f;       // lying on a side: 0 = slide the whole breast only, 1 = spread and roll, up to 1.5
    float faceDownSquash = 1.0f;  // lying face down: 0 = press only, 1 = squash and spread, up to 1.5
    float hangShape = 1.0f;       // bent forward on the feet: 0 = the whole breast hangs, 1 = teardrop, up to 1.5
    bool fixedFrameRate = true;   // keep every A-Body spring's travel limit at its 60 fps size at any frame rate
    float springGravity = 0.06f;  // gravity in the breast springs, as a fraction of 1 g (0 = off)
    float springGravityBelly = 0.03f, springGravityButt = 0.04f, springGravityThighs = 0.0f;   // the same, other parts
    float springGravityHips = 0.0f, springGravityCalves = 0.0f, springGravityArms = 0.0f, springGravityTorso = 0.0f;
    bool sizeScaling = true;      // breast gravity follows the breast size sliders (sfee's TF_BreastsLarge / Small)
    float sizeLarge = 0.5f;       // gravity x (1 + sizeLarge x TF_BreastsLarge - sizeSmall x TF_BreastsSmall)
    float sizeSmall = 0.5f;
    float sizeOverride = 0.0f;    // > 0: this factor for every body instead (to try the scaling by hand)
    float maxLevel = 3.0f;   // the installed level x the size factor never goes above this
    float maxBack = 2.0f, maxSide = 2.4f, maxHang = 2.3f, maxDown = 3.0f;   // each gravity shape's largest level
    float gravityLevel = 0.0f;    // [Gravity] fLevel: every body's breast gravity level (its own largest); 0: installed
    float gravityShape = 0.0f;    // [Gravity] fGravityShape: the other parts' gravity shapes, x their clean settle
    bool stretchGuard = true;     // each body's breast gravity held to the strain the base body has at the same setting
    bool snapshots = false;       // log one body's collision positions every 5 s (for diagnosing collisions)
    bool proxies = false;         // watch the world-collision proxy bodies ([World] bProxies), read only
    bool proxyLayer = false;      // set every A-Body body's proxies to layer 62 ([World] bProxyLayer)
    bool proxyInert = true;       // ... and "collide with nothing" (bit 14) on top ([World] bProxyInert)
    bool steer = false;           // ... and steer the player's proxies each frame ([World] bSteer)
    float proxyMaxSpeed = 20.0f;  // needing more (m/s), a proxy is warped, not moved ([World] fProxyMaxSpeed)
    bool steerOnce = true;        // two proxies on one motion: steer it once a frame ([World] bSteerOnce)
    // the time the speed is asked for ([World] iSteerTime): 0 = the timer's whole time (leftover + frame, the
    // game's own keyframe time), 1 = the substeps the physics simulates, this update's or the next one's as measured,
    // 2 = this update's, 3 = the next update's (predicted)
    int steerTime = 1;
    bool tuneReload = true;       // [Tuning] bReload: the tuning files read again when they change
    bool settlePaused = true;     // [Springs] bSettleWhenPaused: a paused game shows each soft part at rest
    bool collision = true;        // hands, arms, thighs and the other breast push the soft bones
    bool measureOnly = false;     // find and log contacts (and position snapshots) but push nothing
    bool innerThighs = false;     // the inner-thigh group (in game it only met legs standing close together)
    bool glutes = true;           // the glute group: hands and forearms push the TF_Glute bones
    bool others = true;           // other bodies' shapes are shared and checked
    bool othersMeasureOnly = true;   // ... and their contacts only logged, not pushed (the first build measures)
    bool deadBodies = true;       // dead bodies take part (share their shapes, push and are pushed)
    bool couplePushes = true;     // [Collision] bCouplePushes: the bones of one part push as one soft mass
    float deadMaxAge = 0.75f;     // ... a dead body lying still is used from a share up to this old (s)
    float maxPushOther = 0.03f;   // ... other bodies push a soft bone at most this far (m), inside its group's cap
    float softScale = 1.1f;       // size of the soft spheres, x the measured size
    float maxPush[kNumGroups] = {kGroupMaxPush[0], kGroupMaxPush[1], kGroupMaxPush[2], kGroupMaxPush[3]};   // metres
    // body shape: a body's factor = 1 + these x its place in the body triangle (thin / muscular / heavy, 0
    // to 1 each, after taking off what the three share: the triangle's centre counts as none), x the part's size
    bool shape = true;
    float jThin = -0.4f, jMuscular = -0.25f, jHeavy = 0.6f;   // jiggle: thin lowers, muscular firmer, heavy raises
    float gThin = -0.3f, gMuscular = 0.15f, gHeavy = 0.5f;    // gravity: muscular a little more
    float bellyHeavy = 1.5f;      // the heavy term x this on the belly
    // part size = 1 + this x (Large - Small) slider (the breasts keep [Size] fLarge / fSmall); the belly: 1 - x Waist Thin
    float partButt = 0.5f, partHips = 0.3f, partThighs = 0.5f, partCalves = 0.5f, partArms = 0.5f, partWaist = 0.3f;
    float jiggleDamping = 1.0f;   // 1 = damping follows the jiggle factor as the presets change it, 0 = it stays
    float shapeMin = 0.3f, shapeMax = 2.0f;   // every part's factor held to this range
    float overrideJiggle = 0.0f, overrideGravity = 0.0f;   // > 0: this body factor for every body (to try by hand)
};
static Settings g_cfg;
static char g_iniPath[MAX_PATH] = {};

static void ReadSettings() {   // from g_iniPath
    auto readF = [](const char* key, float def) {
        char buf[64];
        GetPrivateProfileStringA("Gravity", key, "", buf, sizeof(buf), g_iniPath);
        return buf[0] ? static_cast<float>(atof(buf)) : def;
    };
    // every value is held to a range the math can take (a dead zone of 90 deg or lying angles in the wrong order
    // would divide by zero; a negative push limit would push the wrong way)
    // ("nan" in the file reads as NaN, which no comparison holds back: it is taken as the low end)
    auto clampf = [](float v, float lo, float hi) { return !(v >= lo) ? lo : (v > hi ? hi : v); };
    g_cfg.enabled = GetPrivateProfileIntA("Gravity", "bEnabled", 1, g_iniPath) != 0;
    g_cfg.hangCm = clampf(readF("fHangCm", g_cfg.hangCm), 0, 5);
    g_cfg.pressCm = clampf(readF("fPressCm", g_cfg.pressCm), 0, 5);
    g_cfg.deadZoneDeg = clampf(readF("fDeadZoneDeg", g_cfg.deadZoneDeg), 0, 60);
    g_cfg.lyingStartDeg = clampf(readF("fLyingStartDeg", g_cfg.lyingStartDeg), 0, 170);
    g_cfg.lyingFullDeg = clampf(readF("fLyingFullDeg", g_cfg.lyingFullDeg), g_cfg.lyingStartDeg + 1, 180);
    g_cfg.thighFlatStartDeg = clampf(readF("fThighFlatStartDeg", g_cfg.thighFlatStartDeg), 0, 89);
    g_cfg.thighFlatFullDeg = clampf(readF("fThighFlatFullDeg", g_cfg.thighFlatFullDeg), g_cfg.thighFlatStartDeg + 1, 90);
    g_cfg.slump = readF("fSlump", g_cfg.slump);
    g_cfg.slump = clampf(g_cfg.slump, 0, 1.5f);
    auto clamp15 = [](float v) { return !(v >= 0) ? 0 : (v > 1.5f ? 1.5f : v); };
    g_cfg.sideSlump = clamp15(readF("fSideSlump", g_cfg.sideSlump));
    g_cfg.faceDownSquash = clamp15(readF("fFaceDownSquash", g_cfg.faceDownSquash));
    g_cfg.hangShape = clamp15(readF("fHangShape", g_cfg.hangShape));
    // each gravity shape's largest level (the effective level, installed level x breast size)
    g_cfg.maxBack = clampf(readF("fMaxBack", g_cfg.maxBack), 0.5f, 8.0f);
    g_cfg.maxSide = clampf(readF("fMaxSide", g_cfg.maxSide), 0.5f, 8.0f);
    g_cfg.maxHang = clampf(readF("fMaxHang", g_cfg.maxHang), 0.5f, 8.0f);
    g_cfg.maxDown = clampf(readF("fMaxDown", g_cfg.maxDown), 0.5f, 8.0f);
    // the breast gravity level for every body (its own largest level); 0 or no key: the installed level
    const float lvl = clampf(readF("fLevel", 0.0f), 0.0f, 8.0f);
    g_cfg.gravityLevel = lvl > 0.0f ? (lvl < 0.5f ? 0.5f : lvl) : 0.0f;
    // the gravity shapes of every part but the breasts, x the settle each part's skin takes cleanly (0 = off)
    g_cfg.gravityShape = clampf(readF("fGravityShape", g_cfg.gravityShape), 0.0f, 2.0f);
    g_cfg.fixedFrameRate = GetPrivateProfileIntA("Springs", "bFixedFrameRate", 1, g_iniPath) != 0;
    char buf[64];
    GetPrivateProfileStringA("Springs", "fSpringGravity", "", buf, sizeof(buf), g_iniPath);
    if (buf[0]) g_cfg.springGravity = static_cast<float>(atof(buf));
    g_cfg.springGravity = clampf(g_cfg.springGravity, 0, 0.3f);
    auto readS = [](const char* section, const char* key, float def, float lo, float hi) {
        char b[64];
        GetPrivateProfileStringA(section, key, "", b, sizeof(b), g_iniPath);
        const float v = b[0] ? static_cast<float>(atof(b)) : def;
        return !(v >= lo) ? lo : (v > hi ? hi : v);
    };
    g_cfg.springGravityBelly = readS("Springs", "fSpringGravityBelly", g_cfg.springGravityBelly, 0, 0.3f);
    g_cfg.springGravityButt = readS("Springs", "fSpringGravityButt", g_cfg.springGravityButt, 0, 0.3f);
    g_cfg.springGravityThighs = readS("Springs", "fSpringGravityThighs", g_cfg.springGravityThighs, 0, 0.3f);
    g_cfg.springGravityHips = readS("Springs", "fSpringGravityHips", g_cfg.springGravityHips, 0, 0.3f);
    g_cfg.springGravityCalves = readS("Springs", "fSpringGravityCalves", g_cfg.springGravityCalves, 0, 0.3f);
    g_cfg.springGravityArms = readS("Springs", "fSpringGravityArms", g_cfg.springGravityArms, 0, 0.3f);
    g_cfg.springGravityTorso = readS("Springs", "fSpringGravityTorso", g_cfg.springGravityTorso, 0, 0.3f);
    g_cfg.sizeScaling = GetPrivateProfileIntA("Size", "bEnabled", 1, g_iniPath) != 0;
    g_cfg.sizeLarge = readS("Size", "fLarge", g_cfg.sizeLarge, 0, 2);
    g_cfg.sizeSmall = readS("Size", "fSmall", g_cfg.sizeSmall, 0, 0.9f);
    g_cfg.sizeOverride = readS("Size", "fOverride", g_cfg.sizeOverride, 0, 3);
    g_cfg.maxLevel = readS("Size", "fMaxLevel", g_cfg.maxLevel, 0.5f, 8.0f);
    g_cfg.stretchGuard = GetPrivateProfileIntA("Gravity", "bStretchGuard", 1, g_iniPath) != 0;
    g_cfg.snapshots = GetPrivateProfileIntA("Log", "bSnapshots", 0, g_iniPath) != 0;
    g_cfg.proxies = GetPrivateProfileIntA("World", "bProxies", 0, g_iniPath) != 0;
    g_cfg.proxyLayer = GetPrivateProfileIntA("World", "bProxyLayer", 0, g_iniPath) != 0;
    g_cfg.proxyInert = GetPrivateProfileIntA("World", "bProxyInert", 1, g_iniPath) != 0;
    g_cfg.steer = GetPrivateProfileIntA("World", "bSteer", 0, g_iniPath) != 0;
    g_cfg.proxyMaxSpeed = readS("World", "fProxyMaxSpeed", g_cfg.proxyMaxSpeed, 1.0f, 50.0f);
    g_cfg.steerOnce = GetPrivateProfileIntA("World", "bSteerOnce", 1, g_iniPath) != 0;
    const UINT steerTime = GetPrivateProfileIntA("World", "iSteerTime", 1, g_iniPath);
    g_cfg.steerTime = steerTime <= 3 ? static_cast<int>(steerTime) : 1;
    g_cfg.tuneReload = GetPrivateProfileIntA("Tuning", "bReload", 1, g_iniPath) != 0;
    g_cfg.settlePaused = GetPrivateProfileIntA("Springs", "bSettleWhenPaused", 1, g_iniPath) != 0;
    g_cfg.collision = GetPrivateProfileIntA("Collision", "bEnabled", 1, g_iniPath) != 0;
    auto readC = [](const char* key, float def) {
        char b[64];
        GetPrivateProfileStringA("Collision", key, "", b, sizeof(b), g_iniPath);
        return b[0] ? static_cast<float>(atof(b)) : def;
    };
    g_cfg.measureOnly = GetPrivateProfileIntA("Collision", "bMeasureOnly", 0, g_iniPath) != 0;
    g_cfg.innerThighs = GetPrivateProfileIntA("Collision", "bInnerThighs", 0, g_iniPath) != 0;
    g_cfg.glutes = GetPrivateProfileIntA("Collision", "bGlutes", 1, g_iniPath) != 0;
    g_cfg.others = GetPrivateProfileIntA("Collision", "bOtherActors", 1, g_iniPath) != 0;
    g_cfg.othersMeasureOnly = GetPrivateProfileIntA("Collision", "bOtherActorsMeasureOnly", 1, g_iniPath) != 0;
    g_cfg.deadBodies = GetPrivateProfileIntA("Collision", "bDeadBodies", 1, g_iniPath) != 0;
    g_cfg.couplePushes = GetPrivateProfileIntA("Collision", "bCouplePushes", 1, g_iniPath) != 0;
    g_cfg.softScale = readC("fSoftScale", g_cfg.softScale);
    g_cfg.softScale = clampf(g_cfg.softScale, 0.5f, 1.5f);
    const char* pushKeys[kNumGroups] = {"fMaxPushBreastCm", "fMaxPushBellyCm", "fMaxPushThighCm", "fMaxPushGluteCm"};
    for (int g = 0; g < kNumGroups; ++g) g_cfg.maxPush[g] = clampf(readC(pushKeys[g], g_cfg.maxPush[g] * 100), 0, 10) / 100;
    g_cfg.maxPushOther = clampf(readC("fMaxPushOtherCm", g_cfg.maxPushOther * 100), 0, 10) / 100;
    g_cfg.deadMaxAge = clampf(readC("fDeadMaxAgeMs", g_cfg.deadMaxAge * 1000), 0, 1000) / 1000;
    // [BodyShape]; a factor never reaches 0 or turns negative, whatever the file says
    g_cfg.shape = GetPrivateProfileIntA("BodyShape", "bEnabled", 1, g_iniPath) != 0;
    auto readB = [&](const char* key, float def, float lo, float hi) { return readS("BodyShape", key, def, lo, hi); };
    g_cfg.jThin = readB("fJiggleThin", g_cfg.jThin, -0.9f, 1.5f);
    g_cfg.jMuscular = readB("fJiggleMuscular", g_cfg.jMuscular, -0.9f, 1.5f);
    g_cfg.jHeavy = readB("fJiggleHeavy", g_cfg.jHeavy, -0.9f, 1.5f);
    g_cfg.gThin = readB("fGravityThin", g_cfg.gThin, -0.9f, 1.5f);
    g_cfg.gMuscular = readB("fGravityMuscular", g_cfg.gMuscular, -0.9f, 1.5f);
    g_cfg.gHeavy = readB("fGravityHeavy", g_cfg.gHeavy, -0.9f, 1.5f);
    g_cfg.bellyHeavy = readB("fBellyHeavy", g_cfg.bellyHeavy, 0, 3);
    g_cfg.partButt = readB("fButt", g_cfg.partButt, 0, 0.9f);
    g_cfg.partHips = readB("fHips", g_cfg.partHips, 0, 0.9f);
    g_cfg.partThighs = readB("fThighs", g_cfg.partThighs, 0, 0.9f);
    g_cfg.partCalves = readB("fCalves", g_cfg.partCalves, 0, 0.9f);
    g_cfg.partArms = readB("fArms", g_cfg.partArms, 0, 0.9f);
    g_cfg.partWaist = readB("fWaist", g_cfg.partWaist, 0, 0.9f);
    g_cfg.jiggleDamping = readB("fJiggleDamping", g_cfg.jiggleDamping, 0, 1);
    g_cfg.shapeMin = readB("fMin", g_cfg.shapeMin, 0.1f, 1);
    g_cfg.shapeMax = readB("fMax", g_cfg.shapeMax, 1, 3);
    g_cfg.overrideJiggle = readB("fOverrideJiggle", g_cfg.overrideJiggle, 0, 3);
    g_cfg.overrideGravity = readB("fOverrideGravity", g_cfg.overrideGravity, 0, 3);
    if constexpr (!kCollisions) {
        g_cfg.collision = false;
        g_cfg.others = false;
    }
    if constexpr (!kDiag) {   // release builds: no snapshots, no world collision, whatever the INI says
        g_cfg.snapshots = false;
        g_cfg.proxies = false;
        g_cfg.proxyLayer = false;
        g_cfg.steer = false;
    }
}
static void LoadSettings() {   // TFPhysics.ini next to the DLL
    char dll[MAX_PATH] = {};
    GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), dll, MAX_PATH);
    if (char* slash = strrchr(dll, '\\')) *slash = 0;
    snprintf(g_iniPath, sizeof(g_iniPath), "%s\\TFPhysics.ini", dll);
    ReadSettings();
}

// ------------------------------------------------------------------ tuning each part
// On top of the installed physics: TFPhysics.ini [Tuning] and every .ini file in the TFPhysics folder next to it
// (Data\SFSE\Plugins\TFPhysics\, in name order; the installer puts its choices there, a file per part).  Sections:
//   [All]       sLevel: every part's level where the part's own section names none (the installer's Lower and
//               Minimal presets)
//   [Breasts] [Butt] [Belly] [Thighs] [Hips] [Calves] [Arms] [Torso]
//               sLevel: whose springs the part uses: Preset (as installed), Off, or Minimal, Lower, Realistic, Low,
//               Medium, High, Max, Ultra (that preset's springs at the installed extra-jiggle level, Ultra Max's, and
//               the level's swing on top: x0.25 .. x3 of Realistic's movement, preset_data.h); bMatchLevel (1: the part
//               moves the level's strength too, where the presets give it a smaller share); fSwing; fSideToSide,
//               fFrontBack, fUpDown (per COM axis); fSettle (x its damping, never looser than the Max preset);
//               fBounceSpeed (x its springs' frequency); fSag (g, the gravity inside its springs); and a key per bone
//               of the part (x that bone's swing)
//   [Player], [NPC <hex form ID>]   fAll, fBreasts .. fTorso: x the swing of that character's parts (an NPC by its
//               reference's form ID, as the console shows it, or its base record's)
//   [Outfit <name>]   sSlots (biped slots, comma separated) or sForm (an armour's form ID, hex), and fAll, fBreasts ..
//               fTorso: x the swing of a body wearing such a piece (per part the lowest of everything it wears)
// Every value is held to its range; a key left out keeps what an earlier file said.
enum { kLevelPreset = -1, kLevelOff = kNumLevels };
static_assert(kNumMatchParts == kNumParts, "preset_data.h's match table lists the parts of body_shape_data.h");
static const char* const kPartSection[kNumParts] = {"Breasts", "Butt", "Belly", "Thighs", "Hips", "Calves", "Arms", "Torso"};
// the breast gravity shapes, in PostUpdate's order: lying on the back, on a side, bent forward, face down
static const char* const kGravityPoseKey[4] = {"fGravityBack", "fGravitySide", "fGravityHang", "fGravityDown"};
struct PartTune {
    int level = kLevelPreset;
    float swing = 1.0f, dir[3] = {1.0f, 1.0f, 1.0f}, settle = 1.0f, bounce = 1.0f;   // dir: side, fore/aft, up
    float sag = -1.0f;   // the springs' gravity, g; -1: TFPhysics.ini [Springs]
    bool match = false;  // bMatchLevel: the part moves the level's strength (kLevelMatch), not the presets' share
    float limit = -1.0f; // fTravelLimit: the farthest it travels, x its Max + Max travel; -1: kPartTravelLimit
    float gravLevel = -1.0f;   // fGravityLevel ([Breasts]): every body's breast gravity level; -1: as installed
    float gravPose[4] = {-1.0f, -1.0f, -1.0f, -1.0f};   // fGravityBack, Side, Hang, Down; -1: the level
    float shape = -1.0f;       // fGravityShape (not [Breasts]): this part's gravity shape; -1: TFPhysics.ini's
};
struct CharTune { std::uint32_t id = 0; bool player = false; float part[kNumParts] = {}; };
struct OutfitTune { std::uint64_t slots = 0; std::uint32_t form = 0; float part[kNumParts] = {}; char name[40] = {}; };
struct TuneSettings {
    PartTune part[kNumParts];
    int allLevel = kLevelPreset;
    float bone[kNumPresetBones];
    CharTune chars[64];
    int nChars = 0;
    OutfitTune outfits[24];
    int nOutfits = 0;
    bool planet = true;          // [Tuning] bPlanetGravity
    float planetAmount = 1.0f;   // [Tuning] fPlanetGravity: 1 = the planet's gravity as it is, 0 = Earth's always
    bool outfitsOn = true;       // [Tuning] bOutfits
    bool springs = false;        // some part's springs may differ from the installed ones (a level, a bounce speed)
    bool scaled = false;         // some swing factor is not 1 (a level's, Off, the multipliers, a bone)
    bool damping = false;        // some part's damping may differ (a level, fSettle)
    int files = 0;
    char fileNames[240] = {};
    TuneSettings() {
        for (float& b : bone) b = 1.0f;
    }
};
static TuneSettings g_tunes[2];
static std::atomic<int> g_tuneNow{0};
static const TuneSettings& Tune() { return g_tunes[g_tuneNow.load(std::memory_order_acquire)]; }
static int LevelOf(const TuneSettings& t, int part) {   // a part's level: its own, else [All]'s
    const int l = t.part[part].level;
    return l == kLevelPreset ? t.allLevel : l;
}

// the part of each bone in preset_data.h (by its name in body_shape_data.h), worked out once
static int PresetBonePart(int b) {
    static int part[kNumPresetBones];
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < kNumPresetBones; ++i) {
            part[i] = -1;
            for (const ShapeBoneDef& d : kShapeBones)
                if (!strcmp(d.name, kPresetBones[i].name)) part[i] = d.part;
        }
    });
    return b >= 0 && b < kNumPresetBones ? part[b] : -1;
}

static bool IniHas(const char* file, const char* sec, const char* key, char* buf, DWORD n) {
    GetPrivateProfileStringA(sec, key, "\x01", buf, n, file);
    return buf[0] != '\x01';
}
static void IniF(const char* file, const char* sec, const char* key, float& v, float lo, float hi) {
    char b[64];
    if (!IniHas(file, sec, key, b, sizeof(b))) return;
    const float x = static_cast<float>(atof(b));
    v = !(x >= lo) ? lo : (x > hi ? hi : x);   // "nan" reads as NaN: the low end
}
static int LevelNamed(const char* s) {   // -2: not a level's name
    if (!_stricmp(s, "Preset") || !_stricmp(s, "Installed") || !_stricmp(s, "Default")) return kLevelPreset;
    if (!_stricmp(s, "Off") || !_stricmp(s, "None")) return kLevelOff;
    for (int l = 0; l < kNumLevels; ++l)
        if (!_stricmp(s, kLevelName[l])) return l;
    return -2;
}
static void PartKeys(const char* file, const char* sec, float out[kNumParts]) {   // fAll, then fBreasts .. fTorso
    float all = 1.0f;
    IniF(file, sec, "fAll", all, 0.0f, 3.0f);
    for (int p = 0; p < kNumParts; ++p) {
        out[p] = all;
        char k[24];
        snprintf(k, sizeof(k), "f%s", kPartSection[p]);
        IniF(file, sec, k, out[p], 0.0f, 3.0f);
    }
}
static void ReadTuneFile(const char* file, TuneSettings& t) {
    char names[8192];
    const DWORD n = GetPrivateProfileSectionNamesA(names, sizeof(names), file);
    for (const char* sec = names; sec < names + n && *sec; sec += strlen(sec) + 1) {
        char b[160];
        if (!_stricmp(sec, "All")) {
            if (IniHas(file, sec, "sLevel", b, sizeof(b)) && LevelNamed(b) != -2) t.allLevel = LevelNamed(b);
            continue;
        }
        int part = -1;
        for (int p = 0; p < kNumParts; ++p)
            if (!_stricmp(sec, kPartSection[p])) part = p;
        if (part >= 0) {
            PartTune& pt = t.part[part];
            if (IniHas(file, sec, "sLevel", b, sizeof(b)) && LevelNamed(b) != -2) pt.level = LevelNamed(b);
            if (IniHas(file, sec, "bMatchLevel", b, sizeof(b))) pt.match = atoi(b) != 0;
            if (IniHas(file, sec, "fTravelLimit", b, sizeof(b))) {
                float lim = 1.0f;
                IniF(file, sec, "fTravelLimit", lim, 0.5f, 3.0f);
                pt.limit = lim;
            }
            if (part == kPartBreast && IniHas(file, sec, "fGravityLevel", b, sizeof(b))) {   // 0: as installed
                float g = 0.0f;
                IniF(file, sec, "fGravityLevel", g, 0.0f, 8.0f);
                pt.gravLevel = g > 0.0f ? (g < 0.5f ? 0.5f : g) : -1.0f;
            }
            for (int k = 0; part == kPartBreast && k < 4; ++k)   // per shape; 0: the level
                if (IniHas(file, sec, kGravityPoseKey[k], b, sizeof(b))) {
                    float g = 0.0f;
                    IniF(file, sec, kGravityPoseKey[k], g, 0.0f, 8.0f);
                    pt.gravPose[k] = g > 0.0f ? (g < 0.5f ? 0.5f : g) : -1.0f;
                }
            if (part != kPartBreast && IniHas(file, sec, "fGravityShape", b, sizeof(b))) {
                float g = 0.0f;
                IniF(file, sec, "fGravityShape", g, 0.0f, 2.0f);
                pt.shape = g;
            }
            IniF(file, sec, "fSwing", pt.swing, 0.0f, 3.0f);
            IniF(file, sec, "fSideToSide", pt.dir[0], 0.0f, 3.0f);
            IniF(file, sec, "fFrontBack", pt.dir[1], 0.0f, 3.0f);
            IniF(file, sec, "fUpDown", pt.dir[2], 0.0f, 3.0f);
            IniF(file, sec, "fSettle", pt.settle, 0.25f, 4.0f);
            IniF(file, sec, "fBounceSpeed", pt.bounce, 0.5f, 2.0f);
            if (IniHas(file, sec, "fSag", b, sizeof(b))) {
                float g = 0.0f;
                IniF(file, sec, "fSag", g, 0.0f, 0.3f);
                pt.sag = g;
            }
            for (int i = 0; i < kNumPresetBones; ++i)
                if (PresetBonePart(i) == part) IniF(file, sec, kPresetBones[i].name, t.bone[i], 0.0f, 3.0f);
            continue;
        }
        const bool player = !_stricmp(sec, "Player");
        if (player || !_strnicmp(sec, "NPC ", 4)) {
            CharTune c;
            c.player = player;
            c.id = player ? 0 : static_cast<std::uint32_t>(strtoul(sec + 4, nullptr, 16));
            if ((!player && !c.id) || t.nChars >= 64) continue;
            PartKeys(file, sec, c.part);
            t.chars[t.nChars++] = c;
            continue;
        }
        if (!_strnicmp(sec, "Outfit ", 7)) {
            OutfitTune o;
            strncpy_s(o.name, sec + 7, _TRUNCATE);
            if (IniHas(file, sec, "sForm", b, sizeof(b))) o.form = static_cast<std::uint32_t>(strtoul(b, nullptr, 16));
            if (IniHas(file, sec, "sSlots", b, sizeof(b)))
                for (char* s = b; *s;) {
                    if (*s < '0' || *s > '9') {
                        ++s;
                        continue;
                    }
                    char* end = nullptr;
                    const long v = strtol(s, &end, 10);
                    s = end;
                    if (v >= 0 && v < 64) o.slots |= 1ull << v;
                }
            if ((!o.form && !o.slots) || t.nOutfits >= 24) continue;
            PartKeys(file, sec, o.part);
            t.outfits[t.nOutfits++] = o;
        }
    }
}
static void TuneFlags(TuneSettings& t);   // below
static void TuneDir(char* out, size_t n) {   // the TFPhysics folder next to TFPhysics.ini
    snprintf(out, n, "%s", g_iniPath);
    if (char* slash = strrchr(out, '\\')) snprintf(slash + 1, n - static_cast<size_t>(slash + 1 - out), "TFPhysics");
}
static void LoadTuning(TuneSettings& t) {
    t = TuneSettings{};
    t.planet = GetPrivateProfileIntA("Tuning", "bPlanetGravity", 1, g_iniPath) != 0;
    IniF(g_iniPath, "Tuning", "fPlanetGravity", t.planetAmount, 0.0f, 1.0f);
    t.outfitsOn = GetPrivateProfileIntA("Tuning", "bOutfits", 1, g_iniPath) != 0;
    char dir[MAX_PATH], pat[MAX_PATH];
    TuneDir(dir, sizeof(dir));
    snprintf(pat, sizeof(pat), "%s\\*.ini", dir);
    static char names[64][MAX_PATH];
    int n = 0;
    WIN32_FIND_DATAA f;
    const HANDLE h = FindFirstFileA(pat, &f);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < 64) strncpy_s(names[n++], f.cFileName, _TRUNCATE);
        } while (FindNextFileA(h, &f));
        FindClose(h);
    }
    std::qsort(names, static_cast<size_t>(n), sizeof(names[0]),
               [](const void* a, const void* b) { return _stricmp(static_cast<const char*>(a), static_cast<const char*>(b)); });
    for (int i = 0; i < n; ++i) {
        char file[MAX_PATH];
        snprintf(file, sizeof(file), "%s\\%s", dir, names[i]);
        ReadTuneFile(file, t);
        const size_t len = strlen(t.fileNames);
        if (len + strlen(names[i]) + 3 < sizeof(t.fileNames)) snprintf(t.fileNames + len, sizeof(t.fileNames) - len, "%s%s", len ? ", " : "", names[i]);
    }
    t.files = n;
    TuneFlags(t);
}
// what a tuning changes at all (so a body with nothing tuned takes the installed path)
static void TuneFlags(TuneSettings& t) {
    t.springs = t.scaled = t.damping = false;
    for (int p = 0; p < kNumParts; ++p) {
        const PartTune& pt = t.part[p];
        const int l = LevelOf(t, p);
        if (l >= 0 && l < kNumLevels) t.springs = t.damping = true;
        if (l == kLevelOff || (l >= 0 && l < kNumLevels && (kLevelSwing[l] != 1.0f || pt.match))) t.scaled = true;
        if (pt.swing != 1.0f || pt.dir[0] != 1.0f || pt.dir[1] != 1.0f || pt.dir[2] != 1.0f || pt.limit > 0.0f) t.scaled = true;
        if (pt.bounce != 1.0f) t.springs = t.damping = true;
        if (pt.settle != 1.0f) t.damping = true;
    }
    for (float b : t.bone)
        if (b != 1.0f) t.scaled = true;
}
// the files' names, sizes and times, hashed: a change means a reload
static std::uint64_t TuneStamp() {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ c[i]) * 1099511628211ull;
    };
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &a)) {
        mix(&a.ftLastWriteTime, sizeof(a.ftLastWriteTime));
        mix(&a.nFileSizeLow, 4);
    }
    char dir[MAX_PATH], pat[MAX_PATH];
    TuneDir(dir, sizeof(dir));
    snprintf(pat, sizeof(pat), "%s\\*.ini", dir);
    WIN32_FIND_DATAA f;
    const HANDLE fh = FindFirstFileA(pat, &f);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            mix(f.cFileName, strlen(f.cFileName));
            mix(&f.ftLastWriteTime, sizeof(f.ftLastWriteTime));
            mix(&f.nFileSizeLow, 4);
        } while (FindNextFileA(fh, &f));
        FindClose(fh);
    }
    return h;
}
static void LogTuning(const char* what, const TuneSettings& t);   // below, with the log
static std::atomic<std::uint64_t> g_tuneCheckMs{0};
static std::atomic<bool> g_tuneBusy{false};
static std::uint64_t g_tuneStamp = 0;
// Every 2 s, on whichever game thread gets here first: when a file changed, the other copy is read and then made the
// one in use (a reader holds a copy for one update, so the copy written is two seconds out of use)
static void MaybeReloadTuning() {
    if (!g_cfg.tuneReload) return;
    const std::uint64_t now = GetTickCount64();
    if (now < g_tuneCheckMs.load(std::memory_order_relaxed) || g_tuneBusy.exchange(true)) return;
    g_tuneCheckMs.store(now + 2000, std::memory_order_relaxed);
    const std::uint64_t stamp = TuneStamp();
    if (stamp != g_tuneStamp) {
        g_tuneStamp = stamp;
        const int next = 1 - g_tuneNow.load(std::memory_order_relaxed);
        LoadTuning(g_tunes[next]);
        g_tuneNow.store(next, std::memory_order_release);
        LogTuning("tuning: read again (a file changed)", g_tunes[next]);
    }
    g_tuneBusy.store(false);
}

// three game functions, checked at load, that show where the tuning reads three things: the player's gravity
// scale with its modifiers (GetPlayerGravityScale, ID 65694: mov ecx, [player+0x1138]), a form's ID (Papyrus
// GetFormID, ID 117985: mov eax, [form+0x28]) and what a character wears (WornCoversBipedSlot, ID 65296; ReadWorn)
enum TuneAddr { kGravityScaleFn, kGetFormIdFn, kWornSlotFn, kNumTuneAddrs };
constexpr AddrRef kTuneAddrRef[kNumTuneAddrs] = {
    {65694, 0, "GetPlayerGravityScale"}, {117985, 0, "Papyrus GetFormID"}, {65296, 0, "WornCoversBipedSlot"}};
static bool g_gravityScaleOk = false, g_formIdOk = false, g_wornOk = false;
constexpr std::ptrdiff_t kPlayerGravityScale = 0x1138, kFormId = 0x28;

// ------------------------------------------------------------------ log and statistics
static std::mutex g_sampleMutex;   // the gravity sample below
static char g_logPath[MAX_PATH] = {};
static std::uint64_t g_startMs = 0;
static std::uint32_t g_mainThread = 0;
static std::atomic<std::uint64_t> g_nextLogMs{0};
static std::atomic<std::uint64_t> g_calls[3];
static std::atomic<std::uint64_t> g_gravityActors{0};
static std::atomic<std::uint64_t> g_springsScaled{0};
static std::atomic<std::uint32_t> g_lastScaleBits{0}, g_lastDtBits{0};
static std::atomic<std::uint64_t> g_nextSampleMs{0};
static std::atomic<std::uint64_t> g_pushes{0};      // soft bones pushed by a collision
static std::atomic<std::uint32_t> g_maxPushMm{0};   // ... and the largest push
static std::atomic<std::uint64_t> g_collideTicks{0}, g_collideCalls{0};   // time spent on collisions
static std::atomic<bool> g_snapDue{false};   // log one body's positions (set every 5 s)
static std::atomic<std::uint64_t> g_workTicks{0}, g_workCalls{0};
// the work per part, for A-Body bodies [0] and other bodies [1]: body read, before the game's update,
// after it, the log
static std::atomic<std::uint64_t> g_partTicks[2][4], g_partCalls[2];
// the proxy watch per 5 s: A-Body updates with the proxy nodes / with both proxies in a world / with the
// skeleton's own C_Chest ragdoll body in a world / without the proxy nodes; changes logged for the player; the largest
// distance of the player's proxy bodies from their nodes (mm)
static std::atomic<std::uint64_t> g_proxyBodies{0}, g_proxyInWorld{0}, g_proxyChest{0}, g_proxyNoNodes{0}, g_proxyChanges{0};
static std::atomic<std::uint32_t> g_proxyDistMm[2];
// layer sets sent (and refused), steering calls (and warps), how far the player's proxy bodies were from the
// target asked the frame before (should be ~0: the step applied it) and from this frame's target (the frame's motion)
static std::atomic<std::uint64_t> g_proxySets{0}, g_proxySetFails{0}, g_proxySteers{0}, g_proxyWarps{0};
static std::atomic<std::uint32_t> g_proxyLagMm[2], g_proxyStepMm[2];
// why a steer was skipped: [0] no body or world, [1] not on layer 62 yet, [2] no anchor or not its four bones,
// [3] no usable step time, [4] not keyframed: the game made the motion dynamic (a ragdoll); and the step time seen (0.1 ms
// units)
static std::atomic<std::uint64_t> g_proxySkips[5];
static std::atomic<std::uint32_t> g_proxyStepMin{0xFFFFFFFFu}, g_proxyStepMax{0};
// steers that moved a proxy with velocity / warps TF Physics asked for (over the cap) and, of those, ones the
// game didn't report as warps; the largest speed asked, the largest the bodies had (mm/s); the cap last used (mm/s); the
// largest second position part (body+0x40, um)
static std::atomic<std::uint64_t> g_proxyMoved{0}, g_proxyOwnWarps{0}, g_proxyOwnMissed{0};
static std::atomic<std::uint32_t> g_proxyAskedMmS{0}, g_proxyBodyMmS{0}, g_proxyCapMmS{0}, g_proxyLoUm{0};
// how far each of the player's proxies was from its target before each steer, in six bands (cm: under 2, 5,
// 10, 25, 50, more); frames steered, frames whose two proxies shared one motion, right-proxy steers left to that shared
// motion; the C_Chest ragdoll body's largest distance from its bone (mm)
static std::atomic<std::uint64_t> g_proxyGapHist[2][6];
static std::atomic<std::uint64_t> g_proxyFrames{0}, g_proxyShared{0}, g_proxyRides{0};
static std::atomic<std::uint32_t> g_chestDistMm{0};
// the physics timer at each steer: reads with fixed substeps / without / not usable; updates by their substeps
// (0, 1, 2, 3 or more); the substep and the leftover seen (0.01 ms units) and the most substeps an update may take; what
// the speed was asked for ([0] this update's substeps, [1] the next update's, predicted, [2] the timer's whole time) and
// [3] steers left out because the coming update simulates no time; the move after each steer against the time read at
// that steer and at the next one ([0] that steer's only, [1] the next one's only, [2] both, [3] neither) and the time
// moved (0.01 ms units); steers checked for the physics update running on another thread (its step lock held), and
// mid-step (the simulation stage not DONE)
static std::atomic<std::uint64_t> g_timerReads[3], g_timerSubsteps[4], g_timeFrom[4], g_orderHits[4];
static std::atomic<std::uint32_t> g_substepMin{0xFFFFFFFFu}, g_substepMax{0}, g_leftMin{0xFFFFFFFFu}, g_leftMax{0},
    g_movedMin{0xFFFFFFFFu}, g_movedMax{0}, g_timerMaxN{0};
static std::atomic<std::uint64_t> g_steerChecked{0}, g_steerInUpdate{0}, g_steerMidStep{0};
static std::atomic<int> g_orderUsed{0};   // 0 = the update after a steer is this frame's, 1 = the next frame's
static int g_orderTally[2] = {0, 0};      // ... and the checks that told the two apart since the last choice (steering lock)
static void KeepMaxU(std::atomic<std::uint32_t>& slot, float v) {
    const std::uint32_t u = !(v > 0) ? 0u : v > 4.0e9f ? 4000000000u : static_cast<std::uint32_t>(v);
    std::uint32_t prev = slot.load(std::memory_order_relaxed);
    while (u > prev && !slot.compare_exchange_weak(prev, u, std::memory_order_relaxed)) {
    }
}
static void KeepMinU(std::atomic<std::uint32_t>& slot, float v) {
    const std::uint32_t u = !(v > 0) ? 0u : v > 4.0e9f ? 4000000000u : static_cast<std::uint32_t>(v);
    std::uint32_t prev = slot.load(std::memory_order_relaxed);
    while (u < prev && !slot.compare_exchange_weak(prev, u, std::memory_order_relaxed)) {
    }
}
static std::atomic<std::uint64_t> g_boneWaits{0}, g_boneWaitTicks{0};   // waits for the bone lock, and their time
static std::atomic<std::uint64_t> g_lifeReads{0};                       // life states read
static std::atomic<std::uint64_t> g_searches[2], g_searchTicks[2];      // bone searches, COM searches   // the plugin's own time around the game's update
static std::atomic<std::uint64_t> g_shapeScaled{0}, g_shapeHeld{0}, g_shapeBodies{0};   // (ScaleJiggle)
static std::atomic<std::uint32_t> g_shapeMaxMm{0};
static std::atomic<std::uint64_t> g_pausedBodies{0}, g_skippedFrames{0};
static std::atomic<std::uint32_t> g_pausedMaxMm{0}, g_skippedDtBits{0};
// per pair kind: soft group x (collider kind, or kNumColliders = the other side's soft bones): contacts and deepest
static std::atomic<std::uint32_t> g_pairHits[kNumGroups][kNumColliders + 1], g_pairMaxMm[kNumGroups][kNumColliders + 1];
static void CountPair(int group, int kind, float depth) {
    if (depth < 0.001f) return;   // a pair sitting at its rest distance (float rounding), not a contact
    g_pairHits[group][kind].fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t mm = static_cast<std::uint32_t>(depth * 1000);
    std::uint32_t prev = g_pairMaxMm[group][kind].load(std::memory_order_relaxed);
    while (mm > prev && !g_pairMaxMm[group][kind].compare_exchange_weak(prev, mm)) {}
}
// Other bodies: contacts per soft group x the other body's shape, and the shared table's own numbers.
// split by whose body it is (kWhoYours: your body; kWhoByYou: an NPC's, touched by you; kWhoNpcs: an NPC's,
// touched by another NPC), with the deepest contact of each in detail.  kWhoDead, either body dead; and the
// lower legs and feet as shapes of their own.
enum OtherKind { kOtherUpperArm, kOtherForearm, kOtherHand, kOtherThigh, kOtherChest, kOtherPelvis, kOtherCalf, kOtherFoot,
                 kOtherSoft, kNumOtherKinds };
enum Who { kWhoYours, kWhoByYou, kWhoNpcs, kWhoDead, kNumWho };
static std::atomic<std::uint32_t> g_otherHits[kNumWho][kNumGroups][kNumOtherKinds];
static std::atomic<std::uint32_t> g_otherMaxMm[kNumWho][kNumGroups][kNumOtherKinds];
static std::atomic<std::uint64_t> g_otherShared{0}, g_otherSharedABody{0}, g_otherNear{0}, g_otherTorn{0}, g_otherFull{0};
static std::atomic<std::uint64_t> g_otherSharedDead{0};   // shares by dead bodies
static std::atomic<std::uint64_t> g_otherCrowd{0};    // bodies left out of a check (too many shapes near at once)
static std::atomic<std::uint64_t> g_otherTooOld{0};   // bodies near enough but passed over: shared over two frames ago
static std::atomic<std::uint32_t> g_otherOldestMs{0};   // the oldest shared entry a body used (lag)
static void CountOther(int who, int group, int kind, float depth) {
    if (depth < 0.001f) return;
    g_otherHits[who][group][kind].fetch_add(1, std::memory_order_relaxed);
    const std::uint32_t mm = static_cast<std::uint32_t>(depth * 1000);
    std::uint32_t prev = g_otherMaxMm[who][group][kind].load(std::memory_order_relaxed);
    while (mm > prev && !g_otherMaxMm[who][group][kind].compare_exchange_weak(prev, mm)) {}
}
// The deepest contact with another body in the last 5 s, per who: which bone, which shape, and where the other body
// was (its COM against this body's), how old its entry was, how fast it moved, how far that moved its shapes.
struct DeepContact {
    float depth, contact;            // how deep, and the distance contact starts at (m)
    float apart, up;                 // the two COMs: distance apart, and how much higher the other one is (m)
    float ageMs, speed, shiftCm;     // the other body's entry: age, its speed (m/s), how far that moved it forward
    int group, kind;
    bool abody;                       // the other body is A-Body
    bool selfDead, otherDead;        // this body / the other one is dead
    float selfUp, aUp, bUp, r;       // the bone above its own COM; the other's shape's ends above the other's
    float bodyR;                     // ... COM, its radius; this bone's sphere radius (m)
    char bone[28];
};
static std::mutex g_deepMutex;
static DeepContact g_deep[kNumWho];
static std::atomic<std::uint32_t> g_deepMm[kNumWho];
static void NoteDeep(int who, const DeepContact& c) {
    const std::uint32_t mm = static_cast<std::uint32_t>(c.depth * 1000);
    if (mm < 1 || mm <= g_deepMm[who].load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_deepMutex);
    if (mm <= g_deepMm[who].load(std::memory_order_relaxed)) return;
    g_deepMm[who].store(mm, std::memory_order_relaxed);
    g_deep[who] = c;
}
struct Sample { int site; float gc[3]; float hipTilt; float w[4]; float lying; float level; float off[3]; float size; float thigh;
                float jiggle; };
static Sample g_sample{};
static bool g_haveSample = false;

// The log is written by its own thread: a game thread only formats its line and queues it.  Opening and closing the
// file per line on a game worker thread costs far too much (the Documents folder can be on OneDrive), so the writer
// keeps the file open.  A full queue drops the line (and counts it) rather than make a game thread wait.
constexpr size_t kLogQueueMax = 4096;
static std::mutex g_logQueueMutex;
static std::condition_variable g_logWake;
static std::deque<std::string>* g_logQueue = nullptr;   // made with the writer thread, on the first line
static std::once_flag g_logStart;
static std::atomic<std::uint64_t> g_logQueued{0}, g_logWritten{0}, g_logDropped{0}, g_logWriteTicks{0};

static void LogWriter() {
    FILE* f = nullptr;
    std::deque<std::string> batch;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_logQueueMutex);
            g_logWake.wait(lock, [] { return !g_logQueue->empty(); });
            batch.swap(*g_logQueue);
        }
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        // opened once and kept open, shared: fopen_s would lock everyone else (you, OneDrive) out while the game runs
        if (!f) f = _fsopen(g_logPath, "a", _SH_DENYNO);
        if (f) {
            for (const std::string& line : batch) {
                fputs(line.c_str(), f);
                fputc('\n', f);
            }
            fflush(f);
        }
        QueryPerformanceCounter(&t1);
        g_logWriteTicks.fetch_add(static_cast<std::uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
        g_logWritten.fetch_add(batch.size(), std::memory_order_release);
        batch.clear();
    }
}

static void Log(const char* fmt, ...) {
    if constexpr (!kDiag) return;
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::call_once(g_logStart, [] {
        g_logQueue = new std::deque<std::string>();
        std::thread(LogWriter).detach();
    });
    {
        std::lock_guard<std::mutex> lock(g_logQueueMutex);
        if (g_logQueue->size() >= kLogQueueMax) {
            g_logDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_logQueue->emplace_back(buf);
        g_logQueued.fetch_add(1, std::memory_order_relaxed);
    }
    g_logWake.notify_one();
}

// Waits (up to 2 s) until every queued line is in the file (the tests use it before reading their log).
[[maybe_unused]] static void FlushLog() {
    for (int i = 0; i < 2000 && g_logWritten.load(std::memory_order_acquire) < g_logQueued.load(); ++i) Sleep(1);
}

static void MaybeLog() {
    if constexpr (!kDiag) return;
    const std::uint64_t now = GetTickCount64();
    std::uint64_t next = g_nextLogMs.load(std::memory_order_relaxed);
    if (now < next || !g_nextLogMs.compare_exchange_strong(next, now + 5000)) return;
    g_snapDue = g_cfg.snapshots;
    Sample s;
    bool have;
    {
        std::lock_guard<std::mutex> lock(g_sampleMutex);
        s = g_sample;
        have = g_haveSample;
        g_haveSample = false;
    }
    float scale, dt;
    const std::uint32_t sb = g_lastScaleBits.load(), db = g_lastDtBits.load();
    std::memcpy(&scale, &sb, 4);
    std::memcpy(&dt, &db, 4);
    Log("t=%6.1fs  calls A %llu B %llu C %llu | gravity applied %llu times | springs held to 60 fps limits %llu times "
        "(last frame %.1f fps, limit x%.2f)", (now - g_startMs) / 1000.0,
        static_cast<unsigned long long>(g_calls[0].exchange(0)), static_cast<unsigned long long>(g_calls[1].exchange(0)),
        static_cast<unsigned long long>(g_calls[2].exchange(0)), static_cast<unsigned long long>(g_gravityActors.exchange(0)),
        static_cast<unsigned long long>(g_springsScaled.exchange(0)), dt > 0 ? 1.0 / dt : 0.0, scale);
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        const std::uint64_t ticks = g_workTicks.exchange(0), calls = g_workCalls.exchange(0);
        const double ms = static_cast<double>(ticks) * 1e3 / static_cast<double>(freq.QuadPart);
        const double writeMs = static_cast<double>(g_logWriteTicks.exchange(0)) * 1e3 / static_cast<double>(freq.QuadPart);
        Log("   plugin work: %.2f ms per second of play (%.1f us per update), bad reads turned away %llu, caught %llu; "
            "log: %llu lines so far, writing took %.1f ms on its own thread in the last 5 s, %llu dropped",
            ms / 5.0, calls ? ms * 1e3 / static_cast<double>(calls) : 0.0,
            static_cast<unsigned long long>(g_badReads.exchange(0)), static_cast<unsigned long long>(g_caughtReads.exchange(0)),
            static_cast<unsigned long long>(g_logWritten.load()), writeMs, static_cast<unsigned long long>(g_logDropped.load()));
        // where that time went
        const double f = 1e3 / static_cast<double>(freq.QuadPart) / 5.0;   // ticks in 5 s -> ms per second
        auto per = [&](std::atomic<std::uint64_t>& a) { return static_cast<double>(a.exchange(0)) * f; };
        const unsigned long long ct = g_partCalls[0].exchange(0), co = g_partCalls[1].exchange(0);
        const double tr = per(g_partTicks[0][0]), tp = per(g_partTicks[0][1]), ta = per(g_partTicks[0][2]),
                     tl = per(g_partTicks[0][3]);
        const double orr = per(g_partTicks[1][0]), op = per(g_partTicks[1][1]), oa = per(g_partTicks[1][2]),
                     ol = per(g_partTicks[1][3]);
        const unsigned long long vq = g_vqCalls.exchange(0), bw = g_boneWaits.exchange(0), lr = g_lifeReads.exchange(0),
                                 s0 = g_searches[0].exchange(0), s1 = g_searches[1].exchange(0);
        Log("   work by part, ms per second: A-Body (%llu updates a second): read %.2f, before %.2f, after %.2f, log %.2f; other "
            "bodies (%llu a second): read %.2f, before %.2f, sharing %.2f, log %.2f; Windows memory checks %llu (%.2f ms), bone "
            "lock waits %llu (%.2f ms), bone searches %llu (%.2f ms), COM searches %llu (%.2f ms), life states read %llu",
            ct / 5, tr, tp, ta, tl, co / 5, orr, op, oa, ol, vq, per(g_vqTicks), bw, per(g_boneWaitTicks), s0,
            per(g_searchTicks[0]), s1, per(g_searchTicks[1]), lr);
    }
    if (g_cfg.shape)
        Log("   body shape: %llu body updates with jiggle scaled, %llu spring updates scaled (%llu held on some axis at the "
            "release's largest travel), largest swing added %.1f cm", static_cast<unsigned long long>(g_shapeBodies.exchange(0)),
            static_cast<unsigned long long>(g_shapeScaled.exchange(0)), static_cast<unsigned long long>(g_shapeHeld.exchange(0)),
            g_shapeMaxMm.exchange(0) / 10.0);
    {   // paused frames (photo mode) and frames whose time the springs can't take
        const unsigned long long pb = g_pausedBodies.exchange(0), sf = g_skippedFrames.exchange(0);
        if (pb || sf) {
            float skipDt;
            const std::uint32_t bits = g_skippedDtBits.load();
            std::memcpy(&skipDt, &bits, 4);
            Log("   paused: %llu body updates with every soft bone at its rest point for the pose (largest move %.1f cm); "
                "%llu skipped with a frame time out of range (last %.3f s)", pb, g_pausedMaxMm.exchange(0) / 10.0, sf, skipDt);
        }
    }
    if (g_cfg.collision) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        const std::uint64_t calls = g_collideCalls.exchange(0), ticks = g_collideTicks.exchange(0);
        Log("   collisions: %llu bodies checked (%.1f us each), %llu bone pushes, largest %.1f cm",
            static_cast<unsigned long long>(calls), calls ? ticks * 1e6 / freq.QuadPart / calls : 0.0,
            static_cast<unsigned long long>(g_pushes.exchange(0)), g_maxPushMm.exchange(0) / 10.0);
        if (g_cfg.proxies)
            Log("   world proxies: A-Body updates with the proxy nodes %llu (both proxies in a world %llu, their C_Chest "
                "ragdoll body in a world %llu), without them %llu; your proxies at most %.1f / %.1f cm from their nodes; "
                "%llu changes logged",
                static_cast<unsigned long long>(g_proxyBodies.exchange(0)), static_cast<unsigned long long>(g_proxyInWorld.exchange(0)),
                static_cast<unsigned long long>(g_proxyChest.exchange(0)), static_cast<unsigned long long>(g_proxyNoNodes.exchange(0)),
                g_proxyDistMm[0].exchange(0) / 10.0, g_proxyDistMm[1].exchange(0) / 10.0,
                static_cast<unsigned long long>(g_proxyChanges.exchange(0)));
        if (g_cfg.proxies && (g_cfg.proxyLayer || g_cfg.steer))
            Log("   world proxies, written: layer 62%s set %llu times (%llu refused); your proxies steered %llu times: moved "
                "%llu (asked at most %.2f m/s, the bodies moving at most %.2f m/s), warped by TF Physics %llu (over %.1f m/s; "
                "%llu not taken), warped by the game %llu; at most %.1f / %.1f mm from the target asked the frame before, "
                "%.1f / %.1f cm from the new one; the position's second part at most %.3f mm",
                g_cfg.proxyInert ? " + collide with nothing" : "", static_cast<unsigned long long>(g_proxySets.exchange(0)),
                static_cast<unsigned long long>(g_proxySetFails.exchange(0)), static_cast<unsigned long long>(g_proxySteers.exchange(0)),
                static_cast<unsigned long long>(g_proxyMoved.exchange(0)), g_proxyAskedMmS.exchange(0) / 1000.0,
                g_proxyBodyMmS.exchange(0) / 1000.0, static_cast<unsigned long long>(g_proxyOwnWarps.exchange(0)),
                g_proxyCapMmS.load() / 1000.0, static_cast<unsigned long long>(g_proxyOwnMissed.exchange(0)),
                static_cast<unsigned long long>(g_proxyWarps.exchange(0)), static_cast<double>(g_proxyLagMm[0].exchange(0)),
                static_cast<double>(g_proxyLagMm[1].exchange(0)), g_proxyStepMm[0].exchange(0) / 10.0, g_proxyStepMm[1].exchange(0) / 10.0,
                g_proxyLoUm.exchange(0) / 1000.0);
        if (g_cfg.proxies && g_cfg.steer) {
            const std::uint32_t lo = g_proxyStepMin.exchange(0xFFFFFFFFu), hi = g_proxyStepMax.exchange(0);
            Log("   world proxies, steering skipped: no body or world %llu, not on layer 62 yet %llu, no anchor or bones %llu, "
                "no step time %llu, not keyframed (a ragdoll) %llu; the physics step time %s %.1f-%.1f ms",
                static_cast<unsigned long long>(g_proxySkips[0].exchange(0)), static_cast<unsigned long long>(g_proxySkips[1].exchange(0)),
                static_cast<unsigned long long>(g_proxySkips[2].exchange(0)), static_cast<unsigned long long>(g_proxySkips[3].exchange(0)),
                static_cast<unsigned long long>(g_proxySkips[4].exchange(0)), hi ? "seen" : "not seen", hi ? lo / 10.0 : 0.0, hi / 10.0);
            // the physics timer, and which update's time the moves matched
            auto ull = [](std::atomic<std::uint64_t>& a) { return static_cast<unsigned long long>(a.exchange(0)); };
            const std::uint32_t sLo = g_substepMin.exchange(0xFFFFFFFFu), sHi = g_substepMax.exchange(0),
                                lLo = g_leftMin.exchange(0xFFFFFFFFu), lHi = g_leftMax.exchange(0),
                                mLo = g_movedMin.exchange(0xFFFFFFFFu), mHi = g_movedMax.exchange(0);
            const unsigned long long nFixed = ull(g_timerReads[0]), nFree = ull(g_timerReads[1]), nBad = ull(g_timerReads[2]);
            const unsigned long long s0 = ull(g_timerSubsteps[0]), s1 = ull(g_timerSubsteps[1]), s2 = ull(g_timerSubsteps[2]),
                                     s3 = ull(g_timerSubsteps[3]);
            const unsigned long long f0 = ull(g_timeFrom[0]), f1 = ull(g_timeFrom[1]), f2 = ull(g_timeFrom[2]), f3 = ull(g_timeFrom[3]);
            Log("   world proxies, physics timer: fixed substeps in %llu of %llu reads (%.2f-%.2f ms each, up to %u an update; "
                "leftover %.2f-%.2f ms), %llu not usable; updates with 0 substeps %llu, 1: %llu, 2: %llu, 3 or more: %llu; "
                "the speed asked for this update's substeps %llu times, the next update's %llu, the timer's whole time %llu; "
                "not steered, no time simulated next %llu",
                nFixed, nFixed + nFree + nBad, sHi ? sLo / 100.0 : 0.0, sHi / 100.0, g_timerMaxN.exchange(0),
                lHi ? lLo / 100.0 : 0.0, lHi / 100.0, nBad, s0, s1, s2, s3, f0, f1, f2, f3);
            const unsigned long long o0 = ull(g_orderHits[0]), o1 = ull(g_orderHits[1]), o2 = ull(g_orderHits[2]),
                                     o3 = ull(g_orderHits[3]);
            Log("   world proxies, the move after each steer: as long as that steer's timer said %llu, as the next steer's %llu, "
                "both %llu, neither %llu (moved for %.2f-%.2f ms); the speed is asked for %s; steered while the physics "
                "update ran on another thread %llu of %llu (mid-step %llu)",
                o0, o1, o2, o3, mHi ? mLo / 100.0 : 0.0, mHi / 100.0,
                g_cfg.steerTime == 0   ? "the timer's whole time"
                : g_cfg.steerTime == 2 ? "this update's substeps"
                : g_cfg.steerTime == 3 ? "the next update's substeps"
                : g_orderUsed.load()   ? "the next update's substeps (as measured)"
                                       : "this update's substeps (as measured so far)",
                ull(g_steerInUpdate), ull(g_steerChecked), ull(g_steerMidStep));
            char hist[2][140];
            for (int w = 0; w < 2; ++w) {
                unsigned long long h[6];
                for (int k = 0; k < 6; ++k) h[k] = g_proxyGapHist[w][k].exchange(0);
                snprintf(hist[w], sizeof(hist[w]), "under 2: %llu, 2-5: %llu, 5-10: %llu, 10-25: %llu, 25-50: %llu, over 50: %llu",
                         h[0], h[1], h[2], h[3], h[4], h[5]);
            }
            Log("   world proxies, how far behind before each steer (cm): L %s; R %s; one shared motion in %llu of %llu frames "
                "(the R proxy rode on it %llu times); the C_Chest ragdoll body at most %.1f cm from its bone", hist[0], hist[1],
                static_cast<unsigned long long>(g_proxyShared.exchange(0)), static_cast<unsigned long long>(g_proxyFrames.exchange(0)),
                static_cast<unsigned long long>(g_proxyRides.exchange(0)), g_chestDistMm.exchange(0) / 10.0);
        }
        static const char* groupName[kNumGroups] = {"breast", "belly", "inner thigh", "glute"};
        static const char* kindName[kNumColliders + 1] = {"upper arm", "forearm", "hand", "thigh", "other side"};
        char line[900];
        size_t len = 0;
        for (int g = 0; g < kNumGroups; ++g)
            for (int k = 0; k <= kNumColliders; ++k) {
                const std::uint32_t hits = g_pairHits[g][k].exchange(0), mm = g_pairMaxMm[g][k].exchange(0);
                if (hits && len + 60 < sizeof(line))
                    len += snprintf(line + len, sizeof(line) - len, " %s<-%s %u (deepest %.1f cm);", groupName[g],
                                    kindName[k], hits, mm / 10.0);
            }
        if (len) Log("   contacts by pair:%s", line);
        if (g_cfg.others) {
            Log("   other bodies: %llu shared (%llu A-Body, %llu by dead bodies), %llu found near an A-Body body, oldest "
                "used %u ms, %llu passed over as too old, %llu skipped mid-write, %llu not shared (table full), %llu left out "
                "(crowd); their contacts are %s",
                static_cast<unsigned long long>(g_otherShared.exchange(0)), static_cast<unsigned long long>(g_otherSharedABody.exchange(0)),
                static_cast<unsigned long long>(g_otherSharedDead.exchange(0)),
                static_cast<unsigned long long>(g_otherNear.exchange(0)), g_otherOldestMs.exchange(0),
                static_cast<unsigned long long>(g_otherTooOld.exchange(0)), static_cast<unsigned long long>(g_otherTorn.exchange(0)),
                static_cast<unsigned long long>(g_otherFull.exchange(0)), static_cast<unsigned long long>(g_otherCrowd.exchange(0)),
                g_cfg.othersMeasureOnly ? "measured only" : "pushed");
            static const char* otherName[kNumOtherKinds] = {"upper arm", "forearm", "hand", "thigh", "chest", "pelvis", "calf",
                                                            "foot", "soft"};
            static const char* whoName[kNumWho] = {"yours", "NPCs touched by you", "NPCs touched by NPCs",
                                                   "with dead bodies (either one dead)"};
            DeepContact deep[kNumWho];
            std::uint32_t deepMm[kNumWho];
            {
                std::lock_guard<std::mutex> lock(g_deepMutex);
                for (int w = 0; w < kNumWho; ++w) {
                    deep[w] = g_deep[w];
                    deepMm[w] = g_deepMm[w].exchange(0);
                }
            }
            for (int w = 0; w < kNumWho; ++w) {
                len = 0;
                for (int g = 0; g < kNumGroups; ++g)
                    for (int k = 0; k < kNumOtherKinds; ++k) {
                        const std::uint32_t hits = g_otherHits[w][g][k].exchange(0), mm = g_otherMaxMm[w][g][k].exchange(0);
                        if (hits && len + 70 < sizeof(line))
                            len += snprintf(line + len, sizeof(line) - len, " %s<-other's %s %u (deepest %.1f cm);",
                                            groupName[g], otherName[k], hits, mm / 10.0);
                    }
                if (len) Log("   contacts with other bodies, %s:%s", whoName[w], line);
                if (deepMm[w]) {
                    const DeepContact& c = deep[w];
                    Log("   deepest, %s: %s <- other's %s %.1f cm deep (contact starts at %.1f cm); the bodies' COMs %.2f m "
                        "apart, the other's %.2f m higher; its entry %.0f ms old, moving %.1f m/s, moved %.1f cm forward; "
                        "other body %s%s, this body %s; this bone %.2f m above its COM (sphere %.1f cm), the other's %s "
                        "from %.2f to %.2f m above its COM (radius %.1f cm)", whoName[w], c.bone, otherName[c.kind],
                        c.depth * 100, c.contact * 100, c.apart, c.up, c.ageMs, c.speed, c.shiftCm,
                        c.abody ? "A-Body" : "not A-Body", c.otherDead ? " (dead)" : "", c.selfDead ? "dead" : "alive",
                        c.selfUp, c.bodyR * 100, otherName[c.kind], c.aUp, c.bUp, c.r * 100);
                }
            }
        }
    }
    if (have)
        Log("   sample (site %c, left breast): gravity in chest frame (up %.2f fwd %.2f left %.2f), hip tilt %.0f deg, "
            "thighs %.0f deg from vertical, weights back %.2f fwd %.2f left %.2f right %.2f, lying %.2f, level x%.2f, "
            "size x%.2f (breast jiggle x%.2f) -> anchor up %.2f fwd %.2f left %.2f cm",
            'A' + s.site, s.gc[0], s.gc[1], s.gc[2], s.hipTilt, s.thigh, s.w[0], s.w[1], s.w[2], s.w[3], s.lying, s.level,
            s.size, s.jiggle, s.off[0] * 100, s.off[1] * 100, s.off[2] * 100);
}

// ------------------------------------------------------------------ gravity
struct Vec3 { float x, y, z; };
static float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 Scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
static Vec3 Row(void* node, std::ptrdiff_t matrixOff, int r) {
    const float* m = reinterpret_cast<const float*>(static_cast<char*>(node) + matrixOff + 16 * r);
    return {m[0], m[1], m[2]};
}
static float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static float AngleDeg(const Vec3& unitA, const Vec3& unitB) {
    float d = Dot(unitA, unitB);
    d = d < -1 ? -1 : (d > 1 ? 1 : d);
    return std::acos(d) * 57.2957795f;
}

// Real-level offsets in cm, C_Chest frame (X up the body, Y forward, Z to the character's left); side 0 = left.
static const Vec3 kFaceUpSlide[2] = {{0.8f, -1.5f, 1.5f}, {0.8f, -1.5f, -1.5f}};   // whole breast only (fSlump 0)
static const Vec3 kFaceUpSlump[2] = {{0.5f, -1.0f, 0.8f}, {0.5f, -1.0f, -0.8f}};   // anchor part of the slump
static const Vec3 kLeftSide[2] = {{0, 0, 0.8f}, {0, 0, 1.8f}};
static const Vec3 kRightSide[2] = {{0, 0, -1.8f}, {0, 0, -0.8f}};
// Slump lying on the back, per breast bone, Real level at fSlump 1, in the anchor's frame (L_Pecs: X forward, Y down,
// Z left), LEFT breast; the right one mirrors Z.  Designed and measured with A-Body's p3_gravity_slump.py.
enum { kUpper, kLower, kInner, kOuter };
static const Vec3 kSlumpBone[4] = {{-1.0f, -0.3f, 0.5f}, {-0.8f, 0.0f, 1.0f}, {-0.5f, 0.0f, 1.2f}, {-1.0f, 0.0f, 2.0f}};
// The other shapes, same frames and units (Real, cm, the breast's own outside = +Z), designed and measured with
// A-Body's p3_gravity_shapes.py.
// Lying on a side: the breast below spreads toward the floor and armpit (SD2 "down"), the one above rolls over onto
// it (SD2 "up"); the whole-breast slide halves.  x2: nipples 2.5 / 3.6 cm, peak stretch 48%.
static const Vec3 kSideDown[4] = {{0, 0, 1.0f}, {-0.2f, 0, 1.4f}, {0, 0, 0.8f}, {0, 0, 2.2f}};
static const Vec3 kSideUp[4] = {{0, 0, -1.2f}, {0, 0, -1.6f}, {0, 0, -2.0f}, {0, 0, -0.8f}};
// Face down: squashed back onto the chest and spread up, down and out, on top of the 0.5 cm press (FD1d).  x2: nipple
// 1.8 cm back (0.7 before), nipples 0.7 cm further apart, peak stretch 32%; a 0.9 cm2 skin fold under the breast
// from More up (every face-down variant has it).
static const Vec3 kSquashBone[4] = {{-0.6f, -0.3f, 0}, {-0.8f, 0.3f, 0.3f}, {-0.4f, 0, 0}, {-0.6f, 0, 0.6f}};
// Bent forward on the feet: a teardrop, the tip (Lower, with the nipple) hanging most and the base closing in; the
// whole-breast hang halves.  x2: nipple 3.4 cm from the chest (1.8 before), peak stretch 53%.
static const Vec3 kHangBone[4] = {{1.0f, 0, 0}, {2.0f, 0.2f, 0}, {1.2f, 0, 0.4f}, {1.2f, 0, -0.4f}};
// How far each shape may grow with the breast size: the largest effective level (installed level x size) at which it
// stays within the approved back slump's peak stretch at x2 (62%) on the base body (measured on A-Body's mesh:
// at x3 the back slump reached 95%, the teardrop 81%, the side 76%, face down 46%).  A bigger breast stretches less at
// the same level (Large 100% at x3: 83 / 62 / 61 / 42%), so these hold for every size.
// these are the defaults of [Gravity] fMaxBack (2.0), fMaxSide (2.4), fMaxHang (2.3) and fMaxDown (3.0), in
// Settings.  The skin's own limits are higher (tests\stretch_limit_parts.py, folds or skin through itself: back and
// side from x3.75, bent forward and on all fours from x6, face down from x7; on all fours with the springs' sag at its
// most on top: none up to x5.75).

struct Anchor { void* node = nullptr; void* com = nullptr; const float* rest = nullptr; void* bones[4] = {}; };

// The springs write a bone's local translation each frame; the slump is added on top.  If a spring skipped a frame
// (at rest, or switched off by distance), the node still holds the plugin's last value, so the base is that minus the plugin's offset.
void WriteTranslation(void* node, const float t[3]);   // below: the node's local translation and its rig copy
void WriteRigCopy(void* node, const float t[3]);       // ... the rig's copy only
struct BoneState { void* node; float written[3]; float added[3]; std::uint32_t usedMs; };
static std::mutex g_boneMutex;
// g_boneMutex taken through this; the log counts the times a thread had to wait for it, and how long
struct BoneLock {
    std::unique_lock<std::mutex> lock;
    BoneLock() : lock(g_boneMutex, std::defer_lock) {
        if constexpr (kDiag) {
            if (lock.try_lock()) return;
            LARGE_INTEGER a, b;
            QueryPerformanceCounter(&a);
            lock.lock();
            QueryPerformanceCounter(&b);
            g_boneWaits.fetch_add(1, std::memory_order_relaxed);
            g_boneWaitTicks.fetch_add(static_cast<std::uint64_t>(b.QuadPart - a.QuadPart), std::memory_order_relaxed);
        } else {
            lock.lock();
        }
    }
};
// hashed by node; when 3/4 full, bones unused for 10 s are dropped (old actors').  16384: a body whose
// jiggle is scaled keeps up to ~55 bones here (its spring bones), so 4096 would have started over with ~37 such bodies
constexpr int kBoneBits = 14, kBoneSlots = 1 << kBoneBits;
static BoneState g_boneStates[kBoneSlots];
static BoneState g_boneKeep[kBoneSlots];   // scratch for the drop (g_boneMutex held)
static int g_boneCount = 0;

static std::uint64_t BoneHash(void* node) {
    return (reinterpret_cast<std::uintptr_t>(node) >> 4) * 0x9E3779B97F4A7C15ull >> (64 - kBoneBits);
}

// Keeps the bones used in the last 10 s: dropping a bone that still carries an offset would let that offset be taken
// for its base once and doubled (until its spring next moves).  If 2048+ are recent the table is cleared instead.
static void DropStaleStates(std::uint32_t now) {
    int kept = 0;
    for (const BoneState& st : g_boneStates)
        if (st.node && now - st.usedMs < 10000) g_boneKeep[kept++] = st;
    std::memset(g_boneStates, 0, sizeof(g_boneStates));
    g_boneCount = 0;
    if (kept >= kBoneSlots / 2) return;   // start over (the old behaviour; needs 100+ bodies with offsets at once)
    for (int i = 0; i < kept; ++i) {
        for (int probe = 0; probe < kBoneSlots; ++probe) {
            BoneState& st = g_boneStates[(BoneHash(g_boneKeep[i].node) + probe) & (kBoneSlots - 1)];
            if (st.node) continue;
            st = g_boneKeep[i];
            ++g_boneCount;
            break;
        }
    }
}

static BoneState* FindState(void* node, bool create) {   // g_boneMutex held
    const std::uint32_t now = static_cast<std::uint32_t>(GetTickCount64());
    const std::uint64_t h = BoneHash(node);
    for (int probe = 0; probe < kBoneSlots; ++probe) {
        BoneState& st = g_boneStates[(h + probe) & (kBoneSlots - 1)];
        if (st.node == node) {
            st.usedMs = now;
            return &st;
        }
        if (st.node) continue;
        if (!create) return nullptr;
        if (g_boneCount >= kBoneSlots * 3 / 4) {
            DropStaleStates(now);
            return FindState(node, true);
        }
        ++g_boneCount;
        const float* local = &At<float>(node, 0x70);
        st = {node, {local[0] + 1e9f, 0, 0}, {0, 0, 0}, now};   // never matches: first frame takes the node as is
        return &st;
    }
    return nullptr;
}

// The bone's position without the plugin's offset (what its spring or animation put there).
static bool BaseOf(void* node, float base[3]) {
    float local[3];
    if (!SafeCopy(local, static_cast<char*>(node) + 0x70, 12)) return false;
    std::memcpy(base, local, 12);
    BoneLock lock;
    if (const BoneState* st = FindState(node, false))
        if (std::memcmp(st->written, local, 12) == 0)
            for (int k = 0; k < 3; ++k) base[k] = st->written[k] - st->added[k];
    return true;
}

// g_boneMutex held: this frame's offset on the node's own copy in place of last frame's; true (the value in `out`)
// when the node was written, so its rig copy follows.  The node's own copy is read and written inside the lock: two
// writers of one bone at once would otherwise let one take the other's old offset for its base, and keep it (the game
// updates an actor on one thread, so only the stress test's shared bones meet this)
static bool AddOnTopLocked(void* node, const float add[3], float out[3]) {
    float* local = &At<float>(node, 0x70);
    const bool zero = add[0] == 0 && add[1] == 0 && add[2] == 0;
    BoneState* st = FindState(node, !zero);
    if (!st) return false;   // no offset now and never had one: nothing to write
    if (zero && st->added[0] == 0 && st->added[1] == 0 && st->added[2] == 0) return false;
    float base[3];
    std::memcpy(base, local, 12);
    if (std::memcmp(st->written, local, 12) == 0)
        for (int k = 0; k < 3; ++k) base[k] = st->written[k] - st->added[k];
    for (int k = 0; k < 3; ++k) {
        st->added[k] = add[k];
        st->written[k] = base[k] + add[k];
    }
    std::memcpy(local, st->written, 12);
    std::memcpy(out, st->written, 12);
    return true;
}

// one bone alone (the tests use it; the plugin writes a whole body at once: AddOnTopAll)
[[maybe_unused]] static void AddOnTop(void* node, const float add[3]) {
    float t[3];
    bool wrote;
    {
        BoneLock lock;
        wrote = AddOnTopLocked(node, add, t);
    }
    if (wrote) WriteRigCopy(node, t);   // the rig's copy (game calls: outside the plugin's lock)
}

static bool IsOurFaceUpRecord(void* data, int* side, float* level) {
    const float* v = &At<float>(data, 0x20);
    const float* pos = &At<float>(data, 0x2C);
    if (At<std::uint8_t>(data, 0x58) != 1 || std::fabs(v[0]) > 1e-4f || std::fabs(v[1]) > 1e-4f || std::fabs(v[2] - 1.0f) > 1e-4f)
        return false;
    if (At<float>(data, 0x50) != 0.0f || std::fabs(At<float>(data, 0x54) - 90.0f) > 1e-3f) return false;
    if (!(pos[1] < 0) || std::fabs(pos[2]) < 1e-5f) return false;           // flatten (-Y) and spread (+-Z)
    *side = pos[2] > 0 ? 0 : 1;                                              // left breast spreads to +Z
    *level = -pos[1] / 0.015f;                                               // Real = 1.5 cm toward the chest
    return *level > 0.1f && *level < 10.0f;
}

void WriteTranslation(void* node, const float t[3]) {
    std::memcpy(static_cast<char*>(node) + 0x70, t, 12);
    WriteRigCopy(node, t);
}

void WriteRigCopy(void* node, const float t[3]) {
    void* rig = At<void*>(node, 0x20);
    if (!rig) return;
    _InterlockedIncrement(reinterpret_cast<volatile long*>(rig));
    const std::uint64_t idx = g_rigFind(static_cast<char*>(rig) + 0x48, static_cast<char*>(node) + 0x10);
    if (idx != At<std::uint64_t>(rig, 0x60)) {
        const std::uint64_t value = *reinterpret_cast<std::uint64_t*>(At<char*>(rig, 0x58) + idx * 24 + 8);
        const std::uint64_t slot = value & 0x3FF;
        if (slot != 0x3FF) std::memcpy(At<char*>(At<void*>(rig, 0x08), 0x10) + slot * 64 + 0x30, t, 12);
    }
    g_rigRelease(rig);
}

// a spring's record as tuned (strength, damping ratio, MaxDist per COM axis) and the swing on top of it
struct SpringEff { float strength[3], ratio[3], travel[3], swing; };

// One actor's view of its modifier list: A-Body's gravity anchors and its springs (driver COM).
static_assert(kNumParts == 8, "ActorView's per-part defaults below list 8 parts");
struct ActorView {
    static constexpr int kMaxAdds = 192;   // every spring bone (body shape scales up to 50) + the soft and breast bones
    Anchor anchors[2];
    float level = 0;
    void* com = nullptr;
    void* springs[128];  // SpringBone instances driven by COM
    const struct SpringInfo* infos[128] = {};   // ... their record entries, looked up once per update (InfoOf)
    int nSprings = 0;
    float anchorLocal[2][3] = {};   // the anchors' local translation written this frame
    bool anchorWritten[2] = {};
    void* addNode[kMaxAdds];        // offsets to add on top of the springs this frame (jiggle, slump, collision), per bone
    float add[kMaxAdds][3];
    int nAdd = 0;
    int site = -1;                  // which call path (for the snapshot)
    void* owner = nullptr;          // the character's 3D root (the hook's first argument)
    void* actor = nullptr;          // the character from the game's call (only compared, to tell bodies apart)
    bool dead = false;              // this body is dead (the game's life state), when collisions are on
    float size = 1.0f;              // breast gravity x this (breast size sliders, x the body's gravity)
    float jiggle[kNumParts] = {1, 1, 1, 1, 1, 1, 1, 1};    // per part, 1 = as installed
    float gravity[kNumParts] = {1, 1, 1, 1, 1, 1, 1, 1};   // ... the gravity inside the part's springs
    bool shapeOn = false;           // ... [BodyShape] in use for this body
    float partCeil[kNumParts][3];   // ... each part's jiggle ceiling per COM axis (PartCeilings)
    bool ceilReady = false;
    float strain = 1.0f;            // the stretch guard (StrainOf)
    float ceilScale = 1.0f;         // ... the gravity shapes' ceilings x this
    bool tuned = false;             // a character, outfit or planet factor in use for this body
    SpringEff eff[128];             // ... each spring as tuned, worked out once per update (EffOf)
    bool effDone[128] = {};
    float settle[128][3];           // each spring's gravity shape this frame (COM axes, m): PreUpdate, AddSettle
    bool settleOn = false;          // ... some part of this body has its gravity shape on
    bool paused = false;            // the game is paused this frame (settle[] holds each spring's rest point)
    void AddTo(void* node, const float d[3]) {
        for (int i = 0; i < nAdd; ++i)
            if (addNode[i] == node) {
                for (int k = 0; k < 3; ++k) add[i][k] += d[k];
                return;
            }
        if (nAdd < kMaxAdds) {
            addNode[nAdd] = node;
            for (int k = 0; k < 3; ++k) add[nAdd][k] = d[k];
            ++nAdd;
        }
    }
    const float* AddFor(void* node) const {
        for (int i = 0; i < nAdd; ++i)
            if (addNode[i] == node) return add[i];
        return nullptr;
    }
};

// Every offset of this body this frame under one lock (a body has up to 50 spring bones),
// then the rig copies (game calls: outside the plugin's lock)
static void AddOnTopAll(const ActorView& v) {
    void* nodes[ActorView::kMaxAdds];
    float written[ActorView::kMaxAdds][3];
    int n = 0;
    {
        BoneLock lock;
        for (int i = 0; i < v.nAdd; ++i)
            if (AddOnTopLocked(v.addNode[i], v.add[i], written[n])) nodes[n++] = v.addNode[i];
    }
    for (int i = 0; i < n; ++i) WriteRigCopy(nodes[i], written[i]);
}

// --- before the game's update: fixed travel limits, and gravity in the springs
// Per spring record (SpringBoneData, shared by every actor): its original travel limits, copied before the plugin
// ever changes them, and which part it moves, from its target bone's name.  Hashed by record.
enum SpringGroup { kGroupOther, kGroupBreast, kGroupBelly, kGroupButt, kGroupThighs };
// side/bone: breasts.  part: which part's sliders scale it, -1 = not one of the body's 50 TF_ spring bones;
// ceil: its own jiggle ceiling per COM axis (the release's largest travel / the installed one, at least 1; a part
// takes the smallest of its bones', PartCeilings);
// dampFloor: the lowest damping ratio a body may give it (the release's lowest, or the record's own if lower)
// presetBone: its entry in preset_data.h (-1: not one of the 50); instPreset / instExtra: the installer
// setting its record was built with (the physics preset and extra-jiggle level whose springs match it exactly; -1:
// none, e.g. another mod's springs); origStrength / origRatio: the record's strength and damping ratio as installed;
// maxTravel: the release's largest MaxDist for the bone (Max physics + Max jiggle)
struct SpringInfo {
    std::atomic<void*> data;
    float orig[3];
    int group, side, bone, part;
    float ceil[3], dampFloor[3];
    int presetBone, instPreset, instExtra;
    float origStrength[3], origRatio[3], maxTravel[3];
    mutable std::atomic<bool> touched;   // the plugin has given the record other springs (put back when untuned)
};
static std::mutex g_springMutex;   // only for adding; lookups take no lock (entries are only ever added, published whole)
constexpr int kSpringSlots = 2048;
static SpringInfo g_springInfo[kSpringSlots];
static int g_springCount = 0;

// Which part a spring moves, from its target bone's name; for the breasts also which side and which of the four bones
// (TF_Breast_Upper_L ...), so an outfit's own spring bone under the breast anchor is never taken for one of them.
static void Classify(void* target, SpringInfo& e) {
    e.group = kGroupOther;
    e.side = e.bone = -1;
    char name[64];
    if (!NodeName(target, name, sizeof(name))) return;
    if (!strncmp(name, "TF_Breast_", 10)) {
        e.group = kGroupBreast;
        static const char* kBones[4] = {"Upper_", "Lower_", "Inner_", "Outer_"};   // kUpper, kLower, kInner, kOuter
        for (int b = 0; b < 4; ++b)
            if (!strncmp(name + 10, kBones[b], 6) && (name[16] == 'L' || name[16] == 'R') && !name[17]) {
                e.bone = b;
                e.side = name[16] == 'L' ? 0 : 1;
            }
    } else if (!strncmp(name, "TF_Abdomen_", 11)) {
        e.group = kGroupBelly;
    } else if (!strncmp(name, "TF_Glute_", 9)) {
        e.group = kGroupButt;
    } else if (!strncmp(name, "TF_Thigh_", 9)) {
        e.group = kGroupThighs;
    }
}

// the body-shape limits of one of the body's own spring bones (by its exact name), from its record as
// installed (orig = its MaxDist before the plugin changed anything; +0x30 its damping ratio, never changed)
static void ShapeLimits(void* data, void* target, SpringInfo& e) {
    e.part = -1;
    for (int k = 0; k < 3; ++k) e.ceil[k] = 1.0f, e.dampFloor[k] = 0.0f, e.maxTravel[k] = e.orig[k];
    char name[64];
    if (!NodeName(target, name, sizeof(name))) return;
    const float* ratio = &At<float>(data, 0x30);
    for (const ShapeBoneDef& d : kShapeBones) {
        if (strcmp(name, d.name)) continue;
        e.part = d.part;
        for (int k = 0; k < 3; ++k) {
            e.ceil[k] = e.orig[k] > 1e-6f && d.maxDist[k] > e.orig[k] ? d.maxDist[k] / e.orig[k] : 1.0f;
            e.dampFloor[k] = ratio[k] < d.damp[k] ? ratio[k] : d.damp[k];
            e.maxTravel[k] = d.maxDist[k] > e.orig[k] ? d.maxDist[k] : e.orig[k];
        }
        return;
    }
}

// which installer setting a record was built with: the physics preset and extra-jiggle level whose springs
// for its bone (preset_data.h) match its strength, damping ratio and MaxDist as installed, to 1 part in 10,000.  Every
// setting differs from every other preset's on every bone; the extra-jiggle levels only differ on the parts they move
static void MatchPreset(void* target, SpringInfo& e) {
    e.presetBone = e.instPreset = e.instExtra = -1;
    char name[64];
    if (!NodeName(target, name, sizeof(name))) return;
    auto same3 = [](const float* x, const float* y) {
        for (int k = 0; k < 3; ++k)
            if (!(std::fabs(x[k] - y[k]) <= 1e-4f * (std::fabs(y[k]) > 1.0f ? std::fabs(y[k]) : 1.0f))) return false;
        return true;
    };
    for (int b = 0; b < kNumPresetBones; ++b) {
        if (strcmp(name, kPresetBones[b].name)) continue;
        e.presetBone = b;
        for (int p = 0; p < kNumSpringPresets && e.instPreset < 0; ++p)
            for (int j = 0; j < kNumExtraJiggle; ++j) {
                const PresetSpring& s = kPresetBones[b].s[p][j];
                if (same3(e.origStrength, s.strength) && same3(e.origRatio, s.damping) && same3(e.orig, s.maxDist)) {
                    e.instPreset = p;
                    e.instExtra = j;
                    break;
                }
            }
        return;
    }
}

static const SpringInfo* SpringInfoFor(void* data, void* target) {
    const std::uint64_t h = (reinterpret_cast<std::uintptr_t>(data) >> 4) * 0x9E3779B97F4A7C15ull >> 53;
    for (int probe = 0; probe < kSpringSlots; ++probe) {   // without the lock: found, or not there yet
        SpringInfo& e = g_springInfo[(h + probe) & (kSpringSlots - 1)];
        void* d = e.data.load(std::memory_order_acquire);
        if (d == data) return &e;
        if (!d) break;
    }
    std::lock_guard<std::mutex> lock(g_springMutex);
    for (int probe = 0; probe < kSpringSlots; ++probe) {
        SpringInfo& e = g_springInfo[(h + probe) & (kSpringSlots - 1)];
        void* d = e.data.load(std::memory_order_relaxed);
        if (d == data) return &e;   // another thread just added it
        if (d) continue;
        if (g_springCount >= kSpringSlots * 3 / 4) return nullptr;   // never forget an original: stop adding instead
        ++g_springCount;
        std::memcpy(e.orig, &At<float>(data, 0x50), 12);   // copied before the plugin ever changes it
        std::memcpy(e.origStrength, &At<float>(data, 0x20), 12);   // and these two
        std::memcpy(e.origRatio, &At<float>(data, 0x30), 12);
        Classify(target, e);
        ShapeLimits(data, target, e);
        MatchPreset(target, e);
        e.data.store(data, std::memory_order_release);   // published whole: readers see orig and group with it
        return &e;
    }
    return nullptr;
}

// A spring's record entry, looked up once per body-update (PreUpdate, PostUpdate and the passes in between all ask)
static const SpringInfo* InfoOf(ActorView& v, int i) {
    if (!v.infos[i]) v.infos[i] = SpringInfoFor(At<void*>(v.springs[i], 0x20), At<void*>(v.springs[i], 0x18));
    return v.infos[i];
}

static bool Collect(void* owner, ActorView& v) {
    if (!owner) return false;
    v.owner = owner;
    void* extra = g_getExtra(owner);
    if (!extra) return false;
    void* allSprings[256];   // the body's and any outfit's springs
    int nAll = 0;
    const std::uintptr_t pdVtbl = g_addr[kPoseDeformerVtbl], sbVtbl = g_addr[kSpringBoneVtbl];
    for (void* inst = At<void*>(extra, 0x28); inst; inst = At<void*>(inst, 0x08)) {
        const std::uintptr_t vt = At<std::uintptr_t>(inst, 0);
        if (vt == sbVtbl) {
            if (nAll < 256 && At<void*>(inst, 0x18) && At<void*>(inst, 0x20)) allSprings[nAll++] = inst;
            continue;
        }
        if (vt != pdVtbl) continue;
        int side;
        float lv;
        void* data = At<void*>(inst, 0x20);
        if (!data || !IsOurFaceUpRecord(data, &side, &lv)) continue;
        v.anchors[side].node = At<void*>(inst, 0x18);
        v.anchors[side].com = At<void*>(inst, 0x10);
        v.anchors[side].rest = &At<float>(inst, 0x28);
        v.level = lv;
    }
    if (!v.anchors[0].node && !v.anchors[1].node) return false;   // not A-Body's gravity: leave this actor alone
    v.com = v.anchors[0].com ? v.anchors[0].com : v.anchors[1].com;
    for (int i = 0; i < nAll; ++i)
        if (At<void*>(allSprings[i], 0x10) == v.com && v.nSprings < 128) v.springs[v.nSprings++] = allSprings[i];
    // the four breast bones, by name (TF_Breast_Upper_L ...), and only if they hang under their anchor
    for (int i = 0; i < v.nSprings; ++i) {
        void* target = At<void*>(v.springs[i], 0x18);
        const SpringInfo* info = InfoOf(v, i);
        if (!info || info->group != kGroupBreast || info->bone < 0) continue;
        Anchor& a = v.anchors[info->side];
        if (a.node && At<void*>(target, 0x38) == a.node) a.bones[info->bone] = target;
    }
    return true;
}

// ------------------------------------------------------------------ breast size
// sfee's body sliders are morph targets with a weight on the character's record: TESNPC+0x448 points at a table
// (made on the first non-zero weight; TESNPC::Load, BMPN/BMPV -> 0xce7990): entries at +0x18, capacity at +0x20,
// 24 bytes each (name BSFixedString, weight at +8, slot marker at +0x10, -1 = empty); weights 0..1.  The character
// is found from its 3D root, and its record from the character, by class (vtables checked against the exe's own
// class names at load); the offsets found are logged and reused.
constexpr std::ptrdiff_t kNpcMorphWeights = 0x448;
static std::uintptr_t g_actorVt = 0, g_playerVt = 0, g_npcVt = 0;   // absolute; 0 = size scaling unavailable
static std::atomic<int> g_refOff{-1}, g_npcOff{-1};
static std::atomic<int> g_sizeLogs{0};

static bool VtableAt(void* obj, const std::uintptr_t* vts, int n) {
    std::uintptr_t vt = 0;
    if (!obj || (reinterpret_cast<std::uintptr_t>(obj) & 7) || !SafeProbe(&vt, obj, 8)) return false;
    for (int i = 0; i < n; ++i)
        if (vt == vts[i]) return true;
    return false;
}

// The same check for an object the game handed over or points at (a live object): a guarded copy, never
// VirtualQuery.  One query can take milliseconds (the kernel measures the whole region, and the game's heap regions
// are huge: 65 us at 64 MB, 0.6 ms at 512 MB, 16 ms at 2 GB), so VtableAt is only for pointers that may be anything
// (the searches).
static bool ClassAt(void* obj, const std::uintptr_t* vts, int n) {
    std::uintptr_t vt = 0;
    if (!obj || (reinterpret_cast<std::uintptr_t>(obj) & 7) || !SafeCopy(&vt, obj, 8)) return false;
    for (int i = 0; i < n; ++i)
        if (vt == vts[i]) return true;
    return false;
}

// The object of one of these classes that `holder` points at: first at the offset found before, else by scanning.
static void* FindByClass(void* holder, int span, const std::uintptr_t* vts, int n, std::atomic<int>& known) {
    const int k = known.load(std::memory_order_relaxed);
    void* p = nullptr;
    if (k >= 0 && SafeCopy(&p, static_cast<char*>(holder) + k, 8) && ClassAt(p, vts, n)) return p;   // no query
    for (int off = 0; off < span; off += 8) {
        if (!SafeCopy(&p, static_cast<char*>(holder) + off, 8) || !VtableAt(p, vts, n)) continue;
        if (known.exchange(off) != off) Log("size: found at +0x%X (%s)", off, &known == &g_refOff ? "3D root -> character" : "character -> record");
        return p;
    }
    return nullptr;
}

// The sliders read per body (sfee's keys, abody_morph.json)
enum Slider { kBreastsLarge, kBreastsSmall, kButtLarge, kButtSmall, kHipsWide, kHipsNarrow, kWaistThin, kThighsLarge,
              kThighsSlim, kCalvesLarge, kCalvesSmall, kArmsLarge, kArmsSmall, kBreastsGravity, kBreastsTogether,
              kNipplesLarge, kNipplesSmall, kNumSliders };
static const char* const kSliderKey[kNumSliders] = {
    "TF_BreastsLarge", "TF_BreastsSmall", "TF_ButtLarge", "TF_ButtSmall", "TF_HipsWide", "TF_HipsNarrow", "TF_WaistThin",
    "TF_ThighsLarge", "TF_ThighsSlim", "TF_CalvesLarge", "TF_CalvesSmall", "TF_ArmsLarge", "TF_ArmsSmall",
    "TF_BreastsGravity", "TF_BreastsTogether", "TF_NipplesLarge", "TF_NipplesSmall"};

// The body triangle: three floats on the record, TESNPC+0x350 Thin, +0x354 Strong (muscular), +0x358
// Overweight (heavy), 0 to 1 each.  The game hands them to the body as morph weights of those names, next to the
// slider table's (0x34ed40, clamped 0..1), so (0, 0, 0) is the base body; vanilla NPCs mostly sit there or at
// (1/3, 1/3, 1/3), the triangle's centre.  So what all three share counts as none: thin = Thin - the smallest of the
// three, and so on.  Checked at load (CheckSizeReading).
constexpr std::ptrdiff_t kNpcTriangle = 0x350;
static bool g_triangleOk = false;

struct SizeRead {
    bool character = false, record = false, triangle = false;
    int weights = 0;
    float slider[kNumSliders] = {};
    float tri[3] = {};      // Thin, Strong, Overweight as stored
    char names[320] = {};   // the weights found, for the log
    // the player's body; the reference's and the record's form IDs (0: not read); what it wears (each worn
    // armour's form ID and biped slots); the player's gravity scale (-1: not read)
    bool player = false;
    std::uint32_t refId = 0, baseId = 0;
    int nWorn = 0;
    std::uint32_t wornForm[24] = {};
    std::uint64_t wornSlots[24] = {};
    float gravityScale = -1.0f;
};

// what a character wears, the way WornCoversBipedSlot (ID 65296) reads it: the inventory at +0xA0 (the game
// holds the lock at +0xA8 for it; this guarded read goes without, so a list changing under it gives one wrong answer,
// for 2 s at most), its count at +0x28 and its entries (0x28 bytes) at +0x30: the item at +0x00, worn when the byte at
// +0x20 has any of bits 0-2; an armour (form type 0x22 at +0x2E) covers the biped slots set in the 64 bits at +0x208
static void ReadWorn(void* actor, SizeRead& r) {
    char* inv = nullptr;
    char* items = nullptr;
    std::uint32_t count = 0;
    if (!SafeCopy(&inv, static_cast<char*>(actor) + 0xA0, 8) || !inv || !SafeCopy(&count, inv + 0x28, 4) ||
        !SafeCopy(&items, inv + 0x30, 8) || !items || count > 4096)
        return;
    for (std::uint32_t i = 0; i < count && r.nWorn < 24; ++i) {
        unsigned char e[0x28];
        if (!SafeCopy(e, items + static_cast<size_t>(i) * 0x28, sizeof(e))) return;
        if (!(e[0x20] & 7)) continue;
        void* form = nullptr;
        std::memcpy(&form, e, 8);
        std::uint8_t type = 0;
        std::uint64_t mask = 0;
        std::uint32_t id = 0;
        if (!form || !SafeCopy(&type, static_cast<char*>(form) + 0x2E, 1) || type != 0x22 ||
            !SafeCopy(&mask, static_cast<char*>(form) + 0x208, 8) || !SafeCopy(&id, static_cast<char*>(form) + kFormId, 4))
            continue;
        r.wornForm[r.nWorn] = id;
        r.wornSlots[r.nWorn++] = mask;
    }
}

static std::atomic<bool> g_hintLogged{false};
static float Unit(float v) { return !(v > 0) ? 0.0f : (v > 1 ? 1.0f : v); }   // 0..1, and NaN -> 0

static SizeRead ReadSize(void* owner, void* actorHint) {
    SizeRead r;
    const std::uintptr_t actors[2] = {g_actorVt, g_playerVt};
    // the character the game itself held when it asked for this 3D (see kSiteActor); the 3D root holds no pointer to
    // it, so there is nothing to search there
    (void)owner;
    void* actor = ClassAt(actorHint, actors, 2) ? actorHint : nullptr;   // the game's own object, no query
    if (kDiag && actor && !g_hintLogged.exchange(true)) Log("size: character taken from the game's call (its register)");
    if (!actor) return r;
    r.character = true;
    // who it is, what it wears, and for the player the gravity it feels
    r.player = ClassAt(actor, &g_playerVt, 1);
    if (g_formIdOk) SafeCopy(&r.refId, static_cast<char*>(actor) + kFormId, 4);
    if (g_wornOk) ReadWorn(actor, r);
    float gs = -1.0f;
    if (r.player && g_gravityScaleOk && SafeCopy(&gs, static_cast<char*>(actor) + kPlayerGravityScale, 4) && gs >= 0.0f && gs < 10.0f)
        r.gravityScale = gs;
    void* npc = FindByClass(actor, 0x300, &g_npcVt, 1, g_npcOff);
    if (!npc) return r;
    r.record = true;
    if (g_formIdOk) SafeCopy(&r.baseId, static_cast<char*>(npc) + kFormId, 4);
    if (g_triangleOk && SafeCopy(r.tri, static_cast<char*>(npc) + kNpcTriangle, 12)) r.triangle = true;
    void* table = nullptr;
    if (!SafeCopy(&table, static_cast<char*>(npc) + kNpcMorphWeights, 8) || !table) return r;   // no weights at all
    char* entries = nullptr;
    std::uint64_t cap = 0;
    if (!SafeCopy(&entries, static_cast<char*>(table) + 0x18, 8) || !SafeCopy(&cap, static_cast<char*>(table) + 0x20, 8) ||
        !entries || cap > 4096)
        return r;
    for (std::uint64_t i = 0; i < cap; ++i) {
        std::int32_t marker = -1;
        void* key = nullptr;
        float w = 0;
        char* e = entries + i * 24;
        if (!SafeCopy(&marker, e + 0x10, 4) || marker == -1) continue;
        if (!SafeCopy(&key, e, 8) || !key || !SafeCopy(&w, e + 8, 4)) continue;   // the key is read guarded below
        ++r.weights;
        char name[48];
        if (!PoolString(key, name, sizeof(name))) continue;
        const size_t len = strlen(r.names);
        if (kDiag && len + 60 < sizeof(r.names)) snprintf(r.names + len, sizeof(r.names) - len, " %s %.2f;", name, w);
        for (int k = 0; k < kNumSliders; ++k)
            if (!strcmp(name, kSliderKey[k])) r.slider[k] = Unit(w);
    }
    return r;
}

// A body's factors, from what was read: 1 = as installed.  jiggle: its springs' swing and damping;
// gravity: the gravity inside its springs, and for the breasts also the gravity shapes (ActorView::size).
struct BodyFactors {
    float breastSize = 1.0f;   // the breast size factor ([Size])
    float jiggle[kNumParts], gravity[kNumParts];
    float thin = 0, muscular = 0, heavy = 0;   // the body's place in the triangle (the shared part taken off)
    float strain = 1.0f;       // how much more its breast skin stretches than the base body's (StrainOf)
    float ceilScale = 1.0f;    // ... so its gravity shapes' ceilings x this (1 / strain, 0.15 to 1)
    bool shape = false;        // [BodyShape] in use for this body
    bool tuned = false;        // a character, outfit or planet factor in use for this body
    BodyFactors() {
        for (int p = 0; p < kNumParts; ++p) jiggle[p] = gravity[p] = 1.0f;
    }
};

// The stretch guard.  The gravity shapes' ceilings (back x2.0, side x2.4, bent forward x2.3, face down x3)
// were measured on the base body (the approved back slump: 62% stretch).  The body morphs change how far the same
// offsets stretch the breast skin (tests\body_shape_gravity.py on the v0.7 mesh, shapes alone, against the base
// body): Breasts Small x(1 + w^2) (x2.0 at 1: smaller breasts stretch twice as much), Thin x1.4, Strong x1.04, Large
// x0.9, Overweight x0.82; Together x1.07, Nipples Large x1.06, Gravity x1.03, Nipples Small x1.02 on their own, but
// more together (Together with Nipples Large x1.27; with thin and small breasts, Together and Gravity took a
// strain-scaled body to 81% where the limit was 70%), so the small sliders count about double and the product is
// raised to the power 1.15 above 1.  Large and Small count as their difference (Large 0.3 with Small 0.75 measured
// x1.08, not x1.55).  The triangle counts as stored: the mesh carries all three morphs.  The result errs high on
// every body measured (19 bodies x 3 presets x 3 gravity levels), so the guard errs on the safe side.
static float StrainOf(const SizeRead& r) {
    const float thin = Unit(r.tri[0]), mus = Unit(r.tri[1]), heavy = Unit(r.tri[2]);
    const float* sl = r.slider;
    const float net = sl[kBreastsLarge] - sl[kBreastsSmall];
    const float size = net < 0 ? 1.0f + net * net : 1.0f - 0.1f * net;
    const float s = (1.0f + 0.45f * thin) * (1.0f + 0.04f * mus) * (1.0f - 0.18f * heavy) * size *
                    (1.0f + 0.05f * sl[kBreastsGravity]) * (1.0f + 0.12f * sl[kBreastsTogether]) *
                    (1.0f + 0.10f * sl[kNipplesLarge]) * (1.0f + 0.03f * sl[kNipplesSmall]);
    return s > 1.0f ? std::pow(s, 1.15f) : s;
}

// on top of the body's shape: the character's own section, what it wears (per part the lowest factor of all
// its pieces; a section naming an armour's form ID decides for that piece instead of the slots), and the planet's
// gravity (the player's gravity scale, kept for every body: the bodies loaded are near the player)
static std::atomic<std::uint32_t> g_gravityScaleBits{0};
static std::atomic<bool> g_gravityKnown{false};
static float PlanetFactor(const TuneSettings& T) {
    if (!T.planet || !g_gravityKnown.load(std::memory_order_relaxed)) return 1.0f;
    const std::uint32_t bits = g_gravityScaleBits.load(std::memory_order_relaxed);
    float s;
    std::memcpy(&s, &bits, 4);
    const float k = 1.0f + T.planetAmount * (s - 1.0f);
    return !(k >= 0.0f) ? 0.0f : (k > 3.0f ? 3.0f : k);
}
static void TuneBody(const SizeRead& r, BodyFactors& f) {
    const TuneSettings& T = Tune();
    for (int i = T.nChars - 1; i >= 0; --i) {   // the last section naming it
        const CharTune& c = T.chars[i];
        if (!(c.player ? r.player : (c.id && (c.id == r.refId || c.id == r.baseId)))) continue;
        for (int p = 0; p < kNumParts; ++p) f.jiggle[p] *= c.part[p];
        f.tuned = true;
        break;
    }
    if (T.outfitsOn && T.nOutfits) {
        float most[kNumParts];
        for (float& m : most) m = 1.0f;
        bool any = false;
        for (int w = 0; w < r.nWorn; ++w) {
            const OutfitTune* own = nullptr;
            for (int o = 0; o < T.nOutfits && !own; ++o)
                if (T.outfits[o].form && T.outfits[o].form == r.wornForm[w]) own = &T.outfits[o];
            for (int o = 0; o < T.nOutfits; ++o) {
                const OutfitTune& ot = T.outfits[o];
                if (own ? &ot != own : (ot.form || !(ot.slots & r.wornSlots[w]))) continue;
                for (int p = 0; p < kNumParts; ++p) most[p] = ot.part[p] < most[p] ? ot.part[p] : most[p];
                any = true;
            }
        }
        if (any) {
            for (int p = 0; p < kNumParts; ++p) f.jiggle[p] *= most[p];
            f.tuned = true;
        }
    }
    if (r.gravityScale >= 0.0f) {
        std::uint32_t bits;
        std::memcpy(&bits, &r.gravityScale, 4);
        g_gravityScaleBits.store(bits, std::memory_order_relaxed);
        g_gravityKnown.store(true, std::memory_order_relaxed);
    }
    const float k = PlanetFactor(T);
    if (k != 1.0f) {
        for (float& g : f.gravity) g *= k;
        f.tuned = true;
    }
}

static BodyFactors FactorsFrom(const SizeRead& r) {
    BodyFactors f;
    const float size = 1.0f + g_cfg.sizeLarge * r.slider[kBreastsLarge] - g_cfg.sizeSmall * r.slider[kBreastsSmall];
    f.breastSize = !g_cfg.sizeScaling ? 1.0f : (g_cfg.sizeOverride > 0 ? g_cfg.sizeOverride : (size < 0.1f ? 0.1f : size));
    f.gravity[kPartBreast] = f.breastSize;   // without [BodyShape]: the breast size alone (but for the guard, below)
    if (g_cfg.stretchGuard) {
        f.strain = StrainOf(r);
        const float c = 1.0f / f.strain;
        f.ceilScale = c > 1.0f ? 1.0f : (c < 0.15f ? 0.15f : c);
    }
    if (!g_cfg.shape) {
        TuneBody(r, f);
        return f;
    }
    f.shape = true;
    const float t[3] = {Unit(r.tri[0]), Unit(r.tri[1]), Unit(r.tri[2])};
    const float common = std::fmin(t[0], std::fmin(t[1], t[2]));
    f.thin = t[0] - common;
    f.muscular = t[1] - common;
    f.heavy = t[2] - common;
    const float* sl = r.slider;
    float part[kNumParts];
    part[kPartBreast] = f.breastSize;
    part[kPartButt] = 1.0f + g_cfg.partButt * (sl[kButtLarge] - sl[kButtSmall]);
    part[kPartBelly] = 1.0f - g_cfg.partWaist * sl[kWaistThin];
    part[kPartThigh] = 1.0f + g_cfg.partThighs * (sl[kThighsLarge] - sl[kThighsSlim]);
    part[kPartHip] = 1.0f + g_cfg.partHips * (sl[kHipsWide] - sl[kHipsNarrow]);
    part[kPartCalf] = 1.0f + g_cfg.partCalves * (sl[kCalvesLarge] - sl[kCalvesSmall]);
    part[kPartArm] = 1.0f + g_cfg.partArms * (sl[kArmsLarge] - sl[kArmsSmall]);
    part[kPartTorso] = 1.0f;
    auto held = [](float v) { return !(v >= g_cfg.shapeMin) ? g_cfg.shapeMin : (v > g_cfg.shapeMax ? g_cfg.shapeMax : v); };
    for (int p = 0; p < kNumParts; ++p) {
        const float hx = p == kPartBelly ? g_cfg.bellyHeavy : 1.0f;
        const float jb = g_cfg.overrideJiggle > 0 ? g_cfg.overrideJiggle
                         : 1.0f + g_cfg.jThin * f.thin + g_cfg.jMuscular * f.muscular + g_cfg.jHeavy * f.heavy * hx;
        const float gb = g_cfg.overrideGravity > 0 ? g_cfg.overrideGravity
                         : 1.0f + g_cfg.gThin * f.thin + g_cfg.gMuscular * f.muscular + g_cfg.gHeavy * f.heavy * hx;
        f.jiggle[p] = held(jb * part[p]);
        f.gravity[p] = held(gb * part[p]);
    }
    TuneBody(r, f);
    return f;
}

static bool SameFactors(const BodyFactors& a, const BodyFactors& b) {
    bool same = std::fabs(a.breastSize - b.breastSize) < 0.005f && std::fabs(a.ceilScale - b.ceilScale) < 0.005f;
    for (int p = 0; p < kNumParts; ++p)
        same &= std::fabs(a.jiggle[p] - b.jiggle[p]) < 0.005f && std::fabs(a.gravity[p] - b.gravity[p]) < 0.005f;
    return same;
}

struct SizeEntry { void* owner; BodyFactors f; std::uint64_t nextMs; };
static std::mutex g_sizeMutex;
constexpr int kSizeSlots = 512;
static SizeEntry g_sizes[kSizeSlots];
static std::atomic<int> g_shapeChangeLogs{0};

static void LogFactors(const char* what, const SizeRead& r, const BodyFactors& f) {
    char parts[400];
    size_t len = 0;
    for (int p = 0; p < kNumParts; ++p)
        len += snprintf(parts + len, sizeof(parts) - len, " %s %.2f/%.2f;", kPartName[p], f.jiggle[p], f.gravity[p]);
    // who, and what it wears
    std::uint64_t slots = 0;
    for (int w = 0; w < r.nWorn; ++w) slots |= r.wornSlots[w];
    char who[160];
    snprintf(who, sizeof(who), " (%s, reference %08X, record %08X, wearing %d pieces, biped slots 0x%016llX%s)",
             r.player ? "you" : "not you", r.refId, r.baseId, r.nWorn, static_cast<unsigned long long>(slots),
             r.gravityScale >= 0.0f ? ", gravity scale read" : "");
    Log("%s%s: character %s, record %s, %d morph weights, triangle %s (stored thin %.2f muscular %.2f heavy %.2f -> %.2f "
        "%.2f %.2f), TF_BreastsLarge %.2f, TF_BreastsSmall %.2f -> breast size x%.2f, breast gravity x%.2f; stretch guard: "
        "breast strain x%.2f, shape ceilings x%.2f; jiggle/gravity per part:%s%s", what, who, r.character ? "found" : "NOT found",
        r.record ? "found" : "NOT found", r.weights, r.triangle ? "read" : "not read", r.tri[0], r.tri[1], r.tri[2], f.thin,
        f.muscular, f.heavy, r.slider[kBreastsLarge], r.slider[kBreastsSmall], f.breastSize, f.gravity[kPartBreast],
        f.strain, f.ceilScale, f.shape || f.tuned ? parts : " (body shape off)", r.names);
}

// the tuning in use, at load and whenever it is read again
static void LogTuning(const char* what, const TuneSettings& t) {
    if constexpr (!kDiag) {
        (void)what;
        (void)t;
        return;
    } else {
        char parts[3600];
        size_t len = 0;
        for (int p = 0; p < kNumParts && len + 430 < sizeof(parts); ++p) {
            const PartTune& pt = t.part[p];
            const int l = LevelOf(t, p);
            char sag[260] = "";
            if (pt.sag >= 0.0f) snprintf(sag, sizeof(sag), ", sag %.3f g", pt.sag);
            if (pt.match) strcat_s(sag, ", the level's full strength");
            if (pt.limit > 0.0f) {
                const size_t sl = strlen(sag);
                snprintf(sag + sl, sizeof(sag) - sl, ", travel limit x%.2f", pt.limit);
            }
            if (pt.gravLevel > 0.0f) {
                const size_t sl = strlen(sag);
                snprintf(sag + sl, sizeof(sag) - sl, ", gravity level x%.2f for every body", pt.gravLevel);
            }
            if (pt.shape >= 0.0f) {
                const size_t sl = strlen(sag);
                snprintf(sag + sl, sizeof(sag) - sl, ", gravity shape x%.2f", pt.shape);
            }
            if (pt.gravPose[0] > 0.0f || pt.gravPose[1] > 0.0f || pt.gravPose[2] > 0.0f || pt.gravPose[3] > 0.0f) {
                static const char* const shape[4] = {"back", "side", "bent forward", "face down"};
                strcat_s(sag, ", gravity level per shape");
                for (int k = 0; k < 4; ++k) {
                    const size_t sl = strlen(sag);
                    if (pt.gravPose[k] > 0.0f) snprintf(sag + sl, sizeof(sag) - sl, " %s x%.2f", shape[k], pt.gravPose[k]);
                    else snprintf(sag + sl, sizeof(sag) - sl, " %s as the level", shape[k]);
                }
            }
            len += snprintf(parts + len, sizeof(parts) - len, " %s %s (swing x%.2f, side x%.2f, fore/aft x%.2f, up x%.2f, settle "
                            "x%.2f, bounce x%.2f%s);", kPartName[p], l == kLevelPreset ? "as installed" : l == kLevelOff ? "off"
                            : kLevelName[l], pt.swing, pt.dir[0], pt.dir[1], pt.dir[2], pt.settle, pt.bounce, sag);
        }
        int bones = 0;
        for (float b : t.bone) bones += b != 1.0f;
        char outfits[400] = "";
        size_t ol = 0;
        for (int o = 0; o < t.nOutfits && ol + 60 < sizeof(outfits); ++o)
            ol += snprintf(outfits + ol, sizeof(outfits) - ol, "%s%s (breasts x%.2f)", ol ? ", " : "", t.outfits[o].name,
                           t.outfits[o].part[kPartBreast]);
        Log("%s: %d files in TFPhysics\\ (%s); [All] %s;%s %d bones changed; %d character sections; outfits %s, %d sections "
            "(%s); planet gravity %s (x%.2f of the difference); the game reads: gravity scale %s, form IDs %s, worn items %s; "
            "read again on a change %s", what, t.files, t.files ? t.fileNames : "none", t.allLevel == kLevelPreset ? "as installed"
            : t.allLevel == kLevelOff ? "off" : kLevelName[t.allLevel], parts, bones, t.nChars, t.outfitsOn ? "on" : "off",
            t.nOutfits, outfits, t.planet ? "on" : "off", t.planetAmount, g_gravityScaleOk ? "checked" : "NOT confirmed",
            g_formIdOk ? "checked" : "NOT confirmed", g_wornOk ? "checked" : "NOT confirmed", g_cfg.tuneReload ? "on" : "off");
    }
}

// The factors of the body whose 3D root this is (read again every 2 s, so slider changes in the looks menu show up)
static bool TuneReadsBodies() {   // the tuning needs to know who a body is, what it wears, or the gravity
    const TuneSettings& T = Tune();
    return T.nChars > 0 || (T.outfitsOn && T.nOutfits > 0) || (T.planet && g_gravityScaleOk);
}
static BodyFactors BodyFactorsOf(void* owner, void* actorHint = nullptr) {
    if ((!g_cfg.sizeScaling && !g_cfg.shape && !TuneReadsBodies()) || !g_actorVt || !owner) return FactorsFrom(SizeRead{});
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t h = (reinterpret_cast<std::uintptr_t>(owner) >> 4) * 0x9E3779B97F4A7C15ull >> 55;
    SizeEntry* slot = nullptr;
    BodyFactors before;
    bool known = false;
    {
        // the body's entry among 4 neighbours (with one place each, two bodies hashed to the same place
        // would read each other out on every update), else a free one or the one read longest ago
        std::lock_guard<std::mutex> lock(g_sizeMutex);
        SizeEntry* oldest = nullptr;
        for (int w = 0; w < 4 && !slot; ++w) {
            SizeEntry* e = &g_sizes[(h + w) & (kSizeSlots - 1)];
            if (e->owner == owner) slot = e;
            else if (!oldest || e->nextMs < oldest->nextMs) oldest = e;
        }
        if (slot && now < slot->nextMs) return slot->f;
        known = slot != nullptr;
        if (known) before = slot->f;
        else slot = oldest;
    }
    const SizeRead r = ReadSize(owner, actorHint);
    const BodyFactors f = FactorsFrom(r);
    {
        std::lock_guard<std::mutex> lock(g_sizeMutex);
        *slot = {owner, f, now + 2000};
    }
    if constexpr (kDiag) {
        if (!known && g_sizeLogs.fetch_add(1) < 12) LogFactors("size: new body", r, f);
        else if (known && !SameFactors(before, f) && g_shapeChangeLogs.fetch_add(1) < 40) LogFactors("size: body changed", r, f);
    }
    return f;
}

// the breast size factor alone (the tests read it)
[[maybe_unused]] static float BodySize(void* owner, void* actorHint = nullptr) { return BodyFactorsOf(owner, actorHint).breastSize; }

// Dead bodies.  Actor::IsDead (Actor and PlayerCharacter vtable slot 269) reads the life state from bits 17-20
// of the dword at +0xF8: 1 dying, 2 dead, 5 recycled are dead, and 7 (an essential actor knocked down) too when asked
// about any death.  The function's own first bytes are checked at load, through the class's vtable (CheckDeadReading).
constexpr std::ptrdiff_t kActorLifeFlags = 0xF8;
constexpr int kIsDeadSlot = 269;
static bool g_deadOk = false;
static int LifeStateOf(void* actor) {   // -1: unknown (not checked, or not a character)
    if (!g_deadOk || !actor || (reinterpret_cast<std::uintptr_t>(actor) & 7)) return -1;
    // the character comes from the game's own call, a live object, so a guarded copy is enough (never VtableAt here:
    // its VirtualQuery on a cache miss is far too slow for every update)
    const std::uintptr_t actors[2] = {g_actorVt, g_playerVt};
    if (!ClassAt(actor, actors, 2)) return -1;
    std::uint32_t flags = 0;
    if (!SafeCopy(&flags, static_cast<char*>(actor) + kActorLifeFlags, 4)) return -1;
    if constexpr (kDiag) g_lifeReads.fetch_add(1, std::memory_order_relaxed);
    return static_cast<int>((flags >> 17) & 0xF);
}
static bool DeadLifeState(int s) { return s == 1 || s == 2 || s == 5 || s == 7; }
static const char* LifeStateName(int s) {
    static const char* const kNames[] = {"alive", "dying", "dead", "unconscious", "reanimated", "recycled", "restrained",
                                         "essential, down", "bleeding out"};
    return s >= 0 && s < 9 ? kNames[s] : "unknown";
}
static void NoteDeadBody(bool abody, int life) {   // the first dead bodies seen, in the log
    static std::atomic<int> deadLogs{0};
    if (kDiag && deadLogs.fetch_add(1) < 12)
        Log("dead bodies: a %s body, life state %d (%s); %s", abody ? "A-Body" : "other", life, LifeStateName(life),
            g_cfg.deadBodies ? "it takes part in collisions" : "left out (bDeadBodies=0)");
}

static void TakeFactors(ActorView& v, const BodyFactors& f) {
    v.size = f.gravity[kPartBreast];
    v.strain = f.strain;
    v.ceilScale = f.ceilScale;
    for (int p = 0; p < kNumParts; ++p) {
        v.jiggle[p] = f.jiggle[p];
        v.gravity[p] = f.gravity[p];
    }
    v.shapeOn = f.shape;
    v.tuned = f.tuned;
}

// a spring as tuned.  Its part's level gives it that preset's springs (the bone's in preset_data.h, at the
// extra-jiggle level its record was built with; Ultra: Max's) and that level's swing (the swing that makes
// the level's strength, x0.25 .. x3 of Realistic, and with bMatchLevel the part's factor to reach it too); Off keeps
// the springs and swings nothing; fBounceSpeed multiplies the strength by its square (the frequency goes with
// sqrt(strength)).  A record no installer setting matches keeps its own springs (a level then changes only the swing)
static void EffectiveOf(const SpringInfo& info, const TuneSettings& T, SpringEff& e) {
    std::memcpy(e.strength, info.origStrength, 12);
    std::memcpy(e.ratio, info.origRatio, 12);
    std::memcpy(e.travel, info.orig, 12);
    e.swing = 1.0f;
    if (info.part < 0) return;
    const PartTune& pt = T.part[info.part];
    const int l = LevelOf(T, info.part);
    if (l == kLevelOff) {
        e.swing = 0.0f;
    } else if (l >= 0 && l < kNumLevels) {
        e.swing = kLevelSwing[l];
        if (info.presetBone >= 0 && info.instExtra >= 0) {
            const PresetSpring& s = kPresetBones[info.presetBone].s[kLevelSprings[l]][info.instExtra];
            std::memcpy(e.strength, s.strength, 12);
            std::memcpy(e.ratio, s.damping, 12);
            std::memcpy(e.travel, s.maxDist, 12);
            if (pt.match) e.swing *= kLevelMatch[l][info.instExtra][info.part];
        }
    }
    if (pt.bounce != 1.0f)
        for (float& x : e.strength) x *= pt.bounce * pt.bounce;
}
static const SpringEff& EffOf(ActorView& v, int i, const TuneSettings& T) {
    if (!v.effDone[i]) {
        const SpringInfo* info = InfoOf(v, i);
        if (info) {
            EffectiveOf(*info, T, v.eff[i]);
        } else {
            v.eff[i] = SpringEff{};
            v.eff[i].swing = 1.0f;
        }
        v.effDone[i] = true;
    }
    return v.eff[i];
}

// --- a part's jiggle on its springs.  Per COM axis (the driver's, where the spring clamps its travel): the
// part's factor, but above 1 never past the release's largest travel (Max physics + Max jiggle; body_shape_data.h).
// One ceiling for the whole part, the smallest of its bones' (that travel / the one in use: as tuned, else as
// installed), so a part's bones keep their proportions: a ceiling per bone let each bone of the thighs
// stop at its own size, and the skin between them stretched up to x1.19 the release's worst (tests\body_shape_stretch.py:
// 15 installer settings x 7 bodies x 5 motions at 60 fps; one ceiling per part: 8 of 525 runs above it, by 0.09 mm at
// most, peak 11.59 mm against the release's 11.57).
// the farthest each part may travel, x that largest travel: the skin's tested limit (tests\stretch_limit_parts.py:
// one part at a time past it, on the v0.7 mesh, walk, run and jump landing at 60 fps, the last step before the skin
// folds over or passes through itself).  [Part] fTravelLimit overrides it.
constexpr float kPartTravelLimit[kNumParts] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
// how far each part's soft bones may settle toward the floor under gravity, x 85% of their Max + Max travel
// (the most a spring's own steady sag reaches on the Max springs): the skin's tested limit (tests\stretch_limit_settle.py:
// one part at a time, on all fours, on the back and on a side, on the v0.7 mesh, the last step before the skin folds
// over or passes through itself).  The breasts have gravity shapes of their own.
constexpr float kPartSettleLimit[kNumParts] = {0.0f, 1.5f, 0.7f, 1.75f, 2.5f, 2.0f, 1.75f, 2.25f};
static void PartCeilings(ActorView& v) {
    const TuneSettings& T = Tune();
    for (auto& c : v.partCeil)
        for (float& x : c) x = 1e30f;
    for (int i = 0; i < v.nSprings; ++i) {
        const SpringInfo* info = InfoOf(v, i);
        if (!info || info->part < 0) continue;
        const SpringEff& e = EffOf(v, i, T);   // against the travel as tuned (the installed one untuned)
        const float lim = T.part[info->part].limit > 0.0f ? T.part[info->part].limit : kPartTravelLimit[info->part];
        for (int k = 0; k < 3; ++k) {
            const float c = e.travel[k] > 1e-6f ? info->maxTravel[k] * lim / e.travel[k] : 1.0f;
            if (c < v.partCeil[info->part][k]) v.partCeil[info->part][k] = c;
        }
    }
    for (auto& c : v.partCeil)
        for (float& x : c)
            if (x > 1e29f) x = 1.0f;
    v.ceilReady = true;
}

// the part's jiggle on a spring: the body's (shape, character, outfit) x the level's swing (and match) x fSwing
static float PartJiggle(const ActorView& v, const SpringInfo& info, const SpringEff& e, const TuneSettings& T) {
    return v.jiggle[info.part] * e.swing * T.part[info.part].swing;
}
// ... per COM axis, x the bone's own factor and the part's per direction; above 1 held to the part's ceiling.  Returns
// whether an axis was held
static bool AxisJiggleOf(const ActorView& v, const SpringInfo& info, const SpringEff& e, const TuneSettings& T, float out[3]) {
    const float j = PartJiggle(v, info, e, T) * (info.presetBone >= 0 ? T.bone[info.presetBone] : 1.0f);
    bool held = false;
    for (int k = 0; k < 3; ++k) {
        const float jk = j * T.part[info.part].dir[k];
        out[k] = jk < v.partCeil[info.part][k] ? jk : v.partCeil[info.part][k];   // (a limit below 1 holds too)
        held |= out[k] < jk;
    }
    return held;
}

// The presets give more travel with less damping (Realistic -> Max: x2.5 vertical travel, x0.6 damping); a body's
// jiggle moves its damping the same way, never below the release's lowest for that bone (or the record's own if that
// is lower), never above critical.  The instance's damping (inst+0xC0 = 2 sqrt(strength) x ratio) is per actor, so this
// recomputes it every frame from the spring as tuned (the level's strength and ratio, x fSettle): untuned, at
// x1, bit for bit the constructor's value.
static void SetDampingOf(void* inst, const SpringEff& e, const SpringInfo& info, float j, float settle) {
    float kd = 1.0f - g_cfg.jiggleDamping * (0.4f / 1.5f) * (j - 1.0f);
    kd = kd < 0.5f ? 0.5f : (kd > 1.5f ? 1.5f : kd);
    kd *= settle;
    float c[3];
    for (int k = 0; k < 3; ++k) {
        if (!(e.strength[k] > 0) || !(e.ratio[k] >= 0)) return;   // a record the game itself would choke on: leave it
        float z = e.ratio[k] * kd;
        z = z < info.dampFloor[k] ? info.dampFloor[k] : z;
        const float top = e.ratio[k] > 1.0f ? e.ratio[k] : 1.0f;
        z = z > top ? top : z;
        c[k] = 2.0f * std::sqrt(e.strength[k]) * z;
    }
    std::memcpy(&At<float>(inst, 0xC0), c, 12);
}

// the chosen breast gravity level: a tuning file's [Breasts] fGravityLevel, else TFPhysics.ini [Gravity]
// fLevel; 0 when neither is set (the installed level, the gravity record's, applies)
static float GravityChosen() {
    const float tuned = Tune().part[kPartBreast].gravLevel;
    return tuned > 0.0f ? tuned : (g_cfg.gravityLevel > 0.0f ? g_cfg.gravityLevel : 0.0f);
}
// ... and the highest level a shape is given: the chosen or the installed level, or a shape's own ([Breasts]
// fGravityBack, fGravitySide, fGravityHang, fGravityDown)
static float GravityTop(const ActorView& v) {
    const float chosen = GravityChosen();
    float top = chosen > 0.0f ? chosen : v.level;
    for (const float p : Tune().part[kPartBreast].gravPose) top = p > top ? p : top;
    return top;
}

// the breast springs' gravity factor.  From Plus gravity up the shapes sit at (or near) their ceilings, and the
// springs' sag adds on top of them: on all fours a heavy body at x1.5 reached 72% where the base body at Max reaches 70%.
// So from x2 up the factor is held to 1 / strain (at most x1.3; the base body x1); at x1 it is free (every body
// measured stayed under 52% there), and in between it blends.
static float BreastSag(const ActorView& v) {
    if (!g_cfg.stretchGuard) return v.size;
    float cap = 1.0f / v.strain;
    cap = cap > 1.3f ? 1.3f : (cap < 0.15f ? 0.15f : cap);
    const float free = g_cfg.shapeMax > cap ? g_cfg.shapeMax : cap;
    const float lvl = GravityTop(v);   // a chosen level counts as installed, and so does a shape's own
    const float t = lvl <= 1.0f ? 0.0f : (lvl >= 2.0f ? 1.0f : lvl - 1.0f);
    const float most = free + t * (cap - free);
    return v.size < most ? v.size : most;
}

static void PreUpdate(ActorView& v) {
    const float dt = *reinterpret_cast<const float*>(g_addr[kFrameTime]);
    // a zero frame time is the game paused (photo mode): its springs hold their last bounce and nothing is
    // stepped or written here; with [Springs] bSettleWhenPaused each spring's rest point for the pose is worked out
    // instead (the larger of its own steady sag and its part's gravity shape) and put on in PostUpdate (PausedSettle)
    const bool paused = dt == 0.0f && g_cfg.settlePaused;
    if (!(dt > 0.0f && dt < 0.25f) && !paused) {
        if constexpr (kDiag) {
            if (dt != 0.0f) {
                g_skippedFrames.fetch_add(1, std::memory_order_relaxed);
                std::uint32_t bits;
                std::memcpy(&bits, &dt, 4);
                g_skippedDtBits.store(bits, std::memory_order_relaxed);
            }
        }
        return;
    }
    if (!v.com) return;
    v.paused = paused;
    const TuneSettings& T = Tune();
    const bool tuning = T.springs || T.scaled || T.damping;
    // gravity relative to standing, per 1 g, in COM's frame (its world rows are its axes): g_now - g_standing
    const Vec3 rel = {-Row(v.com, 0x80, 0).z, -Row(v.com, 0x80, 1).z, 1.0f - Row(v.com, 0x80, 2).z};
    // (x each part's gravity factor; the breasts' is in v.size.  per part, the part's fSag where its
    // file gives one; the planet's gravity is in the factors)
    float sagG[kNumParts] = {g_cfg.springGravity, g_cfg.springGravityButt, g_cfg.springGravityBelly, g_cfg.springGravityThighs,
                             g_cfg.springGravityHips, g_cfg.springGravityCalves, g_cfg.springGravityArms, g_cfg.springGravityTorso};
    static_assert(kPartBreast == 0 && kPartButt == 1 && kPartBelly == 2 && kPartThigh == 3 && kPartHip == 4 &&
                  kPartCalf == 5 && kPartArm == 6 && kPartTorso == 7, "sagG lists the parts in order");
    float pull[kNumParts];
    for (int p = 0; p < kNumParts; ++p) {
        if (T.part[p].sag >= 0.0f) sagG[p] = T.part[p].sag;
        pull[p] = 9.81f * sagG[p] * (p == kPartBreast ? BreastSag(v) : v.gravity[p]);
    }
    // each part's gravity shape level (every part but the breasts): its file's fGravityShape, else
    // TFPhysics.ini's, x the body's gravity factor up to 1 (a thinner body, or a lighter planet, settles less)
    float shapeLevel[kNumParts] = {};
    v.settleOn = false;
    for (int p = 1; p < kNumParts && g_cfg.enabled; ++p) {
        const float lv = T.part[p].shape >= 0.0f ? T.part[p].shape : g_cfg.gravityShape;
        const float f = v.gravity[p] < 1.0f ? (v.gravity[p] > 0.0f ? v.gravity[p] : 0.0f) : 1.0f;
        shapeLevel[p] = lv * f;
        v.settleOn |= shapeLevel[p] > 0.0f;
    }
    for (int i = 0; i < v.nSprings; ++i) {
        v.settle[i][0] = v.settle[i][1] = v.settle[i][2] = 0.0f;
        void* inst = v.springs[i];
        void* data = At<void*>(inst, 0x20);
        const SpringInfo* info = InfoOf(v, i);
        if (!info) continue;
        const SpringEff& e = EffOf(v, i, T);
        const int part = info->part;
        // the body's jiggle on this spring, per COM axis (its swing is scaled after the update, in
        // ScaleJiggle), and its damping (per instance; recomputed every frame, so x1 restores it)
        float j[3] = {1.0f, 1.0f, 1.0f};
        // a record once tuned (its springs or its damping) stays in this path, so taking the tuning out
        // puts everything back: untuned, the values below are bit for bit the installed ones
        const bool touched = part >= 0 && info->touched.load(std::memory_order_relaxed);
        if (paused) {   // the rest point, per COM axis (springs, records and damping as they are)
            if (part < 0) continue;
            if (v.shapeOn || v.tuned || tuning || touched) {
                if (!v.ceilReady) PartCeilings(v);
                AxisJiggleOf(v, *info, e, T, j);
            }
            const float G = pull[part], shape = part > 0 ? shapeLevel[part] : 0.0f;
            const float r3[3] = {rel.x, rel.y, rel.z};
            for (int k = 0; k < 3; ++k) {
                float seen = 0.0f;   // the spring's own steady sag, held at 85% of its 60 fps travel, as its swing shows it
                if (G > 0.0f && j[k] >= 0.05f && e.strength[k] > 0.0f) {
                    const float most = 0.85f * e.travel[k] / 60.0f;
                    const float off = std::fabs(r3[k]) * G / (j[k] * e.strength[k]);
                    seen = (off < most ? off : most) * j[k];
                }
                float want = 0.0f;   // the part's gravity shape
                if (shape > 0.0f) {
                    const float r = r3[k] > 1.0f ? 1.0f : (r3[k] < -1.0f ? -1.0f : r3[k]);
                    want = std::fabs(shape * kPartSettleLimit[part] * 0.85f * info->maxTravel[k] / 60.0f * r);
                }
                const float at = want > seen ? want : seen;
                v.settle[i][k] = r3[k] < 0.0f ? -at : at;
            }
            continue;
        }
        if (part >= 0 && (T.springs || T.damping) && !touched) info->touched.store(true, std::memory_order_relaxed);
        if (part >= 0 && (v.shapeOn || v.tuned || tuning || touched)) {
            if (!v.ceilReady) PartCeilings(v);
            AxisJiggleOf(v, *info, e, T, j);
            // the level's swing is not in the damping's jiggle: a level is its preset's springs as they are,
            // with the swing on top (so a level's strength is exact); the body's factors and fSwing move it as before
            SetDampingOf(inst, e, *info, v.jiggle[part] * T.part[part].swing, T.part[part].settle);
        }
        // the tuned springs go into the shared record (every body writes the same values, so their order
        // doesn't matter: the game reads strength and MaxDist from it on every update), and the instance's sub-step
        // rate follows the strength the way the constructor works it out (0.63662 x the root of the largest)
        const bool records = part >= 0 && (T.springs || touched);
        if (records) {
            float* s = &At<float>(data, 0x20);
            if (std::memcmp(s, e.strength, 12)) std::memcpy(s, e.strength, 12);
            const float top = std::fmax(e.strength[0], std::fmax(e.strength[1], e.strength[2]));
            At<float>(inst, 0xD0) = 0.63662f * std::sqrt(top);
        }
        // the game clamps each axis to MaxDist x (dt / sub-steps); keep that at its 60 fps size at any frame rate
        if (g_cfg.fixedFrameRate) {
            const float rate = At<float>(inst, 0xD0);
            int n = static_cast<int>(std::ceil(dt * rate));
            n = n < 1 ? 1 : n;
            const float scale = n / (60.0f * dt);
            float* caps = &At<float>(data, 0x50);
            for (int k = 0; k < 3; ++k) caps[k] = e.travel[k] * scale;
            if constexpr (kDiag) {
                g_springsScaled.fetch_add(1, std::memory_order_relaxed);
                std::uint32_t bits;
                std::memcpy(&bits, &scale, 4);
                g_lastScaleBits.store(bits, std::memory_order_relaxed);
                std::memcpy(&bits, &dt, 4);
                g_lastDtBits.store(bits, std::memory_order_relaxed);
            }
        } else if (records) {   // the tuned travel as it is
            std::memcpy(&At<float>(data, 0x50), e.travel, 12);
        }
        // gravity: the spring's offset x = goal - simulated, so a pull a on the bone is -a on x'.  The pull never
        // holds a spring against its travel limit: its steady sag (pull / strength) stays within 85% of the 60 fps
        // limit (MaxDist / 60), so the spring still bounces both ways (a bigger size factor pinned the breasts)
        const float G = part >= 0 ? pull[part] : 0.0f;
        const float shape = part > 0 ? shapeLevel[part] : 0.0f;
        if (G > 0.0f || shape > 0.0f) {
            const float* strength = &At<float>(data, 0x20);
            float* vel = &At<float>(inst, 0xB0);
            const float r3[3] = {rel.x, rel.y, rel.z};
            // the travel limit now: the 60 fps size if the plugin holds it there, else the game's own (MaxDist x dt / n)
            const float rate = At<float>(inst, 0xD0);
            const int n0 = static_cast<int>(std::ceil(dt * rate)), n = n0 < 1 ? 1 : n0;
            const float perLimit = g_cfg.fixedFrameRate ? 1.0f / 60.0f : dt / static_cast<float>(n);
            for (int k = 0; k < 3; ++k) {
                float seen = 0.0f;   // the spring's own steady sag on this axis, as its swing shows it
                if (G > 0.0f && j[k] >= 0.05f) {   // a spring swinging (almost) nothing gets no sag either
                    const float most = 0.85f * e.travel[k] * perLimit * strength[k];
                    // / j: the swing (sag included) comes out x j, so the sag seen is G's (muscular: less swing, a little
                    // more sag); the guard still holds the spring's own sag inside its limit
                    float a = r3[k] * G / j[k];
                    a = a > most ? most : (a < -most ? -most : a);
                    vel[k] -= a * dt;
                    if (strength[k] > 0.0f) seen = std::fabs(a) / strength[k] * j[k];
                }
                // the part's gravity shape: from the springs' own sag the rest of the way to the shape level
                // x kPartSettleLimit x 85% of the bone's Max + Max travel (its 60 fps size), toward the change of
                // gravity on this axis; put on after the game's update (AddSettle)
                if (shape > 0.0f) {
                    const float r = r3[k] > 1.0f ? 1.0f : (r3[k] < -1.0f ? -1.0f : r3[k]);
                    const float want = std::fabs(shape * kPartSettleLimit[part] * 0.85f * info->maxTravel[k] / 60.0f * r);
                    const float more = want - seen;
                    if (more > 0.0f) v.settle[i][k] = r3[k] < 0.0f ? -more : more;
                }
            }
        }
    }
}

// each part's gravity shape (worked out per spring in PreUpdate, in COM's axes) onto its bone, in the bone's
// parent frame like every offset added on top; one per bone (two springs on one bone: the first one's).  A bone of a
// part with its shape on gets its offset every frame, a zero one too, so last frame's comes off when the body stands
static void AddSettle(ActorView& v) {
    void* done[128];
    int nDone = 0;
    const TuneSettings& T = Tune();
    for (int i = 0; i < v.nSprings; ++i) {
        const SpringInfo* info = InfoOf(v, i);
        if (!info || info->part <= 0) continue;
        const int part = info->part;
        if (!((T.part[part].shape >= 0.0f ? T.part[part].shape : g_cfg.gravityShape) > 0.0f)) continue;
        void* inst = v.springs[i];
        void* target = At<void*>(inst, 0x18);
        void* parent = target ? At<void*>(target, 0x38) : nullptr;
        void* driver = At<void*>(inst, 0x10);
        if (!parent || !driver) continue;
        bool twice = false;
        for (int q = 0; q < nDone && !twice; ++q) twice = done[q] == target;
        if (twice) continue;
        done[nDone++] = target;
        const float* s = v.settle[i];
        Vec3 w = {0, 0, 0};   // rows of a world rotation are the node's axes in the world
        for (int k = 0; k < 3; ++k) w = Add(w, Scale(Row(driver, 0x80, k), s[k]));
        const float add[3] = {Dot(w, Row(parent, 0x80, 0)), Dot(w, Row(parent, 0x80, 1)), Dot(w, Row(parent, 0x80, 2))};
        v.AddTo(target, add);
    }
}

// --- after the game's update: the static gravity on the anchors and the slump on the breast bones
static void Collide(ActorView& v);

// The thighs' angle from vertical (degrees), from the thigh springs' parents (L/R_Thigh_Twist*, whose X axis runs
// along the thigh); -1 when the body has no thigh springs.  Lying face down and on all fours both have the hips flat;
// only lying has the thighs flat too.
static float ThighTiltDeg(ActorView& v) {
    float sum = 0;
    int n = 0;
    for (int i = 0; i < v.nSprings; ++i) {
        void* inst = v.springs[i];
        void* target = At<void*>(inst, 0x18);
        const SpringInfo* info = InfoOf(v, i);
        if (!info || info->group != kGroupThighs) continue;
        void* parent = At<void*>(target, 0x38);
        if (!parent) continue;
        sum += std::fabs(Row(parent, 0x80, 0).z);
        ++n;
    }
    if (!n) return -1.0f;
    const float c = sum / n;
    return std::acos(c > 1 ? 1 : c) * 57.2957795f;
}

// the body's jiggle on its springs' swing.  A spring writes its bone's local translation = its rest (inst+0x60,
// its goal in the parent's frame) minus its offset turned into the parent's frame, the offset clamped per COM axis
// (0x877910); so its swing is (what it wrote - rest), and x j per COM axis is that swing turned into COM's axes,
// scaled, and turned back (one factor on all three axes: just the swing x j).  The springs' own state is never touched,
// so nothing added here feeds back into them.  Runs before the gravity shapes and collisions, so collisions see the
// scaled swing.  A neutral body (every part x1) skips this: whatever a bone kept from before is a swing of a spring at
// rest (it stops writing only below 0.5 mm), x the old factor, and goes with its next write.
// a paused frame (photo mode): every soft bone at its rest point for the pose (PreUpdate's settle[], in COM's
// axes) in place of the bounce its spring holds.  The bone's offset on top is that point (in the bone's parent frame)
// minus the spring's own offset, read the way ScaleJiggle reads it; a bone further out than its travel allows is left
// as it is, and every bone gets its offset, a zero one too, so last frame's comes off
static void PausedSettle(ActorView& v) {
    struct Item { void* target; void* parent; void* driver; const float* rest; const float* at; float local[3]; float most; };
    Item items[128];
    int n = 0;
    void* taken[128];
    int nTaken = 0;
    const TuneSettings& T = Tune();
    for (int i = 0; i < v.nSprings && n < 128; ++i) {
        void* inst = v.springs[i];
        void* target = At<void*>(inst, 0x18);
        const SpringInfo* info = InfoOf(v, i);
        if (!info || info->part < 0 || !target) continue;
        bool twice = false;   // two springs on one bone: the first one's
        for (int q = 0; q < nTaken && !twice; ++q) twice = taken[q] == target;
        if (twice) continue;
        taken[nTaken++] = target;
        Item& it = items[n];
        it.target = target;
        it.parent = At<void*>(target, 0x38);
        it.driver = At<void*>(inst, 0x10);
        if (!it.parent || !it.driver) continue;
        it.rest = &At<float>(inst, 0x60);
        it.at = v.settle[i];
        std::memcpy(it.local, static_cast<char*>(target) + 0x70, 12);
        const float* o = EffOf(v, i, T).travel;
        it.most = 1.5f * std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) / 60.0f + 0.001f;
        ++n;
    }
    {   // the spring's own offset: without ours from last frame where the node still holds it (one lock for all)
        BoneLock lock;
        for (int q = 0; q < n; ++q)
            if (const BoneState* st = FindState(items[q].target, false))
                if (std::memcmp(st->written, items[q].local, 12) == 0)
                    for (int k = 0; k < 3; ++k) items[q].local[k] = st->written[k] - st->added[k];
    }
    std::uint32_t maxMm = 0;
    const bool fresh = v.nAdd == 0;
    for (int q = 0; q < n; ++q) {
        const Item& it = items[q];
        float d[3], add[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) d[k] = it.local[k] - it.rest[k];
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= it.most * it.most) {
            Vec3 w = {0, 0, 0};   // rows of a world rotation are the node's axes in the world
            for (int k = 0; k < 3; ++k) w = Add(w, Scale(Row(it.driver, 0x80, k), it.at[k]));
            for (int r = 0; r < 3; ++r) add[r] = Dot(w, Row(it.parent, 0x80, r)) - d[r];
        }
        const float len = std::sqrt(add[0] * add[0] + add[1] * add[1] + add[2] * add[2]);
        if (len < 2e-5f) add[0] = add[1] = add[2] = 0;
        if (fresh && v.nAdd < ActorView::kMaxAdds) {
            v.addNode[v.nAdd] = it.target;
            std::memcpy(v.add[v.nAdd], add, 12);
            ++v.nAdd;
        } else {
            v.AddTo(it.target, add);
        }
        const std::uint32_t mm = static_cast<std::uint32_t>(len * 1000);
        maxMm = mm > maxMm ? mm : maxMm;
    }
    if constexpr (kDiag) {
        g_pausedBodies.fetch_add(1, std::memory_order_relaxed);
        std::uint32_t prev = g_pausedMaxMm.load(std::memory_order_relaxed);
        while (maxMm > prev && !g_pausedMaxMm.compare_exchange_weak(prev, maxMm)) {}
    } else {
        (void)maxMm;
    }
}

static void ScaleJiggle(ActorView& v) {
    struct Item { void* target; void* inst; void* parent; float j[3]; float local[3]; float most; bool held; };
    Item items[128];
    int n = 0;
    if (!v.ceilReady) PartCeilings(v);
    // a spring clamps its offset per COM axis to its travel this frame (MaxDist x dt / sub-steps; held at 60 fps size
    // when bFixedFrameRate), so its swing is never longer than that vector: anything longer is not its swing
    const float dt = *reinterpret_cast<const float*>(g_addr[kFrameTime]);
    const float per = g_cfg.fixedFrameRate ? 1.0f / 60.0f : (dt > 1.0f / 60.0f && dt < 0.25f ? dt : 1.0f / 60.0f);
    void* seen[256] = {};   // the bones taken (open addressing; a body has at most 128 springs)
    auto firstTime = [&seen](void* t) {
        const std::uint64_t h = (reinterpret_cast<std::uintptr_t>(t) >> 4) * 0x9E3779B97F4A7C15ull >> 56;
        for (int probe = 0; probe < 256; ++probe) {
            void*& slot = seen[(h + probe) & 255];
            if (slot == t) return false;
            if (!slot) {
                slot = t;
                return true;
            }
        }
        return false;
    };
    const TuneSettings& T = Tune();
    for (int i = 0; i < v.nSprings; ++i) {
        void* inst = v.springs[i];
        void* target = At<void*>(inst, 0x18);
        const SpringInfo* info = InfoOf(v, i);
        if (!info || info->part < 0) continue;
        // two lists springing one bone (each writes it, the last one's stands): scale it once
        if (!firstTime(target)) continue;
        Item& it = items[n];
        it.target = target;
        it.inst = inst;
        it.parent = At<void*>(target, 0x38);   // no parent: the spring doesn't run (0x877910 returns at once)
        if (!it.parent) continue;
        std::memcpy(it.local, static_cast<char*>(target) + 0x70, 12);   // the bone its spring just wrote (live)
        const SpringEff& e = EffOf(v, i, T);   // the spring as tuned, its travel and swing
        it.held = AxisJiggleOf(v, *info, e, T, it.j);
        const float* o = e.travel;
        it.most = 1.5f * std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) * per + 0.001f;
        ++n;
    }
    {   // what each spring wrote: without the plugin's offset from last frame where the node still holds it (one lock for all)
        BoneLock lock;
        for (int q = 0; q < n; ++q)
            if (const BoneState* st = FindState(items[q].target, false))
                if (std::memcmp(st->written, items[q].local, 12) == 0)
                    for (int k = 0; k < 3; ++k) items[q].local[k] = st->written[k] - st->added[k];
    }
    std::uint32_t maxMm = 0;
    std::uint64_t scaled = 0, held = 0;
    const bool fresh = v.nAdd == 0;   // the first to hand on adds this frame: its bones (each once) go straight in
    for (int q = 0; q < n; ++q) {
        const Item& it = items[q];
        const float* rest = &At<float>(it.inst, 0x60);
        float d[3], add[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) d[k] = it.local[k] - rest[k];
        // longer than its travel allows (x1.5, + 1 mm): not this spring's swing (something else put the bone there,
        // or the plugin's offset was lost with its record): leave the bone as it is
        const bool sane = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= it.most * it.most;
        const bool one = it.j[0] == it.j[1] && it.j[1] == it.j[2];
        if (sane && one) {
            for (int k = 0; k < 3; ++k) add[k] = (it.j[0] - 1.0f) * d[k];
        } else if (sane) {   // rows of a world rotation are the node's axes in the world
            void* driver = At<void*>(it.inst, 0x10);
            Vec3 w = {0, 0, 0}, w2 = {0, 0, 0};
            for (int r = 0; r < 3; ++r) w = Add(w, Scale(Row(it.parent, 0x80, r), d[r]));
            for (int k = 0; k < 3; ++k) {
                const Vec3 axis = Row(driver, 0x80, k);
                w2 = Add(w2, Scale(axis, (it.j[k] - 1.0f) * Dot(w, axis)));
            }
            for (int r = 0; r < 3; ++r) add[r] = Dot(w2, Row(it.parent, 0x80, r));
        }
        const float len = std::sqrt(add[0] * add[0] + add[1] * add[1] + add[2] * add[2]);
        if (len < 2e-5f) add[0] = add[1] = add[2] = 0;   // under 0.02 mm: nothing (and nothing to write)
        if (fresh && v.nAdd < ActorView::kMaxAdds) {   // also when zero: last frame's offset comes off
            v.addNode[v.nAdd] = it.target;
            std::memcpy(v.add[v.nAdd], add, 12);
            ++v.nAdd;
        } else {
            v.AddTo(it.target, add);
        }
        if constexpr (kDiag) {
            scaled += it.j[0] != 1.0f || it.j[1] != 1.0f || it.j[2] != 1.0f;
            held += it.held;
            const std::uint32_t mm = static_cast<std::uint32_t>(len * 1000);
            maxMm = mm > maxMm ? mm : maxMm;
        }
    }
    if constexpr (kDiag) {
        g_shapeBodies.fetch_add(1, std::memory_order_relaxed);
        g_shapeScaled.fetch_add(scaled, std::memory_order_relaxed);
        g_shapeHeld.fetch_add(held, std::memory_order_relaxed);
        std::uint32_t prev = g_shapeMaxMm.load(std::memory_order_relaxed);
        while (maxMm > prev && !g_shapeMaxMm.compare_exchange_weak(prev, maxMm)) {}
    }
}

static bool Neutral(const ActorView& v) {   // and nothing tuned on every body's swing
    for (int p = 0; p < kNumParts; ++p)
        if (v.jiggle[p] != 1.0f) return false;
    return !Tune().scaled;
}

static void PostUpdate(ActorView& v, int site) {
    // first: the shapes and collisions below build on the scaled swing (a tuned body's too); a paused frame
    // puts every soft bone at its rest point instead
    if (v.paused) PausedSettle(v);
    else if ((v.shapeOn || v.tuned || Tune().scaled) && !Neutral(v)) ScaleJiggle(v);
    Anchor* anchors = v.anchors;
    const float level = v.level;
    if (kDiag && g_cfg.enabled) g_gravityActors.fetch_add(1, std::memory_order_relaxed);
    const float thighTilt = g_cfg.enabled ? ThighTiltDeg(v) : -1.0f;
    const float thighFlat = thighTilt < 0 ? 1.0f   // no thigh springs: the hips alone decide
        : Clamp01((thighTilt - g_cfg.thighFlatStartDeg) / (g_cfg.thighFlatFullDeg - g_cfg.thighFlatStartDeg));
    // [Gravity] bEnabled=0 leaves the anchors to the game's own correctives and adds no shapes; springs and collisions go on
    for (int side = 0; side < 2 && g_cfg.enabled; ++side) {
        const Anchor& a = anchors[side];
        if (!a.node || !a.com) continue;
        void* chest = At<void*>(a.node, 0x38);
        if (!chest) continue;
        // world "down" (-Z) seen in the chest's own axes (rows of its world rotation are its axes in the world)
        Vec3 gc = {-Row(chest, 0x80, 0).z, -Row(chest, 0x80, 1).z, -Row(chest, 0x80, 2).z};
        const float n = std::sqrt(Dot(gc, gc));
        if (n < 1e-6f) continue;
        gc = {gc.x / n, gc.y / n, gc.z / n};
        const float span = 90.0f - g_cfg.deadZoneDeg;
        auto cone = [&](const Vec3& dir) { return Clamp01((span - AngleDeg(gc, dir)) / span); };
        const float wBack = cone({0, -1, 0}), wFwd = cone({0, 1, 0}), wLeft = cone({0, 0, 1}), wRight = cone({0, 0, -1});
        // how flat the hips (COM) lie: its up axis against the world's up
        const float hipTilt = std::acos(std::fmax(-1.f, std::fmin(1.f, Row(a.com, 0x80, 2).z))) * 57.2957795f;
        const float lying = Clamp01((hipTilt - g_cfg.lyingStartDeg) / (g_cfg.lyingFullDeg - g_cfg.lyingStartDeg)) * thighFlat;
        // the installed level x the breast size, held per shape to its largest level ([Gravity] fMaxBack ..
        // fMaxDown; the old ones kept the approved back slump's stretch at x2, 62%, on the base body:
        // tests\size_stretch.py).  a chosen level ([Gravity] fLevel, a tuning file's [Breasts] fGravityLevel,
        // or per shape its fGravityBack, fGravitySide, fGravityHang, fGravityDown) is what a normal or bigger breast
        // gets, and its own largest level: smaller or thinner breasts get less (x the size or the ceiling scale, the
        // lower, when it is below 1)
        const float chosen = GravityChosen();
        const float base = chosen > 0.0f ? chosen : level;
        const float* pose = Tune().part[kPartBreast].gravPose;
        const float share = v.size < v.ceilScale ? v.size : v.ceilScale;   // (the ceiling scale is at most 1)
        auto at = [&](int shape, float most) {   // (x the body's ceiling scale, 1 on the base body)
            const float own = pose[shape] > 0.0f ? pose[shape] : chosen;
            if (own > 0.0f) return own * share;
            const float lv = level * v.size;
            const float m = (most < g_cfg.maxLevel ? most : g_cfg.maxLevel) * v.ceilScale;
            return lv > m ? m : lv;
        };
        const float lvBack = at(0, g_cfg.maxBack), lvSide = at(1, g_cfg.maxSide), lvHang = at(2, g_cfg.maxHang),
                    lvDown = at(3, g_cfg.maxDown);
        // per-bone shapes need the four breast bones; without them only the whole breast moves
        const bool haveBones = a.bones[0] && a.bones[1] && a.bones[2] && a.bones[3];
        const float s = haveBones ? g_cfg.slump : 0.0f;              // on the back
        const float ss = haveBones ? g_cfg.sideSlump : 0.0f;         // on a side
        const float fs = haveBones ? g_cfg.faceDownSquash : 0.0f;    // face down
        const float hs = haveBones ? g_cfg.hangShape : 0.0f;         // bent forward
        const float b = s < 1 ? s : 1, bs = ss < 1 ? ss : 1, bh = hs < 1 ? hs : 1;
        // hang when bending (its whole-breast part halves as the teardrop takes over), press when prone
        const float fwd = (1 - lying) * g_cfg.hangCm * (1 - 0.5f * bh) * lvHang - lying * g_cfg.pressCm * lvDown;
        // lying back: blend the anchor from slide-only to the slump's anchor part, and add the bones' part
        const Vec3 faceUp = {kFaceUpSlide[side].x + b * (kFaceUpSlump[side].x - kFaceUpSlide[side].x),
                             kFaceUpSlide[side].y + b * (kFaceUpSlump[side].y - kFaceUpSlide[side].y),
                             kFaceUpSlide[side].z + b * (kFaceUpSlump[side].z - kFaceUpSlide[side].z)};
        const float sideK = 1 - 0.5f * bs;   // the side slide halves as the side shape takes over
        Vec3 off = {
            (wBack * faceUp.x * lvBack + (wLeft * kLeftSide[side].x + wRight * kRightSide[side].x) * sideK * lvSide) / 100,
            (wBack * faceUp.y * lvBack + (wLeft * kLeftSide[side].y + wRight * kRightSide[side].y) * sideK * lvSide +
             wFwd * fwd) / 100,
            (wBack * faceUp.z * lvBack + (wLeft * kLeftSide[side].z + wRight * kRightSide[side].z) * sideK * lvSide) / 100};
        const float t[3] = {a.rest[0] + off.x, a.rest[1] + off.y, a.rest[2] + off.z};
        WriteTranslation(a.node, t);
        std::memcpy(v.anchorLocal[side], t, 12);
        v.anchorWritten[side] = true;
        if (haveBones) {
            const float sgn = side == 0 ? 1.0f : -1.0f;   // tables are for the breast's own outside: mirror the right
            // on the left side the left breast is below and the right one above; on the right side the reverse
            const Vec3* onLeft = side == 0 ? kSideDown : kSideUp;
            const Vec3* onRight = side == 1 ? kSideDown : kSideUp;
            const float kBack = wBack * lvBack * s / 100, kLeft = wLeft * lvSide * ss / 100, kRight = wRight * lvSide * ss / 100;
            const float kSquash = wFwd * lying * lvDown * fs / 100, kHang = wFwd * (1 - lying) * lvHang * hs / 100;
            for (int bone = 0; bone < 4; ++bone) {
                auto mix = [&](float Vec3::*c) {
                    return kSlumpBone[bone].*c * kBack + onLeft[bone].*c * kLeft + onRight[bone].*c * kRight +
                           kSquashBone[bone].*c * kSquash + kHangBone[bone].*c * kHang;
                };
                const float add[3] = {mix(&Vec3::x), mix(&Vec3::y), mix(&Vec3::z) * sgn};
                v.AddTo(a.bones[bone], add);
            }
        }

        if (kDiag && side == 0) {
            const std::uint64_t now = GetTickCount64();
            std::uint64_t next = g_nextSampleMs.load(std::memory_order_relaxed);
            if (now >= next && g_nextSampleMs.compare_exchange_strong(next, now + 1000)) {
                std::lock_guard<std::mutex> lock(g_sampleMutex);
                g_sample = {site, {gc.x, gc.y, gc.z}, hipTilt, {wBack, wFwd, wLeft, wRight}, lying, base, {off.x, off.y, off.z},
                            v.size, thighTilt, v.jiggle[kPartBreast]};
                g_haveSample = true;
            }
        }
    }
    if (v.settleOn && !v.paused) AddSettle(v);   // the other parts' gravity shapes, on top like the breasts'
    if (kCollisions && g_cfg.collision) {
        v.site = site;
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        Collide(v);
        QueryPerformanceCounter(&t1);
        g_collideTicks.fetch_add(static_cast<std::uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
        g_collideCalls.fetch_add(1, std::memory_order_relaxed);
    }
    AddOnTopAll(v);   // one write per bone, on top of its spring
}

// ------------------------------------------------------------------ collisions
// Colliders are capsules on the upper arms, forearms, hands and thighs; soft bones are spheres (breasts, belly, inner
// thighs, and the glutes, which only the hands and forearms reach).  A soft bone is pushed straight out of every collider it overlaps, but only when closer than in the rest
// pose, so nothing moves standing still; soft against soft (the other breast) takes half each.  Sizes and rest
// distances: collision_data.h, measured with A-Body's p3_collision.py.  Every bone involved sits under
// COM, so they are found by name walking down from COM, which is live this frame (it drives the springs).
// A node's child list: the game's own layout (kGameChildLayout, below) is tried first.  Other layouts are learned from
// a breast anchor whose four TF_Breast bones are known (spring targets with the anchor as parent).  Two shapes are
// tried at every offset of the node:
//  * an array: kept inside the node (small lists) or behind a pointer (both read from the same offset);
//  * a linked list: the node points at its first child, and each child at its next sibling.
// No count is read: entries are taken while they name the node as their parent, so the count's place doesn't matter.
// A shape passes only if all four bones come out; if none does, the log gets a map of where the bones appear.
enum ChildMode { kArray = 1, kSiblingList = 2 };
// next: the sibling link's offset in a child, or the array's entry size; countOff: the 16-bit element count's
// offset in the node, -1 = unknown (the list is read until its entries stop naming the node as parent)
struct ChildLayout { int mode, off, next; int countOff = -1; };
// the game's own layout, read from NiNode::UpdateWorldBound (ID 147191): movzx r9d,word [rcx+0x142] (the
// count); mov r8,[rcx+0x138] (the array); mov rcx,[r8+rdx*8] for edx below the count, empty entries skipped
constexpr ChildLayout kGameChildLayout{kArray, 0x138, 8, 0x142};
static ChildLayout g_childLayout{};
static std::atomic<int> g_layoutState{0};   // 0 not found yet, 1 being tried, 2 found, -1 given up
static std::atomic<int> g_layoutTries{0};
static std::atomic<std::uint64_t> g_layoutRetryMs{0};

static bool IsChildOf(void* p, void* node) {
    void* parent = nullptr;
    return p && (reinterpret_cast<std::uintptr_t>(p) & 7) == 0 && SafeProbe(&parent, static_cast<char*>(p) + 0x38, 8) &&
           parent == node;
}

// skipped (optional): entries in an array that don't name this node as their parent are passed over and handed back
// here instead of ending the list (the list ends after 4 such entries or gaps in a row).
static int Children(void* node, const ChildLayout& L, void** out, int cap, void** skipped = nullptr,
                    int* nSkipped = nullptr, int skipCap = 0) {
    int n = 0;
    if (nSkipped) *nSkipped = 0;
    void* first = nullptr;
    if (!SafeCopy(&first, static_cast<char*>(node) + L.off, 8) || !first) return 0;
    if (L.mode == kSiblingList) {
        for (void* c = first; c && n < cap && IsChildOf(c, node);) {
            for (int i = 0; i < n; ++i)
                if (out[i] == c) return n;   // a loop
            out[n++] = c;
            if (!SafeCopy(&c, static_cast<char*>(c) + L.next, 8)) break;
        }
        return n;
    }
    if (L.countOff >= 0) {
        // exactly `count` entries, read with guarded copies (the game reads the same ones): no Windows check.
        // A node that isn't an NiNode (geometry) holds other data here: its "count" is out of range or its "array"
        // pointer isn't one, and it is passed over without a read
        std::uint16_t count = 0;
        const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(first);
        if (!SafeCopy(&count, static_cast<char*>(node) + L.countOff, 2) || !count || count > 4096 || (at & 7) ||
            at < 0x10000 || at > 0x00007FFFFFFF0000ull)
            return 0;
        void* chunk[64];
        for (int base = 0; base < count && n < cap; base += 64) {
            const int m = count - base < 64 ? count - base : 64;
            if (!SafeCopy(chunk, static_cast<const char*>(first) + 8 * static_cast<size_t>(base), sizeof(void*) * static_cast<size_t>(m)))
                break;
            for (int i = 0; i < m && n < cap; ++i) {
                void* c = chunk[i];
                void* parent = nullptr;
                if (!c || (reinterpret_cast<std::uintptr_t>(c) & 7) || !SafeCopy(&parent, static_cast<char*>(c) + 0x38, 8))
                    continue;
                if (parent == node) out[n++] = c;
                else if (skipped && nSkipped && *nSkipped < skipCap) skipped[(*nSkipped)++] = c;   // listed, not theirs
            }
        }
        return n;
    }
    // array: entries inside the node if the first one is a child, else the offset holds a pointer to them
    const char* arr = IsChildOf(first, node) ? static_cast<char*>(node) + L.off : static_cast<const char*>(first);
    for (int i = 0, bad = 0; i < 1024 && n < cap; ++i) {
        void* c = nullptr;
        if (!SafeProbe(&c, arr + L.next * i, 8)) break;
        if (!c) {
            if (++bad > 4) break;
            continue;
        }
        if (!IsChildOf(c, node)) {
            if (!skipped || !nSkipped || ++bad > 4) break;
            if (*nSkipped < skipCap && (reinterpret_cast<std::uintptr_t>(c) & 7) == 0) skipped[(*nSkipped)++] = c;
            continue;
        }
        bad = 0;
        out[n++] = c;
    }
    if (nSkipped && *nSkipped > 0) {   // only entries before the last child count; later ones are past the list's end
        void* last = n ? out[n - 1] : nullptr;
        int keep = 0;
        for (int i = 0; i < 1024 && last; ++i) {
            void* c = nullptr;
            if (!SafeProbe(&c, arr + L.next * i, 8) || c == last) break;
            for (int k = keep; k < *nSkipped; ++k)
                if (skipped[k] == c) std::swap(skipped[keep++], skipped[k]);
        }
        *nSkipped = keep;
    }
    return n;
}

static bool HasAllBones(void* const* kids, int n, const Anchor& a) {
    for (int b = 0; b < 4; ++b) {
        bool in = false;
        for (int i = 0; i < n && !in; ++i) in = kids[i] == a.bones[b];
        if (!in) return false;
    }
    return true;
}

static bool TryLayouts(const Anchor& a, ChildLayout& found, int& count) {
    void* kids[64];
    for (int stride : {8, 16})   // plain pointers, or pointers with a word beside each
        for (int off = 0x08; off < 0x400; off += 8) {
            const ChildLayout L{kArray, off, stride};
            const int n = Children(a.node, L, kids, 64);
            if (n >= 4 && HasAllBones(kids, n, a)) {
                found = L;
                count = n;
                return true;
            }
        }
    for (int off = 0x08; off < 0x400; off += 8) {   // linked: the node's first child is one of the four bones
        void* first = nullptr;
        if (!SafeCopy(&first, static_cast<char*>(a.node) + off, 8)) continue;
        bool isBone = false;
        for (int b = 0; b < 4; ++b) isBone |= first == a.bones[b];
        if (!isBone) continue;
        for (int next = 0x08; next < 0x400; next += 8) {
            const ChildLayout L{kSiblingList, off, next};
            const int n = Children(a.node, L, kids, 64);
            if (n >= 4 && HasAllBones(kids, n, a)) {
                found = L;
                count = n;
                return true;
            }
        }
    }
    return false;
}

static void LogBoneMap(const Anchor& a) {   // where the four bones' addresses appear, for the next build
    char line[900];
    size_t len = snprintf(line, sizeof(line), "collisions: map of anchor %p (bones %p %p %p %p):", a.node, a.bones[0],
                          a.bones[1], a.bones[2], a.bones[3]);
    for (int off = 0; off < 0x400 && len + 60 < sizeof(line); off += 8) {
        void* q = nullptr;
        if (!SafeCopy(&q, static_cast<char*>(a.node) + off, 8) || !q) continue;
        for (int b = 0; b < 4; ++b)
            if (q == a.bones[b]) len += snprintf(line + len, sizeof(line) - len, " bone%d at +0x%X;", b, off);
        void* block[64];
        if ((reinterpret_cast<std::uintptr_t>(q) & 7) || !SafeProbe(block, q, sizeof(block))) continue;
        for (int i = 0; i < 64 && len + 60 < sizeof(line); ++i)
            for (int b = 0; b < 4; ++b)
                if (block[i] == a.bones[b])
                    len += snprintf(line + len, sizeof(line) - len, " bone%d via +0x%X [%d];", b, off, i);
    }
    Log("%s", line);
    for (int b = 0; b < 4; ++b) {   // a bone holding a sibling's address: a linked list
        len = snprintf(line, sizeof(line), "collisions: bone%d %p holds:", b, a.bones[b]);
        for (int off = 0; off < 0x400 && len + 40 < sizeof(line); off += 8) {
            void* q = nullptr;
            if (!SafeCopy(&q, static_cast<char*>(a.bones[b]) + off, 8)) continue;
            for (int o = 0; o < 4; ++o)
                if (o != b && q == a.bones[o]) len += snprintf(line + len, sizeof(line) - len, " bone%d at +0x%X;", o, off);
        }
        Log("%s", line);
    }
}

static void DiscoverLayout(const Anchor& a) {
    const std::uint64_t now = GetTickCount64();
    if (now < g_layoutRetryMs.load()) return;
    int expected = 0;
    if (!g_layoutState.compare_exchange_strong(expected, 1)) return;
    char an[48], cn[48];
    NodeName(a.node, an, sizeof(an));
    NodeName(a.com, cn, sizeof(cn));
    ChildLayout L{};
    int n = 0;
    {   // the game's own layout first (no speculative reads); the scan below only if the anchor disagrees
        void* kids[64];
        const int k = Children(a.node, kGameChildLayout, kids, 64);
        if (k >= 4 && HasAllBones(kids, k, a)) {
            g_childLayout = kGameChildLayout;
            g_layoutState = 2;
            Log("collisions: child lists are the game's layout (array at +0x138, 16-bit count at +0x142): anchor '%s' "
                "has %d children incl. its 4 breast bones; driver '%s'", an, k, cn);
            return;
        }
    }
    if (TryLayouts(a, L, n)) {
        g_childLayout = L;
        g_layoutState = 2;
        Log("collisions: child lists found (%s at +0x%X, %s +0x%X): anchor '%s' has %d children incl. its 4 breast "
            "bones; driver '%s'", L.mode == kArray ? "array" : "linked list, first child", L.off,
            L.mode == kArray ? "entry size" : "next sibling in each child at", L.next, an, n, cn);
        return;
    }
    const int tries = g_layoutTries.fetch_add(1) + 1;
    if (tries == 1 || tries == 10) LogBoneMap(a);
    if (tries >= 10) {
        g_layoutState = -1;
        Log("collisions: no child list found on anchor '%s' (driver '%s') after %d tries; collisions are off", an, cn, tries);
        return;
    }
    Log("collisions: no child list found on anchor '%s' (driver '%s'), try %d of 10; trying again on another body", an,
        cn, tries);
    g_layoutRetryMs = now + 2000;
    g_layoutState = 0;
}

// The bones collisions use, found under COM and cached per COM.  Each use re-checks every cached bone's name (its
// string-pool pointer), so a rebuilt skeleton is searched again; a bone is only written if it still hangs under COM.
// Two torso bones: the bottom and top of the chest shape a body shares with the bodies near it.
// (+ the feet and toes, for the lower-leg and foot shapes; the knee is the thigh collider's end, L/R_Calf)
constexpr const char* kTorsoBone[6] = {"C_Spine2", "C_Neck", "L_Foot", "R_Foot", "L_Toe", "R_Toe"};
constexpr int kNumTorso = 6;
// the world-collision proxies' nodes (A-Body's test skeletons) and, to compare, the skeleton's own C_Chest
// ragdoll body; optional (not counted as missing)
constexpr const char* kWorldBone[3] = {"ABody_Proxy_Breast_L", "ABody_Proxy_Breast_R", "C_Chest"};
constexpr int kNumWorld = 3;
constexpr int kNumBodyNodes = 4 * kNumColliders + kNumSoft + kNumTorso + kNumWorld;
static int ColA(int k, int s) { return k * 2 + s; }                       // collider start, per side
static int ColB(int k, int s) { return 2 * kNumColliders + k * 2 + s; }   // collider end, per side
static int SoftN(int i) { return 4 * kNumColliders + i; }
static int TorsoN(int t) { return 4 * kNumColliders + kNumSoft + t; }
static int WorldN(int w) { return 4 * kNumColliders + kNumSoft + kNumTorso + w; }
struct BodyNodes {
    void* com = nullptr;
    void* node[kNumBodyNodes] = {};
    void* name[kNumBodyNodes] = {};
    int found = 0;
    std::uint64_t retryMs = 0;   // nothing found: try again after this
};
static std::mutex g_nodesMutex;
constexpr int kBodySlots = 512;   // hashed by COM, 4 tries; a crowd bigger than that just searches more often
static BodyNodes g_nodes[kBodySlots];
static std::atomic<int> g_bodyLogs{0};

static BodyNodes* BodySlot(void* com, bool forInsert) {   // g_nodesMutex held
    const std::uint64_t h = (reinterpret_cast<std::uintptr_t>(com) >> 4) * 0x9E3779B97F4A7C15ull >> 55;
    for (int probe = 0; probe < 4; ++probe) {
        BodyNodes& b = g_nodes[(h + probe) & (kBodySlots - 1)];
        if (b.com == com || (forInsert && !b.com)) return &b;
    }
    return forInsert ? &g_nodes[h & (kBodySlots - 1)] : nullptr;
}

static void WantedName(int idx, char* out, size_t cap) {
    if (idx >= WorldN(0)) {
        snprintf(out, cap, "%s", kWorldBone[idx - WorldN(0)]);
        return;
    }
    if (idx >= TorsoN(0)) {
        snprintf(out, cap, "%s", kTorsoBone[idx - TorsoN(0)]);
        return;
    }
    if (idx >= SoftN(0)) {
        snprintf(out, cap, "%s", kSoft[idx - SoftN(0)].name);
        return;
    }
    const bool end = idx >= ColB(0, 0);
    const int k = (idx - (end ? ColB(0, 0) : 0)) / 2, s = idx % 2;
    snprintf(out, cap, "%c_%s", s ? 'R' : 'L', end ? kColliderBoneB[k] : kColliderBoneA[k]);
}

// Walks down from COM.  List entries that don't name their list's node as parent are followed too, but marked: a
// collider found through one is used (it is only read), a soft bone is not (it would be written).  The first bodies
// log what was passed over, and how many nodes there were, for the next build.
static std::atomic<int> g_skipLogs{0};
static void FindNodes(void* com, BodyNodes& b, int* visitedOut = nullptr) {
    b = BodyNodes{};
    b.com = com;
    char want[kNumBodyNodes][48];
    for (int i = 0; i < kNumBodyNodes; ++i) WantedName(i, want[i], sizeof(want[i]));
    bool viaSkipped[kNumBodyNodes] = {};
    struct Item { void* node; bool marked; };
    Item stack[512];
    int sp = 0, visited = 0;
    stack[sp++] = {com, false};
    char name[64];
    while (sp && visited < 2048) {
        const Item it = stack[--sp];
        ++visited;
        if (NodeName(it.node, name, sizeof(name)))
            for (int i = 0; i < kNumBodyNodes; ++i) {
                if ((b.node[i] && !viaSkipped[i]) || strcmp(name, want[i])) continue;
                if (it.marked && i >= SoftN(0) && i < TorsoN(0)) continue;   // soft bones are written: never via a mark
                void* nm = nullptr;
                if (!SafeCopy(&nm, static_cast<char*>(it.node) + 0x10, 8)) continue;
                b.found += !b.node[i];
                b.node[i] = it.node;
                b.name[i] = nm;
                viaSkipped[i] = it.marked;
            }
        void* kids[256];
        void* skipped[16];
        int nSkipped = 0;
        const int n = Children(it.node, g_childLayout, kids, 256, skipped, &nSkipped, 16);
        for (int i = 0; i < n && sp < 512; ++i) stack[sp++] = {kids[i], it.marked};
        for (int i = 0; i < nSkipped && sp < 512; ++i) {
            stack[sp++] = {skipped[i], true};
            if (g_skipLogs.fetch_add(1) < 24) {
                char sn[64], pn[64] = "?";
                void* parent = nullptr;
                NodeName(skipped[i], sn, sizeof(sn));
                if (SafeCopy(&parent, static_cast<char*>(skipped[i]) + 0x38, 8)) NodeName(parent, pn, sizeof(pn));
                Log("collisions: '%s' lists '%s' (%p), whose parent is '%s' (%p)", name, sn, skipped[i], pn, parent);
            }
        }
    }
    if (visitedOut) *visitedOut = visited;
}

// Once: which of the missing bones the skeleton's bone table (the rig) has, and their place relative to their parent.
// Tells whether a bone with no node is still animated there, for a later build.
static std::atomic<bool> g_rigChecked{false};
static void CheckRig(void* com, const BodyNodes& b) {
    if (g_rigChecked.exchange(true)) return;
    void* rig = nullptr;
    void* entries = nullptr;
    std::uint64_t end = 0;
    void* xfHolder = nullptr;
    void* xforms = nullptr;
    if (!SafeCopy(&rig, static_cast<char*>(com) + 0x20, 8) || !rig || !SafeCopy(&entries, static_cast<char*>(rig) + 0x58, 8) ||
        !SafeCopy(&end, static_cast<char*>(rig) + 0x60, 8) || !SafeCopy(&xfHolder, static_cast<char*>(rig) + 0x08, 8) ||
        !xfHolder || !SafeCopy(&xforms, static_cast<char*>(xfHolder) + 0x10, 8)) {
        Log("collisions: no bone table on COM");
        return;
    }
    int named = 0;
    for (std::uint64_t idx = 0; idx < end && idx < 4096; ++idx) {
        char* e = static_cast<char*>(entries) + idx * 24;
        void* key = nullptr;
        std::uint64_t value = 0;
        if (!SafeCopy(&key, e, 8) || !key || !SafeCopy(&value, e + 8, 8)) continue;   // the key read guarded below
        char nm[64];   // the key is a BSFixedString, read like a node's name
        void* fake[3] = {nullptr, nullptr, key};
        if (!NodeName(fake, nm, sizeof(nm))) continue;
        ++named;
        for (int i = 0; i < kNumBodyNodes; ++i) {
            char want[48];
            WantedName(i, want, sizeof(want));
            if (b.node[i] || strcmp(nm, want)) continue;
            const std::uint64_t slot = value & 0x3FF;
            float t[3] = {0, 0, 0};
            if (slot != 0x3FF) SafeCopy(t, static_cast<char*>(xforms) + slot * 64 + 0x30, 12);
            Log("collisions: bone table has '%s' (no node found): slot %llu, offset from parent %.1f %.1f %.1f cm", nm,
                static_cast<unsigned long long>(slot), t[0] * 100, t[1] * 100, t[2] * 100);
            break;
        }
    }
    Log("collisions: bone table has %d named bones", named);
}

static bool StillValid(const BodyNodes& b) {
    for (int i = 0; i < kNumBodyNodes; ++i) {
        if (!b.node[i]) continue;
        void* name = nullptr;
        if (!SafeCopy(&name, static_cast<char*>(b.node[i]) + 0x10, 8) || name != b.name[i]) return false;
    }
    return true;
}

// abody: an A-Body body (all its bones are wanted); otherwise a body that only shares its shapes, whose log
// line counts just the shape bones.
static std::atomic<int> g_otherBodyLogs{0};
static bool GetNodes(void* com, BodyNodes& out, bool abody = true) {
    const std::uint64_t now = GetTickCount64();
    bool cached = false;
    {
        std::lock_guard<std::mutex> lock(g_nodesMutex);
        if (const BodyNodes* b = BodySlot(com, false)) {
            out = *b;
            cached = true;
        }
    }
    if (cached) {
        if (out.found == 0 && now < out.retryMs) return false;
        if (out.found > 0 && StillValid(out)) return true;
    }
    BodyNodes fresh;
    int visited = 0;
    LARGE_INTEGER s0{}, s1{};
    if constexpr (kDiag) QueryPerformanceCounter(&s0);
    FindNodes(com, fresh, &visited);
    if constexpr (kDiag) {
        QueryPerformanceCounter(&s1);
        g_searches[0].fetch_add(1, std::memory_order_relaxed);
        g_searchTicks[0].fetch_add(static_cast<std::uint64_t>(s1.QuadPart - s0.QuadPart), std::memory_order_relaxed);
    }
    fresh.retryMs = now + 5000;
    if (fresh.found < kNumBodyNodes) CheckRig(com, fresh);
    {
        std::lock_guard<std::mutex> lock(g_nodesMutex);
        *BodySlot(com, true) = fresh;
    }
    out = fresh;
    if (abody ? g_bodyLogs.fetch_add(1) < 12 : g_otherBodyLogs.fetch_add(1) < 6) {
        char missing[500] = "";
        size_t len = 0;
        int wanted = 0, found = 0;
        for (int i = 0; i < kNumBodyNodes; ++i) {
            if (!abody && i >= SoftN(0) && i < TorsoN(0)) continue;
            if (i >= WorldN(0)) continue;   // optional, reported below
            ++wanted;
            found += fresh.node[i] != nullptr;
            if (!fresh.node[i] && len + 50 < sizeof(missing)) {
                char nm[48];
                WantedName(i, nm, sizeof(nm));
                len += snprintf(missing + len, sizeof(missing) - len, " %s", nm);
            }
        }
        Log("collisions: bones of a %s %s body: %d of %d found among %d nodes under COM%s%s%s", cached ? "rebuilt" : "new",
            abody ? "A-Body" : "other", found, wanted, visited, len ? "; missing:" : "", missing,
            (abody && g_cfg.proxies) ? (fresh.node[WorldN(0)] && fresh.node[WorldN(1)] ? "; world proxies: both nodes"
                                                                                     : "; world proxies: NO nodes (not the test skeleton)")
                                    : "");
    }
    return fresh.found > 0;
}

// World transform: rotation rows = axes, translation, uniform scale (+0x80, +0xB0, +0xBC).
struct Xf { Vec3 ax[3]; Vec3 pos; float scale; };
static bool WorldOf(void* node, Xf& x) {
    float m[16];
    if (!node || !SafeCopy(m, static_cast<char*>(node) + 0x80, 64)) return false;
    x.ax[0] = {m[0], m[1], m[2]};
    x.ax[1] = {m[4], m[5], m[6]};
    x.ax[2] = {m[8], m[9], m[10]};
    x.pos = {m[12], m[13], m[14]};
    x.scale = (m[15] > 0.05f && m[15] < 20.0f) ? m[15] : 1.0f;
    return true;
}
static Vec3 Apply(const Xf& x, const float l[3]) {
    const float s = x.scale;
    return {x.pos.x + s * (l[0] * x.ax[0].x + l[1] * x.ax[1].x + l[2] * x.ax[2].x),
            x.pos.y + s * (l[0] * x.ax[0].y + l[1] * x.ax[1].y + l[2] * x.ax[2].y),
            x.pos.z + s * (l[0] * x.ax[0].z + l[1] * x.ax[1].z + l[2] * x.ax[2].z)};
}
static Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 ClosestOnSegment(const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = Sub(b, a);
    const float L2 = Dot(ab, ab);
    float t = L2 > 1e-12f ? Dot(Sub(c, a), ab) / L2 : 0.0f;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return {a.x + t * ab.x, a.y + t * ab.y, a.z + t * ab.z};
}
static bool UnderNode(void* node, void* top) {
    for (int i = 0; i < 16 && node; ++i) {
        void* parent = nullptr;
        if (!SafeCopy(&parent, static_cast<char*>(node) + 0x38, 8)) return false;
        if (parent == top) return true;
        node = parent;
    }
    return false;
}

// A body's limb capsules (world), each starting partway down its limb (kColliderStart); a missing wrist or finger bone
// is placed from the skeleton's rest pose.  radiusScale: 1 for A-Body; other bodies' limbs aren't measured.
struct LimbCap { Vec3 a, b, joint; float r; bool ok, estA, estB; };   // joint: a before the trim
static void LimbCaps(const BodyNodes& b, float size, float radiusScale, LimbCap caps[kNumColliders][2]) {
    for (int k = 0; k < kNumColliders; ++k)
        for (int s = 0; s < 2; ++s) {
            LimbCap& c = caps[k][s];
            c.ok = false;
            c.estA = c.estB = false;
            c.r = kColliderRadius[k] * size * radiusScale;
            Xf xa, xb, xf;
            const bool haveA = WorldOf(b.node[ColA(k, s)], xa);
            const bool haveB = WorldOf(b.node[ColB(k, s)], xb);
            if (haveA && haveB) {
                c.a = xa.pos;
                c.b = xb.pos;
            } else if (k == 1 && haveA) {   // forearm without the wrist
                c.a = xa.pos;
                c.b = Apply(xa, kWristInForearm[s]);
                c.estB = true;
            } else if (k == 2 && !haveA && WorldOf(b.node[ColA(1, s)], xf)) {   // hand without the wrist: from the forearm
                c.a = Apply(xf, kWristInForearm[s]);
                xf.pos = c.a;
                c.b = Apply(xf, kHandTipInWrist[s]);
                c.estA = c.estB = true;
            } else if (k == 2 && haveA) {   // hand without the finger
                c.a = xa.pos;
                c.b = Apply(xa, kHandTipInWrist[s]);
                c.estB = true;
            } else {
                continue;
            }
            // start partway down the limb: the joint end sits inside the shoulder or pelvis
            c.joint = c.a;
            c.a = {c.a.x + (c.b.x - c.a.x) * kColliderStart[k], c.a.y + (c.b.y - c.a.y) * kColliderStart[k],
                   c.a.z + (c.b.z - c.a.z) * kColliderStart[k]};
            c.ok = true;
        }
}

// ------------------------------------------------------------------ other bodies
// Every body shares its shapes in this table during its own update: its limbs (whole, from the joint: the trim only
// keeps a body's own shoulder and hip off its own breasts and belly), a chest capsule (C_Spine2 to C_Neck), a pelvis
// capsule (hip joint to hip joint) and, for an A-Body body, its soft bones.  Each A-Body body then checks the bodies
// near it.  Only copied numbers are shared, never a pointer into another body's skeleton: that body's 3D can be
// unloaded at any moment on another thread.  One writer per entry (the body's own update); a reader copies an entry
// and keeps the copy only if its sequence number was even and the same before and after (a seqlock).
// Positions are the last compose's, about a frame old like the body's own; an entry shared before this frame started
// is moved forward by its body's speed, so two bodies walking together don't drift a frame apart.
// world metres; a sphere when a == b; kind: OtherKind; body: which of a reader's gathered bodies it came from
struct OtherShape { Vec3 a, b; float r; std::int16_t kind; std::uint8_t body, pad; };
static_assert(sizeof(OtherShape) == 32, "OtherShape is copied in bulk");
constexpr int kOtherShapes = 2 * kNumColliders + 2 + 4 + kNumSoft;   // limbs, chest and pelvis, calves and feet, soft
constexpr float kChestRadius = 0.10f, kPelvisRadius = 0.09f;   // first guesses, not measured: the log calibrates them
// measured on the v0.7 mesh like the limbs (60th percentile of the skin's distance from the bone segment):
// the lower leg knee to ankle (L_Calf to L_Foot), the foot ankle to toe (L_Foot to L_Toe)
constexpr float kCalfRadius = 0.053f, kFootRadius = 0.057f;
constexpr float kOtherLimbScale = 1.15f;   // limbs of bodies that aren't A-Body (mostly men): not measured either
constexpr float kBodyReach = 1.2f;         // every shape lies within this of COM (arms and legs too), at scale 1
constexpr int kMaxNearShapes = 192;        // a body checks at most this many shapes of other bodies (5 A-Body bodies)
constexpr int kMaxNearBodies = 8;          // ... from at most this many bodies
struct OtherEntry {
    std::atomic<std::uint32_t> seq{0};
    std::atomic<void*> key{nullptr};      // the sharing body's COM node: identity only, never read through
    std::atomic<std::int64_t> tick{0};    // when shared (QueryPerformanceCounter)
    void* lastKey = nullptr;              // whose shares the fields below hold (a reused entry's speed starts over)
    void* actor = nullptr;                // its character (identity only)
    Vec3 center{}, vel{};                 // COM (world) and its speed (m/s)
    float reach = 0;
    int n = 0;
    bool abody = false;
    bool player = false;                  // shared by the player's body (path C)
    bool dead = false;                    // shared by a dead body
    OtherShape shape[kOtherShapes];
};
constexpr int kOtherSlots = 128;
static OtherEntry g_others[kOtherSlots];
// What a reader keeps of each body it gathered: where it was put, and why (for the deepest-contact line).
struct OtherBody { Vec3 center, vel, shift; float ageMs; bool abody, player, dead; };

static std::int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
static std::int64_t QpcFreq() {
    static const std::int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<std::int64_t>(f.QuadPart);
    }();
    return freq;
}
static float FrameTime() {   // the game's frame time (the one the springs use), or 1/60 if it can't be read
    if (!g_addr[kFrameTime]) return 1.0f / 60.0f;
    const float dt = *reinterpret_cast<const float*>(g_addr[kFrameTime]);
    return (dt > 0.0f && dt < 0.25f) ? dt : 1.0f / 60.0f;
}

static void Share(void* key, void* actor, const Vec3& center, float reach, const OtherShape* shapes, int n, bool abody,
                  bool player, bool dead = false) {
    if (!key || n <= 0 || n > kOtherShapes) return;
    const std::int64_t now = Qpc(), freq = QpcFreq();
    const std::uint64_t h = (reinterpret_cast<std::uintptr_t>(key) >> 4) * 0x9E3779B97F4A7C15ull >> 57;   // 128 slots
    OtherEntry* e = nullptr;
    for (int p = 0; p < kOtherSlots && !e; ++p) {
        OtherEntry& s = g_others[(h + p) & (kOtherSlots - 1)];
        if (s.key.load(std::memory_order_acquire) == key) e = &s;
    }
    for (int p = 0; p < kOtherSlots && !e; ++p) {   // a free entry, or one nobody has shared to for a second
        OtherEntry& s = g_others[(h + p) & (kOtherSlots - 1)];
        void* k = s.key.load(std::memory_order_acquire);
        if (k && now - s.tick.load(std::memory_order_relaxed) < freq) continue;
        if (s.key.compare_exchange_strong(k, key)) e = &s;
    }
    if (!e) {
        g_otherFull.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::uint32_t seq = e->seq.load(std::memory_order_relaxed);
    if ((seq & 1) || !e->seq.compare_exchange_strong(seq, seq + 1, std::memory_order_acquire)) return;   // being written
    const std::int64_t last = e->tick.load(std::memory_order_relaxed);
    const float since = last ? static_cast<float>(now - last) / static_cast<float>(freq) : 0.0f;
    Vec3 vel = {0, 0, 0};
    if (e->lastKey == key && e->n > 0 && since > 0.005f && since < 0.25f) {   // speed from the share before
        vel = Scale(Sub(center, e->center), 1.0f / since);
        const float v2 = Dot(vel, vel);
        if (v2 > 100.0f) vel = Scale(vel, 10.0f / std::sqrt(v2));   // at most 10 m/s (a teleport is not a speed)
    }
    e->lastKey = key;
    e->actor = actor;
    e->center = center;
    e->vel = vel;
    e->reach = reach;
    e->n = n;
    e->abody = abody;
    e->player = player;
    e->dead = dead;
    std::memcpy(e->shape, shapes, sizeof(OtherShape) * static_cast<size_t>(n));
    e->tick.store(now, std::memory_order_relaxed);
    e->seq.store(seq + 2, std::memory_order_release);
    g_otherShared.fetch_add(1, std::memory_order_relaxed);
    if (abody) g_otherSharedABody.fetch_add(1, std::memory_order_relaxed);
    if (dead) g_otherSharedDead.fetch_add(1, std::memory_order_relaxed);
}

// The shapes of every other body that can reach (center, reach).  Each entry holds its body's last compose, as this
// body's own data does, so it is moved forward by its speed x its age.
// An entry over two frames old is passed over, not moved:
// after a hitch a speed x 0.13 s could carry it a long way.  Returns the number of shapes; bodies[] / *nBodies: what
// was kept of each body they came from.
static int Gather(void* selfKey, void* selfActor, const Vec3& center, float reach, OtherShape* out, int cap,
                  OtherBody* bodies, int* nBodies) {
    const std::int64_t now = Qpc(), freq = QpcFreq();
    const float maxAge = 2.0f * FrameTime() + 0.004f;   // two frames, and a little scheduling slack
    int n = 0, nb = 0;
    for (OtherEntry& e : g_others) {
        void* k = e.key.load(std::memory_order_acquire);
        if (!k || k == selfKey) continue;
        const std::int64_t tick = e.tick.load(std::memory_order_relaxed);
        if (!tick) continue;
        const float age = static_cast<float>(now - tick) / static_cast<float>(freq);
        if (age > 1.0f || age < -0.01f) continue;   // gone
        const std::uint32_t s1 = e.seq.load(std::memory_order_acquire);
        if (s1 & 1) {
            g_otherTorn.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        void* actor = e.actor;
        const Vec3 c = e.center, vel = e.vel;
        const float r = e.reach;
        const int m = e.n;
        const bool abody = e.abody, player = e.player, dead = e.dead;
        if (selfActor && actor == selfActor) continue;   // the same character (another skeleton of it)
        if (m <= 0 || m > kOtherShapes) continue;
        const Vec3 d0 = Sub(c, center);
        const float far0 = r + reach + 0.5f;   // a little more: it moves forward below
        if (Dot(d0, d0) > far0 * far0) continue;
        if (dead && !g_cfg.deadBodies) continue;
        // a dead body lying still barely moves, so its shapes stay good for longer (a corpse may update less
        // often than every frame) and are not moved forward; a falling one is held to two frames like the living
        const bool resting = dead && Dot(vel, vel) < 0.04f;
        if (age > (resting ? g_cfg.deadMaxAge : maxAge)) {   // near, but lagging: passed over this time (counted unless gone)
            if (age < 0.25f) g_otherTooOld.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const Vec3 shift = resting ? Vec3{0, 0, 0} : Scale(vel, age > 0 ? age : 0.0f), cc = Add(c, shift), dc = Sub(cc, center);
        if (Dot(dc, dc) > (r + reach) * (r + reach)) continue;   // too far to touch
        if (n + m > cap || nb >= kMaxNearBodies) {
            g_otherCrowd.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::memcpy(out + n, e.shape, sizeof(OtherShape) * static_cast<size_t>(m));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (e.seq.load(std::memory_order_relaxed) != s1) {   // rewritten while copying: skip it this time
            g_otherTorn.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (int i = n; i < n + m; ++i) {
            out[i].a = Add(out[i].a, shift);
            out[i].b = Add(out[i].b, shift);
            out[i].body = static_cast<std::uint8_t>(nb);
        }
        bodies[nb++] = {cc, vel, shift, age * 1000.0f, abody, player, dead};
        n += m;
        const std::uint32_t ms = static_cast<std::uint32_t>(age * 1000);
        std::uint32_t prev = g_otherOldestMs.load(std::memory_order_relaxed);
        while (ms > prev && !g_otherOldestMs.compare_exchange_weak(prev, ms)) {}
    }
    *nBodies = nb;
    return n;
}

static void LimbShapes(const LimbCap caps[kNumColliders][2], OtherShape* out, int& n) {
    for (int k = 0; k < kNumColliders; ++k)
        for (int s = 0; s < 2; ++s)   // OtherKind 0-3 = collider kinds 0-3
            if (caps[k][s].ok) out[n++] = {caps[k][s].joint, caps[k][s].b, caps[k][s].r, static_cast<std::int16_t>(k), 0, 0};
}
static void TorsoShapes(const BodyNodes& b, const LimbCap caps[kNumColliders][2], float scale, OtherShape* out, int& n) {
    Xf lo, hi;
    if (WorldOf(b.node[TorsoN(0)], lo) && WorldOf(b.node[TorsoN(1)], hi))
        out[n++] = {lo.pos, hi.pos, kChestRadius * scale, kOtherChest, 0, 0};
    if (caps[3][0].ok && caps[3][1].ok)
        out[n++] = {caps[3][0].joint, caps[3][1].joint, kPelvisRadius * scale, kOtherPelvis, 0, 0};
    for (int s = 0; s < 2; ++s) {   // the lower legs (knee = the thigh collider's end) and the feet
        Xf knee, ankle, toe;
        const bool haveKnee = WorldOf(b.node[ColB(3, s)], knee), haveAnkle = WorldOf(b.node[TorsoN(2 + s)], ankle),
                   haveToe = WorldOf(b.node[TorsoN(4 + s)], toe);
        if (haveKnee && haveAnkle) out[n++] = {knee.pos, ankle.pos, kCalfRadius * scale, kOtherCalf, 0, 0};
        if (haveAnkle && haveToe) out[n++] = {ankle.pos, toe.pos, kFootRadius * scale, kOtherFoot, 0, 0};
    }
}

// Is an A-Body body near p (shared lately)?  Only a quick look (fields read without the seqlock): it decides whether a
// body that isn't A-Body bothers to share its shapes at all.
static bool NearABody(const Vec3& p, float within) {
    const std::int64_t now = Qpc(), freq = QpcFreq();
    for (OtherEntry& e : g_others) {
        if (!e.key.load(std::memory_order_acquire) || !e.abody) continue;
        if (now - e.tick.load(std::memory_order_relaxed) > freq / 4) continue;
        const Vec3 d = Sub(e.center, p);
        if (Dot(d, d) < within * within) return true;
    }
    return false;
}

// COM under a 3D root, by name (3 levels down at most), cached per root.  For bodies that aren't A-Body: an A-Body
// body's COM comes from its gravity correctives.
struct RootCom { void* owner; void* com; void* comName; std::uint64_t retryMs; };
static std::mutex g_rootMutex;
constexpr int kRootSlots = 256;
static RootCom g_roots[kRootSlots];
static void* FindComUnder(void* owner) {
    void* level[64];
    int n = 1;
    level[0] = owner;
    char name[64];
    for (int depth = 0; depth < 4 && n; ++depth) {
        void* next[64];
        int m = 0;
        for (int i = 0; i < n; ++i) {
            if (NodeName(level[i], name, sizeof(name)) && !strcmp(name, "COM")) return level[i];
            void* kids[64];
            const int k = Children(level[i], g_childLayout, kids, 64);
            for (int j = 0; j < k && m < 64; ++j) next[m++] = kids[j];
        }
        std::memcpy(level, next, sizeof(void*) * static_cast<size_t>(m));
        n = m;
    }
    return nullptr;
}
static void* ComOf(void* owner) {
    const std::uint64_t now = GetTickCount64();
    const std::uint64_t h = (reinterpret_cast<std::uintptr_t>(owner) >> 4) * 0x9E3779B97F4A7C15ull >> 56;
    RootCom c{};
    int slot = -1;
    {
        std::lock_guard<std::mutex> lock(g_rootMutex);
        for (int p = 0; p < 4 && slot < 0; ++p)
            if (g_roots[(h + p) & (kRootSlots - 1)].owner == owner) slot = static_cast<int>((h + p) & (kRootSlots - 1));
        if (slot >= 0) c = g_roots[slot];
    }
    if (slot >= 0) {
        if (!c.com && now < c.retryMs) return nullptr;
        void* name = nullptr;
        if (c.com && SafeCopy(&name, static_cast<char*>(c.com) + 0x10, 8) && name == c.comName && UnderNode(c.com, owner))
            return c.com;
    }
    LARGE_INTEGER s0{}, s1{};
    if constexpr (kDiag) QueryPerformanceCounter(&s0);
    RootCom fresh{owner, FindComUnder(owner), nullptr, now + 5000};
    if constexpr (kDiag) {
        QueryPerformanceCounter(&s1);
        g_searches[1].fetch_add(1, std::memory_order_relaxed);
        g_searchTicks[1].fetch_add(static_cast<std::uint64_t>(s1.QuadPart - s0.QuadPart), std::memory_order_relaxed);
    }
    if (fresh.com) SafeCopy(&fresh.comName, static_cast<char*>(fresh.com) + 0x10, 8);
    static std::atomic<int> noComLogs{0};
    if (!fresh.com && noComLogs.fetch_add(1) < 3) {
        char rn[64] = "?";
        NodeName(owner, rn, sizeof(rn));
        Log("other bodies: no COM within 4 levels of a 3D root ('%s', %p); that body shares nothing", rn, owner);
    }
    {
        std::lock_guard<std::mutex> lock(g_rootMutex);
        int put = slot;
        for (int p = 0; p < 4 && put < 0; ++p)
            if (!g_roots[(h + p) & (kRootSlots - 1)].owner) put = static_cast<int>((h + p) & (kRootSlots - 1));
        if (put < 0) put = static_cast<int>(h & (kRootSlots - 1));
        g_roots[put] = fresh;
    }
    return fresh.com;
}

// A body that isn't A-Body only shares its shapes (nothing of it is moved), and only when an A-Body body is near.
static void ShareOtherBody(void* owner, void* actorHint, bool player = false) {
    if (!g_cfg.collision || !g_cfg.others || !owner || g_layoutState.load() != 2) return;
    Xf root;
    if (!WorldOf(owner, root) || !NearABody(root.pos, 3.0f)) return;
    // dead or alive, read only for a body near an A-Body body
    const int life = LifeStateOf(actorHint);
    const bool dead = DeadLifeState(life);
    if (dead) NoteDeadBody(false, life);
    if (dead && !g_cfg.deadBodies) return;
    void* com = ComOf(owner);
    if (!com) return;
    BodyNodes b;
    if (!GetNodes(com, b, false)) return;
    Xf comXf;
    if (!WorldOf(com, comXf)) return;
    LimbCap caps[kNumColliders][2];
    LimbCaps(b, comXf.scale, kOtherLimbScale, caps);
    OtherShape shapes[kOtherShapes];
    int n = 0;
    LimbShapes(caps, shapes, n);
    TorsoShapes(b, caps, comXf.scale * kOtherLimbScale, shapes, n);
    Share(com, actorHint, comXf.pos, kBodyReach * comXf.scale, shapes, n, false, player, dead);
}

// one proxy body, read with guarded copies only (no game calls): node+0x110 the collision object
// (bhkNPCollisionObject, its class checked); +0x20 its physics system, +0x20 the system's instance, co+0x28 the body's
// index there; instance+0x30/+0x38 the body IDs and their count; instance+0x20 = the world + 0x20 (hknpBSWorld, its
// class checked; as GetCollisionFilterInfo ID 125373 reads it); world+0x9B0 its bhkWorld; world+0x38/+0x40 the bodies
// (0xB0 bytes each) and their count; a body's +0x50 full ID, +0x54 flags (IS_STATIC 1, IS_DYNAMIC 2, IS_KEYFRAMED 4,
// IS_ACTIVE 8), +0x8C collision filter info, +0x30 position (as the world's body lookup ID 125204 reads them)
struct ProxyRead {
    void* co = nullptr;
    void* world = nullptr;
    void* bhk = nullptr;
    std::uint32_t bodyId = 0xFFFFFFFF, flags = 0, filter = 0;
    bool body = false;
    Vec3 pos{};
    char* bodyAt = nullptr;   // the body itself (for its motion)
};
static void ReadProxy(void* node, ProxyRead& p) {
    p = ProxyRead{};
    const std::uintptr_t coVt[1] = {g_waddr[kNpCollisionObjectVtbl]}, wVt[1] = {g_waddr[kBSWorldVtbl]};
    void* co = nullptr;
    if (!node || !SafeCopy(&co, static_cast<char*>(node) + 0x110, 8) || !ClassAt(co, coVt, 1)) return;
    p.co = co;
    void* sys = nullptr;
    void* inst = nullptr;
    std::int32_t index = -1;
    if (!SafeCopy(&sys, static_cast<char*>(co) + 0x20, 8) || !sys || !SafeCopy(&inst, static_cast<char*>(sys) + 0x20, 8) ||
        !inst || !SafeCopy(&index, static_cast<char*>(co) + 0x28, 4))
        return;   // no system instance: not in a world
    void* ids = nullptr;
    std::uint32_t count = 0;
    if (SafeCopy(&ids, static_cast<char*>(inst) + 0x30, 8) && SafeCopy(&count, static_cast<char*>(inst) + 0x38, 4) && ids &&
        index >= 0 && static_cast<std::uint32_t>(index) < count && count < 4096)
        SafeCopy(&p.bodyId, static_cast<char*>(ids) + 4 * static_cast<size_t>(index), 4);
    void* ref = nullptr;
    if (!SafeCopy(&ref, static_cast<char*>(inst) + 0x20, 8) || !ref) return;
    void* world = static_cast<char*>(ref) - 0x20;
    if (!ClassAt(world, wVt, 1)) return;
    p.world = world;
    SafeCopy(&p.bhk, static_cast<char*>(world) + 0x9B0, 8);
    const std::uint32_t id = p.bodyId & 0xFFFFFF;
    char* bodies = nullptr;
    std::uint32_t nb = 0;
    if (id == 0xFFFFFF || !SafeCopy(&bodies, static_cast<char*>(world) + 0x38, 8) || !SafeCopy(&nb, static_cast<char*>(world) + 0x40, 4) ||
        !bodies || id >= nb)
        return;
    char* b = bodies + static_cast<size_t>(id) * 0xB0;
    std::uint32_t full = 0, flags = 0;
    float t[3] = {};
    if (!SafeCopy(&full, b + 0x50, 4) || full != p.bodyId || !SafeCopy(&flags, b + 0x54, 4) || !(flags & 3)) return;
    p.body = true;
    p.flags = flags;
    p.bodyAt = b;
    SafeCopy(&p.filter, b + 0x8C, 4);
    if (SafeCopy(t, b + 0x30, 12)) p.pos = {t[0], t[1], t[2]};
}

// a proxy body's motion and both of its speed limits (the layout at g_proxyMotionOk), guarded copies only
struct ProxyMotion {
    bool motion = false, entry = false;
    std::uint32_t index = 0, props = 0, propsCount = 0;
    float limit = 0, angLimit = 0;   // the motion's own: the velocity setter holds it to these (m/s, rad/s)
    Vec3 vel{};                      // its linear velocity now
    Vec3 lo{};                       // the position's second part (body+0x40)
    float e[6] = {};                 // its table entry: gravity factor, time factor, max linear / angular, damping x2
    double com[3] = {};              // its centre of mass (3 doubles at +0x10)
};
static void ReadProxyMotion(const ProxyRead& p, ProxyMotion& m) {
    m = ProxyMotion{};
    if (!g_proxyMotionOk || !p.body || !p.bodyAt || !p.world) return;
    float lo[3], v[4], w[4];
    std::uint32_t index = 0, count = 0;
    char* motions = nullptr;
    char* world = static_cast<char*>(p.world);
    if (!SafeCopy(lo, p.bodyAt + 0x40, 12) || !SafeCopy(&index, p.bodyAt + 0x80, 4) || !SafeCopy(&motions, world + 0x188, 8) ||
        !SafeCopy(&count, world + 0x190, 4) || !motions || count > (1u << 24) || index >= count)
        return;
    char* mo = motions + static_cast<size_t>(index) * 0x80;
    std::uint16_t props = 0xFFFF;
    if (!SafeCopy(&props, mo + 0x58, 2) || !SafeCopy(v, mo + 0x60, 16) || !SafeCopy(w, mo + 0x70, 16)) return;
    m.motion = true;
    m.index = index;
    m.props = props;
    m.lo = {lo[0], lo[1], lo[2]};
    m.vel = {v[0], v[1], v[2]};
    m.limit = v[3];
    m.angLimit = w[3];
    SafeCopy(m.com, mo + 0x10, sizeof(m.com));
    char* table = nullptr;
    char* entries = nullptr;
    std::uint32_t n = 0;
    if (!SafeCopy(&table, world + 0xA58, 8) || !table || !SafeCopy(&entries, table + 0x40, 8) || !SafeCopy(&n, table + 0x48, 4) ||
        !entries || n > 65536 || props >= n)
        return;
    m.propsCount = n;
    m.entry = SafeCopy(m.e, entries + static_cast<size_t>(props) * 0x70 + 0x08, sizeof(m.e));
}

// the physics timer of a bhkWorld, read from 1.16.244.  ID 125014 writes it once a frame for each world, before
// the frame's jobs run (ID 99406 calls it through 99403 and 124990, then has 99392 build the job graph that runs both the
// physics update, job 97742, and the actors' updates): +0x7CA fixed substeps (a byte), +0x7CC the frame's time, +0x7D4
// the substep, +0x7DC the leftover after this update, +0x7E4 the leftover before it + the frame's time (what the game's
// keyframe calls are given), +0x7E8 this update's substeps (the whole ones that fit, at most +0x800).  With fixed
// substeps an update simulates its substeps only, and the rest waits in the leftover (with 1/60 s substeps: at 85 fps
// about one update in three simulates nothing, and at 50 fps an update simulates 0.56 to 1.0 of +0x7E4)
struct PhysTimer {
    bool ok = false, fixed = false;
    float frame = 0, substep = 0, leftover = 0, total = 0;
    std::uint32_t n = 0, maxN = 0;
    float This() const { return fixed ? static_cast<float>(n) * substep : total; }   // this update's simulated time
    float Next() const {   // the next update's, if the next frame takes as long as this one
        if (!fixed) return total;
        std::uint32_t k = static_cast<std::uint32_t>((leftover + frame) / substep);
        if (k > maxN) k = maxN;
        return static_cast<float>(k) * substep;
    }
};
static bool ReadPhysTimer(void* bhk, PhysTimer& t) {
    t = PhysTimer{};
    alignas(4) unsigned char raw[0x40];
    if (!bhk || !SafeCopy(raw, static_cast<char*>(bhk) + 0x7C8, sizeof(raw))) return false;
    auto f32 = [&](int at) {
        float v;
        memcpy(&v, raw + (at - 0x7C8), 4);
        return v;
    };
    auto u32 = [&](int at) {
        std::uint32_t v;
        memcpy(&v, raw + (at - 0x7C8), 4);
        return v;
    };
    t.fixed = raw[0x7CA - 0x7C8] != 0;
    t.frame = f32(0x7CC);
    t.substep = f32(0x7D4);
    t.leftover = f32(0x7DC);
    t.total = f32(0x7E4);
    t.n = u32(0x7E8);
    t.maxN = u32(0x800);
    if (!(t.total > 0.0f && t.total < 0.25f) || !(t.frame >= 0.0f && t.frame < 0.25f)) return false;
    if (t.fixed) {
        // a timer this function can't explain is not used: the substeps plus the leftover make the whole time
        if (!(t.substep >= 0.001f && t.substep <= 0.1f) || t.maxN < 1 || t.maxN > 64 || t.n > t.maxN ||
            !(t.leftover > -0.0005f && t.leftover < t.total + 0.0005f) ||
            std::fabs(static_cast<float>(t.n) * t.substep + t.leftover - t.total) > 0.0005f)
            return false;
    }
    t.ok = true;
    return true;
}
// the order learned, logged when it changes (a test run's worth of lines)
static void NoteOrder(int order, int thisHits, int nextHits) {
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1) >= 20) return;
    Log("world proxies (you): after each steer the body moved as long as %s timer said (%d of %d checks that tell them "
        "apart); the speed is now asked for %s", order ? "the next steer's" : "that steer's", order ? nextHits : thisHits,
        thisHits + nextHits, order ? "the next update's substeps" : "this update's substeps");
}
// which Havok motion each of the player's proxies and its C_Chest ragdoll body is on, logged when that
// changes, with how far each motion's centre of mass is from its body
static void NoteProxyMotions(const ProxyRead* p, const ProxyMotion* mo) {
    static std::mutex nm;
    static std::uint32_t last[3] = {0xFFFFFFFEu, 0xFFFFFFFEu, 0xFFFFFFFEu};
    static int lines = 0;
    std::uint32_t now[3];
    for (int i = 0; i < 3; ++i) now[i] = mo[i].motion ? mo[i].index : 0xFFFFFFFFu;
    {
        std::lock_guard<std::mutex> lock(nm);
        if (!std::memcmp(now, last, sizeof(now))) return;
        std::memcpy(last, now, sizeof(now));
        if (lines++ >= 60) return;
    }
    static const char* const kWho[3] = {"L proxy", "R proxy", "C_Chest ragdoll body"};
    char part[3][120];
    for (int i = 0; i < 3; ++i) {
        if (!mo[i].motion) {
            snprintf(part[i], sizeof(part[i]), "%s: its motion not read", kWho[i]);
            continue;
        }
        const Vec3 at = Add(p[i].pos, mo[i].lo);
        const double dx = mo[i].com[0] - at.x, dy = mo[i].com[1] - at.y, dz = mo[i].com[2] - at.z;
        snprintf(part[i], sizeof(part[i]), "%s: motion %u (its centre of mass %.1f cm from the body)", kWho[i], mo[i].index,
                 std::sqrt(dx * dx + dy * dy + dz * dz) * 100.0);
    }
    const bool lr = mo[0].motion && mo[1].motion && mo[0].index == mo[1].index && p[0].world == p[1].world;
    const bool chest = lr && mo[2].motion && mo[2].index == mo[0].index && p[2].world == p[0].world;
    Log("world proxies (you), motions: %s; %s; %s; the two proxies %s%s", part[0], part[1], part[2],
        lr ? "share one motion" : "have a motion each", chest ? ", and C_Chest is on it too" : "");
}

// the player's proxies' limits, logged when they change (a test run's worth of lines)
static void NoteProxyLimits(int s, const ProxyMotion& mo) {
    static std::mutex lm;
    static float last[2][4];
    static bool seen[2] = {false, false};
    static int lines = 0;
    const float now[4] = {mo.motion ? mo.limit : -1.0f, mo.motion ? mo.angLimit : -1.0f, mo.entry ? mo.e[2] : -1.0f,
                          mo.entry ? mo.e[3] : -1.0f};
    {
        std::lock_guard<std::mutex> lock(lm);
        if (seen[s] && !std::memcmp(now, last[s], sizeof(now))) return;
        std::memcpy(last[s], now, sizeof(now));
        seen[s] = true;
        if (lines++ >= 60) return;
    }
    char own[120], game[200];
    if (mo.motion)
        snprintf(own, sizeof(own), "its own %.2f m/s / %.2f rad/s (motion %u, moving %.2f m/s)", mo.limit, mo.angLimit, mo.index,
                 std::sqrt(Dot(mo.vel, mo.vel)));
    else
        snprintf(own, sizeof(own), "its motion not read");
    if (mo.entry)
        snprintf(game, sizeof(game), "the game's warp test %.2f m/s / %.2f rad/s (motion properties %u of %u: gravity %.2f, "
                 "time %.2f, damping %.2f / %.2f)", mo.e[2], mo.e[3], mo.props, mo.propsCount, mo.e[0], mo.e[1], mo.e[4], mo.e[5]);
    else
        snprintf(game, sizeof(game), "its motion properties not read (index %u)", mo.props);
    Log("world proxies (you), speed limits of the %s proxy: %s; %s; TF Physics warps it above %.2f m/s", s ? "R" : "L", own,
        game, (mo.motion && mo.limit > 0.1f && mo.limit < g_cfg.proxyMaxSpeed) ? mo.limit : g_cfg.proxyMaxSpeed);
}

// when each proxy's layer was last set (by collision object), so a set still in the game's queue isn't
// sent again each frame; two objects on one slot only cost an extra set
struct ProxySet { void* co; std::uint64_t ms; };
static std::mutex g_proxySetMutex;
static ProxySet g_proxySetTimes[256];
static bool ProxySetDue(void* co, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(g_proxySetMutex);
    ProxySet& e = g_proxySetTimes[((reinterpret_cast<std::uintptr_t>(co) >> 4) * 0x9E3779B97F4A7C15ull) >> 56];
    if (e.co == co && now - e.ms < 500) return false;
    e = {co, now};
    return true;
}
static void KeepMax(std::atomic<std::uint32_t>& slot, const Vec3& d, float unitsPerMetre) {
    const std::uint32_t v = static_cast<std::uint32_t>(std::sqrt(Dot(d, d)) * unitsPerMetre);
    std::uint32_t prev = slot.load(std::memory_order_relaxed);
    while (v > prev && !slot.compare_exchange_weak(prev, v, std::memory_order_relaxed)) {
    }
}

static const char* MotionName(const ProxyRead& p) {
    if (!p.body) return p.world ? "no body" : (p.co ? "not in a world" : "no collision object");
    return (p.flags & 4) ? "keyframed" : (p.flags & 2) ? "dynamic" : (p.flags & 1) ? "static" : "?";
}

// Per A-Body update: counted; for the player (path C) each change of world, body, motion or layer is logged, with
// the player's cell (actor+0xB0; interior = cell+0x48 bit 0) and the exterior world (*ID 938080), and the distance of
// each proxy body from its node is kept for the 5 s line
static void WatchProxies(const ActorView& v, int site) {
    if (!kDiag || !g_cfg.proxies || !g_proxyOk || !v.com) return;
    BodyNodes b;
    if (!GetNodes(v.com, b)) return;
    if (!b.node[WorldN(0)] || !b.node[WorldN(1)]) {
        g_proxyNoNodes.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ProxyRead p[kNumWorld];
    for (int i = 0; i < kNumWorld; ++i) ReadProxy(b.node[WorldN(i)], p[i]);
    g_proxyBodies.fetch_add(1, std::memory_order_relaxed);
    if (p[0].world && p[1].world) g_proxyInWorld.fetch_add(1, std::memory_order_relaxed);
    if (p[2].world) g_proxyChest.fetch_add(1, std::memory_order_relaxed);
    // the layer, for every A-Body body: layer 62 (+ bit 14), the game's group kept.  The setter queues the
    // change, so a set the step hasn't applied yet isn't sent again for 0.5 s (and a layer the game takes back is set
    // again then, and counted)
    if (g_cfg.proxyLayer && g_proxyWriteOk) {
        const std::uint64_t now = GetTickCount64();
        for (int s = 0; s < 2; ++s) {
            if (!p[s].body) continue;
            const std::uint32_t want = (p[s].filter & 0xFFFF0000u) | (g_cfg.proxyInert ? 0x4000u : 0u) | kProxyLayer;
            if ((p[s].filter & 0xFFFFu) == (want & 0xFFFFu) || !ProxySetDue(p[s].co, now)) continue;
            const bool ok = reinterpret_cast<SetFilterFn>(g_waddr[kSetFilterFn])(p[s].co, want);
            g_proxySets.fetch_add(1, std::memory_order_relaxed);
            if (!ok) g_proxySetFails.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (site != 2) return;
    // steering, the player's two proxies: to the breast centre after this frame's TF offsets (the mean of the
    // four TF_Breast bones' local translations on the Pecs anchor's last compose), turned like the anchor.  Only a proxy
    // on layer 62 is steered, never a collidable one
    if (g_cfg.steer && g_proxyWriteOk) {
        static std::mutex sm;
        static Vec3 lastTarget[2];
        static bool haveLast[2] = {false, false};
        // the check after each steer: the velocity asked (none after a warp), where Havok had the body then,
        // that steer's simulated time
        static Vec3 lastVel[2], lastAt[2];
        static float lastThis[2] = {0, 0};
        static bool haveCheck[2] = {false, false};
        int* const tally = g_orderTally;
        std::lock_guard<std::mutex> lock(sm);
        // the motions first.  When the two proxies are on one Havok motion, each steer moves both, so the
        // second would undo the first: with [World] bSteerOnce that motion is steered once, through the left proxy, and
        // the right one rides on it (still measured)
        ProxyMotion mo[kNumWorld];
        for (int i = 0; i < kNumWorld; ++i) ReadProxyMotion(p[i], mo[i]);
        NoteProxyMotions(p, mo);
        const bool shared = mo[0].motion && mo[1].motion && mo[0].index == mo[1].index && p[0].world == p[1].world;
        // in game the proxies' motion is the C_Chest ragdoll body's too; when the game makes that body dynamic
        // (a ragdoll: death, a knockdown), the motion is the ragdoll's and nothing on it is steered
        const bool chestOn = mo[2].motion && mo[0].motion && mo[2].index == mo[0].index && p[2].world == p[0].world;
        g_proxyFrames.fetch_add(1, std::memory_order_relaxed);
        if (shared) g_proxyShared.fetch_add(1, std::memory_order_relaxed);
        for (int s = 0; s < 2; ++s) {
            const Anchor& a = v.anchors[s];
            int skip = (!p[s].body || !p[s].bhk) ? 0 : (p[s].filter & 0x7F) != kProxyLayer ? 1 : !a.node ? 2 : -1;
            if (skip < 0 && (!(p[s].flags & 4) || (chestOn && p[2].body && !(p[2].flags & 4)))) skip = 4;
            if (skip >= 0) {
                g_proxySkips[skip].fetch_add(1, std::memory_order_relaxed);
                haveLast[s] = false;
                haveCheck[s] = false;
                continue;
            }
            if (haveLast[s]) KeepMax(g_proxyLagMm[s], Sub(p[s].pos, lastTarget[s]), 1000.0f);
            float mean[3] = {0, 0, 0};
            int nb = 0;
            for (int k = 0; k < 4; ++k) {
                float t[3];
                if (!a.bones[k] || !SafeCopy(t, static_cast<char*>(a.bones[k]) + 0x70, 12)) continue;
                for (int j = 0; j < 3; ++j) mean[j] += t[j];
                ++nb;
            }
            Xf ax;
            float stepS = 0;   // the time the next physics update simulates, seconds
            if (nb != 4 || !WorldOf(a.node, ax)) {
                g_proxySkips[2].fetch_add(1, std::memory_order_relaxed);
                haveLast[s] = false;
                haveCheck[s] = false;
                continue;
            }
            if (!SafeCopy(&stepS, static_cast<char*>(p[s].bhk) + 0x7E4, 4) || !(stepS > 0.0001f && stepS < 0.25f)) {
                g_proxySkips[3].fetch_add(1, std::memory_order_relaxed);
                haveLast[s] = false;
                haveCheck[s] = false;
                continue;
            }
            const std::uint32_t tenths = static_cast<std::uint32_t>(stepS * 10000.0f);
            std::uint32_t was = g_proxyStepMin.load(std::memory_order_relaxed);
            while (tenths < was && !g_proxyStepMin.compare_exchange_weak(was, tenths, std::memory_order_relaxed)) {
            }
            was = g_proxyStepMax.load(std::memory_order_relaxed);
            while (tenths > was && !g_proxyStepMax.compare_exchange_weak(was, tenths, std::memory_order_relaxed)) {
            }
            for (float& m : mean) m *= 0.25f;
            const Vec3 target = Apply(ax, mean);
            alignas(32) float m[16] = {ax.ax[0].x, ax.ax[0].y, ax.ax[0].z, 0, ax.ax[1].x, ax.ax[1].y, ax.ax[1].z, 0,
                                       ax.ax[2].x, ax.ax[2].y, ax.ax[2].z, 0, target.x, target.y, target.z, 1.0f};
            // the speed this step needs, from where Havok has the body (+0x30 plus its second part, as the
            // keyframe function's velocity maths adds them).  Over the cap (the INI's, never above the body's own
            // limit, which the velocity setter would hold it to) the proxy is warped: the keyframe call with a 1 ns
            // step needs a speed over any table limit, so it takes the game's own warp path (placed, velocity 0)
            const ProxyMotion& ms = mo[s];
            NoteProxyLimits(s, ms);
            const Vec3 at = ms.motion ? Add(p[s].pos, ms.lo) : p[s].pos;
            const Vec3 gap = Sub(target, at);
            const float dist = std::sqrt(Dot(gap, gap)), cm = dist * 100.0f;
            g_proxyGapHist[s][cm < 2 ? 0 : cm < 5 ? 1 : cm < 10 ? 2 : cm < 25 ? 3 : cm < 50 ? 4 : 5].fetch_add(
                1, std::memory_order_relaxed);
            if (s == 1 && shared && g_cfg.steerOnce) {   // rides on the motion the left proxy steered
                g_proxyRides.fetch_add(1, std::memory_order_relaxed);
                KeepMax(g_proxyStepMm[s], gap, 1000.0f);
                lastTarget[s] = target;
                haveLast[s] = true;
                haveCheck[s] = false;
                continue;
            }
            // the physics timer.  With fixed substeps the update after the steer simulates whole substeps
            // only, so a speed of gap / +0x7E4 (what the game's own keyframe calls use) covers 0 to 100 % of the gap
            PhysTimer tm;
            const bool timer = ReadPhysTimer(p[s].bhk, tm);
            g_timerReads[!timer ? 2 : tm.fixed ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
            if (timer && tm.fixed) {
                g_timerSubsteps[tm.n < 3 ? tm.n : 3].fetch_add(1, std::memory_order_relaxed);
                KeepMinU(g_substepMin, tm.substep * 1.0e5f);
                KeepMaxU(g_substepMax, tm.substep * 1.0e5f);
                KeepMinU(g_leftMin, tm.leftover * 1.0e5f);
                KeepMaxU(g_leftMax, tm.leftover * 1.0e5f);
                KeepMaxU(g_timerMaxN, static_cast<float>(tm.maxN));
            }
            // the check of the last steer: how long the body moved with the velocity asked then (its move
            // along that velocity / its speed), against the time the timer gave at that steer (the update after it ran
            // in the same frame) and the time it gives now (the update ran at the start of the next frame)
            if (haveCheck[s] && timer && tm.fixed) {
                const float v2 = Dot(lastVel[s], lastVel[s]);
                if (v2 > 0.09f) {   // over 0.3 m/s, so a move this small is still measured well
                    const float moved = Dot(Sub(at, lastAt[s]), lastVel[s]) / v2, then = lastThis[s], now = tm.This();
                    const float tolThen = 0.1f * then > 0.0015f ? 0.1f * then : 0.0015f,
                                tolNow = 0.1f * now > 0.0015f ? 0.1f * now : 0.0015f;
                    const bool mThen = std::fabs(moved - then) <= tolThen, mNow = std::fabs(moved - now) <= tolNow;
                    g_orderHits[mThen && mNow ? 2 : mThen ? 0 : mNow ? 1 : 3].fetch_add(1, std::memory_order_relaxed);
                    KeepMinU(g_movedMin, moved * 1.0e5f);
                    KeepMaxU(g_movedMax, moved * 1.0e5f);
                    if (mThen != mNow) ++tally[mThen ? 0 : 1];
                    if (tally[0] + tally[1] >= 24) {   // a choice from 24 checks that tell the two apart, by 2 to 1
                        const int nChecks = tally[0] + tally[1], used = g_orderUsed.load();
                        const int want = 3 * tally[1] >= 2 * nChecks ? 1 : 3 * tally[0] >= 2 * nChecks ? 0 : used;
                        if (want != used && g_cfg.steerTime == 1) {
                            g_orderUsed.store(want);
                            NoteOrder(want, tally[0], tally[1]);
                        }
                        tally[0] = tally[1] = 0;
                    }
                }
            }
            haveCheck[s] = false;
            // the time the speed is asked for: this update's substeps when the update after a steer runs in
            // the same frame, the next update's (as if the next frame takes as long as this one, at least one substep)
            // when it runs at the start of the next frame
            float h = stepS;
            int from = 2;
            if (timer && tm.fixed && g_cfg.steerTime != 0) {
                const int order = g_cfg.steerTime == 2 ? 0 : g_cfg.steerTime == 3 ? 1 : g_orderUsed.load();
                if (order == 0) {
                    h = tm.This();
                    from = 0;
                } else {
                    h = tm.Next();
                    if (h < tm.substep) h = tm.substep;
                    from = 1;
                }
            }
            if (!(h > 0.0001f)) {   // the update after this steer simulates no time: nothing would move in it
                g_timeFrom[3].fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            g_timeFrom[from].fetch_add(1, std::memory_order_relaxed);
            const float need = dist / h;
            float cap = g_cfg.proxyMaxSpeed;
            if (ms.motion && ms.limit > 0.1f && ms.limit < cap) cap = ms.limit;
            g_proxyCapMmS.store(static_cast<std::uint32_t>(cap * 1000.0f), std::memory_order_relaxed);
            // is the physics update running on another thread right now (its step lock, hknpBSWorld+0xB08:
            // the owning thread's ID, 0 when free; the simulation stage at +0x9C0, DONE = 1)?  Counted only
            std::uint32_t owner = 0, stage = 1;
            if (SafeCopy(&owner, static_cast<char*>(p[s].world) + 0xB08, 4) &&
                SafeCopy(&stage, static_cast<char*>(p[s].world) + 0x9C0, 4)) {
                g_steerChecked.fetch_add(1, std::memory_order_relaxed);
                if (owner && owner != GetCurrentThreadId()) g_steerInUpdate.fetch_add(1, std::memory_order_relaxed);
                if (stage != 1) g_steerMidStep.fetch_add(1, std::memory_order_relaxed);
            }
            bool warped = false;
            if (need > cap) {
                reinterpret_cast<KeyframeFn>(g_waddr[kKeyframeFn])(p[s].co, m, kWarpStep, &warped);
                g_proxyOwnWarps.fetch_add(1, std::memory_order_relaxed);
                if (!warped) g_proxyOwnMissed.fetch_add(1, std::memory_order_relaxed);
            } else {
                reinterpret_cast<KeyframeFn>(g_waddr[kKeyframeFn])(p[s].co, m, h, &warped);
                if (warped) {
                    g_proxyWarps.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_proxyMoved.fetch_add(1, std::memory_order_relaxed);
                    KeepMaxU(g_proxyAskedMmS, need * 1000.0f);
                }
            }
            g_proxySteers.fetch_add(1, std::memory_order_relaxed);
            if (ms.motion) {
                KeepMaxU(g_proxyBodyMmS, std::sqrt(Dot(ms.vel, ms.vel)) * 1000.0f);
                KeepMaxU(g_proxyLoUm, std::sqrt(Dot(ms.lo, ms.lo)) * 1.0e6f);
            }
            KeepMax(g_proxyStepMm[s], gap, 1000.0f);
            lastTarget[s] = target;
            haveLast[s] = true;
            if (!warped && timer && tm.fixed) {   // what the next steer checks
                lastVel[s] = Scale(gap, 1.0f / h);
                lastAt[s] = at;
                lastThis[s] = tm.This();
                haveCheck[s] = true;
            }
        }
    }
    for (int s = 0; s < 2; ++s) {
        Xf nx;
        if (!p[s].body || !WorldOf(b.node[WorldN(s)], nx)) continue;
        const Vec3 d = Sub(p[s].pos, nx.pos);
        const std::uint32_t mm = static_cast<std::uint32_t>(std::sqrt(Dot(d, d)) * 1000);
        std::uint32_t prev = g_proxyDistMm[s].load(std::memory_order_relaxed);
        while (mm > prev && !g_proxyDistMm[s].compare_exchange_weak(prev, mm, std::memory_order_relaxed)) {
        }
    }
    Xf chestXf;   // does the game keep the ragdoll's own C_Chest body on its bone?
    if (p[2].body && WorldOf(b.node[WorldN(2)], chestXf)) KeepMax(g_chestDistMm, Sub(p[2].pos, chestXf.pos), 1000.0f);
    void* cell = nullptr;
    std::uint8_t cellFlags = 0;
    void* exterior = nullptr;
    const std::uintptr_t player[1] = {g_playerVt};
    if (ClassAt(v.actor, player, 1) && SafeCopy(&cell, static_cast<char*>(v.actor) + 0xB0, 8) && cell)
        SafeCopy(&cellFlags, static_cast<char*>(cell) + 0x48, 1);
    void* holder = nullptr;   // the global holds a pointer to the world's pointer
    if (SafeCopy(&holder, reinterpret_cast<void*>(g_waddr[kExteriorWorld]), 8) && holder) SafeCopy(&exterior, holder, 8);
    struct State { void* world[kNumWorld]; std::uint32_t id[kNumWorld], flags[kNumWorld], filter[kNumWorld]; void* cell; void* exterior; };
    State now{};
    for (int i = 0; i < kNumWorld; ++i) {
        now.world[i] = p[i].world;
        now.id[i] = p[i].bodyId;
        now.flags[i] = p[i].flags & 0xF;
        now.filter[i] = p[i].filter;
    }
    now.cell = cell;
    now.exterior = exterior;
    static std::mutex m;
    static State last{};
    static bool first = true;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!first && std::memcmp(&now, &last, sizeof(State)) == 0) return;
        first = false;
        last = now;
    }
    g_proxyChanges.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) >= 400) return;   // a test run's worth
    char part[kNumWorld][200];
    static const char* const kWho[kNumWorld] = {"L proxy", "R proxy", "C_Chest ragdoll body"};
    for (int i = 0; i < kNumWorld; ++i)
        snprintf(part[i], sizeof(part[i]), "%s: world %p (bhkWorld %p), body %08X %s, layer %u group %u", kWho[i], p[i].world,
                 p[i].bhk, p[i].bodyId, MotionName(p[i]), p[i].filter & 0x7F, p[i].filter >> 16);
    Log("world proxies (you), thread %lu: %s; %s; %s; your cell %p (%s), the exterior world %p", GetCurrentThreadId(), part[0],
        part[1], part[2], cell, cell ? ((cellFlags & 1) ? "interior" : "exterior") : "none", exterior);
}

// the soft bones of one part (a breast, the belly, a glute, an inner thigh) push as one soft mass: no two of
// a part's pushes (world) differ by more than the part's spread (kGroupPushSpread: the furthest one bone moved alone
// leaves the skin smooth, no hard crease and no sharp edge, tests\stretch_limit_push_smooth.py).  A bone with no contact
// of its own follows the pushed ones.  Of two pushed bones, the one the move carries further along its own push follows
// the other (its part carries it, away from what pushes it); when the move would carry both, or take both back against
// their pushes (a squeeze), they give way evenly.  Every push stays inside its group's cap
static void CouplePushes(Vec3 push[kNumSoft], const bool on[kNumSoft], const bool contact[kNumSoft], float size) {
    for (int g = 0; g < kNumGroups; ++g)
        for (int side = -1; side <= 1; ++side) {
            int m[kNumSoft], n = 0;
            for (int i = 0; i < kNumSoft; ++i)
                if (on[i] && kSoft[i].group == g && kSoft[i].side == side) m[n++] = i;
            if (n < 2) continue;
            const float spread = kGroupPushSpread[g] * size;
            for (int pass = 0; pass < 4; ++pass)
                for (int a = 0; a < n; ++a)
                    for (int b = a + 1; b < n; ++b) {
                        const int i = m[a], j = m[b];
                        const Vec3 d = Sub(push[i], push[j]);
                        const float L = std::sqrt(Dot(d, d));
                        if (L <= spread) continue;
                        const Vec3 corr = Scale(d, (L - spread) / L);
                        float wi = 0.5f;   // how much of the move is i's
                        if (contact[i] != contact[j]) {
                            wi = contact[i] ? 0.0f : 1.0f;
                        } else if (contact[i]) {
                            const bool carriesI = Dot(d, push[i]) <= 0.0f, carriesJ = Dot(d, push[j]) >= 0.0f;
                            if (carriesI != carriesJ) wi = carriesI ? 1.0f : 0.0f;
                        }
                        push[i] = Sub(push[i], Scale(corr, wi));
                        push[j] = Add(push[j], Scale(corr, 1.0f - wi));
                    }
            const float cap = g_cfg.maxPush[g] * size;
            for (int a = 0; a < n; ++a) {
                const float L = std::sqrt(Dot(push[m[a]], push[m[a]]));
                if (L > cap) push[m[a]] = Scale(push[m[a]], cap / L);
            }
        }
}

static void Collide(ActorView& v) {
    if (g_layoutState.load() == 0)
        for (const Anchor& a : v.anchors)
            if (a.node && a.bones[0] && a.bones[1] && a.bones[2] && a.bones[3]) {
                DiscoverLayout(a);
                break;
            }
    if (g_layoutState.load() != 2) return;
    BodyNodes b;
    if (!GetNodes(v.com, b)) return;
    Xf comXf;
    if (!WorldOf(v.com, comXf)) return;
    const float size = comXf.scale;   // the actor's scale: all sizes were measured at 1

    LimbCap caps[kNumColliders][2];
    LimbCaps(b, size, 1.0f, caps);

    // soft bones (world): from their spring's position plus this frame's gravity (anchor offset and slump), and,
    // for soft against soft, without that gravity: lying on a side it moves one breast toward the other by design
    Vec3 pos[kNumSoft], posNoGravity[kNumSoft], posBase[kNumSoft];
    Xf parentXf[kNumSoft];
    bool have[kNumSoft] = {};
    for (int i = 0; i < kNumSoft; ++i) {
        void* node = b.node[SoftN(i)];
        void* parent = nullptr;
        if (!node || !SafeCopy(&parent, static_cast<char*>(node) + 0x38, 8) || !UnderNode(node, v.com)) continue;
        if (!WorldOf(parent, parentXf[i])) continue;
        Xf parentNoGravity = parentXf[i];
        for (int s = 0; s < 2; ++s)   // a breast anchor was moved this frame: its position from the chest
            if (parent == v.anchors[s].node && v.anchorWritten[s]) {
                Xf chest;
                if (!WorldOf(At<void*>(parent, 0x38), chest)) continue;
                parentXf[i].pos = Apply(chest, v.anchorLocal[s]);
                float rest[3];
                if (v.anchors[s].rest && SafeCopy(rest, v.anchors[s].rest, 12)) parentNoGravity.pos = Apply(chest, rest);
            }
        float local[3];
        if (!BaseOf(node, local)) continue;
        posNoGravity[i] = Apply(parentNoGravity, local);
        posBase[i] = Apply(parentXf[i], local);
        if (const float* add = v.AddFor(node))
            for (int k = 0; k < 3; ++k) local[k] += add[k];
        pos[i] = Apply(parentXf[i], local);
        have[i] = true;
    }

    // other bodies that can reach this one, and of their shapes the ones that come near its soft bones
    OtherShape others[kMaxNearShapes];
    OtherBody nearBodies[kMaxNearBodies];
    int nOthers = 0, nearIdx[kMaxNearShapes], nNear = 0;
    if (g_cfg.others && (!v.dead || g_cfg.deadBodies)) {
        int bodies = 0;
        nOthers = Gather(v.com, v.actor, comXf.pos, kBodyReach * size, others, kMaxNearShapes, nearBodies, &bodies);
        if (bodies) g_otherNear.fetch_add(static_cast<std::uint64_t>(bodies), std::memory_order_relaxed);
        if (nOthers) {
            Vec3 c = {0, 0, 0};
            int cnt = 0;
            for (int i = 0; i < kNumSoft; ++i)
                if (have[i]) {
                    c = Add(c, pos[i]);
                    ++cnt;
                }
            float rad = 0;
            if (cnt) {
                c = Scale(c, 1.0f / static_cast<float>(cnt));
                for (int i = 0; i < kNumSoft; ++i)
                    if (have[i]) {
                        const Vec3 d = Sub(pos[i], c);
                        rad = std::fmax(rad, std::sqrt(Dot(d, d)) + kSoft[i].radius * g_cfg.softScale * size);
                    }
            }
            for (int o = 0; o < nOthers && cnt; ++o) {
                const Vec3 d = Sub(c, ClosestOnSegment(others[o].a, others[o].b, c));
                if (Dot(d, d) < (rad + others[o].r) * (rad + others[o].r)) nearIdx[nNear++] = o;
            }
        }
    }

    // one body every 5 s (site C, usually the player): every position in COM's frame, and its contacts
    const bool snap = v.site == 2 && g_snapDue.load(std::memory_order_relaxed) && g_snapDue.exchange(false);
    char contacts[2400];
    size_t cl = 0;
    contacts[0] = 0;
    if (snap) {
        auto inCom = [&](const Vec3& p) {
            const Vec3 r = Sub(p, comXf.pos);
            return Vec3{Dot(r, comXf.ax[0]) / size * 100, Dot(r, comXf.ax[1]) / size * 100, Dot(r, comXf.ax[2]) / size * 100};
        };
        Log("snapshot (site C body %p): COM at %.2f %.2f %.2f m, scale %.3f; below: positions in COM's frame, cm", v.com,
            comXf.pos.x, comXf.pos.y, comXf.pos.z, size);
        for (int k = 0; k < kNumColliders; ++k)
            for (int s = 0; s < 2; ++s) {
                if (!caps[k][s].ok) continue;
                const Vec3 a = inCom(caps[k][s].joint), e = inCom(caps[k][s].b);   // the joint, untrimmed
                Log("  collider %c_%s -> %s: %.1f %.1f %.1f%s -> %.1f %.1f %.1f%s", s ? 'R' : 'L', kColliderBoneA[k],
                    kColliderBoneB[k], a.x, a.y, a.z, caps[k][s].estA ? " (estimated)" : "", e.x, e.y, e.z,
                    caps[k][s].estB ? " (estimated)" : "");
            }
        for (int i = 0; i < kNumSoft; ++i) {
            if (!have[i]) continue;
            void* parent = At<void*>(b.node[SoftN(i)], 0x38);
            char pn[64];
            NodeName(parent, pn, sizeof(pn));
            Xf own;
            const bool haveOwn = WorldOf(b.node[SoftN(i)], own);
            const Vec3 a = inCom(posBase[i]), o = haveOwn ? inCom(own.pos) : Vec3{0, 0, 0};
            const Vec3 dd = Sub(a, o);
            Log("  soft %s (parent '%s'): from parent %.1f %.1f %.1f, game's own %.1f %.1f %.1f, %.1f cm apart", kSoft[i].name,
                pn, a.x, a.y, a.z, o.x, o.y, o.z, std::sqrt(Dot(dd, dd)));
        }
        if (g_cfg.others) Log("  other bodies near: %d shapes, %d of them near the soft bones", nOthers, nNear);
    }

    static const char* kOtherShapeName[kNumOtherKinds] = {"upper arm", "forearm", "hand", "thigh", "chest", "pelvis", "calf",
                                                          "foot", "soft"};
    Vec3 push[kNumSoft] = {};   // every bone's push (world) first, then the parts coupled, then put on
    bool pushOn[kNumSoft] = {}, touched[kNumSoft] = {};
    for (int i = 0; i < kNumSoft; ++i) {
        if (!have[i]) continue;
        const SoftBoneDef& d = kSoft[i];
        if (d.group == 2 && !g_cfg.innerThighs) continue;
        if (d.group == 3 && !g_cfg.glutes) continue;
        const float r = d.radius * g_cfg.softScale * size;
        Vec3 total = {0, 0, 0};
        for (int k = 0; k < kNumColliders; ++k) {
            const int rule = kGroupMeets[d.group][k];
            if (!rule) continue;
            for (int s = 0; s < 2; ++s) {
                if (!caps[k][s].ok) continue;
                if (d.side >= 0 && ((rule == 2 && s != d.side) || (rule == 3 && s == d.side))) continue;
                const Vec3 dv = Sub(pos[i], ClosestOnSegment(caps[k][s].a, caps[k][s].b, pos[i]));
                const float dist = std::sqrt(Dot(dv, dv));
                float contact = r + caps[k][s].r;
                const float rest = d.restToCollider[k][s] * size;
                if (rest > 0 && rest < contact) contact = rest;
                if (dist > 1e-6f && dist < contact) {
                    CountPair(d.group, k, contact - dist);
                    if (snap && contact - dist >= 0.001f && cl + 80 < sizeof(contacts))
                        cl += snprintf(contacts + cl, sizeof(contacts) - cl, " %s<-%c_%s %.1f/%.1f;", d.name, s ? 'R' : 'L',
                                       kColliderBoneA[k], dist * 100, contact * 100);
                    const float f = (contact - dist) / dist;
                    total = {total.x + dv.x * f, total.y + dv.y * f, total.z + dv.z * f};
                }
            }
        }
        for (int p = 0; p < 4; ++p) {   // the other side's bones of the same group: half each
            const int j = d.pairWith[p];
            if (j < 0 || !have[j]) continue;
            const Vec3 dv = Sub(posNoGravity[i], posNoGravity[j]);
            const float dist = std::sqrt(Dot(dv, dv));
            float contact = r + kSoft[j].radius * g_cfg.softScale * size;
            const float rest = d.restToPair[p] * size;
            if (rest > 0 && rest < contact) contact = rest;
            if (dist > 1e-6f && dist < contact) {
                CountPair(d.group, kNumColliders, contact - dist);
                if (snap && contact - dist >= 0.001f && cl + 80 < sizeof(contacts))
                    cl += snprintf(contacts + cl, sizeof(contacts) - cl, " %s<-%s %.1f/%.1f;", d.name, kSoft[j].name,
                                   dist * 100, contact * 100);
                const float f = 0.5f * (contact - dist) / dist;
                total = {total.x + dv.x * f, total.y + dv.y * f, total.z + dv.z * f};
            }
        }
        // other bodies: no rest distance (no contact with another body happens standing still on your own); another
        // body's soft bone takes half, like the other breast
        Vec3 totalOther = {0, 0, 0};
        for (int o = 0; o < nNear; ++o) {
            const OtherShape& sh = others[nearIdx[o]];
            const Vec3 dv = Sub(pos[i], ClosestOnSegment(sh.a, sh.b, pos[i]));
            const float dist = std::sqrt(Dot(dv, dv));
            const float contact = r + sh.r;
            if (dist > 1e-6f && dist < contact) {
                const OtherBody& ob = nearBodies[sh.body];
                const int who = (v.dead || ob.dead) ? kWhoDead : (v.site == 2 ? kWhoYours : (ob.player ? kWhoByYou : kWhoNpcs));
                CountOther(who, d.group, sh.kind, contact - dist);
                if (contact - dist >= 0.001f &&
                    static_cast<std::uint32_t>((contact - dist) * 1000) > g_deepMm[who].load(std::memory_order_relaxed)) {
                    DeepContact c{};
                    c.depth = contact - dist;
                    c.contact = contact;
                    const Vec3 apart = Sub(ob.center, comXf.pos);
                    c.apart = std::sqrt(Dot(apart, apart));
                    c.up = apart.z;
                    c.ageMs = ob.ageMs;
                    c.speed = std::sqrt(Dot(ob.vel, ob.vel));
                    c.shiftCm = std::sqrt(Dot(ob.shift, ob.shift)) * 100;
                    c.group = d.group;
                    c.kind = sh.kind;
                    c.abody = ob.abody;
                    c.selfDead = v.dead;
                    c.otherDead = ob.dead;
                    c.selfUp = pos[i].z - comXf.pos.z;   // where the contact is
                    c.aUp = sh.a.z - ob.center.z;
                    c.bUp = sh.b.z - ob.center.z;
                    c.r = sh.r;
                    c.bodyR = r;
                    strncpy_s(c.bone, d.name, _TRUNCATE);
                    NoteDeep(who, c);
                }
                if (snap && contact - dist >= 0.001f && cl + 90 < sizeof(contacts))
                    cl += snprintf(contacts + cl, sizeof(contacts) - cl, " %s<-other's %s %.1f/%.1f;", d.name,
                                   kOtherShapeName[sh.kind], dist * 100, contact * 100);
                if (!g_cfg.othersMeasureOnly) {
                    const float f = (sh.kind == kOtherSoft ? 0.5f : 1.0f) * (contact - dist) / dist;
                    totalOther = {totalOther.x + dv.x * f, totalOther.y + dv.y * f, totalOther.z + dv.z * f};
                }
            }
        }
        // other bodies push at most fMaxPushOtherCm.  Another body's shape can arrive deep inside at once
        // (it moved a frame's worth, or its entry was passed over a frame), and a full group cap would snap the bone
        const float lo = std::sqrt(Dot(totalOther, totalOther)), capOther = g_cfg.maxPushOther * size;
        if (lo > capOther) totalOther = Scale(totalOther, capOther / lo);
        total = Add(total, totalOther);
        float L = std::sqrt(Dot(total, total));
        const float cap = g_cfg.maxPush[d.group] * size;
        if (L > cap) {
            total = {total.x * cap / L, total.y * cap / L, total.z * cap / L};
            L = cap;
        }
        push[i] = total;
        pushOn[i] = true;
        touched[i] = L >= 1e-4f;
    }
    if (g_cfg.couplePushes) CouplePushes(push, pushOn, touched, size);
    for (int i = 0; i < kNumSoft; ++i) {
        if (!pushOn[i]) continue;
        const Vec3 total = push[i];
        const float L = std::sqrt(Dot(total, total));
        // into the parent's frame; always handed on, so a push from last frame is taken off when it ends
        const float inv = 1.0f / parentXf[i].scale;
        const float local[3] = {Dot(parentXf[i].ax[0], total) * inv, Dot(parentXf[i].ax[1], total) * inv,
                                Dot(parentXf[i].ax[2], total) * inv};
        if (g_cfg.measureOnly) {
            const float none[3] = {0, 0, 0};
            v.AddTo(b.node[SoftN(i)], none);
        } else {
            v.AddTo(b.node[SoftN(i)], local);
        }
        if (L < 1e-4f) continue;
        g_pushes.fetch_add(1, std::memory_order_relaxed);
        const std::uint32_t mm = static_cast<std::uint32_t>(L * 1000);
        std::uint32_t prev = g_maxPushMm.load(std::memory_order_relaxed);
        while (mm > prev && !g_maxPushMm.compare_exchange_weak(prev, mm)) {}
    }
    if (snap) Log("  contacts (distance/contact cm):%s", cl ? contacts : " none");

    // share this body's shapes for the bodies near it: soft bones where they are this frame (gravity in, no pushes)
    if (g_cfg.others) {
        OtherShape mine[kOtherShapes];
        int n = 0;
        LimbShapes(caps, mine, n);
        TorsoShapes(b, caps, size, mine, n);
        for (int i = 0; i < kNumSoft; ++i)
            if (have[i]) mine[n++] = {pos[i], pos[i], kSoft[i].radius * g_cfg.softScale * size, kOtherSoft, 0, 0};
        if (!v.dead || g_cfg.deadBodies) Share(v.com, v.actor, comXf.pos, kBodyReach * size, mine, n, true, v.site == 2, v.dead);
    }
}

// actorHint: the caller's character, copied into r8 by the thunk (kSiteActor); null on path A or if unchecked
template <int Site>
static void Hooked(void* owner, void* referenceNode, void* actorHint) {
    MaybeReloadTuning();   // every 2 s, the tuning files' times and sizes
    if constexpr (!kDiag) {   // release: the work only
        ActorView view;
        const bool ours = Collect(owner, view);
        view.actor = actorHint;
        if constexpr (kCollisions) {   // release with collisions: dead or alive, as the diagnostic path reads it
            if (ours && g_cfg.collision) view.dead = DeadLifeState(LifeStateOf(actorHint));
        }
        if (ours) {
            TakeFactors(view, BodyFactorsOf(owner, actorHint));
            PreUpdate(view);
        }
        g_original(owner, referenceNode);   // the game's own update
        if (ours) PostUpdate(view, Site);
        else if constexpr (kCollisions) ShareOtherBody(owner, actorHint, Site == 2);   // lends its shapes only
        return;
    }
    LARGE_INTEGER t0, ta, t1, t2, tb, t3;
    QueryPerformanceCounter(&t0);
    ActorView view;
    const bool ours = Collect(owner, view);
    view.actor = actorHint;
    // dead or alive, when collisions are on; here only for an A-Body body (other bodies read it in
    // ShareOtherBody, only when they share)
    if (ours && kCollisions && g_cfg.collision) {
        const int life = LifeStateOf(actorHint);
        view.dead = DeadLifeState(life);
        if (view.dead) NoteDeadBody(true, life);
    }
    QueryPerformanceCounter(&ta);
    if (ours) TakeFactors(view, BodyFactorsOf(owner, actorHint));
    if (ours) PreUpdate(view);
    QueryPerformanceCounter(&t1);
    g_original(owner, referenceNode);   // the game's own update
    QueryPerformanceCounter(&t2);
    g_calls[Site].fetch_add(1, std::memory_order_relaxed);
    if (ours) PostUpdate(view, Site);
    else ShareOtherBody(owner, actorHint, Site == 2);
    if (ours) WatchProxies(view, Site);   // read only   // not A-Body: it only lends its shapes to A-Body bodies near it
    QueryPerformanceCounter(&tb);
    MaybeLog();
    QueryPerformanceCounter(&t3);
    g_workTicks.fetch_add(static_cast<std::uint64_t>((t1.QuadPart - t0.QuadPart) + (t3.QuadPart - t2.QuadPart)),
                          std::memory_order_relaxed);
    g_workCalls.fetch_add(1, std::memory_order_relaxed);
    // each part (A-Body bodies / others): the body read, before the game's update, after it, the log
    const int who = ours ? 0 : 1;
    g_partCalls[who].fetch_add(1, std::memory_order_relaxed);
    g_partTicks[who][0].fetch_add(static_cast<std::uint64_t>(ta.QuadPart - t0.QuadPart), std::memory_order_relaxed);
    g_partTicks[who][1].fetch_add(static_cast<std::uint64_t>(t1.QuadPart - ta.QuadPart), std::memory_order_relaxed);
    g_partTicks[who][2].fetch_add(static_cast<std::uint64_t>(tb.QuadPart - t2.QuadPart), std::memory_order_relaxed);
    g_partTicks[who][3].fetch_add(static_cast<std::uint64_t>(t3.QuadPart - tb.QuadPart), std::memory_order_relaxed);
}

// ------------------------------------------------------------------ Address Library
// The database for game `runtime` (SFSE's packed version: major << 24, minor << 16, build << 4), at the path SFSE
// itself checks before it loads the plugin
static void AddressLibraryPath(std::uint32_t runtime, char* out, size_t cap) {
    snprintf(out, cap, "Data\\SFSE\\Plugins\\versionlib-%u-%u-%u-0.bin", runtime >> 24, (runtime >> 16) & 0xFF,
             (runtime >> 4) & 0xFFF);
}

// The RVAs of `n` IDs from the database at `path`; false, with the reason in `why`, if it is missing, isn't format 5,
// is for another game version, is cut short, or lacks an ID (past its end, or no address in this version).
static bool ReadAddressLibrary(const char* path, std::uint32_t runtime, const std::uint32_t* ids, int n,
                               std::uint32_t* rvas, char* why, size_t whyCap) {
    FILE* f = _fsopen(path, "rb", _SH_DENYNO);
    if (!f) {
        snprintf(why, whyCap, "%s not found", path);
        return false;
    }
    struct Header {
        std::int32_t format;
        std::uint32_t version[4];   // major, minor, build, 0
        char name[64];              // "Starfield.exe"
        std::uint32_t pointerSize, reserved, count;
    } h = {};
    static_assert(sizeof(Header) == 0x60, "format 5 header");
    bool ok = fread(&h, sizeof(h), 1, f) == 1;
    long long size = -1;
    if (_fseeki64(f, 0, SEEK_END) == 0) size = _ftelli64(f);
    const std::uint32_t major = runtime >> 24, minor = (runtime >> 16) & 0xFF, build = (runtime >> 4) & 0xFFF;
    if (!ok || h.format != 5 || h.pointerSize != 8) {
        snprintf(why, whyCap, "%s is not an Address Library database of format 5", path);
        ok = false;
    } else if (h.version[0] != major || h.version[1] != minor || h.version[2] != build) {
        snprintf(why, whyCap, "%s is for game %u.%u.%u, not %u.%u.%u", path, h.version[0], h.version[1], h.version[2],
                 major, minor, build);
        ok = false;
    } else if (size < 0x60 + 4LL * h.count) {
        snprintf(why, whyCap, "%s is cut short (%lld bytes for %u IDs)", path, size, h.count);
        ok = false;
    }
    for (int i = 0; ok && i < n; ++i) {
        rvas[i] = 0;
        if (ids[i] >= h.count) {
            snprintf(why, whyCap, "ID %u is not in %s (it has %u)", ids[i], path, h.count);
            ok = false;
        } else if (_fseeki64(f, 0x60 + 4LL * ids[i], SEEK_SET) != 0 || fread(&rvas[i], 4, 1, f) != 1) {
            snprintf(why, whyCap, "%s could not be read at ID %u", path, ids[i]);
            ok = false;
        } else if (!rvas[i]) {
            snprintf(why, whyCap, "ID %u has no address in %s", ids[i], path);
            ok = false;
        }
    }
    fclose(f);
    return ok;
}

// Every game address (g_addr) from the database at `path` for game `runtime`, each inside the exe's image (`imageSize`
// bytes from g_base).  False, logged, if any can't be had; g_addr is then left empty and nothing is patched.
static bool ResolveAddresses(const char* path, std::uint32_t runtime, std::uint64_t imageSize) {
    std::uint32_t ids[kNumGameAddrs], rvas[kNumGameAddrs];
    for (int i = 0; i < kNumGameAddrs; ++i) ids[i] = kAddrRef[i].id;
    char why[2 * MAX_PATH];
    if (!ReadAddressLibrary(path, runtime, ids, kNumGameAddrs, rvas, why, sizeof(why))) {
        Log("Address Library: %s; nothing patched (install or update Address Library for SFSE Plugins)", why);
        return false;
    }
    for (int i = 0; i < kNumGameAddrs; ++i) {
        if (static_cast<std::uint64_t>(rvas[i]) + kAddrRef[i].off >= imageSize) {
            Log("Address Library: ID %u (%s) is outside the game (RVA 0x%X); nothing patched", kAddrRef[i].id,
                kAddrRef[i].what, rvas[i]);
            return false;
        }
    }
    for (int i = 0; i < kNumGameAddrs; ++i) g_addr[i] = g_base + rvas[i] + kAddrRef[i].off;
    Log("Address Library: all %d game addresses resolved through %s (modifier loop at RVA 0x%X)", kNumGameAddrs, path,
        rvas[kModifierUpdate]);
    return true;
}

static bool RttiNameIs(std::uintptr_t vt, const char* name);   // below
static bool GameBytesAre(std::uintptr_t at, const std::uint8_t* bytes, size_t n);
static std::uintptr_t VtSlot(std::uintptr_t vt, int i);
// the proxy watch's optional addresses; any failure switches only the watch off
static void ResolveWorldAddresses(const char* path, std::uint32_t runtime, std::uint64_t imageSize) {
    std::uint32_t ids[kNumWorldAddrs], rvas[kNumWorldAddrs];
    for (int i = 0; i < kNumWorldAddrs; ++i) ids[i] = kWorldAddrRef[i].id;
    char why[2 * MAX_PATH];
    g_proxyOk = g_proxyWriteOk = g_proxyMotionOk = false;
    if (!ReadAddressLibrary(path, runtime, ids, kNumWorldAddrs, rvas, why, sizeof(why))) {
        Log("world proxies: %s; the proxy watch is off", why);
        return;
    }
    for (int i = 0; i < kNumWorldAddrs; ++i) {
        if (static_cast<std::uint64_t>(rvas[i]) >= imageSize) {
            Log("world proxies: ID %u (%s) is outside the game; the proxy watch is off", kWorldAddrRef[i].id, kWorldAddrRef[i].what);
            return;
        }
        g_waddr[i] = g_base + rvas[i] + kWorldAddrRef[i].off;
    }
    const bool co = RttiNameIs(g_waddr[kNpCollisionObjectVtbl], ".?AVbhkNPCollisionObject@@");
    const bool world = RttiNameIs(g_waddr[kBSWorldVtbl], ".?AVhknpBSWorld@@");
    g_proxyOk = co && world;
    Log(g_proxyOk ? "world proxies: classes checked (bhkNPCollisionObject, hknpBSWorld); the watch reads node -> collision "
                    "object -> body -> world each update, read only"
                  : "world proxies: classes NOT confirmed (collision object %d, world %d); the proxy watch is off", co, world);
    // the two functions written through: their first bytes, and the setter must be the vtable's slot 63
    static const std::uint8_t kSetFilter[16] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57,
                                                0x48, 0x83, 0xEC, 0x50};
    static const std::uint8_t kKeyframe[16] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x18, 0x48,
                                               0x89, 0x78, 0x20, 0x48};
    const bool setter = GameBytesAre(g_waddr[kSetFilterFn], kSetFilter, 16) &&
                        VtSlot(g_waddr[kNpCollisionObjectVtbl], 63) == g_waddr[kSetFilterFn];
    const bool keyframe = GameBytesAre(g_waddr[kKeyframeFn], kKeyframe, 16);
    g_proxyWriteOk = g_proxyOk && setter && keyframe;
    // the motion and motion-properties tables (read only, for the speed limits), in the world's vtable at
    // +0x20: its class, slot 6 = `mov rax, [rcx+0xA38]; ret` (the table at world+0xA58), and slot 26 reading the motions
    // at [this+0x168] (world+0x188), 0x80 bytes each (`mov rax, [rdi+0x168]; mov rcx, rbx; shl rcx, 7`)
    static const std::uint8_t kPropsGetter[8] = {0x48, 0x8B, 0x81, 0x38, 0x0A, 0x00, 0x00, 0xC3};
    static const std::uint8_t kMotionRead[14] = {0x48, 0x8B, 0x87, 0x68, 0x01, 0x00, 0x00, 0x48, 0x8B, 0xCB, 0x48, 0xC1,
                                                 0xE1, 0x07};
    const std::uintptr_t vt20 = g_waddr[kBSWorldVtbl20], motionGetter = VtSlot(vt20, 26);
    g_proxyMotionOk = g_proxyOk && RttiNameIs(vt20, ".?AVhknpBSWorld@@") && GameBytesAre(VtSlot(vt20, 6), kPropsGetter, 8) &&
                      motionGetter && GameBytesAre(motionGetter + 0xCC, kMotionRead, 14);
    if (g_cfg.steer)
        Log(g_proxyMotionOk ? "world proxies: the motion and motion-properties tables checked; the proxies' speed limits are "
                              "read, and a proxy needing more than %.1f m/s (or its own limit) is warped"
                            : "world proxies: the motion tables NOT confirmed; the speed limits aren't read, a proxy needing "
                              "more than %.1f m/s is warped", g_cfg.proxyMaxSpeed);
    if (g_cfg.steer) {
        static const char* const kTime[4] = {
            "the timer's whole time (leftover + frame, as the game's own keyframe calls)",
            "the substeps the update after each steer simulates (this frame's update until the checks show it's the next "
            "frame's)",
            "this update's substeps", "the next update's substeps (predicted from the leftover and this frame's time)"};
        Log("world proxies: the physics timer (bhkWorld+0x7CA..+0x803) is read at each steer; the speed is asked for %s "
            "([World] iSteerTime %d); a motion the game has made dynamic (a ragdoll) is never steered",
            kTime[g_cfg.steerTime], g_cfg.steerTime);
    }
    if (g_cfg.proxyLayer || g_cfg.steer) {
        if (g_proxyWriteOk)
            Log("world proxies: the filter setter (vtable slot 63) and keyframe-with-warp checked; layer 62%s for every "
                "A-Body body %s, the player's proxies steered %s", g_cfg.proxyInert ? " + collide with nothing" : "",
                g_cfg.proxyLayer ? "on" : "off", g_cfg.steer ? "on" : "off");
        else
            Log("world proxies: the filter setter %d / keyframe %d NOT confirmed; nothing is written", setter, keyframe);
    }
}

// the tuning's three reads (kTuneAddrRef), each confirmed by its function's own bytes; a failure turns only that
// read off (the character, outfit or planet factors)
static void ResolveTuneAddresses(const char* path, std::uint32_t runtime, std::uint64_t imageSize) {
    std::uint32_t ids[kNumTuneAddrs], rvas[kNumTuneAddrs];
    for (int i = 0; i < kNumTuneAddrs; ++i) ids[i] = kTuneAddrRef[i].id;
    char why[2 * MAX_PATH];
    g_gravityScaleOk = g_formIdOk = g_wornOk = false;
    if (!ReadAddressLibrary(path, runtime, ids, kNumTuneAddrs, rvas, why, sizeof(why))) {
        Log("tuning: %s; the character, outfit and planet factors are off", why);
        return;
    }
    std::uintptr_t at[kNumTuneAddrs];
    for (int i = 0; i < kNumTuneAddrs; ++i) {
        if (static_cast<std::uint64_t>(rvas[i]) >= imageSize) {
            Log("tuning: ID %u (%s) is outside the game; the character, outfit and planet factors are off", kTuneAddrRef[i].id,
                kTuneAddrRef[i].what);
            return;
        }
        at[i] = g_base + rvas[i] + kTuneAddrRef[i].off;
    }
    static const std::uint8_t kGravity[6] = {0x8B, 0x88, 0x38, 0x11, 0x00, 0x00};   // +0x1B: mov ecx, [rax+0x1138]
    static const std::uint8_t kFormIdRead[5] = {0x41, 0x8B, 0x40, 0x28, 0xC3};        // mov eax, [r8+0x28]; ret
    struct Piece { int off; std::uint8_t b[8]; size_t n; };
    static const Piece kWorn[] = {
        {0x26, {0x80, 0x78, 0x2E, 0x4B}, 4},                     // cmp byte [rax+0x2E], 0x4B: a character
        {0x48, {0x48, 0x8B, 0x87, 0xA0, 0x00, 0x00, 0x00}, 7},   // mov rax, [rdi+0xA0]: its inventory
        {0x54, {0x8B, 0x48, 0x28}, 3},                           // mov ecx, [rax+0x28]: the count
        {0x5B, {0x48, 0x8B, 0x40, 0x30}, 4},                     // mov rax, [rax+0x30]: the entries
        {0x5F, {0x48, 0x8D, 0x14, 0x89}, 4},                     // lea rdx, [rcx+rcx*4] ...
        {0x63, {0x4C, 0x8D, 0x0C, 0xD0}, 4},                     // lea r9, [rax+rdx*8]: 0x28 bytes each
        {0x74, {0xF6, 0x40, 0x20, 0x07}, 4},                     // test byte [rax+0x20], 7: worn
        {0x82, {0x41, 0x80, 0x78, 0x2E, 0x22}, 5},               // cmp byte [r8+0x2E], 0x22: an armour
        {0x9A, {0x49, 0x85, 0x90, 0x08, 0x02, 0x00, 0x00}, 7},   // test [r8+0x208], rdx: its biped slots
    };
    g_gravityScaleOk = GameBytesAre(at[kGravityScaleFn] + 0x1B, kGravity, sizeof(kGravity));
    g_formIdOk = GameBytesAre(at[kGetFormIdFn], kFormIdRead, sizeof(kFormIdRead));
    bool worn = true;
    for (const Piece& p : kWorn) worn = worn && GameBytesAre(at[kWornSlotFn] + p.off, p.b, p.n);
    g_wornOk = worn && g_formIdOk;   // a worn piece is told apart by its form ID too
}

// The checks read the game at addresses from the database, so every read is guarded: a wrong address fails the check.
static bool GameBytesAre(std::uintptr_t at, const std::uint8_t* bytes, size_t n) {
    std::uint8_t buf[16];
    return at && n <= sizeof(buf) && SafeCopy(buf, reinterpret_cast<const void*>(at), n) && !std::memcmp(buf, bytes, n);
}
static std::uintptr_t VtSlot(std::uintptr_t vt, int i) {   // a vtable's entry i, or 0
    std::uintptr_t p = 0;
    return vt && SafeCopy(&p, reinterpret_cast<const void*>(vt + 8 * i), 8) ? p : 0;
}
// Where a rip-relative operand (a call, a load, a lea) points: its 4 bytes at `disp`, counted from `next`, the end of
// the instruction; 0 if unreadable.  Checked against the address found by ID: the operand's bytes change whenever the
// code or what it points at moves, so they are not compared themselves.
static std::uintptr_t RelTarget(std::uintptr_t disp, std::uintptr_t next) {
    std::int32_t rel = 0;
    return disp && SafeCopy(&rel, reinterpret_cast<const void*>(disp), 4) ? next + rel : 0;
}

// ------------------------------------------------------------------ patching
static void* AllocNear(std::uintptr_t target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;
    for (std::uintptr_t delta = gran; delta < 0x7FF00000; delta += gran) {
        for (int dir = -1; dir <= 1; dir += 2) {
            const std::uintptr_t addr = (dir < 0) ? (target > delta ? target - delta : 0) : target + delta;
            if (!addr) continue;
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(addr & ~(gran - 1)), size, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_EXECUTE_READWRITE))
                return p;
        }
    }
    return nullptr;
}

// Where each call site keeps the character whose 3D root it passes (read from the exe).  Path C hands rsi to
// get the 3D (kGet3D) and path B makes a virtual get-3D call on r12; both registers are callee-saved and untouched
// up to the call, so they still hold the character when the hook runs.  The thunk copies that register into r8, a third
// argument the game's function doesn't take (it reads rcx and rdx only; r8 is free at the call).  The block ends right
// at the call site; callAt: a call in it (E8) whose operand is checked by where it lands, kGet3D (0: none).
struct SiteActor { std::uint8_t block[16]; size_t blockLen, callAt; std::uint8_t save[3]; };
static const SiteActor kSiteActor[3] = {
    {{}, 0, 0, {0x45, 0x31, 0xC0}},   // A: never seen firing: no character (xor r8d, r8d)
    {{0x49, 0x8B, 0xCC, 0xFF, 0x90, 0x58, 0x05, 0x00, 0x00, 0x90, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0x08}, 16, 0,
     {0x4D, 0x89, 0xE0}},             // B: mov rcx,r12; call [rax+0x558]; nop; mov rdx,rbx; mov rcx,[rax] -> mov r8, r12
    {{0x48, 0x8B, 0xCE, 0xE8, 0, 0, 0, 0, 0x90, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0x08}, 15, 3,
     {0x49, 0x89, 0xF0}},             // C: mov rcx,rsi; call get-3D; nop; mov rdx,rbx; mov rcx,[rax] -> mov r8, rsi
};
static bool g_siteActorOk[3] = {};

static bool SiteBlockIs(std::uintptr_t at, const SiteActor& sa) {
    std::uint8_t buf[16];
    if (!sa.blockLen || !SafeCopy(buf, reinterpret_cast<const void*>(at), sa.blockLen)) return false;
    if (sa.callAt) {
        if (RelTarget(at + sa.callAt + 1, at + sa.callAt + 5) != g_addr[kGet3D]) return false;
        std::memset(buf + sa.callAt + 1, 0, 4);
    }
    return !std::memcmp(buf, sa.block, sa.blockLen);
}

static bool CheckGame() {
    for (const FnCheck& c : kFnChecks) {
        if (!GameBytesAre(g_addr[c.at], c.bytes, sizeof(c.bytes))) {
            Log("game function %s (ID %u) has unexpected bytes; nothing patched", kAddrRef[c.at].what, kAddrRef[c.at].id);
            return false;
        }
    }
    if (VtSlot(g_addr[kPoseDeformerVtbl], 1) != g_addr[kPoseDeformerUpdate]) {
        Log("PoseDeformer vtable check failed; nothing patched");
        return false;
    }
    if (VtSlot(g_addr[kSpringBoneVtbl], 1) != g_addr[kSpringBoneUpdate]) {
        Log("SpringBone vtable check failed; nothing patched");
        return false;
    }
    if (!GameBytesAre(g_addr[kFrameTimeLoad], kFrameTimeLoadOp, 4) ||
        RelTarget(g_addr[kFrameTimeLoad] + 4, g_addr[kFrameTimeLoad] + 8) != g_addr[kFrameTime]) {
        Log("frame-time check failed; nothing patched");
        return false;
    }
    static const std::uint8_t kCall[1] = {0xE8};
    for (int i = 0; i < 3; ++i) {
        const std::uintptr_t site = g_addr[kCallSiteA + i];
        if (!GameBytesAre(site, kCall, 1) || RelTarget(site + 1, site + 5) != g_addr[kModifierUpdate]) {
            Log("call site %c has unexpected bytes; nothing patched (the game's code changed, or another mod hooks it)",
                'A' + i);
            return false;
        }
        // the block ends right at the call: nothing between it and the call touches the character's register
        g_siteActorOk[i] = SiteBlockIs(site - kSiteActor[i].blockLen, kSiteActor[i]);
    }
    Log("character from the call: path B %s, path C %s", g_siteActorOk[1] ? "yes" : "no", g_siteActorOk[2] ? "yes" : "no");
    return true;
}

// The size reading uses classes and a record layout read from the exe; check them before trusting them.
static bool RttiNameIs(std::uintptr_t vt, const char* name) {
    std::uintptr_t col = 0;
    std::uint32_t sig = 0, tdRva = 0;
    char buf[64] = {};
    if (!vt || !SafeCopy(&col, reinterpret_cast<void*>(vt - 8), 8) || !SafeCopy(&sig, reinterpret_cast<void*>(col), 4) ||
        !SafeCopy(&tdRva, reinterpret_cast<void*>(col + 0x0C), 4) || sig != 1)
        return false;
    return SafeCopy(buf, reinterpret_cast<void*>(g_base + tdRva + 0x10), sizeof(buf) - 1) && !strcmp(buf, name);
}

static void CheckSizeReading() {
    static const std::uint8_t kLea448[7] = {0x48, 0x8D, 0xB9, 0x48, 0x04, 0x00, 0x00};    // lea rdi,[rcx+0x448]
    static const std::uint8_t kEntries[4] = {0x48, 0x8B, 0x43, 0x18};                      // mov rax,[rbx+0x18]
    static const std::uint8_t kCapacity[4] = {0x48, 0x8B, 0x53, 0x20};                     // mov rdx,[rbx+0x20]
    static const std::uint8_t kWeight[7] = {0xC4, 0x81, 0x7A, 0x10, 0x44, 0xCA, 0x08};     // vmovss xmm0,[r10+r9*8+8]
    const bool code = GameBytesAre(g_addr[kMorphLea448], kLea448, 7) && GameBytesAre(g_addr[kMorphEntries], kEntries, 4) &&
                      GameBytesAre(g_addr[kMorphCapacity], kCapacity, 4) && GameBytesAre(g_addr[kMorphWeight], kWeight, 7);
    const bool loader = VtSlot(g_addr[kNpcVtbl], 17) == g_addr[kNpcLoad];
    const bool classes = RttiNameIs(g_addr[kActorVtbl], ".?AVActor@@") &&
                         RttiNameIs(g_addr[kPlayerVtbl], ".?AVPlayerCharacter@@") &&
                         RttiNameIs(g_addr[kNpcVtbl], ".?AVTESNPC@@");
    if (code && loader && classes) {
        g_actorVt = g_addr[kActorVtbl];
        g_playerVt = g_addr[kPlayerVtbl];
        g_npcVt = g_addr[kNpcVtbl];
        Log("size: reading checked (morph weights at record +0x448, classes Actor, PlayerCharacter, TESNPC)");
    } else {
        Log("size: reading NOT available (code %d, loader %d, classes %d); breast gravity keeps its size", code, loader, classes);
    }
    // the body triangle: the morph-weight builder (kTriWeightLoad) reads record +0x350 + 4 i for the names in
    // its table (kTriNameTable: Thin, Strong, Overweight)
    static const std::uint8_t kTriLoad[10] = {0xC4, 0xA1, 0x7A, 0x10, 0xB4, 0x3E, 0x50, 0x03, 0x00, 0x00};   // vmovss xmm6,[rsi+r15+0x350]
    static const std::uint8_t kTriNames[3] = {0x4C, 0x8D, 0x35};   // lea r14,[rip+..] -> the name table
    static const char* const kTriName[3] = {"Thin", "Strong", "Overweight"};
    bool triCode = GameBytesAre(g_addr[kTriWeightLoad], kTriLoad, 10) && GameBytesAre(g_addr[kTriNamesLea], kTriNames, 3) &&
                   RelTarget(g_addr[kTriNamesLea] + 3, g_addr[kTriNamesLea] + 7) == g_addr[kTriNameTable];
    for (int i = 0; i < 3 && triCode; ++i) {
        const char* name = nullptr;
        char buf[16] = {};
        triCode = SafeCopy(&name, reinterpret_cast<void*>(g_addr[kTriNameTable] + 8 * i), 8) && name &&
                  SafeCopy(buf, name, strlen(kTriName[i]) + 1) && !strcmp(buf, kTriName[i]);
    }
    g_triangleOk = triCode && g_npcVt;
    Log(g_triangleOk ? "body shape: triangle reading checked (record +0x350 Thin, +0x354 Strong, +0x358 Overweight)"
                     : "body shape: triangle reading NOT available (code %d, record %d); only the part sliders count",
        triCode, g_npcVt != 0);
}

// the life state, checked through the classes' own vtables (no address of its own): slot 269 of Actor and of
// PlayerCharacter must be one function that starts mov r8d,[rcx+0xF8]; mov eax,r8d; and eax,0x1E0000.
static void CheckDeadReading() {
    static const std::uint8_t kIsDead[15] = {0x44, 0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00, 0x41, 0x8B, 0xC0, 0x25, 0x00, 0x00, 0x1E, 0x00};
    bool ok = false;
    if (g_actorVt && g_playerVt) {
        const std::uintptr_t fa = reinterpret_cast<const std::uintptr_t*>(g_actorVt)[kIsDeadSlot];
        const std::uintptr_t fp = reinterpret_cast<const std::uintptr_t*>(g_playerVt)[kIsDeadSlot];
        std::uint8_t code[15] = {};
        ok = fa && fa == fp && SafeCopy(code, reinterpret_cast<void*>(fa), 15) && !std::memcmp(code, kIsDead, 15);
    }
    g_deadOk = ok;
    Log(ok ? "dead bodies: life state checked (character +0xF8, the game's IsDead)"
           : "dead bodies: life state NOT available; dead bodies are treated as living");
}

// A call site's thunk (32 bytes): r8 = the caller's character (or null), then jump to the handler.
static void WriteThunk(std::uint8_t* t, int site, void* handler) {
    static const std::uint8_t kNoActor[3] = {0x45, 0x31, 0xC0};   // xor r8d, r8d
    std::memcpy(t, g_siteActorOk[site] ? kSiteActor[site].save : kNoActor, 3);
    const std::uint8_t jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};   // jmp [rip+0]; dq handler
    std::memcpy(t + 3, jmp, 6);
    std::memcpy(t + 9, &handler, 8);
}

static bool InstallHooks() {
    // one page for the three thunks, near the middle call site; every call must reach its thunk with a 32-bit offset
    auto* thunks = static_cast<std::uint8_t*>(AllocNear(g_addr[kCallSiteB], 0x100));
    if (!thunks) {
        Log("could not allocate a thunk page near the game; nothing patched");
        return false;
    }
    std::int32_t rels[3];
    for (int i = 0; i < 3; ++i) {
        const std::int64_t d = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(thunks + 32 * i)) -
                               static_cast<std::int64_t>(g_addr[kCallSiteA + i] + 5);
        if (d < INT32_MIN || d > INT32_MAX) {
            Log("thunk page out of reach of call site %c; nothing patched", 'A' + i);
            VirtualFree(thunks, 0, MEM_RELEASE);
            return false;
        }
        rels[i] = static_cast<std::int32_t>(d);
    }
    g_original = reinterpret_cast<ModifierUpdate>(g_addr[kModifierUpdate]);
    g_getExtra = reinterpret_cast<GetModifierExtraFn>(g_addr[kGetModifierExtra]);
    g_rigFind = reinterpret_cast<RigFindFn>(g_addr[kRigFind]);
    g_rigRelease = reinterpret_cast<RigReleaseFn>(g_addr[kRigRelease]);
    void* handlers[3] = {reinterpret_cast<void*>(&Hooked<0>), reinterpret_cast<void*>(&Hooked<1>),
                         reinterpret_cast<void*>(&Hooked<2>)};
    for (int i = 0; i < 3; ++i) WriteThunk(thunks + 32 * i, i, handlers[i]);
    DWORD was;
    VirtualProtect(thunks, 0x100, PAGE_EXECUTE_READ, &was);   // written: no longer writable
    FlushInstructionCache(GetCurrentProcess(), thunks, 0x100);
    int patched = 0;
    for (int i = 0; i < 3; ++i) {
        auto* p = reinterpret_cast<std::uint8_t*>(g_addr[kCallSiteA + i]);
        DWORD old;
        if (!VirtualProtect(p + 1, 4, PAGE_EXECUTE_READWRITE, &old)) {
            Log("could not unprotect call site %c; it stays unpatched", 'A' + i);
            continue;
        }
        std::memcpy(p + 1, &rels[i], 4);
        VirtualProtect(p + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        ++patched;
    }
    return patched > 0;
}

// ------------------------------------------------------------------ entry point
static void OpenLog() {
    PWSTR docs = nullptr;
    char path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        WideCharToMultiByte(CP_ACP, 0, docs, -1, path, MAX_PATH, nullptr, nullptr);
        CoTaskMemFree(docs);
    }
    snprintf(g_logPath, sizeof(g_logPath), "%s\\My Games\\Starfield\\SFSE\\Logs\\TFPhysics.log", path);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);   // fresh log each run
}

extern "C" __declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse) {
    if constexpr (kDiag) OpenLog();   // release: no log file at all
    g_startMs = GetTickCount64();
    g_mainThread = GetCurrentThreadId();
    g_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    LoadSettings();
    Log("TF Physics (logging build: gravity, springs, body shape, tuning files, collisions, world-collision "
        "proxies). "
        "SFSE 0x%08X, game %u.%u.%u (runtime 0x%08X)", sfse->sfseVersion, sfse->runtimeVersion >> 24,
        (sfse->runtimeVersion >> 16) & 0xFF, (sfse->runtimeVersion >> 4) & 0xFFF, sfse->runtimeVersion);
    Log("lying also needs the thighs flat: %.0f-%.0f deg from vertical", g_cfg.thighFlatStartDeg, g_cfg.thighFlatFullDeg);
    Log("settings from %s: enabled %d, hang %.2f cm, press %.2f cm, dead zone %.0f deg, lying %.0f-%.0f deg hip tilt, "
        "slump %.2f, fixed frame rate %d, spring gravity %.3f g, a paused game's soft parts at rest %d",
        g_iniPath, g_cfg.enabled, g_cfg.hangCm, g_cfg.pressCm, g_cfg.deadZoneDeg, g_cfg.lyingStartDeg, g_cfg.lyingFullDeg,
        g_cfg.slump, g_cfg.fixedFrameRate, g_cfg.springGravity, g_cfg.settlePaused);
    char gravityLevel[96] = "as installed";
    if (g_cfg.gravityLevel > 0.0f)
        snprintf(gravityLevel, sizeof(gravityLevel), "x%.2f for every body (its own largest level; smaller or thinner "
                 "breasts less)", g_cfg.gravityLevel);
    Log("shapes: side %.2f, face down %.2f, bent forward %.2f; breast gravity level %s; their largest levels: back x%.2f, "
        "side x%.2f, bent forward x%.2f, face down x%.2f; spring gravity breast %.3f, belly %.3f, butt %.3f, thighs %.3f, "
        "hips %.3f, calves %.3f, arms %.3f, torso %.3f g; the other parts' gravity shapes x%.2f of their clean settle "
        "(butt %.2f, belly %.2f, thighs %.2f, hips %.2f, calves %.2f, arms %.2f, torso %.2f x 85%% of the Max + Max "
        "travel); size scaling %d (large %.2f, small %.2f, override %.2f, level x size at most %.2f); snapshots %d",
        g_cfg.sideSlump, g_cfg.faceDownSquash, g_cfg.hangShape, gravityLevel, g_cfg.maxBack, g_cfg.maxSide, g_cfg.maxHang,
        g_cfg.maxDown, g_cfg.springGravity, g_cfg.springGravityBelly, g_cfg.springGravityButt, g_cfg.springGravityThighs,
        g_cfg.springGravityHips, g_cfg.springGravityCalves, g_cfg.springGravityArms, g_cfg.springGravityTorso,
        g_cfg.gravityShape, kPartSettleLimit[kPartButt], kPartSettleLimit[kPartBelly], kPartSettleLimit[kPartThigh],
        kPartSettleLimit[kPartHip], kPartSettleLimit[kPartCalf], kPartSettleLimit[kPartArm], kPartSettleLimit[kPartTorso],
        g_cfg.sizeScaling, g_cfg.sizeLarge, g_cfg.sizeSmall, g_cfg.sizeOverride, g_cfg.maxLevel, g_cfg.snapshots);
    Log("collisions %d (measure only %d): soft size x%.2f, largest push breast %.1f cm, belly %.1f cm, inner thigh %.1f cm, "
        "glute %.1f cm; inner thighs %d, glutes %d; other bodies %d (measure only %d); a part's bones push as one %d (no "
        "two differ by more than: breast %.1f cm, belly %.1f cm, inner thigh %.1f cm, glute %.1f cm)",
        g_cfg.collision, g_cfg.measureOnly, g_cfg.softScale, g_cfg.maxPush[0] * 100, g_cfg.maxPush[1] * 100, g_cfg.maxPush[2] * 100,
        g_cfg.maxPush[3] * 100, g_cfg.innerThighs, g_cfg.glutes, g_cfg.others, g_cfg.othersMeasureOnly, g_cfg.couplePushes,
        kGroupPushSpread[0] * 100, kGroupPushSpread[1] * 100, kGroupPushSpread[2] * 100, kGroupPushSpread[3] * 100);
    Log("other bodies push at most %.1f cm; dead bodies %d (a dead body lying still: its shapes used up to %.0f ms old)",
        g_cfg.maxPushOther * 100, g_cfg.deadBodies, g_cfg.deadMaxAge * 1000);
    Log("body shape %d: jiggle x(1 %+.2f thin %+.2f muscular %+.2f heavy), gravity x(1 %+.2f thin %+.2f muscular %+.2f "
        "heavy), belly heavy x%.2f; parts: butt %.2f, hips %.2f, thighs %.2f, calves %.2f, arms %.2f, waist %.2f; damping "
        "follows %.2f; each factor %.2f to %.2f; override jiggle %.2f, gravity %.2f; breast stretch guard %d", g_cfg.shape,
        g_cfg.jThin,
        g_cfg.jMuscular, g_cfg.jHeavy, g_cfg.gThin, g_cfg.gMuscular, g_cfg.gHeavy, g_cfg.bellyHeavy, g_cfg.partButt,
        g_cfg.partHips, g_cfg.partThighs, g_cfg.partCalves, g_cfg.partArms, g_cfg.partWaist, g_cfg.jiggleDamping,
        g_cfg.shapeMin, g_cfg.shapeMax, g_cfg.overrideJiggle, g_cfg.overrideGravity, g_cfg.stretchGuard);
    char lib[MAX_PATH];
    AddressLibraryPath(sfse->runtimeVersion, lib, sizeof(lib));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);   // SizeOfImage: the exe's span
    if (!ResolveAddresses(lib, sfse->runtimeVersion, nt->OptionalHeader.SizeOfImage)) return false;
    if (!CheckGame()) return false;
    CheckSizeReading();
    CheckDeadReading();
    // the tuning: its reads checked, then the files read (and watched)
    ResolveTuneAddresses(lib, sfse->runtimeVersion, nt->OptionalHeader.SizeOfImage);
    LoadTuning(g_tunes[0]);
    g_tuneNow.store(0);
    g_tuneStamp = TuneStamp();
    LogTuning("tuning", g_tunes[0]);
    if (kDiag && g_cfg.proxies) ResolveWorldAddresses(lib, sfse->runtimeVersion, nt->OptionalHeader.SizeOfImage);
    const bool ok = InstallHooks();
    Log(ok ? "hooks installed; logging every 5 s" : "hooks NOT installed");
    return ok;
}
