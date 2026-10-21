// Dents: a body's dent rig (DentRig.bin, written with the body by A-Body's abody_dent_rig.py) and its solve, with no
// game types (the plugin hands in the bones' world transforms; tests hand in their own).  The same steps as
// tools/dent_model.py, so a test can hold the two to each other.
//
// A dent bone rides on a bone (its parent) and carries that bone's skin weight in its region: at its rest place it
// moves exactly as its parent, so the skin only moves by its own offset.  Per region:
//   1. every sample's place: its rest (mesh space) with the body's sliders' changes, skinned by its bones (world x
//      mesh-to-bone); its normal turned by its strongest bone;
//   2. the arm, hand and leg shapes the region uses (capsules between bone origins, a tip along its parent's line,
//      the start cut off), only if one comes near the region;
//   3. a sample inside a shape (radius + margin; not flagged; dent share at least 0.05) needs the inward move e along
//      its normal that takes it out (bisection, at most E_MAX);
//   4. the wanted dent t = the largest e x bump(d / FALL) of the samples in contact within FALL (at least e on a
//      contact), smoothed TSM rounds over the samples' neighbours (contacts kept at their e);
//   5. the dent bones under the wanted dent and one ring of their neighbours (the rest stay at 0) take inward moves D
//      that minimise |G D - t|^2 + LAM_S |L D|^2 (G: a sample's share of a bone x the cosine of its inward normal and
//      the bone's line; L the lattice's weighted edges), each in [0, D_MAX] (projected Gauss-Seidel, until no move
//      changes by 0.1 um); then a sample dented past its wanted dent + 2 mm weighs x4, a contact short by over 1 mm
//      x2, and again (4 rounds at most);
//   6. a dent bone's local translation = its rest + D x its inward line (parent frame).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dents {

struct V3 { float x, y, z; };
inline V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }

// a bone's world transform as the game stores it: ax[i] = local axis i in world, pos, uniform scale
struct Xf { V3 ax[3]; V3 pos; float scale; };
inline V3 Apply(const Xf& x, V3 l) {
    return Add(x.pos, Mul(Add(Add(Mul(x.ax[0], l.x), Mul(x.ax[1], l.y)), Mul(x.ax[2], l.z)), x.scale));
}
inline V3 Turn(const Xf& x, V3 l) { return Add(Add(Mul(x.ax[0], l.x), Mul(x.ax[1], l.y)), Mul(x.ax[2], l.z)); }

struct Bind { float r[9]; float t[3]; };   // mesh-to-bone: p_bone = r p + t (r row-major, scale included)
inline V3 ToBone(const Bind& b, V3 p) {
    return {b.r[0] * p.x + b.r[1] * p.y + b.r[2] * p.z + b.t[0], b.r[3] * p.x + b.r[4] * p.y + b.r[5] * p.z + b.t[1],
            b.r[6] * p.x + b.r[7] * p.y + b.r[8] * p.z + b.t[2]};
}
inline V3 TurnToBone(const Bind& b, V3 n) {
    return {b.r[0] * n.x + b.r[1] * n.y + b.r[2] * n.z, b.r[3] * n.x + b.r[4] * n.y + b.r[5] * n.z,
            b.r[6] * n.x + b.r[7] * n.y + b.r[8] * n.z};
}

struct Bone { std::string name; bool hasBind = false; Bind bind{}; };
struct Collider { std::string name; char side = 'L'; std::uint8_t kind = 0; int a = -1, b = -1, tipParent = -1;
                  float tip = 0, radius = 0, start = 0; };
