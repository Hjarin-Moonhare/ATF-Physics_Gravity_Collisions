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
// A version 2 rig (TFDENT02) adds per region, as its flags say:
//   - one shared field (flag 2): the unknowns are the values F of the field's control points (samples of the region);
//     a dent bone's move is D = Pm F (the points' Wendland bumps of the bone's rest distance, radius from the file,
//     normalised), smoothed over the field's edges, each round solved exactly (BoxQP), with the guard rounds'
//     settings from the file;
//   - tilt (flag 1, with the field): a dent bone also turns by the slope of the dent around it, omega = g x i (g the
//     gradient of its lattice neighbours' moves in its tangent plane at rest, i its inward line, mesh space), so the
//     skin's normals follow the dent; the fit includes what the turn moves (G + Grot); the turn is atan |omega| about
//     omega, in the parent's frame (the parent's mesh-to-bone rotation x omega).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
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
    // version 2
    std::uint32_t flags = 0;                   // 1 the dents tilt, 2 one shared field
    float overT = 0.002f, overW = 4.0f, shortT = 0.001f, shortW = 2.0f;   // the guard rounds: a sample dented past its
    std::uint32_t rounds = 4;                  // want by overT weighs x overW, a contact short by shortT x shortW
    std::vector<std::uint16_t> field;          // the shared field's control points (samples)
    float fieldRadius = 0;                     // their bumps' radius (m)
    std::vector<Edge> fieldEdges;
    std::vector<float> fieldLap;               // nf x nf
    std::vector<std::uint32_t> pm0;            // per dent bone: D = sum of pmVal x F[pmIdx]
    std::vector<std::uint16_t> pmIdx;
    std::vector<float> pmVal;
    std::vector<std::uint32_t> om0;            // per dent bone: its tilt (mesh) = sum of omVal x D[omIdx]
    std::vector<std::uint16_t> omIdx;
    std::vector<V3> omVal;
    std::vector<std::uint32_t> gr0;            // per sample: what the tilt moves it inward, per unit of a dent bone's move
    std::vector<std::uint16_t> grIdx;
    std::vector<float> grVal;
    std::vector<float> srot;                   // nd x 9: each dent bone's parent's mesh-to-bone rotation (rows, no scale)
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
constexpr int kSweeps = 60, kBisect = 18;
constexpr float kSettled = 1e-7f;   // a sweep that changes no move by more than this (m) ends the sweeps
constexpr double kTiltRidge = 1e-6;   // m^2, on a dent bone's gradient fit

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

