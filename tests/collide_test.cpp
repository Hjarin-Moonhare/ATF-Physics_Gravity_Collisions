// Offline test of the plugin's collisions: builds fake game nodes from tests/cases/<pose>.txt (export_collision_cases.py),
// runs the plugin's own Collide() on them and compares each soft bone's push with the design.  The fake nodes use the
// game's layout (name +0x10, parent +0x38, local translation +0x70, world rows +0x80, world position +0xB0, scale
// +0xBC) and keep their child list at an offset the plugin has to find by itself.
// Build and run: tests\run_tests.bat
#include "../src/plugin.cpp"

#include <map>
#include <string>
#include <vector>

static char g_tests[MAX_PATH];   // <plugin>\tests, found from the exe at <plugin>\build\tests
constexpr int kNodeBytes = 0x400, kChildData = 0x138, kChildCount = 0x130;
// three ways a node could keep its children; the plugin must find each by itself
enum FakeLayout { kPointerAndCount, kSmallArray, kLinked, kWide, kGame };   // kGame: the game's own
constexpr int kSmallHeader = 0x148, kSmallData = 0x150, kFirstChild = 0x160, kNextSibling = 0x168, kWideData = 0x170;
static int g_fakeLayout = kPointerAndCount;
static int g_bigLists = 0;
static bool g_foreignFirst = false;   // put a node that isn't theirs first in some lists (the reader must skip it)
static const char* kFakeLayoutName[] = {"pointer + count", "small array in the node", "linked list",
                                         "pointer to 16-byte entries (u16 counts)",
                                         "the game's: pointer at +0x138, 16-bit count at +0x142"};

struct Fake {
    std::map<std::string, char*> node;
    std::vector<std::vector<void*>> kidLists;
};

static char* NewEntry(const std::string& s, bool external) {
    char* e = static_cast<char*>(calloc(1, 0x18 + 64));
    memcpy(e + 0x18, s.c_str(), s.size() + 1);
    if (!external) return e;
    char* outer = static_cast<char*>(calloc(1, 0x18 + 8));   // an "external" entry that forwards to the real one
    outer[0x14] = 2;
    memcpy(outer + 0x08, &e, 8);
    return outer;
}

static bool Load(const char* path, Fake& f, std::map<std::string, Vec3>& expect, const std::vector<std::string>& hide) {
    FILE* in = nullptr;
    if (fopen_s(&in, path, "r") || !in) return false;
    char kind[4], name[96], par[96];
    std::map<std::string, std::string> parentOf;
    std::vector<std::string> order;
    while (fscanf_s(in, "%3s", kind, 4) == 1) {
        if (kind[0] == 'N') {
            float v[15];
            fscanf_s(in, "%95s %95s", name, 96, par, 96);
            for (float& x : v) fscanf_s(in, "%f", &x);
            char* n = static_cast<char*>(_aligned_malloc(kNodeBytes, 16));
            memset(n, 0, kNodeBytes);
            std::string nm = name;
            for (const std::string& h : hide)
                if (nm == h) nm += "_hidden";   // a bone this skeleton doesn't have
            char* entry = NewEntry(nm, nm == "COM");
            memcpy(n + 0x10, &entry, 8);
            memcpy(n + 0x70, v, 12);
            for (int r = 0; r < 3; ++r) memcpy(n + 0x80 + 16 * r, v + 3 + 3 * r, 12);
            memcpy(n + 0xB0, v + 12, 12);
            const float one = 1.0f;
            memcpy(n + 0xBC, &one, 4);
            f.node[name] = n;
            parentOf[name] = par;
            order.push_back(name);
        } else {
            Vec3 e;
            fscanf_s(in, "%95s %f %f %f", name, 96, &e.x, &e.y, &e.z);
            expect[name] = e;
        }
    }
    fclose(in);
    std::map<std::string, std::vector<void*>> kids;
    for (const std::string& n : order) {
        auto it = f.node.find(parentOf[n]);
        if (it == f.node.end()) continue;
        memcpy(f.node[n] + 0x38, &it->second, 8);
        kids[parentOf[n]].push_back(f.node[n]);
    }
    if (g_foreignFirst)
        for (const char* n : {"L_Forearm", "R_Forearm", "L_Thigh_Twist", "R_Thigh_Twist1"})
            kids[n].insert(kids[n].begin(), f.node["C_Head"]);
    for (auto& [n, list] : kids) {
        char* node = f.node[n];
        const std::uint32_t count = static_cast<std::uint32_t>(list.size());
        f.kidLists.push_back(list);
        void* data = f.kidLists.back().data();
        if (g_fakeLayout == kPointerAndCount) {
            memcpy(node + kChildData, &data, 8);
            memcpy(node + kChildCount, &count, 4);
            memcpy(node + kChildCount + 4, &count, 4);   // capacity
        } else if (g_fakeLayout == kSmallArray) {          // 8 entries inside the node, else a pointer in their place
            memcpy(node + kSmallHeader, &count, 4);
            if (count <= 8) {
                memcpy(node + kSmallData, list.data(), count * 8);
            } else {
                memcpy(node + kSmallData, &data, 8);
                ++g_bigLists;
            }
        } else if (g_fakeLayout == kLinked) {              // first child in the node, next sibling in each child
            memcpy(node + kFirstChild, &list[0], 8);
            for (size_t i = 0; i + 1 < list.size(); ++i) memcpy(static_cast<char*>(list[i]) + kNextSibling, &list[i + 1], 8);
        } else if (g_fakeLayout == kWide) {                // each child beside another word; 16-bit size and capacity
            std::vector<void*> wide;
            for (void* c : list) {
                wide.push_back(c);
                wide.push_back(reinterpret_cast<void*>(0x10001));
            }
            f.kidLists.push_back(wide);
            void* wdata = f.kidLists.back().data();
            const std::uint16_t c16[2] = {static_cast<std::uint16_t>(count), static_cast<std::uint16_t>(count)};
            memcpy(node + kWideData, &wdata, 8);
            memcpy(node + kWideData + 8, c16, 4);
        } else if (g_fakeLayout == kGame) {   // the game's own: pointer at +0x138, 16-bit capacity and count at +0x140/2
            const std::uint16_t c16[2] = {static_cast<std::uint16_t>(count), static_cast<std::uint16_t>(count)};
            memcpy(node + kChildData, &data, 8);
            memcpy(node + 0x140, c16, 4);
        }
    }
    return true;
}

// Empties the table of shared body shapes, so a case only sees the bodies it builds itself.
static void ResetOthers() {
    for (OtherEntry& e : g_others) {
        e.key.store(nullptr);
        e.tick.store(0);
        e.seq.store(0);
        e.lastKey = nullptr;
        e.n = 0;
        e.abody = false;
        e.player = false;
        e.dead = false;
        e.vel = {0, 0, 0};
    }
}

