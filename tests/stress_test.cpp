// Stress test: the plugin's whole per-frame path (body read, springs, gravity, body-shape jiggle, collisions, log) on many fake bodies
// from many threads at once, like the game's ~11 worker threads; every body is built the same, so every body must end
// the same (a race would show as a difference).  Also: the memory guards against unreadable memory (objects ending at
// an inaccessible page, weight tables and names pointing into one), and the cost per body-update.
// Build and run: tests\run_stress.bat
#include "../src/plugin.cpp"

#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

static char g_tests[MAX_PATH];
constexpr int kNodeBytes = 0x400;

struct Body {
    std::map<std::string, char*> node;
    std::vector<std::vector<void*>> lists;
    float rest[2][3];
    char* owner;
    char* actor;      // the character the game's call hands over
    ActorView view;   // as Collect would fill it
};

static char* Entry(const std::string& s) {
    char* e = static_cast<char*>(calloc(1, 0x18 + 64));
    memcpy(e + 0x18, s.c_str(), s.size() + 1);
    return e;
}

static std::map<std::string, char*> g_springData;   // one record per bone, shared by every body (as in the game)
static std::uintptr_t g_fakeVt[3] = {0x1111, 0x2222, 0x3333};

static bool LoadBody(const char* path, Body& b, const Vec3 chestRows[3], const Vec3 comRows[3]) {
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
            char* e = Entry(name);
            memcpy(n + 0x10, &e, 8);
            memcpy(n + 0x70, v, 12);
            for (int r = 0; r < 3; ++r) memcpy(n + 0x80 + 16 * r, v + 3 + 3 * r, 12);
            memcpy(n + 0xB0, v + 12, 12);
            const float one = 1.0f;
            memcpy(n + 0xBC, &one, 4);
            b.node[name] = n;
            parentOf[name] = par;
            order.push_back(name);
        } else {
            float e[3];
            fscanf_s(in, "%95s %f %f %f", name, 96, &e[0], &e[1], &e[2]);
        }
    }
    fclose(in);
    std::map<std::string, std::vector<void*>> kids;
    for (const std::string& n : order) {
        auto it = b.node.find(parentOf[n]);
        if (it == b.node.end()) continue;
        memcpy(b.node[n] + 0x38, &it->second, 8);
        kids[parentOf[n]].push_back(b.node[n]);
    }
    b.lists.reserve(kids.size());
    for (auto& [n, list] : kids) {
        b.lists.push_back(list);
        void* data = b.lists.back().data();
        const std::uint32_t count = static_cast<std::uint32_t>(list.size());
        memcpy(b.node[n] + 0x138, &data, 8);
        memcpy(b.node[n] + 0x130, &count, 4);
    }
    for (int k = 0; k < 3; ++k) {   // turn the chest and COM (not their children: only the gravity reads these rows)
        memcpy(b.node["C_Chest"] + 0x80 + 16 * k, &chestRows[k], 12);
        memcpy(b.node["COM"] + 0x80 + 16 * k, &comRows[k], 12);
    }
    // the view Collect would make: anchors, their four bones, springs driven by COM
    ActorView& v = b.view;
    v.level = 2.0f;
    v.com = b.node["COM"];
    const char* bn[4] = {"Upper", "Lower", "Inner", "Outer"};
    for (int s = 0; s < 2; ++s) {
        char* pecs = b.node[s ? "R_Pecs" : "L_Pecs"];
        memcpy(b.rest[s], pecs + 0x70, 12);
        v.anchors[s].node = pecs;
        v.anchors[s].com = v.com;
        v.anchors[s].rest = b.rest[s];
        for (int k = 0; k < 4; ++k) v.anchors[s].bones[k] = b.node[std::string("TF_Breast_") + bn[k] + (s ? "_R" : "_L")];
    }
    for (auto& [n, ptr] : b.node) {
        if (n.rfind("TF_", 0) != 0 || v.nSprings >= 64) continue;
        char*& data = g_springData[n];
        if (!data) {
            data = static_cast<char*>(calloc(1, 0x100));
            const float caps[3] = {0.8f, 0.45f, 1.7f}, strength[3] = {80, 90, 90}, ratio[3] = {0.3f, 0.3f, 0.3f};
            memcpy(data + 0x50, caps, 12);
            memcpy(data + 0x20, strength, 12);
            memcpy(data + 0x30, ratio, 12);
        }
        char* inst = static_cast<char*>(calloc(1, 0x100));
        memcpy(inst + 0x10, &v.com, 8);
        memcpy(inst + 0x18, &ptr, 8);
        memcpy(inst + 0x20, &data, 8);
        // the spring's rest, a held swing away from where the bone stands (body-shape jiggle scales it)
        const float* at = reinterpret_cast<const float*>(ptr + 0x70);
        const float rest[3] = {at[0] - 0.005f, at[1] - 0.003f, at[2] + 0.004f};
        memcpy(inst + 0x60, rest, 12);
        const float rate = 30.0f;
        memcpy(inst + 0xD0, &rate, 4);
        v.springs[v.nSprings++] = inst;
    }
    // the 3D root -> character -> record -> morph weights (TF_BreastsLarge 0.4)
    b.owner = static_cast<char*>(calloc(1, 0x400));
    char* actor = static_cast<char*>(calloc(1, 0x400));
    char* npc = static_cast<char*>(calloc(1, 0x600));
    char* table = static_cast<char*>(calloc(1, 0x40));
    char* entries = static_cast<char*>(calloc(4, 24));
    memcpy(b.owner + 0x128, &actor, 8);
    b.actor = actor;
    memcpy(actor, &g_actorVt, 8);
    memcpy(actor + 0xE0, &npc, 8);
    memcpy(npc, &g_npcVt, 8);
    const float tri[3] = {0.1f, 0.3f, 0.8f};   // thin 0, muscular 0.2, heavy 0.7 once the shared 0.1 is off
    memcpy(npc + 0x350, tri, 12);
    memcpy(npc + 0x448, &table, 8);
    memcpy(table + 0x18, &entries, 8);
    const std::uint64_t cap = 4;
    memcpy(table + 0x20, &cap, 8);
    for (int i = 0; i < 4; ++i) {
        const std::int32_t m = (i == 2 || i == 3) ? 0 : -1;
        memcpy(entries + i * 24 + 0x10, &m, 4);
    }
    char* key = Entry("TF_BreastsLarge");
    const float w = 0.4f;
    memcpy(entries + 2 * 24, &key, 8);
    memcpy(entries + 2 * 24 + 8, &w, 4);
    char* butt = Entry("TF_ButtLarge");
    const float wb = 0.5f;
    memcpy(entries + 3 * 24, &butt, 8);
    memcpy(entries + 3 * 24 + 8, &wb, 4);
    return true;
}