struct DentBone { std::string name; int parent = -1; V3 rest{}, inward{}; };
struct Edge { std::uint16_t a, b; float w; };
struct Lbs { std::uint16_t bone; float w; };
struct DentW { std::uint16_t dent; float c; };
struct MorphD { std::uint16_t morph; V3 d; };
struct Sample {
    V3 p{}, n{};
    float share = 0;
    std::uint8_t flags = 0;
    std::uint32_t lbs0 = 0, dent0 = 0, morph0 = 0;
    std::uint8_t nl = 0, nd = 0, nm = 0;
};
struct Region {
    std::string name;
    std::uint8_t kind = 0;   // 0 breast, 1 glute, 2 thigh
    char side = 'L';
    float margin = 0, eMax = 0, fall = 0, dMax = 0, lamS = 0;
    std::uint32_t tsm = 0;
    std::vector<DentBone> dent;
    std::vector<Edge> edges;
    std::vector<Sample> samples;
    std::vector<Lbs> lbs;
    std::vector<DentW> dw;
    std::vector<MorphD> md;
    std::vector<std::uint16_t> use;
    // derived at load
    std::vector<std::uint32_t> nbr0, nbr;      // per sample: the samples within 1.25 x FALL (rest), offsets into nbr
    std::vector<std::uint32_t> sm0, sm;        // per sample: the samples within 1.5 cm (the smoothing)
    std::vector<float> lap;                    // nd x nd, the weighted lattice Laplacian (dense: nd <= 255)
    V3 centre{};                               // rest bounds (mesh space): centre and radius, sliders included
    float radius = 0;
    int carrier = -1;                          // the bone the bounds ride on (the first dent bone's parent)
};
struct Rig {
    std::vector<Bone> bones;
    std::vector<std::string> morphs;
    std::vector<Collider> colliders;
    std::vector<Region> regions;
    int totalDent = 0;
};

constexpr float kNeighbour = 0.015f;
constexpr float kSlack = 0.08f;   // the region's bounds + this much: its bones' own moves (springs, pushes) on top
constexpr int kSweeps = 60, kRounds = 4, kBisect = 18;
constexpr float kSettled = 1e-7f;   // a sweep that changes no move by more than this (m) ends the sweeps

inline float Wendland(float x) {
    if (x <= 0) return 1.0f;
    if (x >= 1) return 0.0f;
    const float a = 1 - x;
    return a * a * a * a * (4 * x + 1);
}

// ---- loading
struct Reader {
    const std::uint8_t* p;
    size_t n, o = 0;
    bool ok = true;
    template <typename T> T Get() {
        T v{};
        if (o + sizeof(T) > n) { ok = false; return v; }
        std::memcpy(&v, p + o, sizeof(T));
        o += sizeof(T);
        return v;
    }
    std::string Str() {
        const std::uint16_t len = Get<std::uint16_t>();
        if (!ok || o + len > n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p + o), len);
        o += len;
        return s;
    }
    V3 Vec() { const float x = Get<float>(), y = Get<float>(), z = Get<float>(); return {x, y, z}; }
};

