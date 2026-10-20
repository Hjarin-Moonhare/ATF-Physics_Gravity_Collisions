// Offline test of the body-shape physics: runs the plugin's own reading (BodyFactorsOf on a fake character and
// record), its per-record limits (SpringInfoFor), PreUpdate (damping, spring gravity) and PostUpdate (the swing scaled
// on fake spring bones), and compares with numbers worked out here from the design, not from the plugin's code:
//   factor = (1 + jThin thin + jMuscular muscular + jHeavy heavy [x1.5 on the belly]) x the part's size, 0.3..2
//   (thin, muscular, heavy = the triangle less what all three share); per COM axis above 1 never past the release's
//   largest travel (Max physics + Max jiggle); damping ratio x(1 - 0.4/1.5 (j - 1)), floored at the release's lowest;
//   the spring's swing (written - rest) x j per COM axis; spring gravity x the part's gravity factor / j.
// Build and run: tests\run_tests.bat
#include "../src/plugin.cpp"

#include <string>
#include <vector>

static int g_fails = 0;
static std::vector<char*> g_keep;

static void Report(const char* what, const char* detail, bool ok) {
    printf("%-34s %s  %s\n", what, detail, ok ? "OK" : "FAIL");
    g_fails += !ok;
}

static char* NewNode(const char* name) {
    char* n = static_cast<char*>(_aligned_malloc(0x400, 16));
    memset(n, 0, 0x400);
    char* e = static_cast<char*>(calloc(1, 0x18 + 64));   // string pool entry, text at +0x18
    strcpy_s(e + 0x18, 64, name);
    memcpy(n + 0x10, &e, 8);
    const float one = 1.0f;
    memcpy(n + 0xBC, &one, 4);
    const Vec3 id[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int k = 0; k < 3; ++k) memcpy(n + 0x80 + 16 * k, &id[k], 12);
    g_keep.push_back(n);
    return n;
}
static void SetRows(char* node, const Vec3 r[3]) {
    for (int k = 0; k < 3; ++k) memcpy(node + 0x80 + 16 * k, &r[k], 12);
}

// A spring as the game makes it: instance (driver, target, data, rest at +0x60) and its record
struct Spring { char* inst; char* data; char* target; };
static Spring NewSpring(char* driver, char* parent, const char* bone, const float maxDist[3], const float strength[3],
                        const float ratio[3], const float rest[3]) {
    Spring s;
    s.inst = static_cast<char*>(calloc(1, 0x100));
    s.data = static_cast<char*>(calloc(1, 0x100));
    s.target = NewNode(bone);
    memcpy(s.target + 0x38, &parent, 8);
    memcpy(s.inst + 0x10, &driver, 8);
    memcpy(s.inst + 0x18, &s.target, 8);
    memcpy(s.inst + 0x20, &s.data, 8);
    memcpy(s.inst + 0x60, rest, 12);
    memcpy(s.target + 0x70, rest, 12);
    memcpy(s.data + 0x20, strength, 12);
    memcpy(s.data + 0x30, ratio, 12);
    memcpy(s.data + 0x50, maxDist, 12);
    const float rate = 30.0f;
    memcpy(s.inst + 0xD0, &rate, 4);
    return s;
}

// ---- the fake character and record: triangle at +0x350, slider table at +0x448
struct FakeBody {
    char* actor;
    char* npc;
    char* entries;
    int used = 0;
};
static std::uintptr_t g_fakeActorVt = 0x1111, g_fakePlayerVt = 0x2222, g_fakeNpcVt = 0x3333;
static FakeBody NewBody(const float tri[3]) {
    FakeBody b;
    b.actor = static_cast<char*>(calloc(1, 0x400));
    b.npc = static_cast<char*>(calloc(1, 0x600));
    char* table = static_cast<char*>(calloc(1, 0x40));
    b.entries = static_cast<char*>(calloc(16, 24));
    memcpy(b.actor, &g_actorVt, 8);
    memcpy(b.actor + 0x98, &b.npc, 8);   // the record at +0x98, as the game keeps it
    memcpy(b.npc, &g_npcVt, 8);
    memcpy(b.npc + 0x350, tri, 12);
    memcpy(b.npc + 0x448, &table, 8);
    memcpy(table + 0x18, &b.entries, 8);
    const std::uint64_t cap = 16;
    memcpy(table + 0x20, &cap, 8);
    for (int i = 0; i < 16; ++i) {
        const std::int32_t empty = -1;
        memcpy(b.entries + i * 24 + 0x10, &empty, 4);
    }
    return b;
}
static void Put(FakeBody& b, const char* name, float w) {
    char* e = static_cast<char*>(calloc(1, 0x18 + 64));
    strcpy_s(e + 0x18, 64, name);
    char* slot = b.entries + b.used++ * 24;
    memcpy(slot, &e, 8);
    memcpy(slot + 8, &w, 4);
    const std::int32_t live = 0;
    memcpy(slot + 0x10, &live, 4);
}
static BodyFactors Read(FakeBody& b) {
    char* root = static_cast<char*>(calloc(1, 0x400));   // a fresh 3D root each time: a fresh cache slot
    return BodyFactorsOf(root, b.actor);
}

// the design, written out again: part sizes and body factors
struct Want { float j[kNumParts], g[kNumParts]; };
static float Hold(float v) { return v < 0.3f ? 0.3f : (v > 2.0f ? 2.0f : v); }
static Want Design(float thin, float mus, float heavy, const float size[kNumParts]) {
    Want w;
    for (int p = 0; p < kNumParts; ++p) {
        const float hx = p == kPartBelly ? 1.5f : 1.0f;
        w.j[p] = Hold((1 - 0.4f * thin - 0.25f * mus + 0.6f * heavy * hx) * size[p]);
        w.g[p] = Hold((1 - 0.3f * thin + 0.15f * mus + 0.5f * heavy * hx) * size[p]);
    }
    return w;
}
static double Diff(const BodyFactors& f, const Want& w) {
    double d = 0;
    for (int p = 0; p < kNumParts; ++p) d = std::fmax(d, std::fmax(std::fabs(f.jiggle[p] - w.j[p]), std::fabs(f.gravity[p] - w.g[p])));
    return d;
}