// version 2: the shared field's bumps and smoothing, the tilt's operators (in double, like the design model)
inline void Derive2(const Rig& rig, Region& R) {
    struct D3 { double x, y, z; };
    auto sub = [](D3 a, D3 b) { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; };
    auto dot = [](D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    auto cross = [](D3 a, D3 b) { return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    auto unit = [&](D3 a) { const double l = std::sqrt(dot(a, a)); return l > 1e-12 ? D3{a.x / l, a.y / l, a.z / l} : a; };
    auto d3 = [](V3 v) { return D3{v.x, v.y, v.z}; };
    const size_t nd = R.dent.size(), ns = R.samples.size();
    std::vector<D3> P(nd), I(nd);
    R.srot.assign(nd * 9, 0.0f);
    for (size_t k = 0; k < nd; ++k) {   // the dent bones' rest places and inward lines in mesh space
        const Bind& b = rig.bones[static_cast<size_t>(R.dent[k].parent)].bind;
        double m[9];
        for (int i = 0; i < 9; ++i) m[i] = b.r[i];
        const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                           m[2] * (m[3] * m[7] - m[4] * m[6]);
        const double inv[9] = {(m[4] * m[8] - m[5] * m[7]) / det, (m[2] * m[7] - m[1] * m[8]) / det,
                               (m[1] * m[5] - m[2] * m[4]) / det, (m[5] * m[6] - m[3] * m[8]) / det,
                               (m[0] * m[8] - m[2] * m[6]) / det, (m[2] * m[3] - m[0] * m[5]) / det,
                               (m[3] * m[7] - m[4] * m[6]) / det, (m[1] * m[6] - m[0] * m[7]) / det,
                               (m[0] * m[4] - m[1] * m[3]) / det};
        auto mul = [&](const double* a, D3 v) {
            return D3{a[0] * v.x + a[1] * v.y + a[2] * v.z, a[3] * v.x + a[4] * v.y + a[5] * v.z,
                      a[6] * v.x + a[7] * v.y + a[8] * v.z};
        };
        const V3 rest = R.dent[k].rest;
        P[k] = mul(inv, D3{rest.x - b.t[0], rest.y - b.t[1], rest.z - b.t[2]});
        I[k] = unit(mul(inv, d3(R.dent[k].inward)));
        const double sc = std::cbrt(std::fabs(det));
        for (int i = 0; i < 9; ++i) R.srot[k * 9 + static_cast<size_t>(i)] = static_cast<float>(m[i] / sc);
    }
    if (R.flags & 2) {   // the shared field: each dent bone's bumps of the control points, normalised
        const size_t nf = R.field.size();
        R.pm0.assign(nd + 1, 0);
        R.pmIdx.clear();
        R.pmVal.clear();
        std::vector<double> w(nf);
        for (size_t k = 0; k < nd; ++k) {
            R.pm0[k] = static_cast<std::uint32_t>(R.pmIdx.size());
            double sum = 0, best = 1e300;
            size_t nearest = 0;
            for (size_t m = 0; m < nf; ++m) {
                const D3 d = sub(P[k], d3(R.samples[R.field[m]].p));
                const double dist = std::sqrt(dot(d, d));
                const double x = dist / R.fieldRadius;
                w[m] = x >= 1 ? 0.0 : std::pow(1 - x, 4) * (4 * x + 1);
                sum += w[m];
                if (dist < best) { best = dist; nearest = m; }
            }
            if (sum <= 0) { w[nearest] = 1; sum = 1; }
            for (size_t m = 0; m < nf; ++m)
                if (w[m] > 0) {
                    R.pmIdx.push_back(static_cast<std::uint16_t>(m));
                    R.pmVal.push_back(static_cast<float>(w[m] / sum));
                }
        }
        R.pm0[nd] = static_cast<std::uint32_t>(R.pmIdx.size());
        R.fieldLap.assign(nf * nf, 0.0f);
        for (const Edge& e : R.fieldEdges) {
            const size_t a = e.a, b = e.b;
            R.fieldLap[a * nf + a] += e.w;
            R.fieldLap[b * nf + b] += e.w;
            R.fieldLap[a * nf + b] -= e.w;
            R.fieldLap[b * nf + a] -= e.w;
        }
    }
    if (R.flags & 1) {   // the tilt: each dent bone's omega from its neighbours' moves; what it moves each sample
        std::vector<std::vector<size_t>> nbr(nd);
        for (const Edge& e : R.edges) {
            nbr[e.a].push_back(e.b);
            nbr[e.b].push_back(e.a);
        }
        std::vector<std::vector<std::pair<size_t, D3>>> om(nd);   // omega_k = sum coef x D_m
        for (size_t k = 0; k < nd; ++k) {
            const D3 n = I[k];
            const D3 t1 = unit(cross(n, std::fabs(n.z) < 0.9 ? D3{0, 0, 1} : D3{1, 0, 0}));
            const D3 t2 = cross(n, t1);
            const std::vector<size_t>& nb = nbr[k];
            if (nb.size() < 2) continue;
            double a = kTiltRidge, bb = 0, c = kTiltRidge;   // U^T U + ridge
            std::vector<double> u1(nb.size()), u2(nb.size());
            for (size_t q = 0; q < nb.size(); ++q) {
                const D3 d = sub(P[nb[q]], P[k]);
                u1[q] = dot(d, t1);
                u2[q] = dot(d, t2);
                a += u1[q] * u1[q];
                bb += u1[q] * u2[q];
                c += u2[q] * u2[q];
            }
            const double det = a * c - bb * bb;
            D3 self{0, 0, 0};
            for (size_t q = 0; q < nb.size(); ++q) {
                const double g1 = (c * u1[q] - bb * u2[q]) / det, g2 = (a * u2[q] - bb * u1[q]) / det;
                const D3 g{t1.x * g1 + t2.x * g2, t1.y * g1 + t2.y * g2, t1.z * g1 + t2.z * g2};   // gradient per move
                const D3 o = cross(n, g);                                                     // -[n]x g = g x n
                om[k].push_back({nb[q], D3{-o.x, -o.y, -o.z}});
                self = D3{self.x + o.x, self.y + o.y, self.z + o.z};
            }
            om[k].push_back({k, self});
        }
        R.om0.assign(nd + 1, 0);
        R.omIdx.clear();
        R.omVal.clear();
        for (size_t k = 0; k < nd; ++k) {
            R.om0[k] = static_cast<std::uint32_t>(R.omIdx.size());
            for (const auto& [m, v] : om[k]) {
                R.omIdx.push_back(static_cast<std::uint16_t>(m));
                R.omVal.push_back({static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)});
            }
        }
        R.om0[nd] = static_cast<std::uint32_t>(R.omIdx.size());
        R.gr0.assign(ns + 1, 0);
        R.grIdx.clear();
        R.grVal.clear();
        std::vector<double> acc(nd);
        for (size_t si = 0; si < ns; ++si) {
            R.gr0[si] = static_cast<std::uint32_t>(R.grIdx.size());
            std::fill(acc.begin(), acc.end(), 0.0);
            const Sample& smp = R.samples[si];
            const D3 nn = unit(d3(smp.n));
            const D3 in{-nn.x, -nn.y, -nn.z};
            for (int i = 0; i < smp.nd; ++i) {
                const DentW& dw = R.dw[smp.dent0 + static_cast<std::uint32_t>(i)];
                const D3 av = cross(in, sub(d3(smp.p), P[dw.dent]));
                for (const auto& [m, v] : om[dw.dent]) acc[m] += -static_cast<double>(dw.c) * dot(av, v);
            }
            for (size_t m = 0; m < nd; ++m)
                if (acc[m] != 0.0) {
                    R.grIdx.push_back(static_cast<std::uint16_t>(m));
                    R.grVal.push_back(static_cast<float>(acc[m]));
                }
        }
        R.gr0[ns] = static_cast<std::uint32_t>(R.grIdx.size());
    }
}

inline bool Load(const std::uint8_t* data, size_t size, Rig& rig, std::string& err) {
    rig = Rig{};
    if (size < 8 || (std::memcmp(data, "TFDENT01", 8) && std::memcmp(data, "TFDENT02", 8))) {
        err = "not a dent rig file";
        return false;
    }
    const int version = data[7] == '2' ? 2 : 1;
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
        if (version == 2) {
            R.flags = r.Get<std::uint32_t>();
            R.overT = r.Get<float>();
            R.overW = r.Get<float>();
            R.shortT = r.Get<float>();
            R.shortW = r.Get<float>();
            R.rounds = r.Get<std::uint32_t>();
            if (!r.ok || R.rounds > 64 || (R.flags & ~3u) || ((R.flags & 1) && !(R.flags & 2))) {
                err = "bad region flags";
                return false;
            }
            if (R.flags & 2) {
                const std::uint32_t nf = r.Get<std::uint32_t>();
                if (!r.ok || nf == 0 || nf > 256) { err = "bad field count"; return false; }
                R.field.resize(nf);
                for (auto& f : R.field) f = r.Get<std::uint16_t>();
                R.fieldRadius = r.Get<float>();
                const std::uint32_t nfe = r.Get<std::uint32_t>();
                if (!r.ok || !(R.fieldRadius > 0) || nfe > 100000) { err = "bad field"; return false; }
                R.fieldEdges.resize(nfe);
                for (auto& e : R.fieldEdges) {
                    e.a = r.Get<std::uint16_t>();
                    e.b = r.Get<std::uint16_t>();
                    e.w = r.Get<float>();
                    if (e.a >= nf || e.b >= nf) { err = "field edge out of range"; return false; }
                }
            }
        }
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
        for (std::uint16_t f : R.field)
            if (f >= ns) { err = "field point out of range"; return false; }
        if (R.flags) Derive2(rig, R);
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
    std::vector<float> gm, F;   // a shared field's rows (samples x control points) and values
    std::vector<double> Md, rhsd, Fd, Lq, bq, xq;   // its fit (double): the normal matrix, its right side, the values,
    std::vector<int> Wq;                            // and the box QP's working set and buffers
    std::vector<size_t> fr;
    std::vector<std::pair<std::uint32_t, float>> more;   // the guard round's samples to weigh more, and by how much
};
struct RegionStats { int contacts = 0; float need = 0, dent = 0, left = 0, bone = 0, tilt = 0; };   // tilt: radians
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

// min 1/2 F'MF - rhs'F with 0 <= F <= hi (M symmetric positive definite, n x n): the primal active-set method, each step
// an exact solve on the free set (Cholesky); wk.Fd is the warm start and the result.  A step that would leave the box
// stops at the first bound it meets (the lowest index on a tie) and fixes that one; at the free set's solution the fixed
// one with the largest wrong-signed gradient (over 1e-9 x (1 + max |rhs|); the lowest index on a tie) is freed; at most
// 4n + 10 steps.
inline void BoxQP(const std::vector<double>& M, const std::vector<double>& rhs, size_t n, double hi, Work& wk) {
    std::vector<double>& F = wk.Fd;
    std::vector<int>& W = wk.Wq;
    W.assign(n, 0);
    double rmax = 0;
    for (size_t i = 0; i < n; ++i) {
        F[i] = F[i] < 0 ? 0 : (F[i] > hi ? hi : F[i]);
        W[i] = F[i] <= 0 ? -1 : (F[i] >= hi ? 1 : 0);
        rmax = std::fmax(rmax, std::fabs(rhs[i]));
    }
    const double tol = 1e-9 * (1.0 + rmax);
    for (size_t it = 0; it < 4 * n + 10; ++it) {
        wk.fr.clear();
        for (size_t i = 0; i < n; ++i)
            if (!W[i]) wk.fr.push_back(i);
        const size_t m = wk.fr.size();
        if (m) {
            wk.bq.resize(m);
            wk.xq.resize(m);
            wk.Lq.resize(m * m);
            for (size_t a = 0; a < m; ++a) {
                const size_t i = wk.fr[a];
                double v = rhs[i];
                for (size_t j = 0; j < n; ++j)
                    if (W[j]) v -= M[i * n + j] * F[j];
                wk.bq[a] = v;
            }
            for (size_t a = 0; a < m; ++a)   // Cholesky: the free block = L L'
                for (size_t c = 0; c <= a; ++c) {
                    double v = M[wk.fr[a] * n + wk.fr[c]];
                    for (size_t k = 0; k < c; ++k) v -= wk.Lq[a * m + k] * wk.Lq[c * m + k];
                    wk.Lq[a * m + c] = a == c ? std::sqrt(v > 1e-300 ? v : 1e-300) : v / wk.Lq[c * m + c];
                }
            for (size_t a = 0; a < m; ++a) {   // L y = b
                double v = wk.bq[a];
                for (size_t k = 0; k < a; ++k) v -= wk.Lq[a * m + k] * wk.xq[k];
                wk.xq[a] = v / wk.Lq[a * m + a];
            }
            for (size_t a = m; a-- > 0;) {   // L' x = y
                double v = wk.xq[a];
                for (size_t k = a + 1; k < m; ++k) v -= wk.Lq[k * m + a] * wk.xq[k];
                wk.xq[a] = v / wk.Lq[a * m + a];
            }
            bool out = false;
            for (size_t a = 0; a < m && !out; ++a) out = wk.xq[a] < 0 || wk.xq[a] > hi;
            if (out) {
                double alpha = 1;
                size_t block = n;
                int bound = 0;
                for (size_t a = 0; a < m; ++a) {
                    const size_t i = wk.fr[a];
                    const double d = wk.xq[a] - F[i];
                    if (wk.xq[a] < 0 && d < 0) {
                        const double al = -F[i] / d;
                        if (al < alpha) { alpha = al; block = i; bound = -1; }
                    } else if (wk.xq[a] > hi && d > 0) {
                        const double al = (hi - F[i]) / d;
                        if (al < alpha) { alpha = al; block = i; bound = 1; }
                    }
                }
                for (size_t a = 0; a < m; ++a) F[wk.fr[a]] += alpha * (wk.xq[a] - F[wk.fr[a]]);
                if (block < n) {
                    F[block] = bound < 0 ? 0 : hi;
                    W[block] = bound;
                }
                continue;
            }
            for (size_t a = 0; a < m; ++a) F[wk.fr[a]] = wk.xq[a];
        }
        size_t worst = n;
        double wv = tol;
        for (size_t i = 0; i < n; ++i) {
            if (!W[i]) continue;
            double g = -rhs[i];
            for (size_t j = 0; j < n; ++j) g += M[i * n + j] * F[j];
            if (W[i] == -1 && -g > wv) { worst = i; wv = -g; }
            else if (W[i] == 1 && g > wv) { worst = i; wv = g; }
        }
        if (worst == n) break;
        W[worst] = 0;
    }
}

// The dent bones' inward moves D (m, one per dent bone of the region; zeros when nothing comes near).  scale: the
// dents' size factor (E_MAX and D_MAX); actorScale: the body's scale (every length).  W (3 per dent bone, or null):
// each dent bone's tilt in its parent's frame (zeros unless the region tilts and tilt is on).
inline void SolveRegion(const Rig& rig, const Region& R, const Xf* world, const std::uint8_t* have, const float* morphW,
                        float scale, float actorScale, float* D, RegionStats* st, Work& wk, float* W = nullptr,
                        bool tilt = true) {
    const int nd = static_cast<int>(R.dent.size());
    for (int j = 0; j < nd; ++j) D[j] = 0.0f;
    if (W)
        for (int j = 0; j < 3 * nd; ++j) W[j] = 0.0f;
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
    const size_t ndz = static_cast<size_t>(nd);
    if (R.flags & 2) {   // version 2: one shared field, the tilt in the rows
        wk.nj.resize(ndz);
        for (size_t j = 0; j < ndz; ++j) {
            V3 n = Turn(world[R.dent[j].parent], R.dent[j].inward);
            const float ln = Len(n);
            wk.nj[j] = ln > 1e-12f ? Mul(n, 1.0f / ln) : n;
        }
        const bool tiltOn = tilt && (R.flags & 1);
        const size_t nf = R.field.size();
        wk.gm.assign(ns * nf, 0.0f);
        wk.fs.clear();
        for (size_t k = 0; k < ns; ++k) {
            float* row = &wk.gm[k * nf];
            const Sample& s = R.samples[k];
            for (int i = 0; i < s.nd; ++i) {
                const DentW& d = R.dw[s.dent0 + static_cast<std::uint32_t>(i)];
                const float gv = d.c * -Dot(wk.N[k], wk.nj[d.dent]);
                for (std::uint32_t q = R.pm0[d.dent]; q < R.pm0[d.dent + 1u]; ++q) row[R.pmIdx[q]] += gv * R.pmVal[q];
            }
            if (tiltOn)
                for (std::uint32_t q = R.gr0[k]; q < R.gr0[k + 1]; ++q) {
                    const std::uint16_t j = R.grIdx[q];
                    for (std::uint32_t p2 = R.pm0[j]; p2 < R.pm0[j + 1u]; ++p2) row[R.pmIdx[p2]] += R.grVal[q] * R.pmVal[p2];
                }
            for (size_t m = 0; m < nf; ++m)
                if (std::fabs(row[m]) > 1e-9f) { wk.fs.push_back(static_cast<std::uint32_t>(k)); break; }
        }
        wk.w.assign(ns, 1.0f);
        wk.Fd.assign(nf, 0.0);
        wk.Md.assign(nf * nf, 0.0);
        wk.rhsd.assign(nf, 0.0);
        wk.got.assign(ns, 0.0f);
        auto addRow = [&](std::uint32_t k, double wgt) {   // M += wgt row row', rhs += wgt row t
            const float* row = &wk.gm[k * nf];
            for (size_t a2 = 0; a2 < nf; ++a2) {
                if (row[a2] == 0.0f) continue;
                const double ra = row[a2] * wgt;
                wk.rhsd[a2] += ra * wk.t[k];
                double* mrow = &wk.Md[a2 * nf];
                for (size_t b2 = 0; b2 < nf; ++b2) mrow[b2] += ra * row[b2];
            }
        };
        for (std::uint32_t k : wk.fs) addRow(k, 1.0);
        for (size_t a2 = 0; a2 < nf * nf; ++a2) wk.Md[a2] += static_cast<double>(R.lamS) * R.fieldLap[a2];
        for (size_t a2 = 0; a2 < nf; ++a2) wk.Md[a2 * nf + a2] += 1e-6;
        DENT_PROF(3)
        const double dMax = static_cast<double>(R.dMax) * scale * actorScale;
        for (std::uint32_t round = 0; round < R.rounds; ++round) {
            BoxQP(wk.Md, wk.rhsd, nf, dMax, wk);
            DENT_PROF(4)
            for (std::uint32_t k : wk.fs) {
                const float* row = &wk.gm[k * nf];
                double v = 0;
                for (size_t m = 0; m < nf; ++m) v += row[m] * wk.Fd[m];
                wk.got[k] = static_cast<float>(v);
            }
            // the guard: overshoots weigh more, then contacts left short
            std::vector<std::pair<std::uint32_t, float>>& more = wk.more;
            more.clear();
            for (std::uint32_t k : wk.fs)
                if (wk.got[k] > wk.t[k] + R.overT) more.push_back({k, R.overW});
            for (std::uint32_t c : wk.con)
                if (wk.got[c] < wk.e[c] - R.shortT) {
                    bool in = false;
                    for (std::uint32_t k : wk.fs)
                        if (k == c) { in = true; break; }
                    if (in) more.push_back({c, R.shortW});
                }
            DENT_PROF(5)
            if (more.empty()) break;
            for (const auto& [k, f] : more) {
                const double dw = static_cast<double>(wk.w[k]) * (f - 1.0);
                wk.w[k] *= f;
                addRow(k, dw);
            }
        }
        wk.F.resize(nf);
        for (size_t m = 0; m < nf; ++m) wk.F[m] = static_cast<float>(wk.Fd[m]);
        for (size_t j = 0; j < ndz; ++j) {
            float v = 0;
            for (std::uint32_t q = R.pm0[j]; q < R.pm0[j + 1]; ++q) v += R.pmVal[q] * wk.F[R.pmIdx[q]];
            D[j] = v;
        }
        float tiltMax = 0;
        if (tiltOn && W)
            for (size_t j = 0; j < ndz; ++j) {
                V3 om{0, 0, 0};
                for (std::uint32_t q = R.om0[j]; q < R.om0[j + 1]; ++q) om = Add(om, Mul(R.omVal[q], D[R.omIdx[q]]));
                const float* sr = &R.srot[j * 9];
                W[3 * j + 0] = sr[0] * om.x + sr[1] * om.y + sr[2] * om.z;
                W[3 * j + 1] = sr[3] * om.x + sr[4] * om.y + sr[5] * om.z;
                W[3 * j + 2] = sr[6] * om.x + sr[7] * om.y + sr[8] * om.z;
                tiltMax = std::fmax(tiltMax, Len(om));
            }
        if (st) {
            for (std::uint32_t k : wk.fs) st->dent = std::fmax(st->dent, wk.got[k]);
            for (std::uint32_t c : wk.con) {
                st->need = std::fmax(st->need, wk.e[c]);
                st->left = std::fmax(st->left, wk.e[c] - wk.got[c]);
            }
            for (size_t j = 0; j < ndz; ++j) st->bone = std::fmax(st->bone, D[j]);
            st->tilt = std::atan(tiltMax);
        }
        return;
    }
    // the dent bones that take part: under the wanted dent, and one ring of their lattice neighbours
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
    for (std::uint32_t round = 0; round < R.rounds; ++round) {
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
            if (v > wk.t[k] + R.overT) { wk.w[k] *= R.overW; any = true; }
        }
        for (std::uint32_t c : wk.con)
            if (wk.got[c] < wk.e[c] - R.shortT) { wk.w[c] *= R.shortW; any = true; }
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

// A tilt's turn (atan |w| about w, w in the parent's frame) as the child's three axes in the parent's frame: the rows
// a node keeps its local rotation in.
inline void TiltAxes(const float* w, V3 ax[3]) {
    const float th = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (th < 1e-12f) {
        ax[0] = {1, 0, 0};
        ax[1] = {0, 1, 0};
        ax[2] = {0, 0, 1};
        return;
    }
    const float kx = w[0] / th, ky = w[1] / th, kz = w[2] / th, a = std::atan(th), sn = std::sin(a), c1 = 1 - std::cos(a);
    ax[0] = {1 + c1 * (kx * kx - 1), sn * kz + c1 * kx * ky, -sn * ky + c1 * kx * kz};   // column 0 of I + s K + c K^2
    ax[1] = {-sn * kz + c1 * kx * ky, 1 + c1 * (ky * ky - 1), sn * kx + c1 * ky * kz};
    ax[2] = {sn * ky + c1 * kx * kz, -sn * kx + c1 * ky * kz, 1 + c1 * (kz * kz - 1)};
}

}  // namespace dents
