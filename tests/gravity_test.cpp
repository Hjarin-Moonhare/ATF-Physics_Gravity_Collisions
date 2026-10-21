// Offline test of the gravity code: runs the plugin's own PostUpdate on fake nodes (chest, both breast anchors
// and their four bones, COM) turned to known orientations, and compares the anchor offsets and per-bone adds with
// numbers worked out here from the design (A-Body's p3_gravity_shapes.py, p3_gravity_slump.py), not
// from the plugin's tables.  Also: spring gravity per part (PreUpdate) and the breast size reading (BodySize).
// Build and run: tests\run_tests.bat
#include "../src/plugin.cpp"

#include <string>
#include <vector>

static int g_fails = 0;

static char* NewNode(const char* name, std::vector<char*>& keep) {
    char* n = static_cast<char*>(_aligned_malloc(0x400, 16));
    memset(n, 0, 0x400);
    char* e = static_cast<char*>(calloc(1, 0x18 + 64));   // string pool entry, text at +0x18
    strcpy_s(e + 0x18, 64, name);
    memcpy(n + 0x10, &e, 8);
    const float one = 1.0f;
    memcpy(n + 0xBC, &one, 4);
    keep.push_back(n);
    return n;
}
static void SetRows(char* node, const Vec3 r[3]) {
    for (int k = 0; k < 3; ++k) memcpy(node + 0x80 + 16 * k, &r[k], 12);
}

struct Rig {
    std::vector<char*> keep;
    char* com;
    char* chest;
    char* pecs[2];
    char* bones[2][4];
    float rest[2][3] = {{0.02f, 0.10f, 0.08f}, {0.02f, 0.10f, -0.08f}};
    char* twist[2];          // L/R_Thigh_Twist: X axis along the thigh
    void* thighSprings[2];   // spring instances whose targets hang under them
};

static void Build(Rig& r) {
    r.com = NewNode("COM", r.keep);
    r.chest = NewNode("C_Chest", r.keep);
    const char* bn[4] = {"Upper", "Lower", "Inner", "Outer"};
    for (int s = 0; s < 2; ++s) {
        r.pecs[s] = NewNode(s ? "R_Pecs" : "L_Pecs", r.keep);
        memcpy(r.pecs[s] + 0x38, &r.chest, 8);
        memcpy(r.pecs[s] + 0x70, r.rest[s], 12);
        for (int b = 0; b < 4; ++b) {
            r.bones[s][b] = NewNode((std::string("TF_Breast_") + bn[b] + (s ? "_R" : "_L")).c_str(), r.keep);
            memcpy(r.bones[s][b] + 0x38, &r.pecs[s], 8);
        }
        r.twist[s] = NewNode(s ? "R_Thigh_Twist" : "L_Thigh_Twist", r.keep);
        char* inst = static_cast<char*>(calloc(1, 0x100));
        char* data = static_cast<char*>(calloc(1, 0x100));
        char* target = NewNode(s ? "TF_Thigh_Outer_R" : "TF_Thigh_Outer_L", r.keep);
        memcpy(target + 0x38, &r.twist[s], 8);
        memcpy(inst + 0x18, &target, 8);
        memcpy(inst + 0x20, &data, 8);
        r.thighSprings[s] = inst;
    }
}