inline bool Load(const std::uint8_t* data, size_t size, Rig& rig, std::string& err) {
    rig = Rig{};
    if (size < 8 || std::memcmp(data, "TFDENT01", 8)) { err = "not a dent rig file"; return false; }
    Reader r{data, size, 8};
    const std::uint32_t nb = r.Get<std::uint32_t>();
    if (!r.ok || nb > 4096) { err = "bad bone count"; return false; }
    rig.bones.resize(nb);
    for (auto& b : rig.bones) {
        b.name = r.Str();
        b.hasBind = r.Get<std::uint8_t>() != 0;
        if (b.hasBind)
            for (int i = 0; i < 12; ++i) (i < 9 ? b.bind.r[i] : b.bind.t[i - 9]) = r.Get<float>();
    }
    const std::uint32_t nm = r.Get<std::uint32_t>();
    if (!r.ok || nm > 64) { err = "bad morph count"; return false; }
    rig.morphs.resize(nm);
    for (auto& m : rig.morphs) m = r.Str();
    const std::uint32_t nc = r.Get<std::uint32_t>();
    if (!r.ok || nc > 256) { err = "bad collider count"; return false; }
    rig.colliders.resize(nc);
    for (auto& c : rig.colliders) {
        c.name = r.Str();
        c.side = static_cast<char>(r.Get<std::uint8_t>());
        c.kind = r.Get<std::uint8_t>();
        c.a = r.Get<std::uint16_t>();
        c.b = r.Get<std::int16_t>();
        c.tipParent = r.Get<std::int16_t>();
        c.tip = r.Get<float>();
        c.radius = r.Get<float>();
        c.start = r.Get<float>();
        if (c.a >= static_cast<int>(nb) || c.b >= static_cast<int>(nb) || c.tipParent >= static_cast<int>(nb)) {
            err = "collider bone out of range";
            return false;
        }
    }
    const std::uint32_t nr = r.Get<std::uint32_t>();
    if (!r.ok || nr > 64) { err = "bad region count"; return false; }
    rig.regions.resize(nr);
    for (auto& R : rig.regions) {
        R.name = r.Str();
        R.kind = r.Get<std::uint8_t>();
        R.side = static_cast<char>(r.Get<std::uint8_t>());
        R.margin = r.Get<float>();
        R.eMax = r.Get<float>();
        R.fall = r.Get<float>();
        R.dMax = r.Get<float>();
        R.lamS = r.Get<float>();
        R.tsm = r.Get<std::uint32_t>();
        (void)r.Get<float>();   // the breast's top (the samples carry their own flag)
        const std::uint32_t nd = r.Get<std::uint32_t>();
        if (!r.ok || nd == 0 || nd > 255) { err = "bad dent bone count"; return false; }
        R.dent.resize(nd);
        for (auto& d : R.dent) {
            d.name = r.Str();
            d.parent = r.Get<std::uint16_t>();
            d.rest = r.Vec();
            d.inward = r.Vec();
            if (d.parent >= static_cast<int>(nb)) { err = "dent bone parent out of range"; return false; }
        }
        const std::uint32_t ne = r.Get<std::uint32_t>();
        if (!r.ok || ne > 100000) { err = "bad edge count"; return false; }
        R.edges.resize(ne);
        for (auto& e : R.edges) {
            e.a = r.Get<std::uint16_t>();
            e.b = r.Get<std::uint16_t>();
            e.w = r.Get<float>();
            if (e.a >= nd || e.b >= nd) { err = "edge out of range"; return false; }
        }
        const std::uint32_t ns = r.Get<std::uint32_t>();
        if (!r.ok || ns == 0 || ns > 20000) { err = "bad sample count"; return false; }
        R.samples.resize(ns);
        for (auto& s : R.samples) {
            s.p = r.Vec();
            s.n = r.Vec();
            s.share = r.Get<float>();
            s.flags = r.Get<std::uint8_t>();
            s.nl = r.Get<std::uint8_t>();
            s.lbs0 = static_cast<std::uint32_t>(R.lbs.size());
            for (int i = 0; i < s.nl; ++i) {
                Lbs l{r.Get<std::uint16_t>(), 0};
                l.w = r.Get<float>();
                if (l.bone >= nb || !rig.bones[l.bone].hasBind) { err = "sample bone without a bind"; return false; }
                R.lbs.push_back(l);
            }
            s.nd = r.Get<std::uint8_t>();
            s.dent0 = static_cast<std::uint32_t>(R.dw.size());
            for (int i = 0; i < s.nd; ++i) {
                DentW d{r.Get<std::uint16_t>(), 0};
                d.c = r.Get<float>();
                if (d.dent >= nd) { err = "sample dent bone out of range"; return false; }
                R.dw.push_back(d);
            }
            s.nm = r.Get<std::uint8_t>();
            s.morph0 = static_cast<std::uint32_t>(R.md.size());
            for (int i = 0; i < s.nm; ++i) {
                MorphD m{r.Get<std::uint16_t>(), {}};
                m.d = r.Vec();
                if (m.morph >= nm) { err = "sample morph out of range"; return false; }
                R.md.push_back(m);
            }
            if (!r.ok) { err = "cut short in a sample"; return false; }
        }
        const std::uint32_t nu = r.Get<std::uint32_t>();
        if (!r.ok || nu > nc) { err = "bad collider use count"; return false; }
        R.use.resize(nu);
        for (auto& u : R.use) {
            u = r.Get<std::uint16_t>();
            if (u >= nc) { err = "collider use out of range"; return false; }
        }
        // derived: neighbours (rest), the Laplacian, bounds with the sliders' largest moves
        R.nbr0.assign(ns + 1, 0);
        R.sm0.assign(ns + 1, 0);
        const float f2 = 1.5625f * R.fall * R.fall, s2 = kNeighbour * kNeighbour;   // 1.25 x FALL: posed distances vary
        for (std::uint32_t k = 0; k < ns; ++k) {
            R.nbr0[k] = static_cast<std::uint32_t>(R.nbr.size());
            R.sm0[k] = static_cast<std::uint32_t>(R.sm.size());
            for (std::uint32_t j = 0; j < ns; ++j) {
                const V3 d = Sub(R.samples[j].p, R.samples[k].p);
                const float dd = Dot(d, d);
                if (dd < f2) R.nbr.push_back(j);
                if (j != k && dd < s2) R.sm.push_back(j);
            }
        }
        R.nbr0[ns] = static_cast<std::uint32_t>(R.nbr.size());
        R.sm0[ns] = static_cast<std::uint32_t>(R.sm.size());
        R.lap.assign(static_cast<size_t>(nd) * nd, 0.0f);
        for (const Edge& e : R.edges) {
            R.lap[e.a * nd + e.a] += e.w;
            R.lap[e.b * nd + e.b] += e.w;
            R.lap[e.a * nd + e.b] -= e.w;
            R.lap[e.b * nd + e.a] -= e.w;
        }
        V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        float slide = 0;
        for (std::uint32_t k = 0; k < ns; ++k) {
            const V3 p = R.samples[k].p;
            lo = {std::fmin(lo.x, p.x), std::fmin(lo.y, p.y), std::fmin(lo.z, p.z)};
            hi = {std::fmax(hi.x, p.x), std::fmax(hi.y, p.y), std::fmax(hi.z, p.z)};
            float s = 0;
            for (int i = 0; i < R.samples[k].nm; ++i) s += Len(R.md[R.samples[k].morph0 + i].d);
            slide = std::fmax(slide, s);
        }
        R.centre = Mul(Add(lo, hi), 0.5f);
        R.radius = Len(Sub(hi, lo)) * 0.5f + slide;
        R.carrier = R.dent[0].parent;
        rig.totalDent += static_cast<int>(nd);
    }
    if (r.o != size) { err = "trailing bytes"; return false; }
    return true;
}