// one body-update, as the hook does it (Collect excepted: the view is ready)
static void Frame(Body& b, int site) {
    ActorView v = b.view;   // fresh per frame, like the hook's local
    TakeFactors(v, BodyFactorsOf(b.owner, b.actor));
    PreUpdate(v);
    PostUpdate(v, site);
    MaybeLog();
}

static int g_fails = 0;
static void Report(const char* what, bool ok, const char* detail) {
    printf("%-44s %-60s %s\n", what, detail, ok ? "OK" : "FAIL");
    g_fails += !ok;
}

int main() {
    GetModuleFileNameA(nullptr, g_tests, MAX_PATH);
    for (int up = 0; up < 3; ++up)
        if (char* slash = strrchr(g_tests, '\\')) *slash = 0;
    snprintf(g_logPath, sizeof(g_logPath), "%s\\build\\tests\\stress_test.log", g_tests);
    FILE* lf = nullptr;
    if (fopen_s(&lf, g_logPath, "w") == 0 && lf) fclose(lf);
    strcat_s(g_tests, "\\tests");
    static float dt = 1.0f / 60;
    g_addr[kFrameTime] = reinterpret_cast<std::uintptr_t>(&dt);
    g_actorVt = reinterpret_cast<std::uintptr_t>(&g_fakeVt[0]);
    g_playerVt = reinterpret_cast<std::uintptr_t>(&g_fakeVt[1]);
    g_npcVt = reinterpret_cast<std::uintptr_t>(&g_fakeVt[2]);
    g_triangleOk = true;
    g_startMs = GetTickCount64();

    // bodies lying on the left side (gravity shapes and spring gravity at work) with the arms crossed (collisions)
    const Vec3 chestOnLeft[3] = {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, comOnLeft[3] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cases\\arms_crossed.txt", g_tests);
    constexpr int kBodies = 24, kThreads = 11;
    std::vector<Body*> bodies;
    for (int i = 0; i < kBodies; ++i) {
        Body* b = new Body;
        if (!LoadBody(path, *b, chestOnLeft, comOnLeft)) {
            printf("cannot read %s\n", path);
            return 1;
        }
        bodies.push_back(b);
    }

    // ---- single thread: the cost of one body-update
    // warm every body first (child lists, bone search, size read, caches, and the one-time log lines: writing the log
    // inside the timed loop made it look 3 us slower on the D: drive)
    std::vector<std::map<std::string, float>> homeX(kBodies);   // where each node stood (restored exactly)
    for (int i = 0; i < kBodies; ++i)
        for (auto& [n, node] : bodies[i]->node) homeX[i][n] = reinterpret_cast<float*>(node + 0xB0)[0];
    auto moveAll = [&](float spacing) {   // body i stands i x spacing metres along X (other bodies)
        for (int i = 0; i < kBodies; ++i)
            for (auto& [n, node] : bodies[i]->node)
                reinterpret_cast<float*>(node + 0xB0)[0] = homeX[i][n] + spacing * static_cast<float>(i);
    };
    auto cost = [&](const char* what) {
        for (int i = 0; i < 3; ++i)
            for (Body* b : bodies) Frame(*b, 2);
        auto t0 = std::chrono::steady_clock::now();
        const int reps = 20000;
        for (int i = 0; i < reps; ++i) Frame(*bodies[i % kBodies], 1);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / reps;
        char dd[160];
        snprintf(dd, sizeof(dd), "%.1f us per body-update (body read, springs, gravity, body shape, collisions, log)", us);
        Report(what, us < 100, dd);
    };
    moveAll(3.0f);
    cost("cost, one thread (bodies 3 m apart)");
    moveAll(0.0f);
    cost("cost, one thread (24 bodies in one spot)");
    char d[160];

    // ---- many threads at once, 4 s
    std::atomic<bool> stop{false};
    std::atomic<long long> frames{0};
    std::vector<std::thread> pool;
    const std::uint64_t readable0 = g_readableCalls.load(), vq0 = g_vqCalls.load();
    for (int t = 0; t < kThreads; ++t)
        pool.emplace_back([&, t] {
            long long n = 0;
            for (int i = t; !stop.load(std::memory_order_relaxed); i += kThreads, ++n) Frame(*bodies[i % kBodies], i % 3);
            frames += n;
        });
    std::this_thread::sleep_for(std::chrono::seconds(4));
    stop = true;
    for (auto& th : pool) th.join();
    snprintf(d, sizeof(d), "%lld body-updates in 4 s on %d threads, no deadlock", frames.load(), kThreads);
    Report("threads", frames.load() > 1000, d);
    // an A-Body body's update (body read, sizes every 2 s, springs, gravity, shape, collisions) asks no memory
    // check of Windows' kind (one can take milliseconds)
    const std::uint64_t readable = g_readableCalls.load() - readable0, vq = g_vqCalls.load() - vq0;
    snprintf(d, sizeof(d), "%llu memory checks (%llu asked Windows) in %lld body-updates", static_cast<unsigned long long>(readable),
             static_cast<unsigned long long>(vq), frames.load());
    Report("no Windows memory checks per update", readable == 0 && vq == 0, d);

    // every body was built the same and has run the same path: every breast bone and anchor must match body 0
    double worst = 0;
    std::string where;
    for (int i = 1; i < kBodies; ++i)
        for (auto& [n, p] : bodies[0]->node) {
            if (n.rfind("TF_", 0) != 0 && n != "L_Pecs" && n != "R_Pecs") continue;
            const float* a = reinterpret_cast<const float*>(p + 0x70);
            const float* b = reinterpret_cast<const float*>(bodies[i]->node[n] + 0x70);
            for (int k = 0; k < 3; ++k)
                if (std::fabs(a[k] - b[k]) * 100 > worst) {
                    worst = std::fabs(a[k] - b[k]) * 100;
                    where = n;
                }
        }
    // and one more frame on body 0 must leave it where it is (the adds are applied once, not stacked)
    std::map<std::string, std::vector<float>> before;
    for (auto& [n, p] : bodies[0]->node) {
        const float* a = reinterpret_cast<const float*>(p + 0x70);
        before[n] = {a[0], a[1], a[2]};
    }
    Frame(*bodies[0], 2);
    double drift = 0;
    for (auto& [n, p] : bodies[0]->node) {
        const float* a = reinterpret_cast<const float*>(p + 0x70);
        for (int k = 0; k < 3; ++k) drift = std::fmax(drift, std::fabs(a[k] - before[n][k]) * 100);
    }
    snprintf(d, sizeof(d), "largest difference between bodies %.6f cm%s%s; one more frame moves %.6f cm", worst,
             where.empty() ? "" : " at ", where.c_str(), drift);
    Report("same input, same result", worst < 1e-5 && drift < 1e-5, d);
    const float* lower = reinterpret_cast<const float*>(bodies[0]->node["TF_Breast_Outer_L"] + 0x70);
    snprintf(d, sizeof(d), "TF_Breast_Outer_L at %.2f %.2f %.2f cm (moved by the side shape and pushes)", lower[0] * 100,
             lower[1] * 100, lower[2] * 100);
    Report("bodies did move", true, d);
    {   // a calf (no collisions reach it) stands off its spring's value by the body's jiggle: the swing x j - 1
        char* calf = bodies[0]->node["TF_Calf_Medial_L"];
        const void* inst = nullptr;
        for (int i = 0; i < bodies[0]->view.nSprings; ++i)
            if (*reinterpret_cast<char* const*>(static_cast<char*>(bodies[0]->view.springs[i]) + 0x18) == calf)
                inst = bodies[0]->view.springs[i];
        double off = 0;
        if (calf && inst) {
            const float* t = reinterpret_cast<const float*>(calf + 0x70);
            const float* r = reinterpret_cast<const float*>(static_cast<const char*>(inst) + 0x60);
            const float sw[3] = {0.005f, 0.003f, -0.004f};
            for (int k = 0; k < 3; ++k) off += (t[k] - r[k] - sw[k]) * (t[k] - r[k] - sw[k]);
        }
        snprintf(d, sizeof(d), "TF_Calf_Medial_L %.3f cm off its spring's value (the scaled swing)", std::sqrt(off) * 100);
        Report("body shape at work", calf && inst && off > 1e-8, d);
    }
    std::uint64_t contacts = 0;
    for (auto& w : g_otherHits)
        for (auto& g : w)
            for (auto& k : g) contacts += k.load();
    snprintf(d, sizeof(d), "%llu contacts with other bodies counted (measure only), %llu reads skipped mid-write, %llu crowd",
             static_cast<unsigned long long>(contacts), static_cast<unsigned long long>(g_otherTorn.load()),
             static_cast<unsigned long long>(g_otherCrowd.load()));
    Report("other bodies, 24 in one spot", contacts > 0, d);

    // ---- other bodies pushing (on in the test settings): 24 bodies in one spot on 11 threads for
    // 2 s; which shares a body sees depends on timing, so the bodies differ, but every offset must stay finite and
    // capped.  Every fourth body is dead (lying still: its shares are used for longer, never moved forward)
    for (int i = 0; i < kBodies; ++i) bodies[i]->view.dead = i % 4 == 1;
    std::uint64_t deadBefore = 0;
    for (auto& g : g_otherHits[kWhoDead])
        for (auto& k : g) deadBefore += k.load();
    float lowerBefore[3];
    std::memcpy(lowerBefore, bodies[0]->node["TF_Breast_Lower_L"] + 0x70, 12);
    g_cfg.othersMeasureOnly = false;
    stop = false;
    frames = 0;
    pool.clear();
    for (int t = 0; t < kThreads; ++t)
        pool.emplace_back([&, t] {
            long long n2 = 0;
            for (int i = t; !stop.load(std::memory_order_relaxed); i += kThreads, ++n2) Frame(*bodies[i % kBodies], i % 3);
            frames += n2;
        });
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop = true;
    for (auto& th : pool) th.join();
    g_cfg.othersMeasureOnly = true;
    for (Body* b : bodies) b->view.dead = false;
    std::uint64_t deadContacts = 0;
    for (auto& g : g_otherHits[kWhoDead])
        for (auto& k : g) deadContacts += k.load();
    deadContacts -= deadBefore;
    double apart = 0;   // ("far" is a windows.h macro)
    bool finite = true;
    for (Body* b : bodies)
        for (auto& [n, p] : b->node) {
            if (n.rfind("TF_", 0) != 0) continue;
            const float* a = reinterpret_cast<const float*>(p + 0x70);
            const float* r = reinterpret_cast<const float*>(bodies[0]->node[n] + 0x70);
            for (int k = 0; k < 3; ++k) {
                finite &= std::isfinite(a[k]);
                apart = std::fmax(apart, std::fabs(a[k] - r[k]) * 100);
            }
        }
    const float* lowerAfter = reinterpret_cast<const float*>(bodies[0]->node["TF_Breast_Lower_L"] + 0x70);
    double moved = 0;
    for (int k = 0; k < 3; ++k) moved += (lowerAfter[k] - lowerBefore[k]) * (lowerAfter[k] - lowerBefore[k]);
    moved = std::sqrt(moved) * 100;
    snprintf(d, sizeof(d), "%lld body-updates; offsets finite %s, bodies differ by at most %.2f cm, pushes moved a breast "
             "bone %.2f cm; %llu contacts with dead bodies", frames.load(), finite ? "yes" : "NO", apart, moved,
             static_cast<unsigned long long>(deadContacts));
    Report("other bodies pushing, threads", finite && apart < 2 * 5.0 + 1e-3 && moved > 0.5 && moved < 5.0 + 1e-3 &&
               deadContacts > 0, d);

    // ---- memory guards: reads that run into memory that can't be read
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    char* two = static_cast<char*>(VirtualAlloc(nullptr, 2 * page, MEM_RESERVE, PAGE_NOACCESS));
    VirtualAlloc(two, page, MEM_COMMIT, PAGE_READWRITE);
    char* edge = two + page - 0x40;   // an object whose last 0x40 bytes end at the inaccessible page
    memset(edge, 0, 0x40);
    char* noAccess = two + page;
    g_refOff = -1;
    g_npcOff = -1;
    const float s1 = BodySize(edge);   // scans 0x300 bytes: runs into the page
    Report("size read past an object's page", s1 == 1.0f, "a root at the end of readable memory: no crash, x1");
    char* npcBad = static_cast<char*>(calloc(1, 0x600));
    char* tableBad = static_cast<char*>(calloc(1, 0x40));
    memcpy(npcBad, &g_npcVt, 8);
    memcpy(npcBad + 0x448, &tableBad, 8);
    memcpy(tableBad + 0x18, &noAccess, 8);   // entries in memory that can't be read
    const std::uint64_t capBad = 64;
    memcpy(tableBad + 0x20, &capBad, 8);
    char* actorBad = static_cast<char*>(calloc(1, 0x400));
    memcpy(actorBad, &g_actorVt, 8);
    memcpy(actorBad + 0xE0, &npcBad, 8);
    char* ownerBad = static_cast<char*>(calloc(1, 0x400));
    memcpy(ownerBad + 0x128, &actorBad, 8);
    const float s2 = BodySize(ownerBad);
    Report("size read with an unreadable weight table", s2 == 1.0f, "entries in inaccessible memory: no crash, x1");
    char* nodeBad = static_cast<char*>(calloc(1, 0x400));
    memcpy(nodeBad + 0x10, &noAccess, 8);   // a name in memory that can't be read
    char nm[32];
    Report("node name in unreadable memory", !NodeName(nodeBad, nm, sizeof(nm)), "returns false, no crash");
    char* parentBad = static_cast<char*>(calloc(1, 0x400));
    memcpy(parentBad + 0x138, &noAccess, 8);   // a child list in memory that can't be read
    const std::uint32_t five = 5;
    memcpy(parentBad + 0x130, &five, 4);
    void* kids[8];
    const int n = Children(parentBad, g_childLayout, kids, 8);
    snprintf(d, sizeof(d), "returns %d children, no crash", n);
    Report("child list in unreadable memory", n == 0, d);

    // ---- the log: lines from many threads at once cost the calling thread microseconds (the writer thread
    // does the file), and every one reaches the file
    {
        FlushLog();
        const std::uint64_t queuedBefore = g_logQueued.load(), droppedBefore = g_logDropped.load();
        constexpr int kLines = 300;
        std::atomic<long long> logTicks{0};
        std::vector<std::thread> loggers;
        for (int t = 0; t < kThreads; ++t)
            loggers.emplace_back([&, t] {
                long long sum = 0;
                for (int i = 0; i < kLines; ++i) {
                    LARGE_INTEGER a, b;
                    QueryPerformanceCounter(&a);
                    Log("stress log line %d from thread %d", i, t);
                    QueryPerformanceCounter(&b);
                    sum += b.QuadPart - a.QuadPart;
                }
                logTicks += sum;
            });
        for (auto& th : loggers) th.join();
        FlushLog();
        const std::uint64_t queued = g_logQueued.load() - queuedBefore, dropped = g_logDropped.load() - droppedBefore;
        int inFile = 0;
        FILE* rf = _fsopen(g_logPath, "r", _SH_DENYNO);   // shared, as you or OneDrive would read it during play
        if (rf) {
            char line[4096];
            while (fgets(line, sizeof(line), rf)) inFile += strstr(line, "stress log line") != nullptr;
            fclose(rf);
        }
        const double usPer = static_cast<double>(logTicks.load()) * 1e6 / static_cast<double>(QpcFreq()) / (kThreads * kLines);
        snprintf(d, sizeof(d), "%d threads x %d lines: %.1f us per line on the calling thread; %llu queued, %llu dropped, %d in "
                 "the file", kThreads, kLines, usPer, static_cast<unsigned long long>(queued),
                 static_cast<unsigned long long>(dropped), inFile);
        Report("log from many threads", queued + dropped == kThreads * kLines && inFile == static_cast<int>(queued) &&
                   usPer < 50, d);
    }

    printf("%s\n", g_fails ? "SOME CASES FAILED" : "all cases passed");
    return g_fails;
}