// world rows of the chest (X up the body, Y forward, Z the character's left) and of COM (X right, Y forward, Z up)
struct Pose { const char* name; Vec3 chest[3]; Vec3 com[3]; };
static const Pose kStanding = {"standing", {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
static const Pose kOnBack = {"lying on the back", {{0, 1, 0}, {0, 0, 1}, {-1, 0, 0}}, {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}};
static const Pose kOnLeft = {"lying on the left side", {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}};
static const Pose kOnRight = {"lying on the right side", {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}}, {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
static const Pose kFaceDown = {"lying face down", {{0, 1, 0}, {0, 0, -1}, {1, 0, 0}}, {{-1, 0, 0}, {0, 0, -1}, {0, 1, 0}}};
static const Pose kBentOver = {"bent forward on the feet", {{0, 1, 0}, {0, 0, -1}, {1, 0, 0}}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};

struct Expect { Vec3 anchor[2]; Vec3 bone[2][4]; };   // cm at level 1: anchor in the chest frame, bones as added

static void Check(const char* what, Rig& r, const Pose& p, const Expect& e, float level, float size,
                  const Vec3* thighAxis = nullptr,   // with thigh springs, their thigh's X axis in the world
                  float kWant = -1,                  // the effective level expected (default level x size)
                  float ceil = 1.0f) {               // the body's ceiling scale (the stretch guard's)
    SetRows(r.chest, p.chest);
    SetRows(r.com, p.com);
    for (int s = 0; s < 2; ++s) memcpy(r.pecs[s] + 0x70, r.rest[s], 12);   // the game's PoseDeformers reset the anchors
    ActorView v;
    v.level = level;
    v.size = size;
    v.ceilScale = ceil;
    v.com = r.com;
    for (int s = 0; s < 2; ++s) {
        v.anchors[s].node = r.pecs[s];
        v.anchors[s].com = r.com;
        v.anchors[s].rest = r.rest[s];
        for (int b = 0; b < 4; ++b) v.anchors[s].bones[b] = r.bones[s][b];
        if (thighAxis) {
            memcpy(r.twist[s] + 0x80, thighAxis, 12);
            v.springs[v.nSprings++] = r.thighSprings[s];
        }
    }
    const bool coll = g_cfg.collision;
    g_cfg.collision = false;
    PostUpdate(v, 2);
    g_cfg.collision = coll;
    double worst = 0;
    const float k = kWant > 0 ? kWant : level * size;
    for (int s = 0; s < 2; ++s) {
        const float* t = reinterpret_cast<const float*>(r.pecs[s] + 0x70);
        const float want[3] = {r.rest[s][0] + e.anchor[s].x * k / 100, r.rest[s][1] + e.anchor[s].y * k / 100,
                               r.rest[s][2] + e.anchor[s].z * k / 100};
        for (int c = 0; c < 3; ++c) worst = std::fmax(worst, std::fabs(t[c] - want[c]) * 100);
        for (int b = 0; b < 4; ++b) {
            const float* a = v.AddFor(r.bones[s][b]);
            const float got[3] = {a ? a[0] : 0, a ? a[1] : 0, a ? a[2] : 0};
            const float wb[3] = {e.bone[s][b].x * k / 100, e.bone[s][b].y * k / 100, e.bone[s][b].z * k / 100};
            for (int c = 0; c < 3; ++c) worst = std::fmax(worst, std::fabs(got[c] - wb[c]) * 100);
        }
    }
    const bool ok = worst < 1e-4;
    printf("%-26s %-34s worst difference %.6f cm  %s\n", p.name, what, worst, ok ? "OK" : "FAIL");
    g_fails += !ok;
}

static Vec3 Mirror(const Vec3& a) { return {a.x, a.y, -a.z}; }

int main() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    for (int up = 0; up < 3; ++up)
        if (char* slash = strrchr(path, '\\')) *slash = 0;
    snprintf(g_logPath, sizeof(g_logPath), "%s\\build\\tests\\gravity_test.log", path);
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);

    Rig r;
    Build(r);
    // the design (cm, Real), written out again here: bones Upper, Lower, Inner, Outer, left breast, +Z outward
    const Vec3 slump[4] = {{-1.0f, -0.3f, 0.5f}, {-0.8f, 0, 1.0f}, {-0.5f, 0, 1.2f}, {-1.0f, 0, 2.0f}};   // S1
    const Vec3 below[4] = {{0, 0, 1.0f}, {-0.2f, 0, 1.4f}, {0, 0, 0.8f}, {0, 0, 2.2f}};                 // SD2 down
    const Vec3 above[4] = {{0, 0, -1.2f}, {0, 0, -1.6f}, {0, 0, -2.0f}, {0, 0, -0.8f}};                 // SD2 up
    const Vec3 squash[4] = {{-0.6f, -0.3f, 0}, {-0.8f, 0.3f, 0.3f}, {-0.4f, 0, 0}, {-0.6f, 0, 0.6f}};   // FD1d
    const Vec3 hang[4] = {{1.0f, 0, 0}, {2.0f, 0.2f, 0}, {1.2f, 0, 0.4f}, {1.2f, 0, -0.4f}};            // H2

    Expect none{};
    Check("nothing moves", r, kStanding, none, 2.0f, 1.0f);

    Expect back{};
    back.anchor[0] = {0.5f, -1.0f, 0.8f};
    back.anchor[1] = {0.5f, -1.0f, -0.8f};
    for (int b = 0; b < 4; ++b) back.bone[0][b] = slump[b], back.bone[1][b] = Mirror(slump[b]);
    Check("slump (S1)", r, kOnBack, back, 2.0f, 1.0f);

    Expect left{};   // gravity toward the character's left (+Z): the left breast is below, the right one above
    left.anchor[0] = {0, 0, 0.4f};
    left.anchor[1] = {0, 0, 0.9f};
    for (int b = 0; b < 4; ++b) left.bone[0][b] = below[b], left.bone[1][b] = Mirror(above[b]);
    Check("side shape, left below", r, kOnLeft, left, 1.0f, 1.0f);

    Expect right{};   // the reverse: the right breast below, the left one above
    right.anchor[0] = {0, 0, -0.9f};
    right.anchor[1] = {0, 0, -0.4f};
    for (int b = 0; b < 4; ++b) right.bone[0][b] = above[b], right.bone[1][b] = Mirror(below[b]);
    Check("side shape, right below", r, kOnRight, right, 1.0f, 1.0f);

    Expect down{};
    down.anchor[0] = down.anchor[1] = {0, -0.5f, 0};
    for (int b = 0; b < 4; ++b) down.bone[0][b] = squash[b], down.bone[1][b] = Mirror(squash[b]);
    Check("squash (FD1d) on the press", r, kFaceDown, down, 1.5f, 1.0f);

    Expect bent{};
    bent.anchor[0] = bent.anchor[1] = {0, 0.6f, 0};
    for (int b = 0; b < 4; ++b) bent.bone[0][b] = hang[b], bent.bone[1][b] = Mirror(hang[b]);
    Check("teardrop (H2), hang halved", r, kBentOver, bent, 1.0f, 1.0f);
    Check("teardrop, Real x size 1.5", r, kBentOver, bent, 1.0f, 1.5f);
    // above a shape's ceiling (installed x2 x size 1.5 = 3): each shape stops at its own measured level
    Check("x2 x size 1.5: slump held x2.0", r, kOnBack, back, 2.0f, 1.5f, nullptr, 2.0f);
    Check("x2 x size 1.5: side held x2.4", r, kOnLeft, left, 2.0f, 1.5f, nullptr, 2.4f);
    Check("x2 x size 1.5: teardrop held x2.3", r, kBentOver, bent, 2.0f, 1.5f, nullptr, 2.3f);
    Check("x2 x size 1.5: face down x3.0", r, kFaceDown, down, 2.0f, 1.5f, nullptr, 3.0f);
    Check("x2 x size 0.5 (Small): x1.0", r, kBentOver, bent, 2.0f, 0.5f);
    // the shapes' largest levels from the INI ([Gravity] fMaxBack .. fMaxDown, [Size] fMaxLevel up to 8), and
    // a tuned level for every body ([Breasts] fGravityLevel) in place of the installed one (what a normal or
    // bigger breast gets, its own largest level; a smaller breast gets it x its size)
    {
        const Settings keep = g_cfg;
        g_cfg.maxBack = g_cfg.maxSide = g_cfg.maxHang = g_cfg.maxDown = 6.0f;
        g_cfg.maxLevel = 6.0f;
        Check("largest levels x6: x2 x 1.5 = x3", r, kOnBack, back, 2.0f, 1.5f, nullptr, 3.0f);
        TuneSettings& tuned = g_tunes[g_tuneNow.load()];
        tuned = TuneSettings{};
        tuned.part[kPartBreast].gravLevel = 5.0f;
        Check("tuned level x5: slump x5", r, kOnBack, back, 1.0f, 1.0f, nullptr, 5.0f);
        Check("tuned level x5: side x5", r, kOnLeft, left, 1.0f, 1.0f, nullptr, 5.0f);
        Check("tuned x5, size 0.5: teardrop x2.5", r, kBentOver, bent, 1.0f, 0.5f, nullptr, 2.5f);
        Check("tuned x5, size 1.5: x5", r, kOnLeft, left, 1.0f, 1.5f, nullptr, 5.0f);
        g_cfg = keep;
        Check("tuned x5 over largest 2.0: x5", r, kOnBack, back, 1.0f, 1.0f, nullptr, 5.0f);
        tuned = TuneSettings{};
    }
    // [Gravity] fLevel, one level for every body in place of the installed one, a tuned level over it, and a
    // level per shape ([Breasts] fGravityBack .. fGravityDown) over both: each is what a normal or bigger breast gets
    // and its own largest level; a smaller breast gets it x its size, a thinner one x its ceiling scale (the lower)
    {
        const Settings keep = g_cfg;
        g_cfg.maxBack = g_cfg.maxSide = g_cfg.maxHang = g_cfg.maxDown = 2.0f;   // hold the installed level only
        g_cfg.maxLevel = 3.0f;
        g_cfg.gravityLevel = 2.5f;
        Check("fLevel x2.5 over installed x1", r, kOnBack, back, 1.0f, 1.0f, nullptr, 2.5f);
        Check("fLevel x2.5, size 1.5: x2.5", r, kOnLeft, left, 1.0f, 1.5f, nullptr, 2.5f);
        Check("fLevel x2.5, size 0.6: x1.5", r, kFaceDown, down, 1.0f, 0.6f, nullptr, 1.5f);
        Check("fLevel x2.5, ceiling x0.8: x2", r, kBentOver, bent, 1.0f, 1.5f, nullptr, 2.0f, 0.8f);
        Check("fLevel x2.5, size 0.5, ceiling 0.8", r, kOnBack, back, 1.0f, 0.5f, nullptr, 1.25f, 0.8f);
        g_cfg.gravityLevel = 6.25f;
        Check("fLevel x6.25 over largest 2.0", r, kOnRight, right, 1.0f, 1.25f, nullptr, 6.25f);
        g_cfg.gravityLevel = 2.5f;
        TuneSettings& tuned = g_tunes[g_tuneNow.load()];
        tuned = TuneSettings{};
        tuned.part[kPartBreast].gravLevel = 4.0f;
        Check("tuned x4 over fLevel x2.5", r, kBentOver, bent, 1.0f, 1.0f, nullptr, 4.0f);
        tuned.part[kPartBreast].gravPose[0] = 3.0f;   // back
        tuned.part[kPartBreast].gravPose[2] = 5.0f;   // bent forward
        Check("per shape: back x3", r, kOnBack, back, 1.0f, 1.0f, nullptr, 3.0f);
        Check("per shape: side keeps x4", r, kOnRight, right, 1.0f, 1.0f, nullptr, 4.0f);
        Check("per shape: bent forward x5", r, kBentOver, bent, 1.0f, 1.0f, nullptr, 5.0f);
        Check("per shape: face down keeps x4", r, kFaceDown, down, 1.0f, 1.0f, nullptr, 4.0f);
        Check("per shape x3 x size 0.5", r, kOnBack, back, 1.0f, 0.5f, nullptr, 1.5f);
        Check("per shape x5, size 1.25: x5", r, kBentOver, bent, 1.0f, 1.25f, nullptr, 5.0f);
        tuned = TuneSettings{};
        g_cfg.gravityLevel = 0.0f;
        Check("fLevel 0: the installed level", r, kOnBack, back, 2.0f, 1.0f, nullptr, 2.0f);
        tuned.part[kPartBreast].gravPose[1] = 4.0f;   // a side only, over the installed level
        Check("per shape x4, the rest installed", r, kOnLeft, left, 1.0f, 1.25f, nullptr, 4.0f);
        Check("... back: installed x1 x 1.25", r, kOnBack, back, 1.0f, 1.25f, nullptr, 1.25f);
        tuned = TuneSettings{};
        Check("installed x2 x 1.5: held x2.0", r, kOnBack, back, 2.0f, 1.5f, nullptr, 2.0f);
        g_cfg = keep;
    }

    const Vec3 thighDown = {0, 0, -1}, thighFlat = {0, 1, 0};
    Check("all fours: hang, not squash", r, kFaceDown, bent, 1.0f, 1.0f, &thighDown);
    Check("thighs flat: squash", r, kFaceDown, down, 1.5f, 1.0f, &thighFlat);

    g_cfg.sideSlump = 0;
    Expect slide{};   // shapes off: the whole-breast slide alone, nothing per bone
    slide.anchor[0] = {0, 0, 0.8f};
    slide.anchor[1] = {0, 0, 1.8f};
    Check("side shape off: slide only", r, kOnLeft, slide, 1.0f, 1.0f);
    g_cfg.sideSlump = 1;
    g_cfg.hangShape = 0;
    Expect hang12{};
    hang12.anchor[0] = hang12.anchor[1] = {0, 1.2f, 0};
    Check("teardrop off: hang 1.2", r, kBentOver, hang12, 1.0f, 1.0f);
    g_cfg.hangShape = 1;

    // ---- spring gravity per part (PreUpdate), lying on the back: g_now - g_standing = (0, -1, +1) in COM's frame
    {
        static float dt = 1.0f / 60;
        g_addr[kFrameTime] = reinterpret_cast<std::uintptr_t>(&dt);   // PreUpdate reads the frame time there
        SetRows(r.com, kOnBack.com);
        // the last spring is a stiff-limited breast: Strength 90, fore/aft MaxDist 0.45 -> the guard caps its pull at
        // 0.85 x 0.45 / 60 x 90 = 0.574 m/s2 (0.06 g x 1.2 x 9.81 = 0.706 would hold it at its travel limit)
        const char* names[6] = {"TF_Breast_Lower_L", "TF_Abdomen_L", "TF_Glute_Lower_L", "TF_Thigh_Outer_L", "C_Spine",
                                "TF_Breast_Upper_L"};
        const float fraction[6] = {0.06f * 1.2f, 0.03f, 0.04f, 0.0f, 0.0f, 0.06f * 1.2f};   // breast x size factor 1.2
        ActorView v;
        v.com = r.com;
        v.size = 1.2f;
        std::vector<char*> mem;
        for (int i = 0; i < 6; ++i) {
            char* inst = static_cast<char*>(calloc(1, 0x100));
            char* data = static_cast<char*>(calloc(1, 0x100));
            char* target = NewNode(names[i], r.keep);
            memcpy(inst + 0x18, &target, 8);
            memcpy(inst + 0x20, &data, 8);
            const float rate = 30.0f, caps[3] = {0.8f, 0.45f, 1.7f};
            const float stiff = i == 5 ? 90.0f : 1000.0f, strength[3] = {stiff, stiff, stiff};
            memcpy(inst + 0xD0, &rate, 4);
            memcpy(data + 0x50, caps, 12);
            memcpy(data + 0x20, strength, 12);
            v.springs[v.nSprings++] = inst;
            mem.push_back(inst);
        }
        PreUpdate(v);
        double worst = 0;
        for (int i = 0; i < 6; ++i) {
            const float* vel = reinterpret_cast<const float*>(mem[i] + 0xB0);
            const float G = 9.81f * fraction[i];
            const float fore = i == 5 ? 0.85f * 0.45f / 60.0f * 90.0f : G;   // the guard, on the stiff-limited breast
            const float want[3] = {0, fore * dt, -G * dt};   // vel -= rel x G x dt, rel = (0, -1, +1)
            for (int c = 0; c < 3; ++c) worst = std::fmax(worst, std::fabs(vel[c] - want[c]));
        }
        const bool ok = worst < 1e-6;
        printf("%-26s %-34s worst difference %.8f m/s  %s\n", "lying on the back", "spring gravity by part", worst,
               ok ? "OK" : "FAIL");
        g_fails += !ok;
    }

    // ---- the breast size reading: 3D root -> character -> record -> morph-target weights
    {
        static std::uintptr_t fakeActorVt = 0x1111, fakePlayerVt = 0x2222, fakeNpcVt = 0x3333;
        g_actorVt = reinterpret_cast<std::uintptr_t>(&fakeActorVt);
        g_playerVt = reinterpret_cast<std::uintptr_t>(&fakePlayerVt);
        g_npcVt = reinterpret_cast<std::uintptr_t>(&fakeNpcVt);
        char* root = static_cast<char*>(calloc(1, 0x400));
        char* actor = static_cast<char*>(calloc(1, 0x400));
        char* npc = static_cast<char*>(calloc(1, 0x600));
        char* table = static_cast<char*>(calloc(1, 0x40));
        char* entries = static_cast<char*>(calloc(8, 24));
        memcpy(root + 0x128, &actor, 8);
        memcpy(actor, &g_playerVt, 8);
        memcpy(actor + 0xE0, &npc, 8);
        memcpy(npc, &g_npcVt, 8);
        memcpy(npc + 0x448, &table, 8);
        memcpy(table + 0x18, &entries, 8);
        const std::uint64_t cap = 8;
        memcpy(table + 0x20, &cap, 8);
        for (int i = 0; i < 8; ++i) {
            const std::int32_t empty = -1;
            memcpy(entries + i * 24 + 0x10, &empty, 4);
        }
        auto put = [&](int i, const char* name, float w) {
            char* e = static_cast<char*>(calloc(1, 0x18 + 64));
            strcpy_s(e + 0x18, 64, name);
            memcpy(entries + i * 24, &e, 8);
            memcpy(entries + i * 24 + 8, &w, 4);
            const std::int32_t used = 0;
            memcpy(entries + i * 24 + 0x10, &used, 4);
        };
        put(1, "TF_BreastsLarge", 0.4f);
        put(4, "Thin", 0.7f);
        put(6, "TF_BreastsSmall", 0.1f);
        // the character comes from the game's call (the hint); the 3D root is not searched (it holds none)
        const float noHint = BodySize(root);
        char* root2 = static_cast<char*>(calloc(1, 0x400));   // a fresh cache slot for the same character
        const float got = BodySize(root2, actor), again = BodySize(root2, actor);
        const float want = 1.0f + 0.5f * 0.4f - 0.5f * 0.1f;
        const bool ok = noHint == 1.0f && std::fabs(got - want) < 1e-6 && got == again && g_npcOff.load() == 0xE0;
        printf("%-26s %-34s size x%.3f (expected x%.3f), record at +0x%X; without the character x%.2f  %s\n",
               "fake character", "breast size from sliders", got, want, g_npcOff.load(), noHint, ok ? "OK" : "FAIL");
        g_fails += !ok;
        char* stranger = static_cast<char*>(calloc(1, 0x400));   // a root with no character: factor 1, no crash
        const float none1 = BodySize(stranger);
        const bool ok2 = none1 == 1.0f;
        printf("%-26s %-34s size x%.3f  %s\n", "no character", "falls back to x1", none1, ok2 ? "OK" : "FAIL");
        g_fails += !ok2;
    }
    // ---- audit: the breast bones by name, read through the plugin's own Collect from a fake modifier list that also
    //      holds an outfit's spring bone under the breast anchor (which must not be taken for a breast bone)
    {
        static char* extra = static_cast<char*>(calloc(1, 0x100));
        struct Fake { static void* Get(void*) { return extra; } };
        g_getExtra = &Fake::Get;
        static std::uintptr_t fakeVt[2];   // the PoseDeformer and SpringBone vtables: any two addresses
        g_addr[kPoseDeformerVtbl] = reinterpret_cast<std::uintptr_t>(&fakeVt[0]);
        g_addr[kSpringBoneVtbl] = reinterpret_cast<std::uintptr_t>(&fakeVt[1]);
        std::vector<char*> insts;
        auto addInst = [&](std::uintptr_t vt, void* driver, void* target, void* data) {
            char* inst = static_cast<char*>(calloc(1, 0x100));
            memcpy(inst, &vt, 8);
            memcpy(inst + 0x10, &driver, 8);
            memcpy(inst + 0x18, &target, 8);
            memcpy(inst + 0x20, &data, 8);
            insts.push_back(inst);
            return inst;
        };
        // the face-up corrective on each anchor (TF_Gravity, x2): vector (0,0,1), axis 1, cone 0-90, flatten and spread
        for (int s = 0; s < 2; ++s) {
            char* pd = static_cast<char*>(calloc(1, 0x100));
            const float vec[3] = {0, 0, 1}, pos[3] = {0.008f * 2, -0.015f * 2, (s ? -0.015f : 0.015f) * 2}, outer = 90;
            memcpy(pd + 0x20, vec, 12);
            memcpy(pd + 0x2C, pos, 12);
            memcpy(pd + 0x54, &outer, 4);
            pd[0x58] = 1;
            char* inst = addInst(g_addr[kPoseDeformerVtbl], r.com, r.pecs[s], pd);
            memcpy(inst + 0x28, r.rest[s], 12);
        }
        char* cloth = NewNode("Outfit_Bra_Cloth_L", r.keep);   // an outfit's spring bone, also under L_Pecs
        memcpy(cloth + 0x38, &r.pecs[0], 8);
        std::vector<void*> order = {cloth};
        for (int s = 0; s < 2; ++s)
            for (int b : {3, 1, 0, 2}) order.push_back(r.bones[s][b]);   // listed out of order on purpose
        for (void* t : order) {
            char* data = static_cast<char*>(calloc(1, 0x100));
            addInst(g_addr[kSpringBoneVtbl], r.com, t, data);
        }
        for (size_t i = 0; i + 1 < insts.size(); ++i) memcpy(insts[i] + 0x08, &insts[i + 1], 8);
        memcpy(extra + 0x28, &insts[0], 8);
        ActorView v;
        const bool found = Collect(reinterpret_cast<void*>(0x1000), v);
        bool ok = found && std::fabs(v.level - 2.0f) < 1e-5f && v.com == r.com && v.nSprings == 9;
        for (int s = 0; s < 2; ++s)
            for (int b = 0; b < 4; ++b) ok &= v.anchors[s].bones[b] == r.bones[s][b];
        printf("%-26s %-34s level x%.2f, %d springs, all 8 bones by name, cloth bone left out  %s\n", "modifier list",
               "breast bones by name", v.level, v.nSprings, ok ? "OK" : "FAIL");
        g_fails += !ok;
    }

    // ---- audit: when the bone table fills, bones used in the last 10 s keep their state (no doubled offset)
    {
        std::lock_guard<std::mutex> lock(g_boneMutex);
        std::memset(g_boneStates, 0, sizeof(g_boneStates));
        g_boneCount = 0;
        char* live = NewNode("TF_Breast_Lower_L", r.keep);
        BoneState* st = FindState(live, true);
        st->added[0] = 0.02f;   // carrying a 2 cm offset
        const std::uint32_t now = static_cast<std::uint32_t>(GetTickCount64());
        std::vector<char*> old;
        while (g_boneCount < kBoneSlots * 3 / 4) {   // fill with bones of actors gone 20 s ago
            char* n = static_cast<char*>(calloc(1, 0x100));
            old.push_back(n);
            FindState(n, true)->usedMs = now - 20000;
        }
        FindState(static_cast<char*>(calloc(1, 0x100)), true);   // one more: the table drops the stale ones
        const BoneState* kept = FindState(live, false);
        const bool ok = kept && kept->added[0] == 0.02f && g_boneCount < 10 && !FindState(old[0], false);
        printf("%-26s %-34s live bone kept with its offset, %d entries left  %s\n", "bone table full", "stale bones dropped",
               g_boneCount, ok ? "OK" : "FAIL");
        g_fails += !ok;
    }

    // ---- audit: the readable-memory check in front of every guarded read
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        char* guard = static_cast<char*>(VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE | PAGE_GUARD));
        char* freed = static_cast<char*>(VirtualAlloc(nullptr, si.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        VirtualFree(freed, 0, MEM_RELEASE);
        int local = 7;
        static int image = 5;
        char* heap = static_cast<char*>(malloc(64));
        struct Probe { const char* name; const void* p; bool want; };
        const Probe probes[] = {{"null", nullptr, false}, {"0x1000", reinterpret_cast<void*>(0x1000), false},
                                {"a float's bits", reinterpret_cast<void*>(0x3F800000ull), false},
                                {"kernel", reinterpret_cast<void*>(0xFFFF800000000000ull), false},
                                {"guard page", guard, false}, {"freed page", freed, false},
                                {"heap", heap, true}, {"stack", &local, true}, {"image", &image, true},
                                {"code", reinterpret_cast<void*>(&Readable), true}};
        bool ok = true;
        char d[300] = "";
        size_t len = 0;
        const std::uint64_t badBefore = g_badReads.load(), caughtBefore = g_caughtReads.load();
        for (const Probe& pr : probes) {   // probes check first: a bad address never reaches an exception
            const bool got = Readable(pr.p, 8);
            std::uint64_t v = 0;
            const bool copied = SafeProbe(&v, pr.p, 8);
            ok &= got == pr.want && copied == pr.want;
            if (got != pr.want || copied != pr.want) len += snprintf(d + len, sizeof(d) - len, " %s wrong;", pr.name);
        }
        const std::uint64_t bad = g_badReads.load() - badBefore, caught = g_caughtReads.load() - caughtBefore;
        ok &= caught == 0 && bad == 6;
        int survived = 0;   // plain copies (memory that should be there) still survive bad memory, through the guard
        for (const Probe& pr : probes)
            if (!pr.want && pr.p != guard) {
                std::uint64_t v = 0;
                survived += !SafeCopy(&v, pr.p, 8);
            }
        const std::uint64_t caught2 = g_caughtReads.load() - caughtBefore;
        ok &= survived == 5 && caught2 == 5;
        snprintf(d + len, sizeof(d) - len, " probes: 6 bad turned away (%llu), none caught (%llu); copies: 5 bad caught (%llu)",
                 static_cast<unsigned long long>(bad), static_cast<unsigned long long>(caught),
                 static_cast<unsigned long long>(caught2));
        printf("%-26s %-34s%s  %s\n", "readable-memory check", "probes and guarded copies", d, ok ? "OK" : "FAIL");
        g_fails += !ok;
        free(heap);
    }

    // ---- audit: settings that would break the math are held in range; no settings file gives the defaults
    {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, '\\')) *slash = 0;
        strcat_s(ini, "\\TFPhysics.ini");
        static char shipped[65536] = {};   // the shipped INI, put back after the bad-values test (the INI is over 8 KB)
        FILE* f0 = nullptr;
        size_t shippedLen = 0;
        if (fopen_s(&f0, ini, "rb") == 0 && f0) {
            shippedLen = fread(shipped, 1, sizeof(shipped), f0);
            if (shippedLen == sizeof(shipped)) {
                printf("the shipped TFPhysics.ini doesn't fit the test's buffer: make it bigger\n");
                return 1;
            }
            fclose(f0);
        }
        FILE* f1 = nullptr;
        fopen_s(&f1, ini, "wb");
        fputs("[Gravity]\r\nfHangCm=-3\r\nfPressCm=99\r\nfDeadZoneDeg=95\r\nfLyingStartDeg=80\r\nfLyingFullDeg=20\r\n"
              "fThighFlatStartDeg=200\r\nfThighFlatFullDeg=-5\r\nfSlump=abc\r\nfSideSlump=9\r\nfMaxBack=99\r\nfMaxSide=-3\r\n"
              "fMaxHang=abc\r\nfMaxDown=nan\r\nfLevel=99\r\nfGravityShape=9\r\n[Springs]\r\nfSpringGravity=-1\r\n"
              "fSpringGravityBelly=7\r\nfSpringGravityHips=5\r\nfSpringGravityCalves=abc\r\nfSpringGravityArms=nan\r\n"
              "fSpringGravityTorso=-2\r\n[Size]\r\nfLarge=50\r\nfMaxLevel=99\r\n[Collision]\r\nfMaxPushBreastCm=-4\r\n"
              "fMaxPushGluteCm=55\r\nfMaxPushOtherCm=-4\r\nfDeadMaxAgeMs=99999\r\n", f1);
        fclose(f1);
        LoadSettings();
        const Settings& c = g_cfg;
        bool ok = c.hangCm == 0 && c.pressCm == 5 && c.deadZoneDeg == 60 && c.lyingFullDeg > c.lyingStartDeg &&
                  c.thighFlatFullDeg > c.thighFlatStartDeg && c.thighFlatFullDeg <= 90 && c.slump == 0 && c.sideSlump == 1.5f &&
                  c.springGravity == 0 && c.springGravityBelly == 0.3f && c.sizeLarge == 2 && c.maxLevel == 8 &&
                  c.maxBack == 8 && c.maxSide == 0.5f && c.maxHang == 0.5f && c.maxDown == 0.5f && c.gravityLevel == 8 &&
                  c.springGravityHips == 0.3f && c.springGravityCalves == 0 && c.springGravityArms == 0 &&
                  c.springGravityTorso == 0 && c.gravityShape == 2 &&
                  c.maxPush[0] == 0 && std::fabs(c.maxPush[3] - 0.10f) < 1e-7f && c.maxPushOther == 0 && c.deadMaxAge == 1.0f;
        // and the gravity math stays finite with them, in every orientation
        bool finite = true;
        for (const Pose* p : {&kStanding, &kOnBack, &kOnLeft, &kOnRight, &kFaceDown, &kBentOver}) {
            SetRows(r.chest, p->chest);
            SetRows(r.com, p->com);
            ActorView v;
            v.level = 2;
            v.size = 3;
            v.com = r.com;
            for (int s = 0; s < 2; ++s) {
                v.anchors[s].node = r.pecs[s];
                v.anchors[s].com = r.com;
                v.anchors[s].rest = r.rest[s];
                for (int b = 0; b < 4; ++b) v.anchors[s].bones[b] = r.bones[s][b];
            }
            g_cfg.collision = false;
            PostUpdate(v, 2);
            for (int s = 0; s < 2; ++s) {
                const float* t = reinterpret_cast<const float*>(r.pecs[s] + 0x70);
                for (int k = 0; k < 3; ++k) finite &= std::isfinite(t[k]) && std::fabs(t[k]) < 1;
            }
            for (int i = 0; i < v.nAdd; ++i)
                for (int k = 0; k < 3; ++k)   // (fLevel 99 is held at 8, its own largest level: x the shapes at 1.5, 26 cm)
                    finite &= std::isfinite(v.add[i][k]) && std::fabs(v.add[i][k]) < 0.3f;
        }
        ok &= finite;
        printf("%-26s %-34s every value held in range, offsets finite  %s\n", "garbage settings", "limits", ok ? "OK" : "FAIL");
        g_fails += !ok;
        DeleteFileA(ini);   // no settings file at all: the built-in defaults
        g_cfg = Settings{};   // the plugin starts from the built-in values; a missing file must leave them all
        LoadSettings();
        const Settings def{};
        const bool ok2 = g_cfg.sideSlump == def.sideSlump && g_cfg.enabled && g_cfg.collision && g_cfg.maxLevel == def.maxLevel &&
                         g_cfg.springGravityBelly == def.springGravityBelly && !g_cfg.snapshots && g_cfg.gravityLevel == 0 &&
                         g_cfg.springGravityHips == 0 && g_cfg.springGravityTorso == 0 && g_cfg.gravityShape == 0;
        printf("%-26s %-34s defaults  %s\n", "no settings file", "built-in defaults", ok2 ? "OK" : "FAIL");
        g_fails += !ok2;
        FILE* f2 = nullptr;   // put the shipped file back for the check below
        if (fopen_s(&f2, ini, "wb") == 0 && f2) {
            fwrite(shipped, 1, shippedLen, f2);
            fclose(f2);
        }
        g_cfg = Settings{};
    }

    // ---- the character handed over by the call site (the thunk copies rsi or r12 into r8)
    {
        // a stand-in for the game: sets rsi and r12 to a value, then calls the thunk with rcx/rdx as the game does
        static const std::uint8_t kCaller[] = {0x56, 0x41, 0x54, 0x48, 0x83, 0xEC, 0x28, 0x4C, 0x89, 0xC6, 0x4D, 0x89,
                                               0xC4, 0x4C, 0x89, 0xC8, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0x41, 0x5C,
                                               0x5E, 0xC3};   // push rsi/r12; rsi = r12 = r8; call r9; pop; ret
        auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        memcpy(page, kCaller, sizeof(kCaller));
        using CallerFn = void (*)(void* owner, void* ref, void* value, void* thunk);
        const CallerFn call = reinterpret_cast<CallerFn>(page);
        static void* seen[3];
        struct Target {
            static void Run(void* owner, void* ref, void* hint) { seen[0] = owner, seen[1] = ref, seen[2] = hint; }
        };
        bool ok = true;
        char detail[200] = "";
        size_t len = 0;
        for (int checked = 1; checked >= 0; --checked)
            for (int site = 0; site < 3; ++site) {
                g_siteActorOk[0] = false;
                g_siteActorOk[1] = g_siteActorOk[2] = checked != 0;
                WriteThunk(page + 64 + 32 * site, site, reinterpret_cast<void*>(&Target::Run));
                FlushInstructionCache(GetCurrentProcess(), page, 4096);
                seen[0] = seen[1] = seen[2] = reinterpret_cast<void*>(1);
                call(reinterpret_cast<void*>(0x1234), reinterpret_cast<void*>(0x5678), reinterpret_cast<void*>(0xABCDEF),
                     page + 64 + 32 * site);
                void* want = (site > 0 && checked) ? reinterpret_cast<void*>(0xABCDEF) : nullptr;
                ok &= seen[0] == reinterpret_cast<void*>(0x1234) && seen[1] == reinterpret_cast<void*>(0x5678) && seen[2] == want;
                len += snprintf(detail + len, sizeof(detail) - len, " %c%s=%s", 'A' + site, checked ? "" : "(unchecked)",
                                seen[2] ? "char" : "null");
            }
        printf("%-26s %-34s%s  %s\n", "call-site thunks", "character handed over in r8", detail, ok ? "OK" : "FAIL");
        g_fails += !ok;
        VirtualFree(page, 0, MEM_RELEASE);

        // the size reader with the handed-over character: a 3D root that doesn't point to it
        char* actor = static_cast<char*>(calloc(1, 0x400));
        char* npc = static_cast<char*>(calloc(1, 0x600));
        char* table = static_cast<char*>(calloc(1, 0x40));
        char* entries = static_cast<char*>(calloc(2, 24));
        memcpy(actor, &g_actorVt, 8);
        memcpy(actor + 0xE0, &npc, 8);
        memcpy(npc, &g_npcVt, 8);
        memcpy(npc + 0x448, &table, 8);
        memcpy(table + 0x18, &entries, 8);
        const std::uint64_t cap = 2;
        memcpy(table + 0x20, &cap, 8);
        char* key = static_cast<char*>(calloc(1, 0x18 + 64));
        strcpy_s(key + 0x18, 64, "TF_BreastsLarge");
        const float w = 1.0f;
        const std::int32_t used = 0, empty = -1;
        memcpy(entries, &key, 8);
        memcpy(entries + 8, &w, 4);
        memcpy(entries + 0x10, &used, 4);
        memcpy(entries + 24 + 0x10, &empty, 4);
        char* bareRoot = static_cast<char*>(calloc(1, 0x400));
        char* bareRoot2 = static_cast<char*>(calloc(1, 0x400));
        char* notActor = static_cast<char*>(calloc(1, 0x40));
        const float withHint = BodySize(bareRoot, actor);           // found through the hint
        const float badHint = BodySize(bareRoot2, notActor);        // a hint that isn't a character: ignored, x1
        const bool ok2 = std::fabs(withHint - 1.5f) < 1e-6 && badHint == 1.0f;
        printf("%-26s %-34s size x%.2f with it (expected x1.50), x%.2f with a non-character  %s\n", "handed-over character",
               "breast size", withHint, badHint, ok2 ? "OK" : "FAIL");
        g_fails += !ok2;
    }

    {   // the shipped INI, read by the plugin's own loader (run_tests.bat copies it next to this exe)
        g_cfg = Settings{};
        g_cfg.sideSlump = g_cfg.faceDownSquash = g_cfg.hangShape = -1;   // must all be read from the file
        g_cfg.springGravityBelly = g_cfg.springGravityButt = -1;
        g_cfg.springGravityHips = g_cfg.springGravityCalves = -1;   // the other parts' too
        g_cfg.springGravityArms = g_cfg.springGravityTorso = -1;
        g_cfg.maxPush[3] = -1;   // the glute cap must come from the file too
        g_cfg.maxPushOther = g_cfg.deadMaxAge = -1;   // and the other-body cap and the dead bodies' age limit
        LoadSettings();
        // the new switches are in the file (not just the built-in defaults)
        char got[16];
        bool keysFile = true;
        for (const char* k : {"bDeadBodies", "fDeadMaxAgeMs", "fMaxPushOtherCm", "bOtherActorsMeasureOnly"})
            keysFile &= GetPrivateProfileStringA("Collision", k, "missing", got, sizeof(got), g_iniPath) && strcmp(got, "missing");
        keysFile &= g_cfg.proxies;   // [World] bProxies=1 in the test INI
        keysFile &= g_cfg.proxyLayer && g_cfg.proxyInert && g_cfg.steer;
        keysFile &= GetPrivateProfileStringA("World", "fProxyMaxSpeed", "missing", got, sizeof(got), g_iniPath) &&
                  !strcmp(got, "20") && g_cfg.proxyMaxSpeed == 20.0f;   // the cap, 20 m/s
        keysFile &= GetPrivateProfileStringA("World", "bSteerOnce", "missing", got, sizeof(got), g_iniPath) &&
                  !strcmp(got, "1") && g_cfg.steerOnce;
        keysFile &= GetPrivateProfileStringA("World", "iSteerTime", "missing", got, sizeof(got), g_iniPath) &&
                  !strcmp(got, "1") && g_cfg.steerTime == 1;
        keysFile &= GetPrivateProfileStringA("Gravity", "fLevel", "missing", got, sizeof(got), g_iniPath) &&
                  !strcmp(got, "0") && g_cfg.gravityLevel == 0.0f;   // the installed level
        keysFile &= GetPrivateProfileStringA("Gravity", "fGravityShape", "missing", got, sizeof(got), g_iniPath) &&
                  !strcmp(got, "0") && g_cfg.gravityShape == 0.0f;   // off
        keysFile &= GetPrivateProfileStringA("Springs", "bSettleWhenPaused", "missing", got, sizeof(got), g_iniPath) &&
                  !strcmp(got, "1") && g_cfg.settlePaused;
        keysFile &= g_cfg.springGravityHips == 0.0f && g_cfg.springGravityCalves == 0.0f && g_cfg.springGravityArms == 0.0f &&
                  g_cfg.springGravityTorso == 0.0f;   // read from the file
        const bool ok = g_cfg.enabled && g_cfg.sideSlump == 1.0f && g_cfg.faceDownSquash == 1.0f && g_cfg.hangShape == 1.0f &&
                        g_cfg.slump == 1.0f && g_cfg.springGravity == 0.06f && g_cfg.springGravityBelly == 0.03f &&
                        g_cfg.springGravityButt == 0.04f && g_cfg.springGravityThighs == 0.0f && g_cfg.sizeScaling &&
                        g_cfg.sizeLarge == 0.5f && g_cfg.sizeSmall == 0.5f && g_cfg.sizeOverride == 0.0f && g_cfg.collision &&
                        !g_cfg.measureOnly && !g_cfg.innerThighs && g_cfg.fixedFrameRate &&
                        g_cfg.thighFlatStartDeg == 30.0f && g_cfg.thighFlatFullDeg == 60.0f && g_cfg.maxLevel == 3.0f &&
                        !g_cfg.snapshots && g_cfg.glutes && g_cfg.others && !g_cfg.othersMeasureOnly &&
                        std::fabs(g_cfg.maxPush[3] - 0.025f) < 1e-7f && std::fabs(g_cfg.maxPush[0] - 0.05f) < 1e-7f &&
                        keysFile && g_cfg.deadBodies && std::fabs(g_cfg.maxPushOther - 0.03f) < 1e-7f &&
                        std::fabs(g_cfg.deadMaxAge - 0.75f) < 1e-7f;
        printf("%-26s %-34s %s (%s)  %s\n", "shipped TFPhysics.ini", "every key read from the file", ok ? "as expected" : "WRONG",
               g_iniPath, ok ? "OK" : "FAIL");
        g_fails += !ok;
    }
    printf("%s\n", g_fails ? "SOME CASES FAILED" : "all cases passed");
    return g_fails;
}