static int RunCase(const char* pose, const std::vector<std::string>& hide, float tolCm, bool rebuildAnchor) {
    ResetOthers();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cases\\%s.txt", g_tests, pose);
    Fake f;
    f.kidLists.reserve(512);   // child arrays must not move once their address is in a node
    std::map<std::string, Vec3> expect;
    if (!Load(path, f, expect, hide)) {
        printf("%s: cannot read %s\n", pose, path);
        return 1;
    }
    ActorView v;
    v.com = f.node["COM"];
    const char* bone[4] = {"Upper", "Lower", "Inner", "Outer"};   // kUpper, kLower, kInner, kOuter
    for (int s = 0; s < 2; ++s) {
        Anchor& a = v.anchors[s];
        a.node = f.node[s ? "R_Pecs" : "L_Pecs"];
        a.com = v.com;
        a.rest = reinterpret_cast<const float*>(static_cast<char*>(a.node) + 0x70);
        for (int b = 0; b < 4; ++b) a.bones[b] = f.node[std::string("TF_Breast_") + bone[b] + (s ? "_R" : "_L")];
        if (rebuildAnchor) {   // the anchor "moved this frame": the plugin rebuilds its world position from the chest
            std::memcpy(v.anchorLocal[s], a.rest, 12);
            v.anchorWritten[s] = true;
        }
    }
    Collide(v);
    double worst = 0, biggest = 0;
    std::string worstName;
    int pushed = 0;
    for (auto& [n, e] : expect) {
        void* node = f.node[n];
        const float* add = v.AddFor(node);
        Vec3 got = {0, 0, 0};
        if (add) {   // back to world: through the parent's axes
            void* parent = At<void*>(node, 0x38);
            for (int k = 0; k < 3; ++k) {
                const Vec3 ax = Row(parent, 0x80, k);
                got = {got.x + add[k] * ax.x, got.y + add[k] * ax.y, got.z + add[k] * ax.z};
            }
        }
        const Vec3 d = Sub(got, e);
        const double err = std::sqrt(Dot(d, d)) * 100, size = std::sqrt(Dot(e, e)) * 100;
        pushed += size > 0.01;
        if (size > biggest) biggest = size;
        if (err > worst) {
            worst = err;
            worstName = n;
        }
    }
    const bool ok = worst <= tolCm;
    printf("%-14s %-40s %2d bones pushed, largest %.2f cm, worst difference %.3f cm (%s)  %s\n", pose,
           hide.empty() ? (rebuildAnchor ? "(anchor rebuilt from chest)" : "") : "(no wrist/finger bones: fallbacks)", pushed,
           biggest, worst, worstName.c_str(), ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Lying on the left side at x2: gravity moves the left breast 1.6 cm and the right one 3.6 cm toward the floor (the
// left), 2 cm closer together by design.  Soft against soft must not undo that: no pushes at all in the rest pose.
static int RunSideGravity() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cases\\rest.txt", g_tests);
    Fake f;
    f.kidLists.reserve(512);
    std::map<std::string, Vec3> expect;
    if (!Load(path, f, expect, {})) return 1;
    ActorView v;
    v.com = f.node["COM"];
    void* pecs[2] = {f.node["L_Pecs"], f.node["R_Pecs"]};
    const Vec3 lw = Row(pecs[0], 0xB0, 0), rw = Row(pecs[1], 0xB0, 0);   // world positions
    Vec3 left = Sub(lw, rw);   // the character's left, roughly
    const float n = std::sqrt(Dot(left, left));
    left = {left.x / n, left.y / n, left.z / n};
    const float shift[2] = {0.016f, 0.036f};
    for (int s = 0; s < 2; ++s) {
        Anchor& a = v.anchors[s];
        a.node = pecs[s];
        a.com = v.com;
        a.rest = reinterpret_cast<const float*>(static_cast<char*>(a.node) + 0x70);
        void* chest = At<void*>(a.node, 0x38);
        for (int k = 0; k < 3; ++k) v.anchorLocal[s][k] = a.rest[k] + shift[s] * Dot(Row(chest, 0x80, k), left);
        v.anchorWritten[s] = true;
    }
    Collide(v);
    double biggest = 0;
    for (int i = 0; i < v.nAdd; ++i) biggest = std::fmax(biggest, std::sqrt(Dot({v.add[i][0], v.add[i][1], v.add[i][2]},
                                                                                 {v.add[i][0], v.add[i][1], v.add[i][2]})) * 100);
    const bool ok = biggest < 1e-3;
    printf("%-14s %-40s largest push %.3f cm  %s\n", "rest", "(lying on the left side, x2 gravity)", biggest, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Collisions against the gravity shapes: each shape at x2 put on the skeleton (anchors moved, bones added as
// PostUpdate does), then the plugin's own Collide: how much it pushes back, and where.
static int RunShapes(const char* pose) {
    struct Shape { const char* name; Vec3 anchor[2]; const Vec3* bones[2]; };
    static const Vec3 kZero[4] = {};
    const float half = 0.5f;
    const Shape shapes[] = {
        {"on the back (S1)", {kFaceUpSlump[0], kFaceUpSlump[1]}, {kSlumpBone, kSlumpBone}},
        {"left side (SD2)", {{0, 0, kLeftSide[0].z * half}, {0, 0, kLeftSide[1].z * half}}, {kSideDown, kSideUp}},
        {"right side (SD2)", {{0, 0, kRightSide[0].z * half}, {0, 0, kRightSide[1].z * half}}, {kSideUp, kSideDown}},
        {"face down (FD1d)", {{0, -0.5f, 0}, {0, -0.5f, 0}}, {kSquashBone, kSquashBone}},
        {"bent forward (H2)", {{0, 0.6f, 0}, {0, 0.6f, 0}}, {kHangBone, kHangBone}},
        {"none", {{0, 0, 0}, {0, 0, 0}}, {kZero, kZero}},
    };
    const float level = 2.0f;
    int fails = 0;
    for (const Shape& sh : shapes) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\cases\\%s.txt", g_tests, pose);
        Fake f;
        f.kidLists.reserve(512);
        std::map<std::string, Vec3> expect;
        if (!Load(path, f, expect, {})) return 1;
        ActorView v;
        v.com = f.node["COM"];
        const char* bn[4] = {"Upper", "Lower", "Inner", "Outer"};
        float added[2][4][3];
        for (int s = 0; s < 2; ++s) {
            Anchor& a = v.anchors[s];
            a.node = f.node[s ? "R_Pecs" : "L_Pecs"];
            a.com = v.com;
            const float* rest = reinterpret_cast<const float*>(static_cast<char*>(a.node) + 0x70);
            const float off[3] = {sh.anchor[s].x, sh.anchor[s].y, sh.anchor[s].z};
            for (int k = 0; k < 3; ++k) v.anchorLocal[s][k] = rest[k] + off[k] * level / 100;
            v.anchorWritten[s] = true;
            a.rest = rest;
            const float sgn = s == 0 ? 1.0f : -1.0f;
            for (int b = 0; b < 4; ++b) {
                a.bones[b] = f.node[std::string("TF_Breast_") + bn[b] + (s ? "_R" : "_L")];
                const Vec3& d = sh.bones[s][b];
                const float add[3] = {d.x * level / 100, d.y * level / 100, d.z * level / 100 * sgn};
                memcpy(added[s][b], add, 12);
                v.AddTo(a.bones[b], add);
            }
        }
        Collide(v);
        double worst = 0;
        std::string where;
        for (int s = 0; s < 2; ++s)
            for (int b = 0; b < 4; ++b) {
                const float* got = v.AddFor(v.anchors[s].bones[b]);
                double d2 = 0;
                for (int k = 0; k < 3; ++k) d2 += (got[k] - added[s][b][k]) * (got[k] - added[s][b][k]);
                if (std::sqrt(d2) * 100 > worst) {
                    worst = std::sqrt(d2) * 100;
                    where = std::string(bn[b]) + (s ? "_R" : "_L");
                }
            }
        double other = 0;   // pushes on the belly bones (the breast shapes shouldn't cause any)
        for (int i = 0; i < v.nAdd; ++i) {
            bool breast = false;
            for (int s = 0; s < 2; ++s)
                for (int b = 0; b < 4; ++b) breast |= v.addNode[i] == v.anchors[s].bones[b];
            if (!breast) other = std::fmax(other, std::sqrt(Dot({v.add[i][0], v.add[i][1], v.add[i][2]},
                                                                {v.add[i][0], v.add[i][1], v.add[i][2]})) * 100);
        }
        printf("%-14s %-22s collisions push the breasts back %.2f cm%s%s, other parts %.2f cm\n", pose, sh.name, worst,
               worst > 0.005 ? " at " : "", worst > 0.005 ? where.c_str() : "", other);
        if (!strcmp(sh.name, "none") && worst > 0.005) ++fails;   // standing with no shape must not push
    }
    return fails;
}

// [Gravity] bEnabled=0 (audit fix): the whole PostUpdate runs with gravity off.  The anchors must be left alone (the
// game's own correctives keep them) and the collisions must still push.
static int RunGravityOff() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cases\\arms_crossed.txt", g_tests);
    Fake f;
    f.kidLists.reserve(512);
    std::map<std::string, Vec3> expect;
    if (!Load(path, f, expect, {})) return 1;
    ActorView v;
    v.level = 2.0f;
    v.com = f.node["COM"];
    const char* bn[4] = {"Upper", "Lower", "Inner", "Outer"};
    float rest[2][3];
    for (int s = 0; s < 2; ++s) {
        Anchor& a = v.anchors[s];
        a.node = f.node[s ? "R_Pecs" : "L_Pecs"];
        a.com = v.com;
        memcpy(rest[s], static_cast<char*>(a.node) + 0x70, 12);
        a.rest = rest[s];
        for (int b = 0; b < 4; ++b) a.bones[b] = f.node[std::string("TF_Breast_") + bn[b] + (s ? "_R" : "_L")];
    }
    // turn the chest as if lying on the left side: with gravity on, the anchors would move
    const Vec3 chestRows[3] = {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}};
    for (int k = 0; k < 3; ++k) memcpy(f.node["C_Chest"] + 0x80 + 16 * k, &chestRows[k], 12);
    g_cfg.enabled = false;
    PostUpdate(v, 2);
    g_cfg.enabled = true;
    double moved = 0, pushed = 0;
    for (int s = 0; s < 2; ++s) {
        const float* t = reinterpret_cast<const float*>(static_cast<char*>(v.anchors[s].node) + 0x70);
        for (int k = 0; k < 3; ++k) moved = std::fmax(moved, std::fabs(t[k] - rest[s][k]) * 100);
    }
    for (auto& [n, e] : expect) {   // the breast bones carry their pushes now (AddOnTop wrote them)
        const float* l = reinterpret_cast<const float*>(f.node[n] + 0x70);
        (void)l;
        pushed = std::fmax(pushed, std::sqrt(Dot(e, e)) * 100);
    }
    double written = 0;
    for (int i = 0; i < v.nAdd; ++i)
        written = std::fmax(written, std::sqrt(Dot({v.add[i][0], v.add[i][1], v.add[i][2]}, {v.add[i][0], v.add[i][1], v.add[i][2]})) * 100);
    const bool ok = moved < 1e-6 && written > 1.0;
    printf("%-14s %-40s anchors moved %.4f cm, collision pushes %.2f cm (design %.2f)  %s\n", "arms_crossed",
           "(gravity switched off)", moved, written, pushed, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// Several frames with the plugin's own writes (AddOnTop) in between and springs that don't write (they skipped the
// frame): the push must stay the same, not add up or flicker; once switched off, the bones must return to their base.
static int RunFrames(const char* pose) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cases\\%s.txt", g_tests, pose);
    Fake f;
    f.kidLists.reserve(512);
    std::map<std::string, Vec3> expect;
    if (!Load(path, f, expect, {})) return 1;
    std::map<std::string, std::vector<float>> base;
    for (auto& [n, e] : expect) {
        const float* l = reinterpret_cast<const float*>(f.node[n] + 0x70);
        base[n] = {l[0], l[1], l[2]};
    }
    auto frame = [&](bool collide) {
        ActorView v;
        v.com = f.node["COM"];
        for (int s = 0; s < 2; ++s) v.anchors[s].node = f.node[s ? "R_Pecs" : "L_Pecs"];
        if (collide) Collide(v);
        for (auto& [n, e] : expect) {
            const float* add = v.AddFor(f.node[n]);
            const float zero[3] = {0, 0, 0};
            AddOnTop(f.node[n], add ? add : zero);
        }
    };
    double drift = 0, back = 0;
    std::map<std::string, std::vector<float>> first;
    for (int i = 0; i < 3; ++i) {
        frame(true);
        for (auto& [n, e] : expect) {
            const float* l = reinterpret_cast<const float*>(f.node[n] + 0x70);
            if (i == 0) first[n] = {l[0], l[1], l[2]};
            for (int k = 0; k < 3; ++k) drift = std::fmax(drift, std::fabs(l[k] - first[n][k]) * 100.0);
        }
    }
    frame(false);
    for (auto& [n, e] : expect) {
        const float* l = reinterpret_cast<const float*>(f.node[n] + 0x70);
        for (int k = 0; k < 3; ++k) back = std::fmax(back, std::fabs(l[k] - base[n][k]) * 100.0);
    }
    double moved = 0;
    for (auto& [n, e] : expect)
        for (int k = 0; k < 3; ++k) moved = std::fmax(moved, std::fabs(first[n][k] - base[n][k]) * 100.0);
    const bool ok = moved > 1.0 && drift < 1e-4 && back < 1e-4;
    printf("%-14s %-40s moved %.2f cm, change over 3 frames %.5f cm, left after switching off %.5f cm  %s\n", pose,
           "(frames, springs skipping)", moved, drift, back, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// A skeleton whose child lists the plugin can't find: no pushes, no crash, the bone map is logged, and it will try
// again later instead of giving up.
static int RunNoList() {
    const int saved = g_fakeLayout;
    g_fakeLayout = -1;   // children not stored anywhere
    g_layoutState = 0;
    g_layoutTries = 0;
    g_layoutRetryMs = 0;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cases\\arms_crossed.txt", g_tests);
    Fake f;
    f.kidLists.reserve(512);
    std::map<std::string, Vec3> expect;
    if (!Load(path, f, expect, {})) return 1;
    ActorView v;
    v.com = f.node["COM"];
    const char* bone[4] = {"Upper", "Lower", "Inner", "Outer"};
    for (int s = 0; s < 2; ++s) {
        v.anchors[s].node = f.node[s ? "R_Pecs" : "L_Pecs"];
        v.anchors[s].com = v.com;
        for (int b = 0; b < 4; ++b) v.anchors[s].bones[b] = f.node[std::string("TF_Breast_") + bone[b] + (s ? "_R" : "_L")];
    }
    Collide(v);
    const bool ok = v.nAdd == 0 && g_layoutState.load() == 0 && g_layoutTries.load() == 1;
    printf("%-14s %-40s pushes %d, will try again: %s  %s\n", "arms_crossed", "(no child list anywhere)", v.nAdd,
           g_layoutState.load() == 0 ? "yes" : "no", ok ? "OK" : "FAIL");
    g_fakeLayout = saved;
    return ok ? 0 : 1;
}

// [Collision] bGlutes=0: the hands on the glutes push nothing there; every other part is as designed.
static int RunGlutesOff() {
    ResetOthers();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cases\\hands_on_glutes.txt", g_tests);
    Fake f;
    f.kidLists.reserve(512);
    std::map<std::string, Vec3> expect;
    if (!Load(path, f, expect, {})) return 1;
    ActorView v;
    v.com = f.node["COM"];
    for (int s = 0; s < 2; ++s) v.anchors[s].node = f.node[s ? "R_Pecs" : "L_Pecs"];
    g_cfg.glutes = false;
    Collide(v);
    g_cfg.glutes = true;
    double glute = 0, otherWorst = 0;
    for (auto& [n, e] : expect) {
        const float* add = v.AddFor(f.node[n]);
        const double got = add ? std::sqrt(add[0] * add[0] + add[1] * add[1] + add[2] * add[2]) * 100 : 0.0;
        if (n.rfind("TF_Glute_", 0) == 0) {
            glute = std::fmax(glute, got);
        } else {
            otherWorst = std::fmax(otherWorst, std::fabs(got - std::sqrt(Dot(e, e)) * 100));
        }
    }
    const bool ok = glute == 0 && otherWorst < 0.01;
    printf("%-14s %-40s glute pushes %.3f cm, other parts off design by %.3f cm  %s\n", "hands_on_glutes",
           "(glutes switched off)", glute, otherWorst, ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

// A rest-pose body turned to face the other one (180 deg about the vertical through pivot) and moved forward by
// `ahead` metres along +Y (the body's front).  Turns world axes and positions; local translations stay.
static void FaceAndMove(Fake& f, const Vec3& pivot, float ahead) {
    for (auto& [n, node] : f.node) {
        for (int r = 0; r < 3; ++r) {
            float* row = reinterpret_cast<float*>(node + 0x80 + 16 * r);
            row[0] = -row[0];
            row[1] = -row[1];
        }
        float* p = reinterpret_cast<float*>(node + 0xB0);
        p[0] = 2 * pivot.x - p[0];
        p[1] = 2 * pivot.y - p[1] + ahead;
    }
}

static std::uint32_t OtherHitsAll() {
    std::uint32_t n = 0;
    for (auto& w : g_otherHits)
        for (auto& g : w)
            for (auto& k : g) n += k.load();
    return n;
}
static void ClearOtherHits() {
    for (auto& w : g_otherHits)
        for (auto& g : w)
            for (auto& k : g) k.store(0);
    for (auto& m : g_deepMm) m.store(0);
}

// Two bodies.  A: the rest pose.  B: the same body turned to face A, its breasts against A's (a hug).
// Each shares its shapes and checks the other.  Measure only: no pushes, contacts counted.  Pushing on: A's breasts go
// back toward A's chest and B's mirror them.  Then what must be ignored: an entry nobody updated for 0.2 s, one being
// written, the same character, a body 3 m away.  Last, B as a body that isn't A-Body (sharing through the light
// path: limbs, chest, pelvis, no soft parts), close enough for its chest to meet A's breasts.
static int RunOtherBodies() {
    ResetOthers();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cases\\rest.txt", g_tests);
    Fake fa, fb;
    fa.kidLists.reserve(512);
    fb.kidLists.reserve(512);
    std::map<std::string, Vec3> ea, eb;
    if (!Load(path, fa, ea, {}) || !Load(path, fb, eb, {})) return 1;
    const Vec3 pivot = Row(fa.node["COM"], 0xB0, 0);
    FaceAndMove(fb, pivot, 0.34f);
    const char* bn[4] = {"Upper", "Lower", "Inner", "Outer"};
    static char actorA[16], actorB[16];   // stand-ins for the two characters (only compared)
    // A plays the player (path C), B an NPC (path B): A's contacts count as yours, B's as an NPC's touched by you
    auto view = [&](Fake& f, void* actor) {
        ActorView v;
        v.com = f.node["COM"];
        v.actor = actor;
        v.site = &f == &fa ? 2 : 1;
        for (int s = 0; s < 2; ++s) {
            Anchor& a = v.anchors[s];
            a.node = f.node[s ? "R_Pecs" : "L_Pecs"];
            a.com = v.com;
            a.rest = reinterpret_cast<const float*>(static_cast<char*>(a.node) + 0x70);
            for (int b = 0; b < 4; ++b) a.bones[b] = f.node[std::string("TF_Breast_") + bn[b] + (s ? "_R" : "_L")];
        }
        return v;
    };
    auto push = [&](ActorView& v, Fake& f, const char* bone) {   // a bone's push in world, from its parent's axes
        const float* add = v.AddFor(f.node[bone]);
        Vec3 got = {0, 0, 0};
        if (!add) return got;
        void* parent = At<void*>(f.node[bone], 0x38);
        for (int k = 0; k < 3; ++k) {
            const Vec3 ax = Row(parent, 0x80, k);
            got = {got.x + add[k] * ax.x, got.y + add[k] * ax.y, got.z + add[k] * ax.z};
        }
        return got;
    };
    auto biggest = [](const ActorView& v) {
        double m = 0;
        for (int i = 0; i < v.nAdd; ++i)
            m = std::fmax(m, std::sqrt(v.add[i][0] * v.add[i][0] + v.add[i][1] * v.add[i][1] + v.add[i][2] * v.add[i][2]));
        return m * 100;
    };
    int fails = 0;
    auto report = [&](const char* what, bool ok, const char* detail) {
        printf("%-14s %-40s %s  %s\n", "two bodies", what, detail, ok ? "OK" : "FAIL");
        fails += ok ? 0 : 1;
    };
    char d[200];

    // measure only (the shipped setting)
    ClearOtherHits();
    {
        ActorView a1 = view(fa, actorA);
        Collide(a1);   // A shares (sees nobody yet)
        ActorView b1 = view(fb, actorB);
        Collide(b1);   // B shares and sees A
        ActorView a2 = view(fa, actorA);
        Collide(a2);   // A sees B
        const std::uint32_t yours = g_otherHits[kWhoYours][0][kOtherSoft].load();
        const std::uint32_t byYou = g_otherHits[kWhoByYou][0][kOtherSoft].load();
        std::uint32_t npcs = 0;
        for (auto& g : g_otherHits[kWhoNpcs])
            for (auto& k : g) npcs += k.load();
        snprintf(d, sizeof(d), "breast<-other's soft: yours %u, NPC by you %u, NPC by NPC %u; largest push A %.4f, B %.4f cm",
                 yours, byYou, npcs, biggest(a2), biggest(b1));
        report("(hug, measure only)", yours > 0 && byYou > 0 && npcs == 0 && biggest(a2) < 1e-3 && biggest(b1) < 1e-3, d);
        // the deepest of yours, in detail: a breast bone against B's breast; the COMs 0.34 m apart; not moved
        DeepContact c;
        {
            std::lock_guard<std::mutex> lock(g_deepMutex);
            c = g_deep[kWhoYours];
        }
        const bool deepOk = g_deepMm[kWhoYours].load() > 0 && !strncmp(c.bone, "TF_Breast_", 10) && c.kind == kOtherSoft &&
                            std::fabs(c.apart - 0.34f) < 0.005f && std::fabs(c.up) < 0.005f && c.ageMs < 100 &&
                            c.shiftCm < 1e-3f && c.abody;
        snprintf(d, sizeof(d), "%s <- other's %d, %.1f cm deep; COMs %.3f m apart (%.3f up); %.0f ms old, moved %.2f cm",
                 c.bone, c.kind, c.depth * 100, c.apart, c.up, c.ageMs, c.shiftCm);
        report("(deepest, yours)", deepOk, d);
    }
    // pushing on
    g_cfg.othersMeasureOnly = false;
    {
        ActorView a2 = view(fa, actorA);
        Collide(a2);
        ActorView b2 = view(fb, actorB);
        Collide(b2);
        const Vec3 pa = push(a2, fa, "TF_Breast_Lower_L"), pb = push(b2, fb, "TF_Breast_Lower_L");
        const double la = std::sqrt(Dot(pa, pa)) * 100, lb = std::sqrt(Dot(pb, pb)) * 100;
        const bool backA = la > 0.5 && -pa.y * 100 > 0.9 * la;   // A faces +Y: back toward its chest is -Y
        const bool backB = lb > 0.5 && pb.y * 100 > 0.9 * lb;    // B faces -Y
        snprintf(d, sizeof(d), "A's lower left breast %.2f cm back, B's %.2f cm back (mirror)", la, lb);
        report("(hug, pushing)", backA && backB && std::fabs(la - lb) < 0.05 && la <= g_cfg.maxPush[0] * 100 + 1e-3, d);
    }
    g_cfg.othersMeasureOnly = true;

    // ignored: stale, mid-write, the same character, far away
    auto contactsSeen = [&](void* actor) {
        ClearOtherHits();
        ActorView a3 = view(fa, actor);
        Collide(a3);
        return OtherHitsAll();
    };
    OtherEntry* entryB = nullptr;
    for (OtherEntry& e : g_others)
        if (e.key.load() == fb.node["COM"]) entryB = &e;
    if (!entryB) {
        report("(B's entry)", false, "B never shared");
    } else {
        const std::int64_t tick = entryB->tick.load();
        entryB->tick.store(tick - QpcFreq() / 5);
        const std::uint32_t stale = contactsSeen(actorA);
        // three frames old (50 ms at 60 fps) is passed over, and counted; 10 ms old at 1 m/s toward A is
        // moved 1 cm forward (by its whole age)
        const std::uint64_t tooOldBefore = g_otherTooOld.load();
        entryB->tick.store(Qpc() - QpcFreq() / 20);
        const std::uint32_t threeFrames = contactsSeen(actorA);
        const bool tooOldCounted = g_otherTooOld.load() > tooOldBefore;
        entryB->vel = {0, -1.0f, 0};
        entryB->tick.store(Qpc() - QpcFreq() / 100);
        const std::uint32_t moving = contactsSeen(actorA);
        DeepContact c;
        {
            std::lock_guard<std::mutex> lock(g_deepMutex);
            c = g_deep[kWhoYours];
        }
        snprintf(d, sizeof(d), "3 frames old: %u contacts (counted as too old: %s); 10 ms old at 1 m/s: %u contacts, moved "
                 "%.2f cm, COMs %.3f m apart", threeFrames, tooOldCounted ? "yes" : "no", moving, c.shiftCm, c.apart);
        report("(lag: two frames at most)", threeFrames == 0 && tooOldCounted && moving > 0 &&
                   std::fabs(c.shiftCm - 1.0f) < 0.15f && std::fabs(c.apart - (0.34f - c.shiftCm / 100)) < 0.003f, d);
        entryB->vel = {0, 0, 0};
        entryB->tick.store(Qpc());
        const std::uint64_t tornBefore = g_otherTorn.load();
        entryB->seq.fetch_add(1);   // as if B were writing right now
        const std::uint32_t writing = contactsSeen(actorA);
        const bool tornCounted = g_otherTorn.load() > tornBefore;
        entryB->seq.fetch_add(1);
        const std::uint32_t same = contactsSeen(actorB);   // A with B's character: the same body to the plugin
        const std::uint32_t fresh = contactsSeen(actorA);
        snprintf(d, sizeof(d), "contacts: 0.2 s old %u, being written %u, same character %u; fresh again %u", stale,
                 writing, same, fresh);
        report("(entries to ignore)", stale == 0 && writing == 0 && tornCounted && same == 0 && fresh > 0, d);
        FaceAndMove(fb, pivot, 0.0f);   // turn B back round...
        FaceAndMove(fb, pivot, 3.0f);   // ...and face A again from 3 m away
        ActorView b4 = view(fb, actorB);
        Collide(b4);
        const std::uint32_t farHits = contactsSeen(actorA);   // ("far" is a windows.h macro)
        snprintf(d, sizeof(d), "contacts with B 3 m away: %u", farHits);
        report("(far body)", farHits == 0, d);
    }

    // B as a body that isn't A-Body: shares through the light path, 22 cm ahead so its chest meets A's breasts
    {
        ResetOthers();
        Fake fc;
        fc.kidLists.reserve(512);
        std::map<std::string, Vec3> ec;
        Load(path, fc, ec, {});
        FaceAndMove(fc, pivot, 0.22f);
        ActorView a5 = view(fa, actorA);
        Collide(a5);   // A shares first: the light path only runs near an A-Body body
        ShareOtherBody(fc.node["HumanExportRoot"], actorB);
        bool shared = false, soft = false;
        for (OtherEntry& e : g_others)
            if (e.key.load() == fc.node["COM"]) {
                shared = !e.abody;
                for (int i = 0; i < e.n; ++i) soft |= e.shape[i].kind == kOtherSoft;
            }
        ClearOtherHits();
        ActorView a6 = view(fa, actorA);
        Collide(a6);
        const std::uint32_t chest = g_otherHits[kWhoYours][0][kOtherChest].load();
        snprintf(d, sizeof(d), "shared as other body %s (soft parts %s), breast<-other's chest %u contacts",
                 shared ? "yes" : "no", soft ? "yes" : "no", chest);
        report("(body without A-Body)", shared && !soft && chest > 0, d);
        // and with no A-Body body near, it doesn't share at all
        ResetOthers();
        ShareOtherBody(fc.node["HumanExportRoot"], actorB);
        bool any = false;
        for (OtherEntry& e : g_others) any |= e.key.load() != nullptr;
        report("(no A-Body body near)", !any, any ? "shared anyway" : "nothing shared");
    }
    ResetOthers();
    return fails;
}

// fake characters for the life state (Actor::IsDead, vtable slot 269: bits 17-20 at +0xF8).  While one of
// these lives, the plugin reads the fake classes; Set gives a fake character its class and life state.
struct FakeLives {
    static inline std::uintptr_t actorVt[300], playerVt[300];
    static constexpr std::uint8_t kIsDead[16] = {0x44, 0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00, 0x41,
                                                 0x8B, 0xC0, 0x25, 0x00, 0x00, 0x1E, 0x00, 0x84};
    std::uintptr_t savedA, savedP;
    bool savedOk;
    FakeLives() : savedA(g_actorVt), savedP(g_playerVt), savedOk(g_deadOk) {
        g_actorVt = reinterpret_cast<std::uintptr_t>(actorVt);
        g_playerVt = reinterpret_cast<std::uintptr_t>(playerVt);
        actorVt[kIsDeadSlot] = playerVt[kIsDeadSlot] = reinterpret_cast<std::uintptr_t>(kIsDead);
        CheckDeadReading();
    }
    ~FakeLives() {
        g_actorVt = savedA;
        g_playerVt = savedP;
        g_deadOk = savedOk;
    }
    static void Set(char* obj, int state, bool player = false) {
        const std::uintptr_t vt = reinterpret_cast<std::uintptr_t>(player ? playerVt : actorVt);
        memcpy(obj, &vt, 8);
        const std::uint32_t flags = static_cast<std::uint32_t>(state) << 17;
        memcpy(obj + 0xF8, &flags, 4);
    }
};

// dead bodies, the lower legs and feet, and the cap on pushes from other bodies.  The hug again (A the
// player, B an NPC facing A 0.34 m away), then B dead; every body's calves and feet among its shapes; a calf and a dead
// body's foot put through A's breast; the game's life state read from fake characters.
static int RunDeadBodies() {
    ResetOthers();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cases\\rest.txt", g_tests);
    Fake fa, fb, fc;
    fa.kidLists.reserve(512);
    fb.kidLists.reserve(512);
    fc.kidLists.reserve(512);
    std::map<std::string, Vec3> ea, eb, ec;
    if (!Load(path, fa, ea, {}) || !Load(path, fb, eb, {}) || !Load(path, fc, ec, {})) return 1;
    const Vec3 pivot = Row(fa.node["COM"], 0xB0, 0);
    FaceAndMove(fb, pivot, 0.34f);
    FaceAndMove(fc, pivot, 0.22f);
    const char* bn[4] = {"Upper", "Lower", "Inner", "Outer"};
    static char actorA[16], actorB[16], actorC[16], actorD[16];
    auto view = [&](Fake& f, void* actor, int site, bool dead) {
        ActorView v;
        v.com = f.node["COM"];
        v.actor = actor;
        v.site = site;
        v.dead = dead;
        for (int s = 0; s < 2; ++s) {
            Anchor& a = v.anchors[s];
            a.node = f.node[s ? "R_Pecs" : "L_Pecs"];
            a.com = v.com;
            a.rest = reinterpret_cast<const float*>(static_cast<char*>(a.node) + 0x70);
            for (int b = 0; b < 4; ++b) a.bones[b] = f.node[std::string("TF_Breast_") + bn[b] + (s ? "_R" : "_L")];
        }
        return v;
    };
    auto entryOf = [&](Fake& f) -> OtherEntry* {
        for (OtherEntry& e : g_others)
            if (e.key.load() == f.node["COM"]) return &e;
        return nullptr;
    };
    auto hits = [](int who) {
        std::uint32_t n = 0;
        for (auto& g : g_otherHits[who])
            for (auto& k : g) n += k.load();
        return n;
    };
    auto deepOf = [](int who) {
        std::lock_guard<std::mutex> lock(g_deepMutex);
        return g_deep[who];
    };
    auto push = [&](ActorView& v, Fake& f, const char* bone) {   // a bone's push in world, from its parent's axes
        const float* add = v.AddFor(f.node[bone]);
        Vec3 got = {0, 0, 0};
        if (!add) return got;
        void* parent = At<void*>(f.node[bone], 0x38);
        for (int k = 0; k < 3; ++k) {
            const Vec3 ax = Row(parent, 0x80, k);
            got = {got.x + add[k] * ax.x, got.y + add[k] * ax.y, got.z + add[k] * ax.z};
        }
        return got;
    };
    auto near3 = [](const Vec3& a, const Vec3& b) { return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) < 1e-5f; };
    int fails = 0;
    auto report = [&](const char* what, bool ok, const char* detail) {
        printf("%-14s %-40s %s  %s\n", "dead bodies", what, detail, ok ? "OK" : "FAIL");
        fails += ok ? 0 : 1;
    };
    char d[240];

    // every body shares its lower legs (knee to ankle) and feet (ankle to toe), left then right
    {
        ActorView a1 = view(fa, actorA, 2, false);
        Collide(a1);
        OtherEntry* e = entryOf(fa);
        int calves = 0, feet = 0;
        bool where = e != nullptr;
        for (int i = 0; e && i < e->n; ++i) {
            const OtherShape& s = e->shape[i];
            if (s.kind == kOtherCalf) {
                const char* side = calves++ ? "R_" : "L_";
                where &= near3(s.a, Row(fa.node[std::string(side) + "Calf"], 0xB0, 0)) &&
                         near3(s.b, Row(fa.node[std::string(side) + "Foot"], 0xB0, 0)) && std::fabs(s.r - kCalfRadius) < 1e-6f;
            } else if (s.kind == kOtherFoot) {
                const char* side = feet++ ? "R_" : "L_";
                where &= near3(s.a, Row(fa.node[std::string(side) + "Foot"], 0xB0, 0)) &&
                         near3(s.b, Row(fa.node[std::string(side) + "Toe"], 0xB0, 0)) && std::fabs(s.r - kFootRadius) < 1e-6f;
            }
        }
        snprintf(d, sizeof(d), "%d shapes shared, %d calves and %d feet, on the knee/ankle/toe bones at their radii: %s",
                 e ? e->n : 0, calves, feet, where ? "yes" : "no");
        report("(calves and feet, A-Body)", calves == 2 && feet == 2 && where, d);
        ShareOtherBody(fc.node["HumanExportRoot"], actorC);   // near A: the light path shares
        OtherEntry* c = entryOf(fc);
        calves = feet = 0;
        bool radii = c != nullptr;
        for (int i = 0; c && i < c->n; ++i) {
            calves += c->shape[i].kind == kOtherCalf;
            feet += c->shape[i].kind == kOtherFoot;
            if (c->shape[i].kind == kOtherCalf) radii &= std::fabs(c->shape[i].r - kCalfRadius * kOtherLimbScale) < 1e-6f;
            if (c->shape[i].kind == kOtherFoot) radii &= std::fabs(c->shape[i].r - kFootRadius * kOtherLimbScale) < 1e-6f;
        }
        snprintf(d, sizeof(d), "%d shapes shared, %d calves and %d feet, x%.2f: %s", c ? c->n : 0, calves, feet,
                 kOtherLimbScale, radii ? "yes" : "no");
        report("(calves and feet, not A-Body)", calves == 2 && feet == 2 && radii && c && c->n == 2 * kNumColliders + 6, d);
    }

    // B dead: its contacts and A's with it count as "with dead bodies", the deepest says which one is dead
    ResetOthers();
    ClearOtherHits();
    {
        const std::uint64_t deadShares = g_otherSharedDead.load();
        ActorView b1 = view(fb, actorB, 1, true);
        Collide(b1);   // dead B shares
        OtherEntry* eB = entryOf(fb);
        const bool markedDead = eB && eB->dead && eB->abody && g_otherSharedDead.load() > deadShares;
        ActorView a2 = view(fa, actorA, 2, false);
        Collide(a2);   // A sees dead B
        const std::uint32_t deadA = hits(kWhoDead), elseA = hits(kWhoYours) + hits(kWhoByYou) + hits(kWhoNpcs);
        const DeepContact ca = deepOf(kWhoDead);
        double biggestA = 0;
        for (int i = 0; i < a2.nAdd; ++i)
            biggestA = std::fmax(biggestA, std::fabs(a2.add[i][0]) + std::fabs(a2.add[i][1]) + std::fabs(a2.add[i][2]));
        snprintf(d, sizeof(d), "B shared as dead %s; A: %u contacts with dead bodies, %u elsewhere; deepest: %s <- %s (%s), "
                 "pushed %.4f cm (measure only)", markedDead ? "yes" : "no", deadA, elseA, ca.bone,
                 ca.otherDead ? "the dead body" : "a living body", ca.selfDead ? "this one dead" : "this one alive", biggestA * 100);
        report("(dead NPC, the player near)", markedDead && deadA > 0 && elseA == 0 && ca.otherDead && !ca.selfDead &&
                   biggestA < 1e-5, d);
        ClearOtherHits();
        ActorView b2 = view(fb, actorB, 1, true);
        Collide(b2);   // dead B sees A
        const std::uint32_t deadB = hits(kWhoDead), elseB = hits(kWhoYours) + hits(kWhoByYou) + hits(kWhoNpcs);
        const DeepContact cb = deepOf(kWhoDead);
        snprintf(d, sizeof(d), "B: %u contacts with dead bodies, %u elsewhere; deepest: this one %s, the other %s", deadB,
                 elseB, cb.selfDead ? "dead" : "alive", cb.otherDead ? "dead" : "alive");
        report("(the dead body's own contacts)", deadB > 0 && elseB == 0 && cb.selfDead && !cb.otherDead, d);
    }

    // a dead body lying still is used from older shares, and not moved forward; a falling one, a too old one and a
    // living one 0.5 s old are passed over
    {
        OtherEntry* eB = entryOf(fb);
        auto seen = [&](float ageS, Vec3 vel, bool dead) {
            eB->vel = vel;
            eB->dead = dead;
            eB->tick.store(Qpc() - static_cast<std::int64_t>(ageS * static_cast<float>(QpcFreq())));
            ClearOtherHits();
            ActorView v = view(fa, actorA, 2, false);
            Collide(v);
            return OtherHitsAll();
        };
        const std::uint32_t still = seen(0.5f, {0, 0, 0}, true);
        const std::uint32_t slow = seen(0.5f, {0, -0.1f, 0}, true);   // 0.1 m/s: lying still
        const float slowShift = deepOf(kWhoDead).shiftCm;
        const std::uint32_t falling = seen(0.5f, {0, -1.0f, 0}, true);
        const std::uint32_t old = seen(0.8f, {0, 0, 0}, true);
        const std::uint32_t living = seen(0.5f, {0, 0, 0}, false);
        g_cfg.deadMaxAge = 0.3f;
        const std::uint32_t shorter = seen(0.5f, {0, 0, 0}, true);
        g_cfg.deadMaxAge = 0.75f;
        eB->vel = {0, 0, 0};
        eB->dead = true;
        eB->tick.store(Qpc());
        snprintf(d, sizeof(d), "0.5 s old: still %u, at 0.1 m/s %u (moved %.2f cm), falling 1 m/s %u; 0.8 s old %u; "
                 "living 0.5 s old %u; limit 0.3 s: %u", still, slow, slowShift, falling, old, living, shorter);
        report("(dead body lying still: older shares)", still > 0 && slow > 0 && std::fabs(slowShift) < 1e-3f &&
                   falling == 0 && old == 0 && living == 0 && shorter == 0, d);
    }

    // pushing: other bodies push a bone at most fMaxPushOtherCm (3 cm), inside the breast cap (5 cm)
    {
        g_cfg.othersMeasureOnly = false;
        if (OtherEntry* eB = entryOf(fb)) eB->dead = false;
        ActorView a3 = view(fa, actorA, 2, false);
        Collide(a3);
        const Vec3 p3 = push(a3, fa, "TF_Breast_Lower_L");
        const double capped = std::sqrt(Dot(p3, p3)) * 100;
        g_cfg.maxPushOther = 0.10f;
        ActorView a4 = view(fa, actorA, 2, false);
        Collide(a4);
        const Vec3 p4 = push(a4, fa, "TF_Breast_Lower_L");
        const double full = std::sqrt(Dot(p4, p4)) * 100;
        g_cfg.maxPushOther = 0.03f;
        g_cfg.othersMeasureOnly = true;
        snprintf(d, sizeof(d), "A's lower left breast %.2f cm back (cap 3 cm); with a 10 cm cap %.2f cm (breast cap %.0f)",
                 capped, full, g_cfg.maxPush[0] * 100);
        report("(pushes from other bodies capped)", std::fabs(capped - 3.0) < 0.01 && -p3.y * 100 > 0.9 * capped &&
                   full > capped + 0.5 && full <= g_cfg.maxPush[0] * 100 + 1e-3, d);
    }

    // a calf (a living NPC's) and a dead body's foot, each put 2 cm in front of A's lower left breast, side to side
    {
        ResetOthers();
        ClearOtherHits();
        const Vec3 p = Row(fa.node["TF_Breast_Lower_L"], 0xB0, 0);
        static char keyCalf[64], keyFoot[64];
        const OtherShape calf = {{p.x - 0.15f, p.y + 0.02f, p.z}, {p.x + 0.15f, p.y + 0.02f, p.z}, kCalfRadius, kOtherCalf, 0, 0};
        const OtherShape foot = {{p.x - 0.10f, p.y + 0.02f, p.z}, {p.x + 0.10f, p.y + 0.02f, p.z}, kFootRadius, kOtherFoot, 0, 0};
        Share(keyCalf, actorC, p, 0.5f, &calf, 1, false, false, false);
        Share(keyFoot, actorD, p, 0.5f, &foot, 1, false, false, true);
        ActorView a5 = view(fa, actorA, 2, false);
        Collide(a5);
        const std::uint32_t byCalf = g_otherHits[kWhoYours][0][kOtherCalf].load();
        const std::uint32_t byFoot = g_otherHits[kWhoDead][0][kOtherFoot].load();
        g_cfg.othersMeasureOnly = false;
        ActorView a6 = view(fa, actorA, 2, false);
        Collide(a6);
        g_cfg.othersMeasureOnly = true;
        const Vec3 pp = push(a6, fa, "TF_Breast_Lower_L");
        const double pushed = std::sqrt(Dot(pp, pp)) * 100;
        snprintf(d, sizeof(d), "breast <- calf %u contacts (yours), <- dead body's foot %u (with dead bodies); pushed %.2f cm "
                 "back", byCalf, byFoot, pushed);
        report("(a calf and a foot)", byCalf > 0 && byFoot > 0 && pushed > 0.5 && -pp.y * 100 > 0.9 * pushed &&
                   pushed <= 3.0 + 1e-3, d);
    }

    // dead bodies off (bDeadBodies=0): a dead body shares nothing, a dead entry is not used, a dead body that isn't
    // A-Body shares nothing; on again, it shares, marked dead; alive again, it shares as living (the other
    // body's life state is read by ShareOtherBody itself, from a fake dead character)
    {
        FakeLives lives;
        alignas(16) static char deadC[0x200];
        FakeLives::Set(deadC, 2);
        g_cfg.deadBodies = false;
        ResetOthers();
        ActorView b3 = view(fb, actorB, 1, true);
        Collide(b3);
        const bool sharedDead = entryOf(fb) != nullptr;
        ActorView b4 = view(fb, actorB, 1, false);
        Collide(b4);   // B alive shares...
        if (OtherEntry* eB = entryOf(fb)) eB->dead = true;   // ...and its entry is then marked dead
        ClearOtherHits();
        ActorView a7 = view(fa, actorA, 2, false);
        Collide(a7);   // A shares (for the light path below) and must not use dead B
        const std::uint32_t used = OtherHitsAll();
        ShareOtherBody(fc.node["HumanExportRoot"], deadC);
        const bool sharedOtherDead = entryOf(fc) != nullptr;
        g_cfg.deadBodies = true;
        ShareOtherBody(fc.node["HumanExportRoot"], deadC);
        OtherEntry* eC = entryOf(fc);
        const bool sharedOn = eC && eC->dead && !eC->abody;
        FakeLives::Set(deadC, 0);
        ShareOtherBody(fc.node["HumanExportRoot"], deadC);
        const bool livingAgain = eC && !eC->dead;
        snprintf(d, sizeof(d), "off: dead B shared %s, dead entry used %u contacts, dead other body shared %s; on: shared "
                 "as dead %s; alive again: shared as living %s", sharedDead ? "yes" : "no", used,
                 sharedOtherDead ? "yes" : "no", sharedOn ? "yes" : "no", livingAgain ? "yes" : "no");
        report("(bDeadBodies=0)", !sharedDead && used == 0 && !sharedOtherDead && sharedOn && livingAgain, d);
    }

    // the hook itself (path B) on a body that isn't A-Body, 22 cm in front of A (an A-Body body): the game's
    // update and the modifier list are stand-ins (no A-Body modifiers: Collect says not ours).  After the first update
    // (the one-time COM and bone searches), 2,000 more ask Windows nothing; the character
    // dead, the body is shared as dead.
    {
        FakeLives lives;
        alignas(16) static char npc[0x200];
        FakeLives::Set(npc, 0);
        ResetOthers();
        ActorView a8 = view(fa, actorA, 2, false);
        Collide(a8);   // A shares: the other body is near an A-Body body
        const GetModifierExtraFn savedExtra = g_getExtra;
        const ModifierUpdate savedOriginal = g_original;
        g_getExtra = [](void*) -> void* { return nullptr; };
        g_original = [](void*, void*) {};
        void* root = fc.node["HumanExportRoot"];
        Hooked<1>(root, nullptr, npc);   // first update: searches
        const std::uint64_t vq0 = g_vqCalls.load(), life0 = g_lifeReads.load(), rd0 = g_readableCalls.load();
        for (int i = 0; i < 2000; ++i) Hooked<1>(root, nullptr, npc);
        const std::uint64_t vq = g_vqCalls.load() - vq0, life = g_lifeReads.load() - life0, rd = g_readableCalls.load() - rd0;
        OtherEntry* eC = entryOf(fc);
        const bool sharedAlive = eC && !eC->dead && !eC->abody;
        FakeLives::Set(npc, 2);
        Hooked<1>(root, nullptr, npc);
        const bool sharedDead = eC && eC->dead;
        FakeLives::Set(npc, 0);
        g_getExtra = savedExtra;
        g_original = savedOriginal;
        snprintf(d, sizeof(d), "2,000 updates: %llu memory checks (%llu asked Windows), %llu life states read; shared alive "
                 "%s, then dead %s", static_cast<unsigned long long>(rd), static_cast<unsigned long long>(vq),
                 static_cast<unsigned long long>(life),
                 sharedAlive ? "yes" : "no", sharedDead ? "yes" : "no");
        report("(the hook: no Windows checks)", rd == 0 && vq == 0 && life == 2000 && sharedAlive && sharedDead, d);
    }

    // the game's life state (Actor::IsDead, vtable slot 269: bits 17-20 at +0xF8), on fake characters
    {
        static std::uintptr_t fakeActorVt[300], fakePlayerVt[300], fakeOtherVt[300];
        static const std::uint8_t isDead[16] = {0x44, 0x8B, 0x81, 0xF8, 0x00, 0x00, 0x00, 0x41, 0x8B, 0xC0, 0x25, 0x00, 0x00, 0x1E, 0x00, 0x84};
        static const std::uint8_t other[16] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0xE8, 0, 0, 0, 0, 0x84, 0xC0};
        const std::uintptr_t savedA = g_actorVt, savedP = g_playerVt;
        g_actorVt = reinterpret_cast<std::uintptr_t>(fakeActorVt);
        g_playerVt = reinterpret_cast<std::uintptr_t>(fakePlayerVt);
        fakeActorVt[kIsDeadSlot] = fakePlayerVt[kIsDeadSlot] = reinterpret_cast<std::uintptr_t>(isDead);
        CheckDeadReading();
        const bool read = g_deadOk;
        alignas(16) static char npc[0x200], player[0x200], thing[0x200];
        const std::uintptr_t ov = reinterpret_cast<std::uintptr_t>(fakeOtherVt);
        memcpy(npc, &g_actorVt, 8);
        memcpy(player, &g_playerVt, 8);
        memcpy(thing, &ov, 8);
        bool states = true;
        char line[80] = "";
        for (int s = 0; s < 9; ++s) {
            const std::uint32_t flags = 0xFFE1FFFFu | (static_cast<std::uint32_t>(s) << 17);   // every other bit set
            memcpy(npc + 0xF8, &flags, 4);
            memcpy(player + 0xF8, &flags, 4);
            memcpy(thing + 0xF8, &flags, 4);
            const bool want = s == 1 || s == 2 || s == 5 || s == 7;
            const bool dead = DeadLifeState(LifeStateOf(npc));
            states &= LifeStateOf(npc) == s && LifeStateOf(player) == s && dead == want && LifeStateOf(thing) == -1;
            strcat_s(line, dead ? "D" : ".");
        }
        states &= LifeStateOf(nullptr) == -1 && LifeStateOf(npc + 4) == -1;
        // never through Windows' memory check (VirtualQuery is far too slow for every update): an unreadable
        // character is turned away by the guarded copy instead
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        char* noAccess = static_cast<char*>(VirtualAlloc(nullptr, si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
        const std::uint64_t vqBefore = g_vqCalls.load(), badBefore = g_badReads.load(), caughtBefore = g_caughtReads.load();
        const int unreadable = LifeStateOf(noAccess);
        for (int i = 0; i < 1000; ++i) (void)LifeStateOf(npc);
        const bool noQuery = unreadable == -1 && g_vqCalls.load() == vqBefore && g_badReads.load() == badBefore &&
                             g_caughtReads.load() == caughtBefore + 1;
        VirtualFree(noAccess, 0, MEM_RELEASE);
        fakePlayerVt[kIsDeadSlot] = reinterpret_cast<std::uintptr_t>(other);
        CheckDeadReading();
        const bool differ = !g_deadOk && LifeStateOf(npc) == -1;
        fakeActorVt[kIsDeadSlot] = reinterpret_cast<std::uintptr_t>(other);
        CheckDeadReading();
        const bool wrong = !g_deadOk;
        g_actorVt = 0;
        fakeActorVt[kIsDeadSlot] = fakePlayerVt[kIsDeadSlot] = reinterpret_cast<std::uintptr_t>(isDead);
        CheckDeadReading();
        const bool unknown = !g_deadOk;
        g_actorVt = savedA;
        g_playerVt = savedP;
        g_deadOk = false;
        snprintf(d, sizeof(d), "checked %s; states 0-8 dead: %s (want .DD..D.D.); slots differ %s, other code %s, no "
                 "classes %s; unreadable: turned away without asking Windows %s", read ? "yes" : "no", line,
                 differ ? "off" : "ON", wrong ? "off" : "ON", unknown ? "off" : "ON", noQuery ? "yes" : "NO");
        report("(life state)", read && states && !strcmp(line, ".DD..D.D.") && differ && wrong && unknown && noQuery, d);
    }
    ResetOthers();
    return fails;
}

// the proxy watch.  A fake chain node -> collision object -> system -> instance -> world -> body, as the
// game lays it out (ReadProxy's comment); its failure cases; no Windows memory checks; then WatchProxies on a fake
// A-Body body whose skeleton carries the two proxy nodes (rest pose + ABody_Proxy_Breast_L/R under the Pecs).
struct FakeWorld {
    alignas(16) char co[0x40], sys[0x40], inst[0x60], world[0xB00], bhk[0x800], bodies[8 * 0xB0];   // bhk: +0x7E4 the step
    alignas(16) char motions[8 * 0x80], table[0x60], entries[4 * 0x70];   // motions, motion-properties table
    alignas(16) char co2[0x40], sys2[0x40], inst2[0x60];   // a second proxy's chain into the same world
    std::uint32_t ids[2], ids2[2];
};
static std::uintptr_t g_fakeCoVt[8], g_fakeWorldVt[8];
static float g_fakeOwnLimit = 20.0f;   // the fake motions' own speed limit (m/s)
static void MakeFakeWorld(FakeWorld& w, int body, std::uint32_t filter, Vec3 pos) {
    memset(&w, 0, sizeof(w));
    const std::uintptr_t cv = reinterpret_cast<std::uintptr_t>(g_fakeCoVt), wv = reinterpret_cast<std::uintptr_t>(g_fakeWorldVt);
    memcpy(w.co, &cv, 8);
    memcpy(w.world, &wv, 8);
    void* p = w.sys;
    memcpy(w.co + 0x20, &p, 8);
    const std::int32_t index = 0;
    memcpy(w.co + 0x28, &index, 4);
    p = w.inst;
    memcpy(w.sys + 0x20, &p, 8);
    w.ids[0] = 0x01000000u | static_cast<std::uint32_t>(body);
    p = w.ids;
    memcpy(w.inst + 0x30, &p, 8);
    const std::uint32_t one = 1, nb = 8, flags = 0x0E;   // keyframed: IS_DYNAMIC | IS_KEYFRAMED | IS_ACTIVE
    memcpy(w.inst + 0x38, &one, 4);
    p = w.world + 0x20;
    memcpy(w.inst + 0x20, &p, 8);
    p = w.bhk;
    memcpy(w.world + 0x9B0, &p, 8);
    p = w.bodies;
    memcpy(w.world + 0x38, &p, 8);
    memcpy(w.world + 0x40, &nb, 4);
    char* b = w.bodies + body * 0xB0;
    memcpy(b + 0x50, &w.ids[0], 4);
    memcpy(b + 0x54, &flags, 4);
    memcpy(b + 0x8C, &filter, 4);
    const float t[3] = {pos.x, pos.y, pos.z};
    memcpy(b + 0x30, t, 12);
    // its motion (index = the body's) with its own limits in w, and the motion-properties table (entry 1)
    const std::uint32_t mi = static_cast<std::uint32_t>(body);
    memcpy(b + 0x80, &mi, 4);
    p = w.motions;
    memcpy(w.world + 0x188, &p, 8);
    memcpy(w.world + 0x190, &nb, 4);
    char* mo = w.motions + body * 0x80;
    const std::uint16_t props = 1;
    memcpy(mo + 0x58, &props, 2);
    const float angLimit = 25.0f;
    memcpy(mo + 0x6C, &g_fakeOwnLimit, 4);
    memcpy(mo + 0x7C, &angLimit, 4);
    p = w.table;
    memcpy(w.world + 0xA58, &p, 8);
    p = w.entries;
    memcpy(w.table + 0x40, &p, 8);
    const std::uint32_t ne = 4;
    memcpy(w.table + 0x48, &ne, 4);
    const float e[6] = {0.0f, 1.0f, 100.0f, 200.0f, 0.1f, 0.05f};   // gravity, time, max linear / angular, damping
    memcpy(w.entries + 1 * 0x70 + 0x08, e, sizeof(e));
    const double com[3] = {pos.x, pos.y, pos.z};   // the motion's centre of mass, on the body
    memcpy(mo + 0x10, com, sizeof(com));
}
// a second proxy in the same fake world (its own collision object, system and instance), body `body` on
// motion `motion`: the motion of the first proxy makes the two share one, as the game seems to do
static void AddFakeProxy(FakeWorld& w, int body, std::uint32_t filter, Vec3 pos, int motion) {
    const std::uintptr_t cv = reinterpret_cast<std::uintptr_t>(g_fakeCoVt);
    memcpy(w.co2, &cv, 8);
    void* p = w.sys2;
    memcpy(w.co2 + 0x20, &p, 8);
    const std::int32_t index = 0;
    memcpy(w.co2 + 0x28, &index, 4);
    p = w.inst2;
    memcpy(w.sys2 + 0x20, &p, 8);
    w.ids2[0] = 0x01000000u | static_cast<std::uint32_t>(body);
    p = w.ids2;
    memcpy(w.inst2 + 0x30, &p, 8);
    const std::uint32_t one = 1, flags = 0x0E, mi = static_cast<std::uint32_t>(motion);
    memcpy(w.inst2 + 0x38, &one, 4);
    p = w.world + 0x20;
    memcpy(w.inst2 + 0x20, &p, 8);
    char* b = w.bodies + body * 0xB0;
    memcpy(b + 0x50, &w.ids2[0], 4);
    memcpy(b + 0x54, &flags, 4);
    memcpy(b + 0x8C, &filter, 4);
    const float t[3] = {pos.x, pos.y, pos.z};
    memcpy(b + 0x30, t, 12);
    memcpy(b + 0x80, &mi, 4);
}
// stand-ins for the two game functions the proxies are written through
static int g_fakeSetCalls = 0, g_fakeKeyCalls = 0;
static std::uint32_t g_fakeSetInfo = 0;
static void* g_fakeSetCo[2] = {};
static char* g_fakeSetBody[2] = {};   // the fake bodies the "queue" applies a set to
static float g_fakeKeyM[16], g_fakeKeyInvDt = 0;
static void* g_fakeKeyCo = nullptr;
static bool g_fakeKeyWarp = false;
static bool FakeSetFilter(void* co, std::uint32_t info) {
    ++g_fakeSetCalls;
    g_fakeSetInfo = info;
    for (int i = 0; i < 2; ++i)
        if (co == g_fakeSetCo[i] && g_fakeSetBody[i]) memcpy(g_fakeSetBody[i] + 0x8C, &info, 4);
    return true;
}
static bool FakeKeyframe(void* co, const float* m, float invDt, bool* warped) {
    ++g_fakeKeyCalls;
    g_fakeKeyCo = co;
    memcpy(g_fakeKeyM, m, sizeof(g_fakeKeyM));
    g_fakeKeyInvDt = invDt;
    *warped = g_fakeKeyWarp;
    return true;
}
// the keyframe function and one physics step, as the game does them (IDs 125392, 161979, 160867): the
// speed asked = the gap from the body (+0x30 plus +0x40) / the step time; over the table's 100 m/s it warps (the body
// placed, velocity 0); under it the velocity setter holds the speed to the motion's own limit (w of +0x60), and the
// step (g_simStep) moves the body that far
static void* g_simCo[2] = {};
static char* g_simBody[2] = {};
static char* g_simMotion[2] = {};
static float g_simStep = 1.0f / 60.0f;
static bool g_simShared = false;   // both proxies on one motion: a move moves both
static bool FakeKeyframeStep(void* co, const float* m, float stepS, bool* warped) {
    ++g_fakeKeyCalls;
    *warped = false;
    const int s = co == g_simCo[0] ? 0 : co == g_simCo[1] ? 1 : -1;
    if (s < 0 || !(stepS > 0)) return false;
    float pos[3], lo[3], lim;
    memcpy(pos, g_simBody[s] + 0x30, 12);
    memcpy(lo, g_simBody[s] + 0x40, 12);
    float start[3];
    memcpy(start, pos, 12);
    memcpy(&lim, g_simMotion[s] + 0x6C, 4);
    float d[3], len2 = 0;
    for (int j = 0; j < 3; ++j) {
        d[j] = m[12 + j] - (pos[j] + lo[j]);
        len2 += d[j] * d[j];
    }
    const float len = std::sqrt(len2), asked = len / stepS;
    float v[4] = {0, 0, 0, lim};
    if (asked > 100.0f) {
        for (int j = 0; j < 3; ++j) pos[j] = m[12 + j] - lo[j];
        *warped = true;
    } else if (len > 0) {
        const float speed = asked > lim ? lim : asked;
        for (int j = 0; j < 3; ++j) {
            v[j] = d[j] / len * speed;
            pos[j] += v[j] * g_simStep;
        }
    }
    memcpy(g_simBody[s] + 0x30, pos, 12);
    memcpy(g_simMotion[s] + 0x60, v, 16);
    if (g_simShared) {   // one motion: the other body moves with it
        float o[3];
        memcpy(o, g_simBody[1 - s] + 0x30, 12);
        for (int j = 0; j < 3; ++j) o[j] += pos[j] - start[j];
        memcpy(g_simBody[1 - s] + 0x30, o, 12);
    }
    return true;
}
static int RunWorldProxies() {
    int fails = 0;
    auto report = [&](const char* what, bool ok, const char* detail) {
        printf("%-14s %-40s %s  %s\n", "world proxies", what, detail, ok ? "OK" : "FAIL");
        fails += ok ? 0 : 1;
    };
    auto report8d = [&](const char* what, bool ok, const char* detail) {
        printf("%-14s %-40s %s  %s\n", "world proxies", what, detail, ok ? "OK" : "FAIL");
        fails += ok ? 0 : 1;
    };
    char d[400];
    const std::uintptr_t savedW[kNumWorldAddrs] = {g_waddr[0], g_waddr[1], g_waddr[2]};
    const bool savedOk = g_proxyOk, savedCfg = g_cfg.proxies;
    static void* exteriorHolder = reinterpret_cast<void*>(0x12345670);
    g_waddr[kNpCollisionObjectVtbl] = reinterpret_cast<std::uintptr_t>(g_fakeCoVt);
    g_waddr[kBSWorldVtbl] = reinterpret_cast<std::uintptr_t>(g_fakeWorldVt);
    g_waddr[kExteriorWorld] = reinterpret_cast<std::uintptr_t>(&exteriorHolder);
    g_proxyOk = true;
    g_cfg.proxies = true;
    {   // the read and its failure cases
        static FakeWorld w;
        alignas(16) static char node[0x200];
        MakeFakeWorld(w, 5, 0x0007003E, {1, 2, 3});
        void* co = w.co;
        memcpy(node + 0x110, &co, 8);
        const std::uint64_t rd0 = g_readableCalls.load();
        ProxyRead p;
        ReadProxy(node, p);
        const bool full = p.co == w.co && p.world == w.world && p.bhk == w.bhk && p.bodyId == 0x01000005u && p.body &&
                          p.flags == 0x0E && p.filter == 0x0007003Eu && p.pos.x == 1 && p.pos.z == 3 && !strcmp(MotionName(p), "keyframed");
        void* zero = nullptr;
        memcpy(w.inst + 0x20, &zero, 8);   // not in a world
        ReadProxy(node, p);
        const bool noWorld = p.co == w.co && !p.world && !p.body && !strcmp(MotionName(p), "not in a world");
        MakeFakeWorld(w, 5, 0x0007003E, {1, 2, 3});
        const std::uint32_t stale = 0x02000005u;   // the body slot reused by another body (serial)
        memcpy(w.bodies + 5 * 0xB0 + 0x50, &stale, 4);
        ReadProxy(node, p);
        const bool staleBody = p.world == w.world && !p.body;
        MakeFakeWorld(w, 5, 0x0007003E, {1, 2, 3});
        const std::int32_t past = 3;   // the body index past the instance's count
        memcpy(w.co + 0x28, &past, 4);
        ReadProxy(node, p);
        const bool outOfRange = p.co == w.co && p.bodyId == 0xFFFFFFFFu && !p.body;
        MakeFakeWorld(w, 5, 0x0007003E, {1, 2, 3});
        const std::uintptr_t other = 0x1111;
        memcpy(w.co, &other, 8);   // a collision object of another class
        ReadProxy(node, p);
        const bool wrongClass = !p.co && !p.world;
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        char* noAccess = static_cast<char*>(VirtualAlloc(nullptr, si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
        memcpy(node + 0x110, &noAccess, 8);
        ReadProxy(node, p);
        const bool unreadable = !p.co;
        VirtualFree(noAccess, 0, MEM_RELEASE);
        ReadProxy(nullptr, p);
        const bool none = !p.co;
        const std::uint64_t rd = g_readableCalls.load() - rd0;
        snprintf(d, sizeof(d), "full chain %s; no world %s; stale body %s; index out of range %s; other class %s; unreadable "
                 "%s; no node %s; %llu memory checks", full ? "read" : "WRONG", noWorld ? "ok" : "WRONG", staleBody ? "ok" : "WRONG",
                 outOfRange ? "ok" : "WRONG", wrongClass ? "ok" : "WRONG", unreadable ? "ok" : "WRONG", none ? "ok" : "WRONG",
                 static_cast<unsigned long long>(rd));
        report("(the proxy read)", full && noWorld && staleBody && outOfRange && wrongClass && unreadable && none && rd == 0, d);
    }
    {   // the motion and both speed limits, and the read's failure cases
        static FakeWorld w;
        alignas(16) static char node[0x200];
        MakeFakeWorld(w, 5, 0x0007403E, {1, 2, 3});
        void* co = w.co;
        memcpy(node + 0x110, &co, 8);
        const bool savedM = g_proxyMotionOk;
        g_proxyMotionOk = true;
        const std::uint64_t rd0 = g_readableCalls.load();
        const float lo[3] = {0.0001f, 0, 0}, vel[3] = {3, 0, 4};
        memcpy(w.bodies + 5 * 0xB0 + 0x40, lo, 12);
        memcpy(w.motions + 5 * 0x80 + 0x60, vel, 12);
        ProxyRead p;
        ReadProxy(node, p);
        ProxyMotion m;
        ReadProxyMotion(p, m);
        const bool full = p.bodyAt == w.bodies + 5 * 0xB0 && m.motion && m.entry && m.index == 5 && m.props == 1 &&
                          m.propsCount == 4 && m.limit == 20.0f && m.angLimit == 25.0f && m.e[0] == 0.0f && m.e[1] == 1.0f &&
                          m.e[2] == 100.0f && m.e[3] == 200.0f && m.e[4] == 0.1f && m.e[5] == 0.05f && m.lo.x == 0.0001f &&
                          m.vel.x == 3 && m.vel.z == 4 && m.com[0] == 1.0 && m.com[2] == 3.0;
        g_proxyMotionOk = false;
        ReadProxyMotion(p, m);
        const bool unchecked = !m.motion && !m.entry;
        g_proxyMotionOk = true;
        const std::uint32_t past = 9;   // the motion index past the world's 8
        memcpy(w.bodies + 5 * 0xB0 + 0x80, &past, 4);
        ReadProxyMotion(p, m);
        const bool noMotion = !m.motion && !m.entry;
        MakeFakeWorld(w, 5, 0x0007403E, {1, 2, 3});
        void* zero = nullptr;
        memcpy(w.world + 0x188, &zero, 8);   // no motions
        ReadProxy(node, p);
        ReadProxyMotion(p, m);
        const bool noMotions = !m.motion;
        MakeFakeWorld(w, 5, 0x0007403E, {1, 2, 3});
        memcpy(w.world + 0xA58, &zero, 8);   // no motion-properties table: the own limits only
        ReadProxy(node, p);
        ReadProxyMotion(p, m);
        const bool noTable = m.motion && !m.entry && m.limit == 20.0f;
        MakeFakeWorld(w, 5, 0x0007403E, {1, 2, 3});
        const std::uint16_t badProps = 7;   // past the table's 4
        memcpy(w.motions + 5 * 0x80 + 0x58, &badProps, 2);
        ReadProxy(node, p);
        ReadProxyMotion(p, m);
        const bool noEntry = m.motion && !m.entry && m.props == 7;
        const std::uint64_t rd = g_readableCalls.load() - rd0;
        g_proxyMotionOk = savedM;
        snprintf(d, sizeof(d), "motion and table %s; tables unchecked %s; index past the motions %s; no motions %s; no table "
                 "%s; index past the table %s; %llu memory checks", full ? "read" : "WRONG", unchecked ? "ok" : "WRONG",
                 noMotion ? "ok" : "WRONG", noMotions ? "ok" : "WRONG", noTable ? "ok" : "WRONG", noEntry ? "ok" : "WRONG",
                 static_cast<unsigned long long>(rd));
        report8d("(the speed limits read)", full && unchecked && noMotion && noMotions && noTable && noEntry && rd == 0, d);
    }
    {   // WatchProxies on a fake A-Body body with the proxy nodes
        char src[MAX_PATH], tmp[MAX_PATH];
        snprintf(src, sizeof(src), "%s\\cases\\rest.txt", g_tests);
        snprintf(tmp, sizeof(tmp), "%s\\..\\build\\tests\\rest_with_proxies.txt", g_tests);
        FILE* in = nullptr;
        FILE* outf = nullptr;
        if (fopen_s(&in, src, "rb") || !in || fopen_s(&outf, tmp, "wb") || !outf) {
            report("(the watch)", false, "cannot write the test case");
            return fails;
        }
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, got, outf);
        fclose(in);
        fputs("\nN ABody_Proxy_Breast_L L_Pecs 0.0276 -0.00456 0.01475 1 0 0 0 1 0 0 0 1 -0.07379 0.12 1.30\n"
              "N ABody_Proxy_Breast_R R_Pecs 0.0276 -0.00456 -0.01475 1 0 0 0 1 0 0 0 1 0.07379 0.12 1.30\n", outf);
        fclose(outf);
        Fake fp;
        fp.kidLists.reserve(512);
        std::map<std::string, Vec3> ep;
        const bool loaded = Load(tmp, fp, ep, {});
        static FakeWorld wl, wr, wc;
        MakeFakeWorld(wl, 1, 0x0004003E, {-0.07379f, 0.12f, 1.31f});   // 1 cm from its node
        MakeFakeWorld(wr, 2, 0x0004003E, {0.07379f, 0.12f, 1.30f});
        MakeFakeWorld(wc, 3, 0x00040408, {0, 0.05f, 1.39f});
        void* p = wl.co;
        memcpy(fp.node["ABody_Proxy_Breast_L"] + 0x110, &p, 8);
        p = wr.co;
        memcpy(fp.node["ABody_Proxy_Breast_R"] + 0x110, &p, 8);
        p = wc.co;
        memcpy(fp.node["C_Chest"] + 0x110, &p, 8);
        ActorView v;
        v.com = fp.node["COM"];
        v.site = 2;
        const char* bn[4] = {"Upper", "Lower", "Inner", "Outer"};
        for (int s = 0; s < 2; ++s) {
            v.anchors[s].node = fp.node[s ? "R_Pecs" : "L_Pecs"];
            v.anchors[s].com = v.com;
            v.anchors[s].rest = reinterpret_cast<const float*>(static_cast<char*>(v.anchors[s].node) + 0x70);
            for (int b = 0; b < 4; ++b) v.anchors[s].bones[b] = fp.node[std::string("TF_Breast_") + bn[b] + (s ? "_R" : "_L")];
        }
        ResetOthers();
        Collide(v);   // learns the layout if needed, finds the body's nodes (with the proxies)
        const std::uint64_t b0 = g_proxyBodies.load(), w0 = g_proxyInWorld.load(), c0 = g_proxyChest.load(),
                            ch0 = g_proxyChanges.load(), rd0 = g_readableCalls.load();
        g_proxyDistMm[0] = 0;
        WatchProxies(v, 2);
        WatchProxies(v, 2);   // nothing changed: no new log line
        const std::uint64_t changesSame = g_proxyChanges.load() - ch0;
        const std::uint32_t moved = 0x0004003F;
        memcpy(wl.bodies + 1 * 0xB0 + 0x8C, &moved, 4);   // the game changed the L body's layer
        WatchProxies(v, 2);
        const std::uint64_t changes = g_proxyChanges.load() - ch0, rd = g_readableCalls.load() - rd0;
        const std::uint32_t distMm = g_proxyDistMm[0].load();
        const bool ok = loaded && g_proxyBodies.load() - b0 == 3 && g_proxyInWorld.load() - w0 == 3 &&
                        g_proxyChest.load() - c0 == 3 && changesSame == 1 && changes == 2 && rd == 0 && distMm >= 9 && distMm <= 11;
        snprintf(d, sizeof(d), "3 updates: with proxies %llu, both in a world %llu, C_Chest body in a world %llu; changes "
                 "logged %llu (1 before the layer change); L proxy %u mm from its node; %llu memory checks",
                 static_cast<unsigned long long>(g_proxyBodies.load() - b0), static_cast<unsigned long long>(g_proxyInWorld.load() - w0),
                 static_cast<unsigned long long>(g_proxyChest.load() - c0), static_cast<unsigned long long>(changes), distMm,
                 static_cast<unsigned long long>(rd));
        report("(the watch on a body)", ok, d);
        {   // the writes, through the stand-ins
            const std::uintptr_t savedSet = g_waddr[kSetFilterFn], savedKey = g_waddr[kKeyframeFn];
            const bool savedWrite = g_proxyWriteOk, savedLayer = g_cfg.proxyLayer, savedSteer = g_cfg.steer, savedInert = g_cfg.proxyInert;
            g_waddr[kSetFilterFn] = reinterpret_cast<std::uintptr_t>(&FakeSetFilter);
            g_waddr[kKeyframeFn] = reinterpret_cast<std::uintptr_t>(&FakeKeyframe);
            g_proxyWriteOk = g_cfg.proxyLayer = g_cfg.steer = g_cfg.proxyInert = true;
            g_fakeSetCo[0] = wl.co;
            g_fakeSetCo[1] = wr.co;
            g_fakeSetBody[0] = wl.bodies + 1 * 0xB0;
            g_fakeSetBody[1] = wr.bodies + 2 * 0xB0;
            const std::uint32_t biped = 0x00040008;   // what the game gave them: layer 8, group 4
            memcpy(g_fakeSetBody[0] + 0x8C, &biped, 4);
            memcpy(g_fakeSetBody[1] + 0x8C, &biped, 4);
            const float stepS = 1.0f / 60.0f;   // the time the next physics update simulates, as the game writes it
            memcpy(wl.bhk + 0x7E4, &stepS, 4);
            memcpy(wr.bhk + 0x7E4, &stepS, 4);
            const std::uint64_t rdW0 = g_readableCalls.load(), steers0 = g_proxySteers.load(), warps0 = g_proxyWarps.load();
            g_fakeSetCalls = g_fakeKeyCalls = 0;
            WatchProxies(v, 2);   // both set to 62 + bit 14 (the group kept); not steered yet (still layer 8 when read)
            const int sets1 = g_fakeSetCalls, keys1 = g_fakeKeyCalls;
            const std::uint32_t sent = g_fakeSetInfo;
            WatchProxies(v, 2);   // layer 62 now: both steered, nothing set again
            const int sets2 = g_fakeSetCalls - sets1, keys2 = g_fakeKeyCalls - keys1;
            // the last call was the right proxy: rows = R_Pecs's world rows, position = the mean of its four bones on it
            char* pec = fp.node["R_Pecs"];
            float mean[3] = {0, 0, 0};
            for (const char* bone : {"TF_Breast_Upper_R", "TF_Breast_Lower_R", "TF_Breast_Inner_R", "TF_Breast_Outer_R"}) {
                const float* t = reinterpret_cast<const float*>(fp.node[bone] + 0x70);
                for (int j = 0; j < 3; ++j) mean[j] += t[j] / 4;
            }
            Xf px;
            WorldOf(pec, px);
            const Vec3 want = Apply(px, mean);
            bool rows = g_fakeKeyCo == wr.co && g_fakeKeyInvDt == stepS && g_fakeKeyM[3] == 0 && g_fakeKeyM[7] == 0 &&
                        g_fakeKeyM[11] == 0 && g_fakeKeyM[15] == 1.0f;
            for (int r = 0; r < 3; ++r) {
                const Vec3 ax = Row(pec, 0x80, r);
                rows &= g_fakeKeyM[4 * r] == ax.x && g_fakeKeyM[4 * r + 1] == ax.y && g_fakeKeyM[4 * r + 2] == ax.z;
            }
            const bool where = std::fabs(g_fakeKeyM[12] - want.x) < 1e-6f && std::fabs(g_fakeKeyM[13] - want.y) < 1e-6f &&
                               std::fabs(g_fakeKeyM[14] - want.z) < 1e-6f;
            g_fakeKeyWarp = true;
            WatchProxies(v, 2);   // warps counted
            g_fakeKeyWarp = false;
            memcpy(g_fakeSetBody[0] + 0x8C, &biped, 4);   // the game takes the left one's layer back
            const int keysBefore = g_fakeKeyCalls, setsBefore = g_fakeSetCalls;
            WatchProxies(v, 2);   // within 0.5 s: not set again; the left one (collidable) not steered, the right one is
            const int setsHeld = g_fakeSetCalls - setsBefore, keysOne = g_fakeKeyCalls - keysBefore;
            const bool rightOnly = g_fakeKeyCo == wr.co;
            Sleep(550);
            WatchProxies(v, 2);   // after 0.5 s: set again
            const int setsAgain = g_fakeSetCalls - setsBefore;
            // an implausible step time (60: a rate, not seconds) is not passed on: the steer is skipped, counted
            const float wrong = 60.0f;
            memcpy(wl.bhk + 0x7E4, &wrong, 4);
            memcpy(wr.bhk + 0x7E4, &wrong, 4);
            const std::uint64_t noStep0 = g_proxySkips[3].load();
            const int keysBad0 = g_fakeKeyCalls;
            WatchProxies(v, 2);
            const bool badStep = g_fakeKeyCalls == keysBad0 && g_proxySkips[3].load() - noStep0 >= 1;
            memcpy(wl.bhk + 0x7E4, &stepS, 4);
            memcpy(wr.bhk + 0x7E4, &stepS, 4);
            const std::uint64_t rdW = g_readableCalls.load() - rdW0;
            const bool okW = badStep && sets1 == 2 && sent == 0x0004403Eu && keys1 == 0 && sets2 == 0 && keys2 == 2 && rows && where &&
                             g_proxyWarps.load() - warps0 == 2 && setsHeld == 0 && keysOne == 1 && rightOnly && setsAgain == 1 &&
                             g_proxySteers.load() - steers0 >= 5 && rdW == 0;
            snprintf(d, sizeof(d), "set %d (0x%08X), steered %d; then set %d, steered %d (rows %s, at the bones' mean %s, step "
                     "%.4f s); warps %llu; layer taken back: held %d, steered %d (%s), after 0.5 s set %d; %llu memory checks",
                     sets1, sent, keys1, sets2, keys2, rows ? "right" : "WRONG", where ? "yes" : "NO", g_fakeKeyInvDt,
                     static_cast<unsigned long long>(g_proxyWarps.load() - warps0), setsHeld, keysOne, rightOnly ? "the right one" : "WRONG",
                     setsAgain, static_cast<unsigned long long>(rdW));
            report("(the writes: layer, steering)", okW, d);
            g_waddr[kSetFilterFn] = savedSet;
            g_waddr[kKeyframeFn] = savedKey;
            g_proxyWriteOk = savedWrite;
            g_cfg.proxyLayer = savedLayer;
            g_cfg.steer = savedSteer;
            g_cfg.proxyInert = savedInert;
        }
        {   // following at speed, through the stand-in for the keyframe function and one physics step
            const std::uintptr_t savedSet = g_waddr[kSetFilterFn], savedKey = g_waddr[kKeyframeFn];
            const bool savedWrite = g_proxyWriteOk, savedLayer = g_cfg.proxyLayer, savedSteer = g_cfg.steer,
                       savedInert = g_cfg.proxyInert, savedM = g_proxyMotionOk;
            const float savedCap = g_cfg.proxyMaxSpeed, savedLimit = g_fakeOwnLimit;
            g_waddr[kSetFilterFn] = reinterpret_cast<std::uintptr_t>(&FakeSetFilter);
            g_waddr[kKeyframeFn] = reinterpret_cast<std::uintptr_t>(&FakeKeyframeStep);
            g_proxyWriteOk = g_cfg.proxyLayer = g_cfg.steer = g_cfg.proxyInert = true;
            char* moving[4] = {fp.node["L_Pecs"], fp.node["R_Pecs"], fp.node["ABody_Proxy_Breast_L"], fp.node["ABody_Proxy_Breast_R"]};
            float startX[4];
            for (int i = 0; i < 4; ++i) memcpy(&startX[i], moving[i] + 0xB0, 4);
            static FakeWorld sl, sr;
            struct Run { std::uint32_t gapMm, lagMm, capMmS; std::uint64_t moved, own, game, missed; float x; };
            // stand still for 10 frames (onto the target), then `frames` frames with the chest moving at `speed` along x
            auto run = [&](float ownLimit, bool limitsRead, float cap, float speed, int frames) {
                g_fakeOwnLimit = ownLimit;
                g_proxyMotionOk = limitsRead;
                g_cfg.proxyMaxSpeed = cap;
                for (int i = 0; i < 4; ++i) memcpy(moving[i] + 0xB0, &startX[i], 4);
                Xf nl, nr;
                WorldOf(moving[2], nl);
                WorldOf(moving[3], nr);
                MakeFakeWorld(sl, 1, 0x0004403E, nl.pos);
                MakeFakeWorld(sr, 2, 0x0004403E, nr.pos);
                const float h = g_simStep;
                memcpy(sl.bhk + 0x7E4, &h, 4);
                memcpy(sr.bhk + 0x7E4, &h, 4);
                g_simCo[0] = sl.co;
                g_simCo[1] = sr.co;
                g_simBody[0] = sl.bodies + 1 * 0xB0;
                g_simBody[1] = sr.bodies + 2 * 0xB0;
                g_simMotion[0] = sl.motions + 1 * 0x80;
                g_simMotion[1] = sr.motions + 2 * 0x80;
                void* c = sl.co;
                memcpy(moving[2] + 0x110, &c, 8);
                c = sr.co;
                memcpy(moving[3] + 0x110, &c, 8);
                for (int f = 0; f < 10; ++f) WatchProxies(v, 2);
                for (int s = 0; s < 2; ++s) g_proxyStepMm[s] = g_proxyLagMm[s] = 0;
                g_proxyMoved = g_proxyOwnWarps = g_proxyWarps = g_proxyOwnMissed = 0;
                Run r{};
                for (int f = 0; f < frames; ++f) {
                    for (int i = 0; i < 4; ++i) {
                        float x;
                        memcpy(&x, moving[i] + 0xB0, 4);
                        x += speed * h;
                        memcpy(moving[i] + 0xB0, &x, 4);
                    }
                    WatchProxies(v, 2);
                }
                const auto most = [](std::uint32_t x, std::uint32_t y) { return x > y ? x : y; };
                r.gapMm = most(g_proxyStepMm[0].load(), g_proxyStepMm[1].load());
                r.lagMm = most(g_proxyLagMm[0].load(), g_proxyLagMm[1].load());
                r.capMmS = g_proxyCapMmS.load();
                r.moved = g_proxyMoved.load();
                r.own = g_proxyOwnWarps.load();
                r.game = g_proxyWarps.load();
                r.missed = g_proxyOwnMissed.load();
                memcpy(&r.x, moving[2] + 0xB0, 4);
                return r;
            };
            const Run t3 = run(4.0f, false, 1000.0f, 6.0f, 240);   // limits not read, no own warps: held to 4 m/s, it falls behind
            const Run old = run(4.0f, true, 10.0f, 6.0f, 240);    // own limit 4 m/s, read: warped every frame
            const Run t4 = run(20.0f, true, 10.0f, 6.0f, 240);    // own limit 20 m/s, cap 10 m/s: moved by velocity
            const Run rest = run(20.0f, true, 10.0f, 0.0f, 30);   // standing
            const Run sprint = run(20.0f, true, 10.0f, 15.0f, 120);   // over the cap
            const int n = 2 * 240;
            const bool okT3 = t3.gapMm > 1000 && t3.game >= 2 && t3.own == 0;
            const bool okOld = old.gapMm <= 101 && old.own == static_cast<std::uint64_t>(n) && old.game == 0 && old.moved == 0 &&
                               old.missed == 0 && old.capMmS == 4000;
            const bool okT4 = t4.gapMm <= 101 && t4.lagMm <= 1 && t4.moved == static_cast<std::uint64_t>(n) && t4.own == 0 &&
                              t4.game == 0 && t4.capMmS == 10000;
            const bool okRest = rest.gapMm <= 1 && rest.own == 0 && rest.game == 0;
            const bool okSprint = sprint.gapMm <= 251 && sprint.own == 2u * 120u && sprint.game == 0 && sprint.missed == 0;
            snprintf(d, sizeof(d), "6 m/s, limits not read (held to 4 m/s): %u mm behind, %llu game warps; own limit 4 m/s: %u mm, own warps "
                     "%llu/%d (cap %.0f m/s); own limit 20 m/s: %u mm (lag %u mm), moved %llu/%d, warps %llu+%llu; "
                     "standing %u mm; 15 m/s: %u mm, own warps %llu/240", t3.gapMm, static_cast<unsigned long long>(t3.game),
                     old.gapMm, static_cast<unsigned long long>(old.own), n, old.capMmS / 1000.0, t4.gapMm, t4.lagMm,
                     static_cast<unsigned long long>(t4.moved), n, static_cast<unsigned long long>(t4.own),
                     static_cast<unsigned long long>(t4.game), rest.gapMm, sprint.gapMm, static_cast<unsigned long long>(sprint.own));
            report8d("(following at speed)", okT3 && okOld && okT4 && okRest && okSprint, d);
            for (int i = 0; i < 4; ++i) memcpy(moving[i] + 0xB0, &startX[i], 4);
            void* c = wl.co;
            memcpy(moving[2] + 0x110, &c, 8);
            c = wr.co;
            memcpy(moving[3] + 0x110, &c, 8);
            g_waddr[kSetFilterFn] = savedSet;
            g_waddr[kKeyframeFn] = savedKey;
            g_proxyWriteOk = savedWrite;
            g_cfg.proxyLayer = savedLayer;
            g_cfg.steer = savedSteer;
            g_cfg.proxyInert = savedInert;
            g_proxyMotionOk = savedM;
            g_cfg.proxyMaxSpeed = savedCap;
            g_fakeOwnLimit = savedLimit;
        }
        {   // one steer per shared motion.  Both proxies in one fake world on one motion: with bSteerOnce the
            // left proxy is steered and the right one rides (the stand-in moves both with the one motion); without it,
            // both are steered.  The six bands count where each proxy was before each steer
            const std::uintptr_t savedSet = g_waddr[kSetFilterFn], savedKey = g_waddr[kKeyframeFn];
            const bool savedWrite = g_proxyWriteOk, savedLayer = g_cfg.proxyLayer, savedSteer = g_cfg.steer,
                       savedInert = g_cfg.proxyInert, savedM = g_proxyMotionOk, savedOnce = g_cfg.steerOnce;
            g_waddr[kSetFilterFn] = reinterpret_cast<std::uintptr_t>(&FakeSetFilter);
            g_waddr[kKeyframeFn] = reinterpret_cast<std::uintptr_t>(&FakeKeyframeStep);
            g_proxyWriteOk = g_cfg.proxyLayer = g_cfg.steer = g_cfg.proxyInert = g_proxyMotionOk = true;
            char* moving[4] = {fp.node["L_Pecs"], fp.node["R_Pecs"], fp.node["ABody_Proxy_Breast_L"], fp.node["ABody_Proxy_Breast_R"]};
            float startX[4];
            for (int i = 0; i < 4; ++i) memcpy(&startX[i], moving[i] + 0xB0, 4);
            static FakeWorld sw;
            struct Shared { std::uint64_t calls, rides, frames, shared, band[2][6]; std::uint32_t gapMm[2]; };
            auto run = [&](bool once, float speed, int frames) {
                g_cfg.steerOnce = once;
                for (int i = 0; i < 4; ++i) memcpy(moving[i] + 0xB0, &startX[i], 4);
                // both bodies start on their targets (the mean of each breast's four bones on its Pecs, as the plugin
                // steers them), so a body riding on the other's motion keeps its place exactly
                auto targetOf = [&](int side) {
                    static const char* const kBone[4] = {"Upper", "Lower", "Inner", "Outer"};
                    float mean[3] = {0, 0, 0};
                    for (const char* bone : kBone) {
                        const float* t = reinterpret_cast<const float*>(
                            fp.node[std::string("TF_Breast_") + bone + (side ? "_R" : "_L")] + 0x70);
                        for (int j = 0; j < 3; ++j) mean[j] += t[j] / 4;
                    }
                    Xf px;
                    WorldOf(fp.node[side ? "R_Pecs" : "L_Pecs"], px);
                    return Apply(px, mean);
                };
                MakeFakeWorld(sw, 1, 0x0004403E, targetOf(0));
                AddFakeProxy(sw, 2, 0x0004403E, targetOf(1), 1);   // the right proxy: body 2, on body 1's motion
                const float h = g_simStep;
                memcpy(sw.bhk + 0x7E4, &h, 4);
                g_simCo[0] = sw.co;
                g_simCo[1] = sw.co2;
                g_simBody[0] = sw.bodies + 1 * 0xB0;
                g_simBody[1] = sw.bodies + 2 * 0xB0;
                g_simMotion[0] = g_simMotion[1] = sw.motions + 1 * 0x80;
                g_simShared = true;
                void* c = sw.co;
                memcpy(moving[2] + 0x110, &c, 8);
                c = sw.co2;
                memcpy(moving[3] + 0x110, &c, 8);
                for (int f = 0; f < 10; ++f) WatchProxies(v, 2);
                for (int s = 0; s < 2; ++s) {
                    g_proxyStepMm[s] = 0;
                    for (auto& band : g_proxyGapHist[s]) band = 0;
                }
                g_proxyRides = g_proxyFrames = g_proxyShared = 0;
                const int calls0 = g_fakeKeyCalls;
                for (int f = 0; f < frames; ++f) {
                    for (int i = 0; i < 4; ++i) {
                        float x;
                        memcpy(&x, moving[i] + 0xB0, 4);
                        x += speed * h;
                        memcpy(moving[i] + 0xB0, &x, 4);
                    }
                    WatchProxies(v, 2);
                }
                Shared r{};
                r.calls = static_cast<std::uint64_t>(g_fakeKeyCalls - calls0);
                r.rides = g_proxyRides.load();
                r.frames = g_proxyFrames.load();
                r.shared = g_proxyShared.load();
                for (int s = 0; s < 2; ++s) {
                    r.gapMm[s] = g_proxyStepMm[s].load();
                    for (int k = 0; k < 6; ++k) r.band[s][k] = g_proxyGapHist[s][k].load();
                }
                g_simShared = false;
                return r;
            };
            // 4.5 m/s = 7.5 cm a frame: before each steer both proxies are 5-10 cm behind (both are read before the
            // left one is steered), and after it both are on target again
            const Shared once = run(true, 4.5f, 120), both = run(false, 4.5f, 120);
            const bool okOnce = once.calls == 120 && once.rides == 120 && once.frames == 120 && once.shared == 120 &&
                                once.band[0][2] == 120 && once.band[1][2] == 120 && once.gapMm[0] <= 76 && once.gapMm[1] <= 76;
            const bool okBoth = both.calls == 240 && both.rides == 0 && both.shared == 120;
            snprintf(d, sizeof(d), "steered once: %llu calls / 120 frames, the right proxy rode %llu times, shared %llu; "
                     "5-10 cm behind before each steer: left %llu, right %llu (at most %u / %u mm); both steered: %llu "
                     "calls, rides %llu",
                     static_cast<unsigned long long>(once.calls), static_cast<unsigned long long>(once.rides),
                     static_cast<unsigned long long>(once.shared), static_cast<unsigned long long>(once.band[0][2]),
                     static_cast<unsigned long long>(once.band[1][2]), once.gapMm[0], once.gapMm[1],
                     static_cast<unsigned long long>(both.calls), static_cast<unsigned long long>(both.rides));
            report8d("(one steer per shared motion)", okOnce && okBoth, d);
            for (int i = 0; i < 4; ++i) memcpy(moving[i] + 0xB0, &startX[i], 4);
            void* c = wl.co;
            memcpy(moving[2] + 0x110, &c, 8);
            c = wr.co;
            memcpy(moving[3] + 0x110, &c, 8);
            g_waddr[kSetFilterFn] = savedSet;
            g_waddr[kKeyframeFn] = savedKey;
            g_proxyWriteOk = savedWrite;
            g_cfg.proxyLayer = savedLayer;
            g_cfg.steer = savedSteer;
            g_cfg.proxyInert = savedInert;
            g_proxyMotionOk = savedM;
            g_cfg.steerOnce = savedOnce;
        }
        ResetOthers();
    }
    for (int i = 0; i < kNumWorldAddrs; ++i) g_waddr[i] = savedW[i];
    g_proxyOk = savedOk;
    g_cfg.proxies = savedCfg;
    return fails;
}

int main() {
    GetModuleFileNameA(nullptr, g_tests, MAX_PATH);
    for (int up = 0; up < 3; ++up)
        if (char* slash = strrchr(g_tests, '\\')) *slash = 0;
    snprintf(g_logPath, sizeof(g_logPath), "%s\\build\\tests\\collide_test.log", g_tests);
    strcat_s(g_tests, "\\tests");
    FILE* f = nullptr;
    if (fopen_s(&f, g_logPath, "w") == 0 && f) fclose(f);
    const char* poses[] = {"rest", "arms_forward", "arms_crossed", "sitting", "deep_squat", "sprint_stride",
                           "hands_on_glutes", "hands_on_glutes_firm"};
    int fails = 0;
    g_cfg.innerThighs = true;   // the designed cases include the inner-thigh group (off by default in game)
    for (int layout = kPointerAndCount; layout <= kGame; ++layout) {
        g_fakeLayout = layout;
        g_layoutState = 0;   // the plugin learns the layout again
        g_layoutTries = 0;
        g_layoutRetryMs = 0;
        printf("--- children kept as: %s\n", kFakeLayoutName[layout]);
        for (const char* p : poses) fails += RunCase(p, {}, 0.01f, false);
        fails += RunCase("arms_forward", {}, 0.01f, true);
        // without the wrist and finger bones the hand is placed from the rest pose: close, not exact
        fails += RunCase("arms_forward", {"L_Wrist", "R_Wrist", "L_Middle1", "R_Middle1"}, 0.3f, false);
        fails += RunCase("arms_crossed", {"L_Wrist", "R_Wrist", "L_Middle1", "R_Middle1"}, 0.3f, false);
        fails += RunFrames("arms_crossed");
        fails += RunSideGravity();
    }
    printf("--- (small arrays: %d nodes had more than 8 children, kept behind a pointer)\n", g_bigLists);
    for (int layout : {static_cast<int>(kPointerAndCount), static_cast<int>(kGame)}) {
        printf("--- %s, with a node that isn't theirs first in the forearm and thigh-twist lists\n", kFakeLayoutName[layout]);
        g_fakeLayout = layout;
        g_foreignFirst = true;
        g_layoutState = 0;
        g_layoutTries = 0;
        g_layoutRetryMs = 0;
        for (const char* p : {"arms_forward", "arms_crossed", "sitting"}) fails += RunCase(p, {}, 0.01f, false);
        g_foreignFirst = false;
    }
    {   // the game's layout is taken without the scan, and learning it, searching a new body and its collisions
        // ask Windows nothing (a search through Windows costs ~25 ms per body); a geometry node (floats where an NiNode
        // keeps its children) is passed over without a read
        g_fakeLayout = kGame;
        g_layoutState = 0;
        g_layoutTries = 0;
        g_layoutRetryMs = 0;
        const std::uint64_t rd0 = g_readableCalls.load(), vq0 = g_vqCalls.load();
        const int bad = RunCase("arms_crossed", {}, 0.01f, false);
        const std::uint64_t rd = g_readableCalls.load() - rd0, vq = g_vqCalls.load() - vq0;
        const bool game = g_childLayout.mode == kArray && g_childLayout.off == 0x138 && g_childLayout.countOff == 0x142;
        alignas(16) static char geo[0x400];
        const float box[7] = {0.0f, 0.151f, 0.055f, 0.0f, 0.037f, 0.190f, 0.081f};   // centre at +0x130, half-extents +0x140
        memcpy(geo + 0x130, box, 12);
        memcpy(geo + 0x140, box + 4, 12);
        void* kids[8];
        const std::uint64_t caught0 = g_caughtReads.load();
        const int k = Children(geo, kGameChildLayout, kids, 8);
        const bool geoOk = k == 0 && g_caughtReads.load() == caught0;
        const bool ok = bad == 0 && game && rd == 0 && vq == 0 && geoOk;
        printf("%-14s %-40s layout %s; %llu memory checks (%llu asked Windows) learning it + search + collisions; geometry node: %d "
               "children, %s  %s\n", "arms_crossed", "(the game's layout, no Windows checks)",
               game ? "the game's (+0x138, count +0x142)" : "NOT the game's", static_cast<unsigned long long>(rd),
               static_cast<unsigned long long>(vq), k, geoOk ? "no read" : "READ", ok ? "OK" : "FAIL");
        fails += ok ? 0 : 1;
    }
    printf("--- collisions against the gravity shapes at x2 (layout: pointer + count)\n");
    g_fakeLayout = kPointerAndCount;
    g_layoutState = 0;
    g_layoutTries = 0;
    g_layoutRetryMs = 0;
    for (const char* p : {"rest", "arms_sides", "arms_hanging"}) fails += RunShapes(p);
    fails += RunGravityOff();
    fails += RunNoList();
    printf("--- glutes switched off, and other bodies\n");
    g_layoutState = 0;
    g_layoutTries = 0;
    g_layoutRetryMs = 0;
    fails += RunGlutesOff();
    fails += RunOtherBodies();
    printf("--- dead bodies, calves and feet, pushes from other bodies\n");
    fails += RunDeadBodies();
    printf("--- the world-collision proxy watch\n");
    fails += RunWorldProxies();
    {   // measure only, with a snapshot: contacts logged, nothing pushed
        g_layoutState = 0;
        g_layoutTries = 0;
        g_layoutRetryMs = 0;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\cases\\arms_crossed.txt", g_tests);
        Fake fk;
        fk.kidLists.reserve(512);
        std::map<std::string, Vec3> expect;
        Load(path, fk, expect, {"L_Wrist", "R_Wrist", "L_Middle1", "R_Middle1"});
        ActorView v;
        v.com = fk.node["COM"];
        v.site = 2;
        const char* bone[4] = {"Upper", "Lower", "Inner", "Outer"};
        for (int s = 0; s < 2; ++s) {
            v.anchors[s].node = fk.node[s ? "R_Pecs" : "L_Pecs"];
            v.anchors[s].com = v.com;
            for (int b = 0; b < 4; ++b) v.anchors[s].bones[b] = fk.node[std::string("TF_Breast_") + bone[b] + (s ? "_R" : "_L")];
        }
        g_cfg.measureOnly = true;
        g_snapDue = true;
        const std::uint64_t before = g_pairHits[0][1].load() + g_pairHits[0][2].load();
        Collide(v);
        g_cfg.measureOnly = false;
        double biggest = 0;
        for (int i = 0; i < v.nAdd; ++i)
            for (int k = 0; k < 3; ++k) biggest = std::fmax(biggest, std::fabs(v.add[i][k]));
        const bool counted = g_pairHits[0][1].load() + g_pairHits[0][2].load() > before;
        const bool ok = biggest == 0 && counted && !g_snapDue.load();
        printf("%-14s %-40s contacts counted %s, largest push %.3f cm, snapshot written %s  %s\n", "arms_crossed",
               "(measure only, snapshot)", counted ? "yes" : "no", biggest * 100, g_snapDue.load() ? "no" : "yes",
               ok ? "OK" : "FAIL");
        fails += ok ? 0 : 1;
    }
    printf("%s\n", fails ? "SOME CASES FAILED" : "all cases passed");
    return fails;
}