// ---- solving
struct Work {
    std::vector<V3> X, N;
    std::vector<float> e, t, t2, w, got, M, rhs, Da, gv;
    std::vector<std::uint32_t> con, fs;
    std::vector<V3> nj;
    std::vector<std::uint8_t> act, act2;
    std::vector<int> ia, slot, rowAt, colOf;
    std::vector<float> valOf;
};
struct RegionStats { int contacts = 0; float need = 0, dent = 0, left = 0, bone = 0; };
#ifdef DENTS_PROFILE
#include <chrono>
inline double g_prof[6] = {};   // place, needs, wanted, fit build, fit sweeps, the rest (microseconds)
struct ProfT {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    void Mark(int i) {
        const auto n = std::chrono::steady_clock::now();
        g_prof[i] += std::chrono::duration<double, std::micro>(n - t).count();
        t = n;
    }
};
#define DENT_PROF_START ProfT prof_;
#define DENT_PROF(i) prof_.Mark(i);
#else
#define DENT_PROF_START
#define DENT_PROF(i)
#endif

inline float SegDist(V3 q, V3 a, V3 b) {
    const V3 ab = Sub(b, a);
    const float l2 = Dot(ab, ab);
    float t = l2 > 1e-12f ? Dot(Sub(q, a), ab) / l2 : 0.0f;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return Len(Sub(q, Add(a, Mul(ab, t))));
}

// a collider's segment and radius in world (false if a bone is missing)
inline bool Segment(const Rig& rig, const Collider& c, const Xf* world, const std::uint8_t* have, float actorScale, V3& A,
                    V3& B, float& rad) {
    if (c.a < 0 || !have[c.a]) return false;
    A = world[c.a].pos;
    if (c.b >= 0) {
        if (!have[c.b]) return false;
        B = world[c.b].pos;
    } else {
        if (c.tipParent < 0 || !have[c.tipParent]) return false;
        V3 u = Sub(A, world[c.tipParent].pos);
        const float l = Len(u);
        if (l < 1e-9f) return false;
        B = Add(A, Mul(u, c.tip * actorScale / l));
    }
    A = Add(A, Mul(Sub(B, A), c.start));
    rad = c.radius * actorScale;
    (void)rig;
    return true;
}