int main() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    for (int up = 0; up < 3; ++up)
        if (char* slash = strrchr(path, '\\')) *slash = 0;
    snprintf(g_logPath, sizeof(g_logPath), "%s\\build\\tests\\shape_test.log", path);
    FILE* lf = nullptr;
    if (fopen_s(&lf, g_logPath, "w") == 0 && lf) fclose(lf);
    g_cfg = Settings{};
    g_actorVt = reinterpret_cast<std::uintptr_t>(&g_fakeActorVt);
    g_playerVt = reinterpret_cast<std::uintptr_t>(&g_fakePlayerVt);
    g_npcVt = reinterpret_cast<std::uintptr_t>(&g_fakeNpcVt);
    g_triangleOk = true;
    char d[300];

    // ================================================================ the reading and the factors
    {
        const float none[kNumParts] = {1, 1, 1, 1, 1, 1, 1, 1};
        const float tri[3] = {0, 0, 1};   // the heavy corner
        FakeBody b = NewBody(tri);
        Put(b, "TF_ButtLarge", 1.0f);
        Put(b, "TF_WaistThin", 0.5f);
        Put(b, "TF_BreastsLarge", 0.4f);
        Put(b, "Thin", 0.9f);   // a name that isn't one of the sliders: ignored
        const BodyFactors f = Read(b);
        float size[kNumParts] = {1.2f, 1.5f, 0.85f, 1, 1, 1, 1, 1};   // breasts 1 + 0.5 x 0.4; butt 1 + 0.5; belly 1 - 0.3 x 0.5
        const Want w = Design(0, 0, 1, size);
        const double e = Diff(f, w);
        snprintf(d, sizeof(d), "breast %.3f/%.3f butt %.3f/%.3f belly %.3f/%.3f thigh %.3f/%.3f, off by %.6f", f.jiggle[0],
                 f.gravity[0], f.jiggle[1], f.gravity[1], f.jiggle[2], f.gravity[2], f.jiggle[3], f.gravity[3], e);
        Report("heavy corner + Butt Large + Waist", d, e < 1e-6 && f.shape && std::fabs(f.breastSize - 1.2f) < 1e-6f &&
                                                       f.heavy == 1.0f && f.thin == 0 && f.muscular == 0);
        // the breasts' gravity: v.size = the body's gravity x the breast size (the gravity shapes and the breast springs)
        ActorView v;
        TakeFactors(v, f);
        Report("heavy: breast gravity shapes", (std::to_string(v.size) + " = 1.5 x 1.2 (held to 2.0: 1.8)").c_str(),
               std::fabs(v.size - 1.8f) < 1e-6f && v.shapeOn);

        const float centre[3] = {1.0f / 3, 1.0f / 3, 1.0f / 3}, zero[3] = {0, 0, 0};
        FakeBody c = NewBody(centre), z = NewBody(zero);
        const BodyFactors fc = Read(c), fz = Read(z);
        snprintf(d, sizeof(d), "centre off by %.6f, (0,0,0) off by %.6f", Diff(fc, Design(0, 0, 0, none)), Diff(fz, Design(0, 0, 0, none)));
        Report("triangle centre and (0,0,0): x1", d, Diff(fc, Design(0, 0, 0, none)) < 1e-6 && Diff(fz, Design(0, 0, 0, none)) < 1e-6);

        const float edge[3] = {0.5f, 0.5f, 0}, mixed[3] = {0.6f, 0.2f, 0.3f};
        FakeBody eb = NewBody(edge), mb = NewBody(mixed);
        const BodyFactors fe = Read(eb), fm = Read(mb);
        const double de = Diff(fe, Design(0.5f, 0.5f, 0, none)), dm = Diff(fm, Design(0.4f, 0, 0.1f, none));
        snprintf(d, sizeof(d), "thin/muscular edge: breast jiggle %.3f (off %.6f); (0.6,0.2,0.3) -> thin 0.4 heavy 0.1 (off %.6f)",
                 fe.jiggle[0], de, dm);
        Report("shared part taken off", d, de < 1e-6 && dm < 1e-6);

        const float corners[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        float j3[3], g3[3];
        for (int k = 0; k < 3; ++k) {
            FakeBody cb = NewBody(corners[k]);
            const BodyFactors fk = Read(cb);
            j3[k] = fk.jiggle[kPartBreast];
            g3[k] = fk.gravity[kPartBreast];
        }
        snprintf(d, sizeof(d), "jiggle thin %.2f muscular %.2f heavy %.2f; gravity %.2f %.2f %.2f", j3[0], j3[1], j3[2], g3[0], g3[1], g3[2]);
        Report("the design's rule at the corners", d, j3[0] < 1 && g3[0] < 1 && j3[1] < 1 && g3[1] > 1 && j3[2] > 1 && g3[2] > 1 &&
                                                     std::fabs(j3[0] - 0.6f) < 1e-6f && std::fabs(j3[1] - 0.75f) < 1e-6f &&
                                                     std::fabs(j3[2] - 1.6f) < 1e-6f && std::fabs(g3[1] - 1.15f) < 1e-6f);

        float big[3] = {0, 0, 1};
        FakeBody lb = NewBody(big);
        for (const char* k : {"TF_BreastsLarge", "TF_ButtLarge", "TF_HipsWide", "TF_ThighsLarge", "TF_CalvesLarge", "TF_ArmsLarge"})
            Put(lb, k, 1.0f);
        const BodyFactors fl = Read(lb);
        const float sizeL[kNumParts] = {1.5f, 1.5f, 1, 1.5f, 1.3f, 1.5f, 1.5f, 1};
        const double dl = Diff(fl, Design(0, 0, 1, sizeL));
        snprintf(d, sizeof(d), "every part held to 2.0 or below (breast %.2f, hip %.2f, torso %.2f), off by %.6f", fl.jiggle[0],
                 fl.jiggle[kPartHip], fl.jiggle[kPartTorso], dl);
        Report("heavy, every part Large", d, dl < 1e-6 && fl.jiggle[0] == 2.0f);

        float bad[3];
        const std::uint32_t nanBits = 0x7FC00000u;
        memcpy(&bad[0], &nanBits, 4);
        bad[1] = -3.0f;
        bad[2] = 7.0f;   // NaN -> 0, below 0 -> 0, above 1 -> 1 (the game clamps them so too)
        FakeBody nb = NewBody(bad);
        Put(nb, "TF_ThighsLarge", 5.0f);   // a weight out of range: 1
        const BodyFactors fn = Read(nb);
        const float sizeT[kNumParts] = {1, 1, 1, 1.5f, 1, 1, 1, 1};
        const double dn = Diff(fn, Design(0, 0, 1, sizeT));
        snprintf(d, sizeof(d), "NaN / -3 / 7 read as 0 / 0 / 1, slider 5 as 1: off by %.6f", dn);
        Report("garbage in the record", d, dn < 1e-6);

        g_triangleOk = false;   // the triangle check failed at load: only the sliders count
        FakeBody hb = NewBody(tri);
        Put(hb, "TF_ButtLarge", 1.0f);
        const BodyFactors fh = Read(hb);
        const float sizeB[kNumParts] = {1, 1.5f, 1, 1, 1, 1, 1, 1};
        Report("triangle reading unavailable", "heavy record, Butt Large: only the butt moves (x1.5)",
               Diff(fh, Design(0, 0, 0, sizeB)) < 1e-6);
        g_triangleOk = true;

        g_cfg.shape = false;   // [BodyShape] bEnabled=0: the breast size alone
        FakeBody ob = NewBody(tri);
        Put(ob, "TF_BreastsLarge", 1.0f);
        Put(ob, "TF_ButtLarge", 1.0f);
        const BodyFactors fo = Read(ob);
        ActorView vo;
        TakeFactors(vo, fo);
        bool ok = !fo.shape && !vo.shapeOn && std::fabs(vo.size - 1.5f) < 1e-6f;
        for (int p = 0; p < kNumParts; ++p) ok &= fo.jiggle[p] == 1.0f && (p == kPartBreast || fo.gravity[p] == 1.0f);
        Report("[BodyShape] off", "jiggle x1 everywhere, breast gravity = the breast size alone (x1.5)", ok);
        g_cfg.shape = true;

        g_cfg.overrideJiggle = 1.3f;
        g_cfg.overrideGravity = 0.8f;
        FakeBody vb = NewBody(zero);
        Put(vb, "TF_ArmsSmall", 1.0f);
        const BodyFactors fv = Read(vb);
        Report("overrides", "every body x1.3 / x0.8, the sliders still on top (arms small x0.5)",
               std::fabs(fv.jiggle[kPartButt] - 1.3f) < 1e-6f && std::fabs(fv.gravity[kPartButt] - 0.8f) < 1e-6f &&
                   std::fabs(fv.jiggle[kPartArm] - 0.65f) < 1e-6f && std::fabs(fv.gravity[kPartArm] - 0.4f) < 1e-6f);
        g_cfg.overrideJiggle = g_cfg.overrideGravity = 0;

        char* stranger = static_cast<char*>(calloc(1, 0x400));
        const BodyFactors fs = BodyFactorsOf(stranger, stranger);   // not a character
        Report("no character", "x1 everywhere", Diff(fs, Design(0, 0, 0, none)) < 1e-6 && fs.breastSize == 1.0f);
    }

    // ================================================================ per-record limits
    const float medium[3] = {1.16f, 0.585f, 2.805f}, maxmax[3] = {1.68f, 0.765f, 4.25f};   // TF_Breast_Lower_L
    const float strength[3] = {80, 90, 90}, ratio[3] = {0.24f, 0.24f, 0.24f};
    char* com = NewNode("COM");
    char* pecs = NewNode("L_Pecs");
    {
        const float rest[3] = {0.01f, 0.02f, 0.03f};
        Spring s = NewSpring(com, pecs, "TF_Breast_Lower_L", medium, strength, ratio, rest);
        const SpringInfo* info = SpringInfoFor(s.data, s.target);
        double e = 0;
        for (int k = 0; k < 3; ++k) e = std::fmax(e, std::fabs(info->ceil[k] - maxmax[k] / medium[k]));
        snprintf(d, sizeof(d), "part %d, ceilings x%.3f x%.3f x%.3f (Max+Max / Medium), damping floor %.2f", info->part,
                 info->ceil[0], info->ceil[1], info->ceil[2], info->dampFloor[0]);
        Report("breast spring on Medium", d, info->part == kPartBreast && e < 1e-5 && std::fabs(info->dampFloor[0] - 0.18f) < 1e-6f);
        const float huge[3] = {3, 3, 9};
        Spring u = NewSpring(com, pecs, "TF_Breast_Upper_L", huge, strength, ratio, rest);
        const SpringInfo* iu = SpringInfoFor(u.data, u.target);
        Spring o = NewSpring(com, pecs, "Outfit_Cloth_L", medium, strength, ratio, rest);
        const SpringInfo* io = SpringInfoFor(o.data, o.target);
        Report("travel above the release's / not ours", "ceiling x1 (never more than installed); an outfit's bone: part -1",
               iu->part == kPartBreast && iu->ceil[0] == 1.0f && iu->ceil[2] == 1.0f && io->part == -1 && io->ceil[1] == 1.0f);
    }

    // ================================================================ damping and spring gravity (PreUpdate)
    {
        static float dt = 1.0f / 60;
        g_addr[kFrameTime] = reinterpret_cast<std::uintptr_t>(&dt);   // PreUpdate reads the frame time there
        const float rest[3] = {0, 0, 0};
        Spring a = NewSpring(com, pecs, "TF_Breast_Lower_L", medium, strength, ratio, rest);
        Spring b2 = a;   // a second actor's instance of the same record
        b2.inst = static_cast<char*>(calloc(1, 0x100));
        memcpy(b2.inst, a.inst, 0x100);
        auto damping = [&](char* inst, float j) {
            ActorView v;
            v.com = com;
            v.shapeOn = true;
            for (float& x : v.jiggle) x = j;
            v.springs[v.nSprings++] = inst;
            const float off = 0;
            const float g0 = g_cfg.springGravity;
            g_cfg.springGravity = off;
            PreUpdate(v);
            g_cfg.springGravity = g0;
            const float* c = reinterpret_cast<const float*>(inst + 0xC0);
            return Vec3{c[0], c[1], c[2]};
        };
        const Vec3 c1 = damping(a.inst, 1.0f), cHeavy = damping(b2.inst, 2.0f), cThin = damping(a.inst, 0.6f);
        // the constructor's own formula at x1: (sqrt(S) + sqrt(S)) x ratio, bit for bit
        const float game0 = (std::sqrt(80.0f) + std::sqrt(80.0f)) * 0.24f;
        const float wantHeavy = 2 * std::sqrt(90.0f) * 0.18f;   // x0.733 = 0.176, floored at the release's 0.18
        const float wantThin = 2 * std::sqrt(90.0f) * 0.24f * (1 - 0.4f / 1.5f * (0.6f - 1));
        snprintf(d, sizeof(d), "x1 %.6f (game %.6f), heavy x2 %.4f (want %.4f), thin x0.6 %.4f (want %.4f)", c1.x, game0,
                 cHeavy.y, wantHeavy, cThin.z, wantThin);
        Report("damping per body", d, c1.x == game0 && std::fabs(cHeavy.y - wantHeavy) < 1e-5f && std::fabs(cThin.z - wantThin) < 1e-5f);
        const float* ca = reinterpret_cast<const float*>(a.inst + 0xC0);
        const float* cb = reinterpret_cast<const float*>(b2.inst + 0xC0);
        Report("damping is per instance", "two actors on one record keep their own (record untouched)",
               std::fabs(ca[2] - wantThin) < 1e-5f && std::fabs(cb[2] - wantHeavy) < 1e-5f &&
                   reinterpret_cast<const float*>(a.data + 0x30)[0] == 0.24f);

        // spring gravity, lying on the back (g_now - g_standing = (0, -1, +1) in COM's frame), muscular breast:
        // jiggle x0.75 (all axes: no ceiling below 1), gravity x1.15 -> the spring's own pull x1.15 / 0.75
        const Vec3 onBack[3] = {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
        SetRows(com, onBack);
        const float stiff[3] = {1000, 1000, 1000};
        Spring g = NewSpring(com, pecs, "TF_Breast_Inner_L", medium, stiff, ratio, rest);
        ActorView v;
        v.com = com;
        v.shapeOn = true;
        v.size = 1.15f;
        for (float& x : v.jiggle) x = 0.75f;
        v.springs[v.nSprings++] = g.inst;
        PreUpdate(v);
        const float* vel = reinterpret_cast<const float*>(g.inst + 0xB0);
        const float G = 9.81f * 0.06f * 1.15f / 0.75f;
        const float want[3] = {0, G * dt, -G * dt};
        double e = 0;
        for (int k = 0; k < 3; ++k) e = std::fmax(e, std::fabs(vel[k] - want[k]));
        snprintf(d, sizeof(d), "fore/aft %.6f up %.6f m/s (want %.6f %.6f)", vel[1], vel[2], want[1], want[2]);
        Report("spring gravity / jiggle (muscular)", d, e < 1e-7);
        // the belly's gravity factor reaches its springs too
        Spring bl = NewSpring(com, pecs, "TF_Abdomen_L", medium, stiff, ratio, rest);
        ActorView vb;
        vb.com = com;
        vb.shapeOn = true;
        vb.gravity[kPartBelly] = 1.5f;
        vb.springs[vb.nSprings++] = bl.inst;
        PreUpdate(vb);
        const float* vb2 = reinterpret_cast<const float*>(bl.inst + 0xB0);
        Report("belly gravity x1.5", "the belly spring's pull x1.5", std::fabs(vb2[2] + 9.81f * 0.03f * 1.5f * dt) < 1e-7f);
        const Vec3 up[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        SetRows(com, up);
    }

    // ================================================================ the swing scaled (PostUpdate)
    {
        g_cfg.collision = false;
        auto frame = [&](std::vector<Spring*> springs, float j, bool shape = true) {
            ActorView v;
            v.com = com;
            v.shapeOn = shape;
            for (float& x : v.jiggle) x = j;
            for (Spring* s : springs) v.springs[v.nSprings++] = s->inst;
            PostUpdate(v, 1);
        };
        auto local = [](const Spring& s) {
            const float* t = reinterpret_cast<const float*>(s.target + 0x70);
            return Vec3{t[0], t[1], t[2]};
        };
        auto springWrites = [](Spring& s, const float t[3]) { memcpy(s.target + 0x70, t, 12); };
        // a breast spring on Medium whose parent is turned 90 degrees about up against COM (its X = COM's forward,
        // its Y = COM's left): heavy x2 is held per COM axis at x1.448 side, x1.308 fore/aft, x1.515 up
        char* turned = NewNode("L_Pecs_turned");
        const Vec3 rows[3] = {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}};
        SetRows(turned, rows);
        const float rest[3] = {0.05f, 0.10f, 0.02f};
        Spring s = NewSpring(com, turned, "TF_Breast_Lower_L", medium, strength, ratio, rest);
        const float out1[3] = {0.05f + 0.010f, 0.10f - 0.004f, 0.02f + 0.006f};   // the spring wrote rest + its swing
        springWrites(s, out1);
        frame({&s}, 2.0f);
        const float js = maxmax[0] / medium[0], jf = maxmax[1] / medium[1], ju = maxmax[2] / medium[2];
        // parent X = COM forward -> scaled as fore/aft; parent Y = COM left (-side) -> as side; Z = up
        const float want1[3] = {rest[0] + 0.010f * jf, rest[1] - 0.004f * js, rest[2] + 0.006f * ju};
        Vec3 got = local(s);
        double e = std::fmax(std::fabs(got.x - want1[0]), std::fmax(std::fabs(got.y - want1[1]), std::fabs(got.z - want1[2])));
        snprintf(d, sizeof(d), "swing 1.0/-0.4/0.6 cm -> %.4f/%.4f/%.4f cm (want %.4f/%.4f/%.4f), off %.7f cm",
                 (got.x - rest[0]) * 100, (got.y - rest[1]) * 100, (got.z - rest[2]) * 100, (want1[0] - rest[0]) * 100,
                 (want1[1] - rest[1]) * 100, (want1[2] - rest[2]) * 100, e * 100);
        Report("heavy x2, held per COM axis", d, e < 1e-7);

        {   // one ceiling per part: two thigh bones on Realistic whose own ceilings differ (inner lower x3.17 / x1.79 /
            // x4.80, rear x2.41 / x1.67 / x3.28, Max+Max / Realistic) both stop at the part's smaller one per axis
            char* hip = NewNode("L_Thigh_parent");
            const float inner[3] = {0.6f, 0.5f, 0.5f}, rear[3] = {0.6f, 0.5f, 1.0f};
            const float st[3] = {60, 60, 65}, rt[3] = {0.38f, 0.38f, 0.38f}, r0[3] = {0.0f, 0.1f, 0.0f};
            Spring a1 = NewSpring(com, hip, "TF_Thigh_InnerLower_L", inner, st, rt, r0);
            Spring a2 = NewSpring(com, hip, "TF_Thigh_Rear_L", rear, st, rt, r0);
            const float sw[3] = {0.004f, 0.003f, -0.005f}, o[3] = {r0[0] + sw[0], r0[1] + sw[1], r0[2] + sw[2]};
            springWrites(a1, o);
            springWrites(a2, o);
            frame({&a1, &a2}, 2.0f);
            const float jp[3] = {std::fmin(1.901f / 0.6f, 1.444f / 0.6f), std::fmin(0.896f / 0.5f, 0.834f / 0.5f),
                                 std::fmin(2.4f / 0.5f, 3.28f / 1.0f)};
            const float want[3] = {r0[0] + sw[0] * std::fmin(2.0f, jp[0]), r0[1] + sw[1] * std::fmin(2.0f, jp[1]),
                                   r0[2] + sw[2] * std::fmin(2.0f, jp[2])};
            double e2 = 0;
            for (const Spring* q : {&a1, &a2}) {
                const Vec3 g2 = local(*q);
                e2 = std::fmax(e2, std::fmax(std::fabs(g2.x - want[0]), std::fmax(std::fabs(g2.y - want[1]), std::fabs(g2.z - want[2]))));
            }
            snprintf(d, sizeof(d), "both thigh bones x%.3f x%.3f x%.3f, off %.7f cm", std::fmin(2.0f, jp[0]), std::fmin(2.0f, jp[1]),
                     std::fmin(2.0f, jp[2]), e2 * 100);
            Report("one ceiling per part", d, e2 < 1e-7);
        }

        frame({&s}, 2.0f);   // the spring skipped a frame (at rest): the node still holds the plugin's value -> same, not doubled
        got = local(s);
        e = std::fmax(std::fabs(got.x - want1[0]), std::fmax(std::fabs(got.y - want1[1]), std::fabs(got.z - want1[2])));
        snprintf(d, sizeof(d), "after a frame the spring skipped: off %.7f cm", e * 100);
        Report("spring skips a frame", d, e < 1e-7);

        const float out2[3] = {0.05f - 0.002f, 0.10f + 0.001f, 0.02f - 0.008f};
        springWrites(s, out2);
        frame({&s}, 2.0f);
        const float want2[3] = {rest[0] - 0.002f * jf, rest[1] + 0.001f * js, rest[2] - 0.008f * ju};
        got = local(s);
        e = std::fmax(std::fabs(got.x - want2[0]), std::fmax(std::fabs(got.y - want2[1]), std::fabs(got.z - want2[2])));
        snprintf(d, sizeof(d), "next frame's swing scaled from the spring's new value: off %.7f cm", e * 100);
        Report("spring writes again", d, e < 1e-7);

        // a thin body (x0.6, the same on every axis): the swing x0.6 whichever way the parent turns
        Spring t = NewSpring(com, turned, "TF_Glute_Lower_L", medium, strength, ratio, rest);
        springWrites(t, out1);
        frame({&t}, 0.6f);
        got = local(t);
        const float want3[3] = {rest[0] + 0.010f * 0.6f, rest[1] - 0.004f * 0.6f, rest[2] + 0.006f * 0.6f};
        e = std::fmax(std::fabs(got.x - want3[0]), std::fmax(std::fabs(got.y - want3[1]), std::fabs(got.z - want3[2])));
        snprintf(d, sizeof(d), "swing x0.6: off %.7f cm", e * 100);
        Report("thin x0.6", d, e < 1e-7);

        // the same bone sprung by two lists (the A-Body race list and an outfit's copy): scaled once
        Spring dup = t;
        dup.inst = static_cast<char*>(calloc(1, 0x100));
        memcpy(dup.inst, t.inst, 0x100);
        springWrites(t, out1);
        frame({&t, &dup}, 0.6f);
        got = local(t);
        e = std::fmax(std::fabs(got.x - want3[0]), std::fmax(std::fabs(got.y - want3[1]), std::fabs(got.z - want3[2])));
        snprintf(d, sizeof(d), "two instances on one bone: off %.7f cm", e * 100);
        Report("one bone, two springs", d, e < 1e-7);

        // neutral body: nothing scaled, the spring's own value stands
        springWrites(t, out2);
        frame({&t}, 1.0f);
        got = local(t);
        Report("neutral body", "the spring's own value, untouched", got.x == out2[0] && got.y == out2[1] && got.z == out2[2]);

        // [BodyShape] off: untouched as well, whatever the factors
        springWrites(t, out1);
        frame({&t}, 2.0f, false);
        got = local(t);
        Report("[BodyShape] off", "untouched", got.x == out1[0] && got.y == out1[1] && got.z == out1[2]);

        // a bone something else put 30 cm away (not a swing), and an outfit's bone: left alone
        Spring distant = NewSpring(com, turned, "TF_Thigh_Rear_L", medium, strength, ratio, rest);   // (not 'far': a windows.h macro)
        const float away[3] = {0.35f, 0.10f, 0.02f};
        springWrites(distant, away);
        Spring cloth = NewSpring(com, turned, "Outfit_Cloth_L", medium, strength, ratio, rest);
        springWrites(cloth, out1);
        frame({&distant, &cloth}, 2.0f);
        const Vec3 gf = local(distant), gc = local(cloth);
        Report("not a swing / not ours", "both left as they were", gf.x == away[0] && gc.x == out1[0] && gc.z == out1[2]);

        // collisions build on the scaled swing: the add handed on for the bone is the scaling itself
        springWrites(t, out1);
        ActorView v;
        v.com = com;
        v.shapeOn = true;
        for (float& x : v.jiggle) x = 0.6f;
        v.springs[v.nSprings++] = t.inst;
        ScaleJiggle(v);
        const float* add = v.AddFor(t.target);
        Report("handed on before collisions", "the bone's add = (x0.6 - 1) x its swing",
               add && std::fabs(add[0] + 0.4f * 0.010f) < 1e-7f && std::fabs(add[2] + 0.4f * 0.006f) < 1e-7f);
    }

    // ================================================================ the stretch guard
    {
        // strain: the base body 1; Breasts Small 2^1.15; Thin (1.45)^1.15; Large 0.9 (below 1: no power); Large 0.30 with
        // Small 0.75 (your sliders) count as their difference, -0.45 -> 1.2025^1.15
        auto strainOf = [](const float tri[3], std::initializer_list<std::pair<const char*, float>> sl) {
            SizeRead r;
            memcpy(r.tri, tri, 12);
            for (auto& [name, w] : sl)
                for (int k = 0; k < kNumSliders; ++k)
                    if (!strcmp(kSliderKey[k], name)) r.slider[k] = w;
            return StrainOf(r);
        };
        const float zero[3] = {0, 0, 0}, thin[3] = {1, 0, 0};
        const float sBase = strainOf(zero, {}), sSmall = strainOf(zero, {{"TF_BreastsSmall", 1.0f}}),
                    sThin = strainOf(thin, {}), sLarge = strainOf(zero, {{"TF_BreastsLarge", 1.0f}}),
                    sYours = strainOf(zero, {{"TF_BreastsLarge", 0.3f}, {"TF_BreastsSmall", 0.75f}});
        const bool ok = sBase == 1.0f && std::fabs(sSmall - std::pow(2.0f, 1.15f)) < 1e-5f &&
                        std::fabs(sThin - std::pow(1.45f, 1.15f)) < 1e-5f && std::fabs(sLarge - 0.9f) < 1e-6f &&
                        std::fabs(sYours - std::pow(1.2025f, 1.15f)) < 1e-4f;
        snprintf(d, sizeof(d), "base %.3f, Small %.3f, Thin %.3f, Large %.3f, Large 0.30 + Small 0.75 %.3f", sBase, sSmall, sThin,
                 sLarge, sYours);
        Report("strain per body", d, ok);

        // the gravity shapes' ceilings x the body's scale, through the plugin's own PostUpdate: lying on the back at
        // x2, the slump's anchor part is (0.5, -1.0, 0.8) cm x the level; a body at ceilings x0.5 stops at x1.0
        char* chest = NewNode("C_Chest");
        const Vec3 chestBack[3] = {{0, 1, 0}, {0, 0, 1}, {-1, 0, 0}}, comBack[3] = {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
        SetRows(chest, chestBack);
        char* comB = NewNode("COM");
        SetRows(comB, comBack);
        char* pec[2];
        const float rest[2][3] = {{0.02f, 0.10f, 0.08f}, {0.02f, 0.10f, -0.08f}};
        auto slumpOf = [&](float level, float size, float scale) {
            ActorView v;
            v.level = level;
            v.size = size;
            v.ceilScale = scale;
            v.com = comB;
            for (int s = 0; s < 2; ++s) {
                pec[s] = NewNode(s ? "R_Pecs" : "L_Pecs");
                memcpy(pec[s] + 0x38, &chest, 8);
                memcpy(pec[s] + 0x70, rest[s], 12);
                v.anchors[s].node = pec[s];
                v.anchors[s].com = comB;
                v.anchors[s].rest = rest[s];
            }
            PostUpdate(v, 2);
            const float* t = reinterpret_cast<const float*>(pec[0] + 0x70);
            return (t[1] - rest[0][1]) * 100;   // the flatten part (Y), -1.0 cm per level (whole breast only: no bones)
        };
        // no bones: the whole breast slides (fSlump 0 part): (0.8, -1.5, 1.5) per level, so Y = -1.5 x the level
        const float full = slumpOf(2.0f, 1.0f, 1.0f), half = slumpOf(2.0f, 1.0f, 0.5f), little = slumpOf(1.0f, 0.5f, 0.5f);   // (not 'small': a windows.h macro)
        snprintf(d, sizeof(d), "flatten %.3f cm at x2, %.3f with ceilings x0.5 (held at x1.0), %.3f at x1 x size 0.5", full, half,
                 little);
        Report("shape ceilings x the scale", d, std::fabs(full + 3.0f) < 1e-4f && std::fabs(half + 1.5f) < 1e-4f &&
                                                  std::fabs(little + 0.75f) < 1e-4f);

        // the breast springs' gravity factor: free at x1, held from x2 up to 1 / strain (at most 1.3), blended between
        ActorView v;
        v.size = 1.8f;
        v.strain = 0.8f;   // 1 / 0.8 = 1.25
        v.level = 1.0f;
        const float at1 = BreastSag(v);
        v.level = 2.0f;
        const float at2 = BreastSag(v);
        v.level = 1.5f;
        const float atMid = BreastSag(v);
        v.strain = 0.5f;
        v.level = 3.0f;
        const float capped = BreastSag(v);
        v.strain = 2.0f;
        v.size = 0.8f;
        const float sensitive = BreastSag(v);
        snprintf(d, sizeof(d), "x1 %.3f, x2 %.3f, x1.5 %.3f (halfway from 2.0 to 1.25), strain 0.5 at x3 %.3f (1.3 at most), "
                 "strain 2 %.3f", at1, at2, atMid, capped, sensitive);
        Report("breast spring gravity held", d, at1 == 1.8f && std::fabs(at2 - 1.25f) < 1e-6f && std::fabs(atMid - 1.625f) < 1e-6f &&
                                                     std::fabs(capped - 1.3f) < 1e-6f && std::fabs(sensitive - 0.5f) < 1e-6f);

        // the guard switched off: strain 1, ceilings x1, and the springs' factor the size alone, at any level
        g_cfg.stretchGuard = false;
        float big[3] = {1, 0, 0};
        FakeBody gb = NewBody(big);
        Put(gb, "TF_BreastsSmall", 1.0f);
        const BodyFactors fo = Read(gb);
        ActorView vo;
        TakeFactors(vo, fo);
        vo.level = 3.0f;
        const float sagOff = BreastSag(vo);
        g_cfg.stretchGuard = true;
        FakeBody gb2 = NewBody(big);
        Put(gb2, "TF_BreastsSmall", 1.0f);
        const BodyFactors fon = Read(gb2);
        snprintf(d, sizeof(d), "off: strain %.2f ceilings x%.2f sag x%.2f; on: strain %.2f ceilings x%.3f", fo.strain, fo.ceilScale,
                 sagOff, fon.strain, fon.ceilScale);
        Report("[Gravity] bStretchGuard", d, fo.strain == 1.0f && fo.ceilScale == 1.0f && sagOff == vo.size &&
                                                 std::fabs(fon.ceilScale - 1.0f / (std::pow(1.45f * 2.0f, 1.15f))) < 1e-5f);
    }

    // ================================================================ settings
    {
        char ini[MAX_PATH];
        GetModuleFileNameA(nullptr, ini, MAX_PATH);
        if (char* slash = strrchr(ini, '\\')) *slash = 0;
        strcat_s(ini, "\\TFPhysics.ini");
        char shipped[16384] = {};
        FILE* f0 = nullptr;
        size_t shippedLen = 0;
        if (fopen_s(&f0, ini, "rb") == 0 && f0) {
            shippedLen = fread(shipped, 1, sizeof(shipped), f0);
            fclose(f0);
        }
        g_cfg = Settings{};
        g_cfg.shape = g_cfg.stretchGuard = false;
        g_cfg.jThin = g_cfg.gHeavy = g_cfg.bellyHeavy = g_cfg.partWaist = g_cfg.shapeMax = -7;   // must come from the file
        LoadSettings();
        const Settings& c = g_cfg;
        const bool ok = c.shape && c.jThin == -0.4f && c.jMuscular == -0.25f && c.jHeavy == 0.6f && c.gThin == -0.3f &&
                        c.gMuscular == 0.15f && c.gHeavy == 0.5f && c.bellyHeavy == 1.5f && c.partButt == 0.5f &&
                        c.partHips == 0.3f && c.partThighs == 0.5f && c.partCalves == 0.5f && c.partArms == 0.5f &&
                        c.partWaist == 0.3f && c.jiggleDamping == 1.0f && c.shapeMin == 0.3f && c.shapeMax == 2.0f &&
                        c.overrideJiggle == 0 && c.overrideGravity == 0 && c.stretchGuard;
        Report("shipped TFPhysics.ini", ok ? "every [BodyShape] key and bStretchGuard as designed" : "WRONG", ok);
        FILE* f1 = nullptr;
        fopen_s(&f1, ini, "wb");
        fputs("[BodyShape]\r\nbEnabled=0\r\nfJiggleThin=-5\r\nfJiggleHeavy=nan\r\nfGravityMuscular=9\r\nfBellyHeavy=-1\r\n"
              "fButt=4\r\nfWaist=-2\r\nfJiggleDamping=7\r\nfMin=nan\r\nfMax=99\r\nfOverrideJiggle=-1\r\nfOverrideGravity=5\r\n"
              "[Size]\r\nfLarge=nan\r\n[Collision]\r\nfSoftScale=nan\r\n", f1);
        fclose(f1);
        g_cfg = Settings{};
        LoadSettings();
        const bool ok2 = !c.shape && c.jThin == -0.9f && c.jHeavy == -0.9f && c.gMuscular == 1.5f && c.bellyHeavy == 0 &&
                         c.partButt == 0.9f && c.partWaist == 0 && c.jiggleDamping == 1 && c.shapeMin == 0.1f &&
                         c.shapeMax == 3 && c.overrideJiggle == 0 && c.overrideGravity == 3 && c.sizeLarge == 0 &&
                         c.softScale == 0.5f;
        Report("garbage [BodyShape] settings", "held in range; nan taken as the low end", ok2);
        FILE* f2 = nullptr;   // put the shipped file back
        if (fopen_s(&f2, ini, "wb") == 0 && f2) {
            fwrite(shipped, 1, shippedLen, f2);
            fclose(f2);
        }
        g_cfg = Settings{};
    }

    // ================================================================ tuning each part
    // Written out from the design: a part at a level takes that preset's springs (strength, damping ratio, MaxDist,
    // per COM axis) at the installed extra-jiggle level, and the level's swing (Minimal x0.25, Lower x0.5, Off x0);
    // fSwing, the per-direction factors and the bone's own multiply the swing; fSettle multiplies the damping ratio
    // (floored at the release's lowest, held at critical); fBounceSpeed multiplies the strength by its square; fSag is
    // the part's gravity in g; a character's, an outfit's (the lowest of the pieces worn) and the planet's factors
    // multiply the body's.
    {
        static float dt9 = 1.0f / 60;
        g_addr[kFrameTime] = reinterpret_cast<std::uintptr_t>(&dt9);
        g_cfg = Settings{};
        g_cfg.collision = false;
        TuneSettings& T = g_tunes[g_tuneNow.load()];
        auto tune = [&](auto edit) {
            T = TuneSettings{};
            edit(T);
            TuneFlags(T);
        };
        auto untune = [&]() {
            T = TuneSettings{};
            TuneFlags(T);
        };
        auto bone = [](const char* name) {
            for (int b = 0; b < kNumPresetBones; ++b)
                if (!strcmp(kPresetBones[b].name, name)) return b;
            return -1;
        };
        auto same3 = [](const float* a, const float* b, float tol) {
            return std::fabs(a[0] - b[0]) <= tol && std::fabs(a[1] - b[1]) <= tol && std::fabs(a[2] - b[2]) <= tol;
        };
        const PresetSpring& realistic = kPresetBones[bone("TF_Breast_Lower_L")].s[0][0];
        const PresetSpring& high = kPresetBones[bone("TF_Breast_Lower_L")].s[3][0];
        const float zero[3] = {0, 0, 0};
        char* com9 = NewNode("COM");
        char* pecs9 = NewNode("L_Pecs");
        {   // which installer setting a record was built with
            Spring r = NewSpring(com9, pecs9, "TF_Breast_Lower_L", realistic.maxDist, realistic.strength, realistic.damping, zero);
            Spring h = NewSpring(com9, pecs9, "TF_Breast_Lower_L", high.maxDist, high.strength, high.damping, zero);
            const float odd[3] = {77, 90, 90};
            Spring u = NewSpring(com9, pecs9, "TF_Breast_Lower_L", realistic.maxDist, odd, realistic.damping, zero);
            const PresetSpring& tm = kPresetBones[bone("TF_Thigh_Rear_L")].s[2][1];   // Medium, extra jiggle Normal
            Spring t = NewSpring(com9, pecs9, "TF_Thigh_Rear_L", tm.maxDist, tm.strength, tm.damping, zero);
            const SpringInfo* ir = SpringInfoFor(r.data, r.target);
            const SpringInfo* ih = SpringInfoFor(h.data, h.target);
            const SpringInfo* iu = SpringInfoFor(u.data, u.target);
            const SpringInfo* it = SpringInfoFor(t.data, t.target);
            snprintf(d, sizeof(d), "Realistic %d/%d, High %d/%d, a thigh on Medium + Normal %d/%d, another mod's record %d/%d",
                     ir->instPreset, ir->instExtra, ih->instPreset, ih->instExtra, it->instPreset, it->instExtra, iu->instPreset,
                     iu->instExtra);
            Report("the installer setting found", d, ir->instPreset == 0 && ir->instExtra == 0 && ih->instPreset == 3 &&
                   it->instPreset == 2 && it->instExtra == 1 && iu->instPreset == -1 && iu->presetBone == bone("TF_Breast_Lower_L"));
        }
        {   // a part at High: the record gets High's springs, the instance its damping and sub-step rate; then back
            Spring s = NewSpring(com9, pecs9, "TF_Breast_Lower_L", realistic.maxDist, realistic.strength, realistic.damping, zero);
            const float* rs = reinterpret_cast<const float*>(s.data + 0x20);
            const float* rm = reinterpret_cast<const float*>(s.data + 0x50);
            const float* c = reinterpret_cast<const float*>(s.inst + 0xC0);
            const float* rate = reinterpret_cast<const float*>(s.inst + 0xD0);
            auto update = [&]() {
                ActorView v;
                v.com = com9;
                v.springs[v.nSprings++] = s.inst;
                PreUpdate(v);
            };
            tune([](TuneSettings& t) { t.part[kPartBreast].level = kLevelHigh; });
            update();
            const float wantC = 2.0f * std::sqrt(high.strength[2]) * high.damping[2];
            const bool atHigh = same3(rs, high.strength, 0) && same3(rm, high.maxDist, 1e-6f) && std::fabs(c[2] - wantC) < 1e-5f &&
                                std::fabs(*rate - 0.63662f * std::sqrt(76.5f)) < 1e-5f;
            snprintf(d, sizeof(d), "strength %.1f %.1f %.1f, travel %.3f %.3f %.3f, damping %.4f (want %.4f), rate %.4f", rs[0], rs[1],
                     rs[2], rm[0], rm[1], rm[2], c[2], wantC, *rate);
            Report("a part at High", d, atHigh);
            untune();
            update();
            const float game0 = (std::sqrt(realistic.strength[2]) + std::sqrt(realistic.strength[2])) * realistic.damping[2];
            const bool back = same3(rs, realistic.strength, 0) && same3(rm, realistic.maxDist, 1e-6f) && c[2] == game0 &&
                              *rate == 0.63662f * std::sqrt(90.0f);
            snprintf(d, sizeof(d), "strength %.1f, travel %.3f, damping %.6f (the constructor's %.6f), rate %.5f", rs[2], rm[2], c[2],
                     game0, *rate);
            Report("back as installed", d, back);
            // a record no installer setting matches keeps its own springs at any level
            const float odd[3] = {77, 90, 90};
            Spring u = NewSpring(com9, pecs9, "TF_Breast_Lower_L", realistic.maxDist, odd, realistic.damping, zero);
            tune([](TuneSettings& t) { t.part[kPartBreast].level = kLevelHigh; });
            ActorView vu;
            vu.com = com9;
            vu.springs[vu.nSprings++] = u.inst;
            PreUpdate(vu);
            Report("another mod's record at High", "keeps its own springs",
                   same3(reinterpret_cast<const float*>(u.data + 0x20), odd, 0) &&
                       same3(reinterpret_cast<const float*>(u.data + 0x50), realistic.maxDist, 1e-6f));
            // bounce speed x1.2: strength x1.44, the rate with it; settle x2: the damping ratio x2
            tune([](TuneSettings& t) {
                t.part[kPartBreast].bounce = 1.2f;
                t.part[kPartBreast].settle = 2.0f;
            });
            update();
            const float s144 = realistic.strength[2] * 1.44f;
            const float wantSettle = 2.0f * std::sqrt(s144) * realistic.damping[2] * 2.0f;
            snprintf(d, sizeof(d), "strength %.2f (want %.2f), rate %.4f, damping %.4f (want %.4f)", rs[2], s144, *rate, c[2], wantSettle);
            Report("bounce speed and settle", d, std::fabs(rs[2] - s144) < 1e-4f &&
                   std::fabs(*rate - 0.63662f * std::sqrt(s144)) < 1e-5f && std::fabs(c[2] - wantSettle) < 1e-4f);
            untune();
            update();
        }
        {   // the swing: a level's, Off, per direction, per bone
            auto local = [](const Spring& s) {
                const float* t = reinterpret_cast<const float*>(s.target + 0x70);
                return Vec3{t[0], t[1], t[2]};
            };
            const PresetSpring& gr = kPresetBones[bone("TF_Glute_Lower_L")].s[0][0];
            const PresetSpring& br = kPresetBones[bone("TF_Breast_Upper_L")].s[0][0];
            Spring g = NewSpring(com9, pecs9, "TF_Glute_Lower_L", gr.maxDist, gr.strength, gr.damping, zero);
            Spring lo = NewSpring(com9, pecs9, "TF_Breast_Lower_L", realistic.maxDist, realistic.strength, realistic.damping, zero);
            Spring up = NewSpring(com9, pecs9, "TF_Breast_Upper_L", br.maxDist, br.strength, br.damping, zero);
            const float out[3] = {0.004f, -0.002f, 0.006f};
            auto swing = [&](std::vector<Spring*> springs) {
                for (Spring* s : springs) memcpy(s->target + 0x70, out, 12);
                ActorView v;
                v.com = com9;
                for (Spring* s : springs) v.springs[v.nSprings++] = s->inst;
                PreUpdate(v);
                for (Spring* s : springs) memcpy(s->target + 0x70, out, 12);   // the spring writes its swing
                PostUpdate(v, 1);
            };
            auto times = [&](const Spring& s, float fx, float fy, float fz) {
                const Vec3 got = local(s);
                return std::fabs(got.x - out[0] * fx) < 1e-7f && std::fabs(got.y - out[1] * fy) < 1e-7f &&
                       std::fabs(got.z - out[2] * fz) < 1e-7f;
            };
            bool ok = true;
            char seen[200] = "";
            const int levels[3] = {kLevelLower, kLevelMinimal, kLevelOff};
            const float want[3] = {0.5f, 0.25f, 0.0f};
            for (int i = 0; i < 3; ++i) {
                tune([&](TuneSettings& t) { t.part[kPartButt].level = levels[i]; });
                swing({&g});
                ok &= times(g, want[i], want[i], want[i]);
                const size_t len = strlen(seen);
                snprintf(seen + len, sizeof(seen) - len, "%s x%.2f; ", i == 2 ? "Off" : kLevelName[levels[i]],
                         (local(g).z) / out[2]);
            }
            Report("Lower, Minimal and Off", seen, ok);
            tune([](TuneSettings& t) { t.allLevel = kLevelLower; t.part[kPartBreast].level = kLevelMax; });
            swing({&g, &lo});
            Report("[All] Lower, the breasts Max", "the butt x0.5 (from [All]), the breasts x1 on Max's springs",
                   times(g, 0.5f, 0.5f, 0.5f) && times(lo, 1, 1, 1) &&
                       same3(reinterpret_cast<const float*>(lo.data + 0x20), kPresetBones[bone("TF_Breast_Lower_L")].s[4][0].strength, 0));
            tune([](TuneSettings& t) {
                t.part[kPartBreast].dir[2] = 0.5f;   // up and down at half
                t.part[kPartBreast].swing = 0.8f;
            });
            swing({&lo, &up});
            Report("per direction", "the breasts x0.8, up-down x0.5 on top", times(lo, 0.8f, 0.8f, 0.4f) && times(up, 0.8f, 0.8f, 0.4f));
            tune([&](TuneSettings& t) { t.bone[bone("TF_Breast_Lower_L")] = 0.5f; });
            swing({&lo, &up});
            Report("per bone", "TF_Breast_Lower_L x0.5, TF_Breast_Upper_L as installed", times(lo, 0.5f, 0.5f, 0.5f) && times(up, 1, 1, 1));
            tune([](TuneSettings& t) { t.part[kPartButt].swing = 3.0f; });   // above 1: held per axis to Max + Max's travel
            swing({&g});
            const SpringInfo* gi = SpringInfoFor(g.data, g.target);
            float ceil3[3];
            for (int k = 0; k < 3; ++k) ceil3[k] = gi->maxTravel[k] / gi->orig[k];   // the body has this one butt spring
            snprintf(d, sizeof(d), "x3 asked: x%.3f x%.3f x%.3f (Max + Max's travel / Realistic's)", local(g).x / out[0],
                     local(g).y / out[1], local(g).z / out[2]);
            Report("more swing than Max", d, times(g, std::fmin(3.0f, ceil3[0]), std::fmin(3.0f, ceil3[1]), std::fmin(3.0f, ceil3[2])));
            untune();
        }
        {   // sag on a part that had none: the hips at 0.05 g, lying on the back
            const Vec3 onBack[3] = {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}};
            SetRows(com9, onBack);
            const float stiff[3] = {1000, 1000, 1000};
            const PresetSpring& hp = kPresetBones[bone("TF_Hip_Outer_L")].s[0][0];
            Spring h = NewSpring(com9, pecs9, "TF_Hip_Outer_L", hp.maxDist, stiff, hp.damping, zero);
            ActorView v0;
            v0.com = com9;
            v0.springs[v0.nSprings++] = h.inst;
            PreUpdate(v0);
            const float before = reinterpret_cast<const float*>(h.inst + 0xB0)[2];
            tune([](TuneSettings& t) { t.part[kPartHip].sag = 0.05f; });
            ActorView v;
            v.com = com9;
            v.springs[v.nSprings++] = h.inst;
            PreUpdate(v);
            const float* vel = reinterpret_cast<const float*>(h.inst + 0xB0);
            const float G = 9.81f * 0.05f;
            snprintf(d, sizeof(d), "none installed (up %.6f), at 0.05 g: fore/aft %.6f up %.6f m/s (want %.6f %.6f)", before, vel[1],
                     vel[2] - before, G * dt9, -G * dt9);
            Report("sag on the hips", d, before == 0.0f && std::fabs(vel[1] - G * dt9) < 1e-7f && std::fabs(vel[2] - before + G * dt9) < 1e-7f);
            untune();
            const Vec3 upright[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            SetRows(com9, upright);
        }
        {   // per character, per outfit and per planet (the reads on a fake character and record)
            g_formIdOk = g_wornOk = g_gravityScaleOk = true;
            auto person = [&](bool player, std::uint32_t ref, std::uint32_t base) {
                char* actor = static_cast<char*>(calloc(1, 0x1200));
                char* npc = static_cast<char*>(calloc(1, 0x600));
                memcpy(actor, player ? &g_playerVt : &g_actorVt, 8);
                memcpy(actor + 0x98, &npc, 8);
                memcpy(npc, &g_npcVt, 8);
                memcpy(actor + 0x28, &ref, 4);
                memcpy(npc + 0x28, &base, 4);
                return actor;
            };
            auto wear = [](char* actor, std::vector<std::pair<std::uint64_t, std::uint32_t>> pieces, bool worn) {
                char* inv = static_cast<char*>(calloc(1, 0x40));
                char* items = static_cast<char*>(calloc(pieces.size() + 1, 0x28));
                const std::uint32_t count = static_cast<std::uint32_t>(pieces.size());
                memcpy(inv + 0x28, &count, 4);
                memcpy(inv + 0x30, &items, 8);
                for (size_t i = 0; i < pieces.size(); ++i) {
                    char* form = static_cast<char*>(calloc(1, 0x220));
                    form[0x2E] = pieces[i].first ? 0x22 : 0x2A;   // an armour, or (no slots) a weapon
                    memcpy(form + 0x208, &pieces[i].first, 8);
                    memcpy(form + 0x28, &pieces[i].second, 4);
                    memcpy(items + i * 0x28, &form, 8);
                    items[i * 0x28 + 0x20] = worn ? 1 : 0;
                }
                memcpy(actor + 0xA0, &inv, 8);
            };
            auto factors = [](char* actor) {
                char* root = static_cast<char*>(calloc(1, 0x400));
                return BodyFactorsOf(root, actor);
            };
            g_cfg.shape = false;
            g_cfg.sizeScaling = false;
            g_gravityKnown = false;
            tune([](TuneSettings& t) {
                t.planet = false;
                t.chars[0].player = true;
                for (float& x : t.chars[0].part) x = 1.0f;
                t.chars[0].part[kPartBreast] = 0.5f;
                t.chars[1].id = 0x0001ABCD;
                for (float& x : t.chars[1].part) x = 0.8f;
                t.nChars = 2;
            });
            const BodyFactors fp = factors(person(true, 0x14, 0x7)), fr = factors(person(false, 0x0001ABCD, 0x99)),
                              fb = factors(person(false, 0x55, 0x0001ABCD)), fo = factors(person(false, 0x56, 0x98));
            snprintf(d, sizeof(d), "you: breasts x%.2f butt x%.2f; the NPC by reference x%.2f, by record x%.2f; another x%.2f",
                     fp.jiggle[kPartBreast], fp.jiggle[kPartButt], fr.jiggle[kPartBelly], fb.jiggle[kPartArm], fo.jiggle[kPartBreast]);
            Report("per character", d, fp.jiggle[kPartBreast] == 0.5f && fp.jiggle[kPartButt] == 1.0f && fp.tuned &&
                   fr.jiggle[kPartBelly] == 0.8f && fb.jiggle[kPartArm] == 0.8f && fo.jiggle[kPartBreast] == 1.0f && !fo.tuned);
            tune([](TuneSettings& t) {
                t.planet = false;
                t.outfits[0].slots = 1ull << 35;   // a spacesuit's upper body
                for (float& x : t.outfits[0].part) x = 1.0f;
                t.outfits[0].part[kPartBreast] = 0.5f;
                t.outfits[0].part[kPartButt] = 0.6f;
                t.outfits[1].slots = 1ull << 3;    // clothes
                for (float& x : t.outfits[1].part) x = 1.0f;
                t.outfits[1].part[kPartBreast] = 0.8f;
                t.outfits[1].part[kPartBelly] = 0.9f;
                t.outfits[2].form = 0x222;         // one particular armour: its own section, not the slots'
                for (float& x : t.outfits[2].part) x = 1.0f;
                t.outfits[2].part[kPartBelly] = 0.95f;
                t.nOutfits = 3;
            });
            char* dressed = person(false, 0x60, 0x61);
            wear(dressed, {{1ull << 35 | 1ull << 39, 0x111}, {1ull << 3 | 1ull << 7, 0x222}, {0, 0x333}}, true);
            char* clothes = person(false, 0x62, 0x63);
            wear(clothes, {{1ull << 3, 0x444}}, true);
            char* bare = person(false, 0x64, 0x65);
            wear(bare, {{1ull << 35, 0x555}}, false);   // a spacesuit carried, not worn
            const BodyFactors fd = factors(dressed), fc = factors(clothes), fn = factors(bare);
            snprintf(d, sizeof(d), "suit + its own clothes: breasts x%.2f butt x%.2f belly x%.2f; clothes: breasts x%.2f belly x%.2f; "
                     "carried only: x%.2f", fd.jiggle[kPartBreast], fd.jiggle[kPartButt], fd.jiggle[kPartBelly], fc.jiggle[kPartBreast],
                     fc.jiggle[kPartBelly], fn.jiggle[kPartBreast]);
            Report("per outfit", d, fd.jiggle[kPartBreast] == 0.5f && fd.jiggle[kPartButt] == 0.6f && fd.jiggle[kPartBelly] == 0.95f &&
                   fc.jiggle[kPartBreast] == 0.8f && fc.jiggle[kPartBelly] == 0.9f && fn.jiggle[kPartBreast] == 1.0f && !fn.tuned);
            tune([](TuneSettings& t) { t.planet = true; });
            char* you = person(true, 0x14, 0x7);
            const float low = 0.4f;
            memcpy(you + 0x1138, &low, 4);
            const BodyFactors fy = factors(you), fz = factors(person(false, 0x70, 0x71));
            tune([](TuneSettings& t) {
                t.planet = true;
                t.planetAmount = 0.5f;
            });
            const BodyFactors fh = factors(person(false, 0x72, 0x73));
            snprintf(d, sizeof(d), "at 0.4 g: your gravity x%.2f, an NPC's x%.2f; half of it: x%.2f", fy.gravity[kPartBreast],
                     fz.gravity[kPartBelly], fh.gravity[kPartThigh]);
            Report("per planet", d, std::fabs(fy.gravity[kPartBreast] - 0.4f) < 1e-6f && std::fabs(fz.gravity[kPartBelly] - 0.4f) < 1e-6f &&
                   std::fabs(fh.gravity[kPartThigh] - 0.7f) < 1e-6f && fy.tuned);
            g_gravityKnown = false;
            g_formIdOk = g_wornOk = g_gravityScaleOk = false;
            g_cfg = Settings{};
            g_cfg.collision = false;
            untune();
        }
        {   // the files: TFPhysics.ini [Tuning] and the TFPhysics folder, garbage held in range, read again on a change
            char saved[MAX_PATH];
            strcpy_s(saved, g_iniPath);
            char base[MAX_PATH], dir[MAX_PATH];
            snprintf(base, sizeof(base), "%s\\build\\tests\\tune_test", path);
            CreateDirectoryA(base, nullptr);
            snprintf(g_iniPath, sizeof(g_iniPath), "%s\\TFPhysics.ini", base);
            TuneDir(dir, sizeof(dir));
            CreateDirectoryA(dir, nullptr);
            auto put = [&](const char* name, const char* text) {
                char file[MAX_PATH];
                snprintf(file, sizeof(file), "%s\\%s", name[0] == '.' ? base : dir, name[0] == '.' ? name + 1 : name);
                FILE* f = nullptr;
                fopen_s(&f, file, "wb");
                fputs(text, f);
                fclose(f);
            };
            put(".TFPhysics.ini", "[Tuning]\r\nbPlanetGravity=0\r\nfPlanetGravity=7\r\nbOutfits=1\r\n");
            put("All.ini", "[All]\r\nsLevel=Lower\r\n");
            put("Breasts.ini", "[Breasts]\r\nsLevel=High\r\nfSwing=1.2\r\nfUpDown=0.5\r\nfSettle=2\r\nfBounceSpeed=1.1\r\nfSag=0.08\r\n"
                "TF_Breast_Lower_L=0.7\r\nTF_Glute_Lower_L=0.1\r\n");
            put("Butt.ini", "[Butt]\r\nsLevel=Banana\r\nfSwing=nan\r\nfBounceSpeed=9\r\n");
            put("Outfits.ini", "[Outfit Spacesuit upper]\r\nsSlots=35, 36\r\nfBreasts=0.5\r\n[Outfit My suit]\r\nsForm=0001A2B3\r\n"
                "fAll=0.7\r\n[Outfit nothing]\r\nfAll=0.1\r\n");
            put("Characters.ini", "[Player]\r\nfBreasts=1.3\r\n[NPC 0001ABCD]\r\nfAll=0.8\r\n[NPC zz]\r\nfAll=0.2\r\n");
            TuneSettings& t = g_tunes[g_tuneNow.load()];
            LoadTuning(t);
            const PartTune& b = t.part[kPartBreast];
            const PartTune& u = t.part[kPartButt];
            const bool read = t.files == 5 && t.allLevel == kLevelLower && b.level == kLevelHigh && b.swing == 1.2f && b.dir[2] == 0.5f &&
                              b.settle == 2.0f && b.bounce == 1.1f && b.sag == 0.08f && t.bone[bone("TF_Breast_Lower_L")] == 0.7f &&
                              t.bone[bone("TF_Glute_Lower_L")] == 1.0f && u.level == kLevelPreset && LevelOf(t, kPartButt) == kLevelLower &&
                              u.swing == 0.0f && u.bounce == 2.0f && !t.planet && t.planetAmount == 1.0f && t.outfitsOn &&
                              t.nOutfits == 2 && t.outfits[0].slots == (1ull << 35 | 1ull << 36) && t.outfits[1].form == 0x1A2B3 &&
                              t.outfits[1].part[kPartCalf] == 0.7f && t.nChars == 2 && t.chars[0].player &&
                              t.chars[0].part[kPartBreast] == 1.3f && t.chars[1].id == 0x1ABCD && t.springs && t.scaled && t.damping;
            snprintf(d, sizeof(d), "%d files (%s); breasts %s x%.1f, the butt's level from [All], garbage held", t.files, t.fileNames,
                     b.level == kLevelHigh ? "High" : "WRONG", b.swing);
            Report("the tuning files", d, read);
            g_cfg.tuneReload = true;
            g_tuneStamp = TuneStamp();
            g_tuneCheckMs = 0;
            const int before = g_tuneNow.load();
            MaybeReloadTuning();
            const bool still = g_tuneNow.load() == before;   // nothing changed: not read again
            put("Breasts.ini", "[Breasts]\r\nsLevel=Low\r\n");
            g_tuneCheckMs = 0;
            MaybeReloadTuning();
            const bool again = g_tuneNow.load() != before && Tune().part[kPartBreast].level == kLevelLow &&
                               Tune().part[kPartBreast].swing == 1.0f && Tune().allLevel == kLevelLower;
            Report("read again on a change", "unchanged: kept; Breasts.ini edited: the other copy read and swapped in", still && again);
            strcpy_s(g_iniPath, saved);
            g_tunes[0] = TuneSettings{};
            g_tunes[1] = TuneSettings{};
            g_tuneNow = 0;
        }
    }
    FlushLog();
    printf("%s\n", g_fails ? "SOME CASES FAILED" : "all cases passed");
    return g_fails;
}