// The dent bones' inward moves D (m, one per dent bone of the region; zeros when nothing comes near).  scale: the
// dents' size factor (E_MAX and D_MAX); actorScale: the body's scale (every length).
inline void SolveRegion(const Rig& rig, const Region& R, const Xf* world, const std::uint8_t* have, const float* morphW,
                        float scale, float actorScale, float* D, RegionStats* st, Work& wk) {
    const int nd = static_cast<int>(R.dent.size());
    for (int j = 0; j < nd; ++j) D[j] = 0.0f;
    if (st) *st = RegionStats{};
    if (R.carrier < 0 || !have[R.carrier]) return;
    for (const DentBone& d : R.dent)
        if (!have[d.parent]) return;
    // the shapes near the region
    const Bone& cb = rig.bones[static_cast<size_t>(R.carrier)];
    const V3 cw = Apply(world[R.carrier], ToBone(cb.bind, R.centre));
    const float cr = (R.radius + kSlack) * actorScale;
    struct Cap { V3 a, b; float rr; };
    Cap caps[128];
    int nCaps = 0;
    const float margin = R.margin * actorScale;
    for (std::uint16_t u : R.use) {
        V3 A, B;
        float rad;
        if (!Segment(rig, rig.colliders[u], world, have, actorScale, A, B, rad)) continue;
        if (SegDist(cw, A, B) > cr + rad + margin) continue;
        if (nCaps < 128) caps[nCaps++] = {A, B, rad + margin};
    }
    if (!nCaps) return;
    DENT_PROF_START
    // the samples' places
    const size_t ns = R.samples.size();
    wk.X.resize(ns);
    wk.N.resize(ns);
    for (size_t k = 0; k < ns; ++k) {
        const Sample& s = R.samples[k];
        V3 p = s.p;
        for (int i = 0; i < s.nm; ++i) {
            const MorphD& m = R.md[s.morph0 + i];
            const float w = morphW ? morphW[m.morph] : 0.0f;
            if (w != 0.0f) p = Add(p, Mul(m.d, w));
        }
        V3 x{0, 0, 0};
        int best = -1;
        float bw = -1;
        for (int i = 0; i < s.nl; ++i) {
            const Lbs& l = R.lbs[s.lbs0 + i];
            if (!have[l.bone]) continue;
            x = Add(x, Mul(Apply(world[l.bone], ToBone(rig.bones[l.bone].bind, p)), l.w));
            if (l.w > bw) { bw = l.w; best = l.bone; }
        }
        wk.X[k] = x;
        V3 n = best >= 0 ? Turn(world[best], TurnToBone(rig.bones[static_cast<size_t>(best)].bind, s.n)) : s.n;
        const float ln = Len(n);
        wk.N[k] = ln > 1e-12f ? Mul(n, 1.0f / ln) : n;
    }
    DENT_PROF(0)
    // needs
    const float eMax = R.eMax * scale * actorScale;
    wk.e.assign(ns, 0.0f);
    for (int c = 0; c < nCaps; ++c) {
        const Cap& cp = caps[c];
        const V3 lo{std::fmin(cp.a.x, cp.b.x) - cp.rr, std::fmin(cp.a.y, cp.b.y) - cp.rr, std::fmin(cp.a.z, cp.b.z) - cp.rr};
        const V3 hi{std::fmax(cp.a.x, cp.b.x) + cp.rr, std::fmax(cp.a.y, cp.b.y) + cp.rr, std::fmax(cp.a.z, cp.b.z) + cp.rr};
        for (size_t k = 0; k < ns; ++k) {
            const V3 x = wk.X[k];
            if (x.x < lo.x || x.y < lo.y || x.z < lo.z || x.x > hi.x || x.y > hi.y || x.z > hi.z) continue;
            const Sample& s = R.samples[k];
            if ((s.flags & 1) || s.share < 0.05f) continue;
            if (SegDist(x, cp.a, cp.b) >= cp.rr) continue;
            float need;
            if (SegDist(Sub(x, Mul(wk.N[k], eMax)), cp.a, cp.b) < cp.rr) {
                need = eMax;
            } else {
                float l = 0, h = eMax;
                for (int it = 0; it < kBisect; ++it) {
                    const float mid = (l + h) * 0.5f;
                    if (SegDist(Sub(x, Mul(wk.N[k], mid)), cp.a, cp.b) < cp.rr) l = mid;
                    else h = mid;
                }
                need = h;
            }
            if (need > wk.e[k]) wk.e[k] = need;
        }
    }
    wk.con.clear();
    for (size_t k = 0; k < ns; ++k)
        if (wk.e[k] > 0) wk.con.push_back(static_cast<std::uint32_t>(k));
    if (st) st->contacts = static_cast<int>(wk.con.size());
    DENT_PROF(1)
    if (wk.con.empty()) return;
    // the wanted dent
    const float fall = R.fall * actorScale, fall2 = fall * fall;
    wk.t.assign(ns, 0.0f);
    for (std::uint32_t c : wk.con) {
        const V3 xc = wk.X[c];
        for (std::uint32_t q = R.nbr0[c]; q < R.nbr0[c + 1]; ++q) {
            const std::uint32_t k = R.nbr[q];
            const V3 d = Sub(wk.X[k], xc);
            const float d2 = Dot(d, d);
            if (d2 >= fall2) continue;
            const float v = wk.e[c] * Wendland(std::sqrt(d2) / fall);
            if (v > wk.t[k]) wk.t[k] = v;
        }
    }
    for (std::uint32_t c : wk.con)
        if (wk.e[c] > wk.t[c]) wk.t[c] = wk.e[c];
    for (std::uint32_t round = 0; round < R.tsm; ++round) {
        wk.t2.resize(ns);
        for (size_t k = 0; k < ns; ++k) {
            const std::uint32_t a0 = R.sm0[k], b0 = R.sm0[k + 1];
            if (a0 == b0) { wk.t2[k] = wk.t[k]; continue; }
            float m = 0;
            for (std::uint32_t q = a0; q < b0; ++q) m += wk.t[R.sm[q]];
            wk.t2[k] = 0.5f * wk.t[k] + 0.5f * m / static_cast<float>(b0 - a0);
        }
        for (size_t k = 0; k < ns; ++k) wk.t[k] = std::fmax(wk.t2[k], wk.e[k]);
    }
    DENT_PROF(2)
    // the dent bones that take part: under the wanted dent, and one ring of their lattice neighbours
    const size_t ndz = static_cast<size_t>(nd);
    wk.act.assign(ndz, 0);
    for (size_t k = 0; k < ns; ++k) {
        if (wk.t[k] <= 0) continue;
        const Sample& s = R.samples[k];
        for (int i = 0; i < s.nd; ++i) wk.act[R.dw[s.dent0 + i].dent] = 1;
    }
    wk.act2 = wk.act;
    for (const Edge& e : R.edges) {
        if (wk.act[e.a]) wk.act2[e.b] = 1;
        if (wk.act[e.b]) wk.act2[e.a] = 1;
    }
    wk.ia.clear();             // the taking part's indices, in order
    wk.slot.assign(ndz, -1);    // a dent bone's place among them
    for (size_t j = 0; j < ndz; ++j)
        if (wk.act2[j]) { wk.slot[j] = static_cast<int>(wk.ia.size()); wk.ia.push_back(static_cast<int>(j)); }
    const size_t na = wk.ia.size();
    wk.fs.clear();             // the samples on them
    for (size_t k = 0; k < ns; ++k) {
        const Sample& s = R.samples[k];
        for (int i = 0; i < s.nd; ++i)
            if (wk.act2[R.dw[s.dent0 + i].dent]) { wk.fs.push_back(static_cast<std::uint32_t>(k)); break; }
    }
    // the fit
    wk.nj.resize(ndz);
    for (size_t j = 0; j < ndz; ++j) {
        V3 n = Turn(world[R.dent[j].parent], R.dent[j].inward);
        const float ln = Len(n);
        wk.nj[j] = ln > 1e-12f ? Mul(n, 1.0f / ln) : n;
    }
    // G (a sample's row: its dent bones' shares x the cosine of its inward normal and each bone's line), once per solve
    wk.gv.resize(R.dw.size());
    for (std::uint32_t k : wk.fs) {
        const Sample& s = R.samples[k];
        for (int i = 0; i < s.nd; ++i) {
            const DentW& d = R.dw[s.dent0 + i];
            wk.gv[s.dent0 + i] = d.c * -Dot(wk.N[k], wk.nj[d.dent]);
        }
    }
    auto g = [&](size_t k, int i) { return wk.gv[R.samples[k].dent0 + i]; };
    wk.w.assign(ns, 1.0f);
    wk.M.resize(static_cast<size_t>(na) * na);
    wk.rhs.resize(na);
    wk.Da.assign(na, 0.0f);
    wk.got.assign(ns, 0.0f);
    const float dMax = R.dMax * scale * actorScale;
    for (int round = 0; round < kRounds; ++round) {
        for (size_t x = 0; x < na; ++x) {
            const float* lrow = &R.lap[static_cast<size_t>(wk.ia[x]) * ndz];
            for (size_t y = 0; y < na; ++y) wk.M[x * na + y] = R.lamS * lrow[static_cast<size_t>(wk.ia[y])];
            wk.M[x * na + x] += 1e-6f;
            wk.rhs[x] = 0;
        }
        for (std::uint32_t k : wk.fs) {
            const Sample& s = R.samples[k];
            for (int i = 0; i < s.nd; ++i) {
                const int xa = wk.slot[R.dw[s.dent0 + i].dent];
                if (xa < 0) continue;
                const float ga = g(k, i) * wk.w[k];
                wk.rhs[static_cast<size_t>(xa)] += ga * wk.t[k];
                for (int i2 = 0; i2 < s.nd; ++i2) {
                    const int xb = wk.slot[R.dw[s.dent0 + i2].dent];
                    if (xb >= 0) wk.M[static_cast<size_t>(xa) * na + static_cast<size_t>(xb)] += ga * g(k, i2);
                }
            }
        }
        // each row's other non-zero entries
        wk.rowAt.assign(na + 1, 0);
        wk.colOf.clear();
        wk.valOf.clear();
        for (size_t x = 0; x < na; ++x) {
            wk.rowAt[x] = static_cast<int>(wk.colOf.size());
            const float* row = &wk.M[x * na];
            for (size_t y = 0; y < na; ++y)
                if (y != x && row[y] != 0.0f) {
                    wk.colOf.push_back(static_cast<int>(y));
                    wk.valOf.push_back(row[y]);
                }
        }
        wk.rowAt[na] = static_cast<int>(wk.colOf.size());
        DENT_PROF(3)
        for (int sw = 0; sw < kSweeps; ++sw) {
            float change = 0;
            for (size_t x = 0; x < na; ++x) {
                const float mxx = wk.M[x * na + x];
                if (mxx <= 0) continue;
                float r = wk.rhs[x];
                for (size_t q = static_cast<size_t>(wk.rowAt[x]); q < static_cast<size_t>(wk.rowAt[x + 1]); ++q)
                    r -= wk.valOf[q] * wk.Da[static_cast<size_t>(wk.colOf[q])];
                float v = r / mxx;
                v = v < 0 ? 0 : (v > dMax ? dMax : v);
                change = std::fmax(change, std::fabs(v - wk.Da[x]));
                wk.Da[x] = v;
            }
            if (change < kSettled) break;
        }
        DENT_PROF(4)
        bool any = false;
        for (std::uint32_t k : wk.fs) {
            const Sample& s = R.samples[k];
            float v = 0;
            for (int i = 0; i < s.nd; ++i) {
                const int x = wk.slot[R.dw[s.dent0 + i].dent];
                if (x >= 0) v += g(k, i) * wk.Da[static_cast<size_t>(x)];
            }
            wk.got[k] = v;
            if (v > wk.t[k] + 0.002f) { wk.w[k] *= 4.0f; any = true; }
        }
        for (std::uint32_t c : wk.con)
            if (wk.got[c] < wk.e[c] - 0.001f) { wk.w[c] *= 2.0f; any = true; }
        DENT_PROF(5)
        if (!any) break;
    }
    for (size_t x = 0; x < na; ++x) D[wk.ia[x]] = wk.Da[x];
    if (st) {
        for (std::uint32_t k : wk.fs) st->dent = std::fmax(st->dent, wk.got[k]);
        for (std::uint32_t c : wk.con) {
            st->need = std::fmax(st->need, wk.e[c]);
            st->left = std::fmax(st->left, wk.e[c] - wk.got[c]);
        }
        for (int j = 0; j < nd; ++j) st->bone = std::fmax(st->bone, D[j]);
    }
}

}  // namespace dents
