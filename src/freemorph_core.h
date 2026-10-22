// FreeMorph: the body's one free-form performance morph (TFFREE: a record for
// every vertex of both breasts in the body's performance morph.dat), rewritten every frame from a real-time flesh
// contact, so fingers sink into the breast and the flesh moves around and between them.
// No game types here: the plugin hands in the bones' world transforms, the sliders and the frame time; the tests
// hand in their own.  The same steps as A-Body's freemorph.py (Solver), so a test holds the two together.
//
// Per frame, per breast a limb comes near:
//   1. the breast's skin nodes: rest (mesh space) + the sliders' changes, skinned by their bones (linear blend); A = the
//      blended mesh-to-world 3x3 (exact for a displacement under linear blend skinning);
//   2. the limb shapes (round cones on the finger bones and the arms, rounded boxes on the palm) in the world;
//   3. projected Gauss-Seidel on the nodes' contact forces (mesh space), warm-started from the last frame: a node in a
//      limb is pushed out along the limb's normal (the nearest shape's, by central differences); a node a limb holds
//      (it touched it a frame before) keeps to its point on the limb while its sideways force stays under grip x its
//      normal force, past that it slides; the press saturates softly from cap1 to cap2 of the tissue's depth; every
//      force change moves the nodes within the table's reach (the flesh's precomputed response);
//   4. at the frame's end: a new touch is held (its point on the limb, in the limb bone's frame), a released node let
//      go, a sliding one held again where it now is;
//   5. the shown displacement: a press at once, a release eased over `release` s;
//   6. the records: f16 position delta, the vertex colour, the normal and the tangent turned as the deformed mesh's
//      welded normal turns (10:10:10, the morph.dat coding).
#pragma once
#include <emmintrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef FM_PROF
#include <chrono>
#endif

namespace fm {

// the test's profile (FM_PROF): µs in skinning, candidates, sweeps, the far columns, the frame's end, the shown
// displacement; table entries spread
inline double g_prof[8] = {};
inline std::uint64_t g_spread = 0;
#ifdef FM_PROF
#define FP_START auto _fp0 = std::chrono::high_resolution_clock::now();
#define FP(i) { const auto _fp1 = std::chrono::high_resolution_clock::now(); fm::g_prof[i] += std::chrono::duration<double, std::micro>(_fp1 - _fp0).count(); _fp0 = _fp1; }
#define FP_COUNT(n) (fm::g_spread += (n))
#else
#define FP_START
#define FP(i)
#define FP_COUNT(n) ((void)0)
#endif

struct V3 { float x, y, z; };
inline V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }

struct M3 { float m[9]; };   // row-major
inline V3 MulM(const M3& A, V3 v) {
    return {A.m[0] * v.x + A.m[1] * v.y + A.m[2] * v.z, A.m[3] * v.x + A.m[4] * v.y + A.m[5] * v.z,
            A.m[6] * v.x + A.m[7] * v.y + A.m[8] * v.z};
}
inline V3 MulMT(const M3& A, V3 v) {   // A^T v
    return {A.m[0] * v.x + A.m[3] * v.y + A.m[6] * v.z, A.m[1] * v.x + A.m[4] * v.y + A.m[7] * v.z,
            A.m[2] * v.x + A.m[5] * v.y + A.m[8] * v.z};
}
inline M3 MulMM(const M3& A, const M3& B) {
    M3 C{};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            C.m[r * 3 + c] = A.m[r * 3] * B.m[c] + A.m[r * 3 + 1] * B.m[3 + c] + A.m[r * 3 + 2] * B.m[6 + c];
    return C;
}
inline M3 Transpose(const M3& A) { return {{A.m[0], A.m[3], A.m[6], A.m[1], A.m[4], A.m[7], A.m[2], A.m[5], A.m[8]}}; }
inline bool Inverse(const M3& A, M3& out) {
    const float* a = A.m;
    const double c00 = a[4] * a[8] - a[5] * a[7], c01 = a[5] * a[6] - a[3] * a[8], c02 = a[3] * a[7] - a[4] * a[6];
    const double det = a[0] * c00 + a[1] * c01 + a[2] * c02;
    if (!(std::fabs(det) > 1e-20)) return false;
    const double id = 1.0 / det;
    out.m[0] = static_cast<float>(c00 * id);
    out.m[1] = static_cast<float>((a[2] * a[7] - a[1] * a[8]) * id);
    out.m[2] = static_cast<float>((a[1] * a[5] - a[2] * a[4]) * id);
    out.m[3] = static_cast<float>(c01 * id);
    out.m[4] = static_cast<float>((a[0] * a[8] - a[2] * a[6]) * id);
    out.m[5] = static_cast<float>((a[2] * a[3] - a[0] * a[5]) * id);
    out.m[6] = static_cast<float>(c02 * id);
    out.m[7] = static_cast<float>((a[1] * a[6] - a[0] * a[7]) * id);
    out.m[8] = static_cast<float>((a[0] * a[4] - a[1] * a[3]) * id);
    return true;
}
// x of A x = b (A symmetric positive definite here), in double
inline bool Solve3(const M3& A, V3 b, V3& x) {
    M3 inv;
    if (!Inverse(A, inv)) return false;
    x = MulM(inv, b);
    return true;
}

// a bone's world transform as the game stores it: ax[i] = local axis i in world (unit), pos, uniform scale
struct Xf { V3 ax[3]; V3 pos; float scale; };
inline V3 Apply(const Xf& x, V3 l) {
    return Add(x.pos, Mul(Add(Add(Mul(x.ax[0], l.x), Mul(x.ax[1], l.y)), Mul(x.ax[2], l.z)), x.scale));
}
inline M3 Axes(const Xf& x) {   // the bone's world rotation: its axes as columns
    return {{x.ax[0].x, x.ax[1].x, x.ax[2].x, x.ax[0].y, x.ax[1].y, x.ax[2].y, x.ax[0].z, x.ax[1].z, x.ax[2].z}};
}
inline V3 ToLocal(const Xf& x, V3 w) {   // a world point into the bone's frame (rotation and scale undone)
    const V3 d = Sub(w, x.pos);
    const float is = x.scale != 0.0f ? 1.0f / x.scale : 0.0f;
    return {Dot(d, x.ax[0]) * is, Dot(d, x.ax[1]) * is, Dot(d, x.ax[2]) * is};
}

struct Bind { float r[9]; float t[3]; };   // mesh-to-bone: p_bone = r p + t (r row-major, scale included)
struct Lbs { std::uint16_t bone; float w; };
struct Delta { std::uint16_t slider; V3 d; };

// kind 0: round cone, a (bone a's frame) radius r1 -> b (bone b's frame) radius r2; kind 1: rounded box in a bone's
// frame, centre c, axes R (columns, row-major storage), half extents h, rounding r
struct Shape {
    std::uint8_t kind = 0, side = 0, part = 0;
    std::uint16_t boneA = 0, boneB = 0, bone = 0;
    V3 a{}, b{}, c{}, h{};
    float r1 = 0, r2 = 0, r = 0;
    float R[9] = {};
};
struct WShape {   // a shape in the world this frame
    std::uint8_t kind = 0;
    std::uint16_t holdBone = 0;   // the bone a node it holds keeps to (cone: bone a)
    V3 A{}, B{}, C{}, H{};
    float R1 = 0, R2 = 0, Rr = 0;
    float Rw[9] = {};   // world axes of the box (columns, row-major storage)
    V3 centre{};
    float radius = 0;
};

// a node's response column: its entries nearest first; the first `inner` (within Params.inner of it) take part in the
// sweeps, the rest join once at the frame's end
struct Column { std::uint32_t at = 0, count = 0, inner = 0; float scale = 0; std::int32_t self = -1; };
struct Region {
    std::string name;
    char side = 'L';
    std::uint32_t n = 0;
    std::vector<V3> p, nrm;
    std::vector<float> depth;
    std::vector<std::uint8_t> fixed;
    std::vector<std::uint32_t> lbs0, d0;
    std::vector<std::uint8_t> nl, nd;
    std::vector<Lbs> lbs;
    std::vector<Delta> deltas;
    std::vector<Column> cols;
    std::vector<std::uint16_t> idx;
    std::vector<std::int16_t> vals;   // 9 per entry: [d * 3 + c] = node idx's displacement c under a force along d
    std::vector<std::uint16_t> tri;   // 3 per triangle, node indices
    std::uint32_t nv = 0;
    std::vector<std::uint16_t> vnode;
    std::vector<std::uint32_t> vrec;
    std::vector<V3> nfile, tfile;
    std::vector<std::uint16_t> color;
    std::vector<std::uint16_t> ring;
    std::vector<float> ringAng;
    std::vector<V3> nRest;   // derived: the welded normals at rest (face-normal sums)
};
struct Params {
    float cut = 0.08f, cap1 = 1.0f / 3.0f, cap2 = 0.5f, grip = 1.0f, cand = 0.015f, reach = 0.05f, tol = 5e-5f,
          release = 0.08f, fd = 1e-4f;
    float reg = 1e-3f;   // m/N: the contact's own give (a node rests reg x its force inside the limb): keeps the solve
                         // well posed where many nodes press under one finger
    float regT = 2e-2f;  // m/N: a held point's sideways give (the grip pulls like a spring of 1 / regT N/m)
    std::uint32_t sweeps = 12;
    float dentDepth = 0.01f, dentSigma = 0.012f, dentRing = 0.045f, dentSpread = 0.17f;
};
struct Data {
    std::string morph;
    std::uint32_t bodyVerts = 0, records = 0;
    std::vector<std::string> bones;
    std::vector<std::uint8_t> hasBind;
    std::vector<Bind> binds;
    std::vector<std::string> sliders;
    Params prm;
    std::vector<Shape> shapes;
    std::vector<Region> regions;
};

// ---- loading
struct Reader {
    const std::uint8_t* p;
    size_t n, at = 0;
    bool ok = true;
    template <class T> T get() {
        T v{};
        if (at + sizeof(T) > n) { ok = false; return v; }
        std::memcpy(&v, p + at, sizeof(T));
        at += sizeof(T);
        return v;
    }
    template <class T> void arr(std::vector<T>& v, size_t count) {
        if (at + sizeof(T) * count > n) { ok = false; v.clear(); return; }
        v.resize(count);
        if (count) std::memcpy(v.data(), p + at, sizeof(T) * count);
        at += sizeof(T) * count;
    }
    std::string str() {
        const std::uint16_t len = get<std::uint16_t>();
        if (!ok || at + len > n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p + at), len);
        at += len;
        return s;
    }
    V3 v3() { const float x = get<float>(), y = get<float>(), z = get<float>(); return {x, y, z}; }
};

inline void FaceNormals(const Region& R, const V3* P, std::vector<V3>& out) {
    out.assign(R.n, V3{0, 0, 0});
    for (size_t t = 0; t + 2 < R.tri.size(); t += 3) {
        const size_t a = static_cast<size_t>(R.tri[t]), b = static_cast<size_t>(R.tri[t + 1]),
                     c = static_cast<size_t>(R.tri[t + 2]);
        const V3 f = Cross(Sub(P[b], P[a]), Sub(P[c], P[a]));
        out[a] = Add(out[a], f);
        out[b] = Add(out[b], f);
        out[c] = Add(out[c], f);
    }
    for (V3& v : out) {
        const float L = Len(v);
        v = Mul(v, 1.0f / std::fmax(L, 1e-20f));
    }
}

// FreeMorph.bin (its layout: docs/BODY_INTEGRATION.md)
inline bool Load(const std::uint8_t* data, size_t size, Data& d, std::string& err) {
    Reader r{data, size};
    if (size < 8 || std::memcmp(data, "TFFM0001", 8)) { err = "not a TFFM0001 file"; return false; }
    r.at = 8;
    d = Data{};
    d.morph = r.str();
    d.bodyVerts = r.get<std::uint32_t>();
    d.records = r.get<std::uint32_t>();
    const std::uint32_t nb = r.get<std::uint32_t>();
    if (!r.ok || nb > 1024 || d.bodyVerts == 0 || d.bodyVerts > 65535 || d.records == 0 || d.records > d.bodyVerts) {
        err = "bad header";
        return false;
    }
    for (std::uint32_t i = 0; i < nb && r.ok; ++i) {
        d.bones.push_back(r.str());
        Bind b{};
        const std::uint8_t has = r.get<std::uint8_t>();
        if (has) {
            for (float& x : b.r) x = r.get<float>();
            for (float& x : b.t) x = r.get<float>();
        }
        d.hasBind.push_back(has);
        d.binds.push_back(b);
    }
    const std::uint32_t ns = r.get<std::uint32_t>();
    if (!r.ok || ns > 128) { err = "bad slider count"; return false; }
    for (std::uint32_t i = 0; i < ns && r.ok; ++i) d.sliders.push_back(r.str());
    Params& P = d.prm;
    P.cut = r.get<float>();
    P.cap1 = r.get<float>();
    P.cap2 = r.get<float>();
    P.grip = r.get<float>();
    P.cand = r.get<float>();
    P.reach = r.get<float>();
    P.tol = r.get<float>();
    P.release = r.get<float>();
    P.fd = r.get<float>();
    P.reg = r.get<float>();
    P.regT = r.get<float>();
    P.sweeps = r.get<std::uint32_t>();
    P.dentDepth = r.get<float>();
    P.dentSigma = r.get<float>();
    P.dentRing = r.get<float>();
    P.dentSpread = r.get<float>();
    if (!r.ok || !(P.cap2 > P.cap1) || !(P.cap1 > 0) || P.sweeps == 0 || P.sweeps > 200 || !(P.fd > 0) || !(P.dentSigma > 0) ||
        !(P.dentSpread > 0) || !(P.reg >= 0)) {
        err = "bad parameters";
        return false;
    }
    const std::uint32_t nsh = r.get<std::uint32_t>();
    if (!r.ok || nsh > 256) { err = "bad shape count"; return false; }
    for (std::uint32_t i = 0; i < nsh && r.ok; ++i) {
        Shape s;
        s.kind = r.get<std::uint8_t>();
        s.side = r.get<std::uint8_t>();
        s.part = r.get<std::uint8_t>();
        if (s.kind == 0) {
            s.boneA = r.get<std::uint16_t>();
            s.boneB = r.get<std::uint16_t>();
            s.a = r.v3();
            s.b = r.v3();
            s.r1 = r.get<float>();
            s.r2 = r.get<float>();
            if (s.boneA >= nb || s.boneB >= nb) { err = "shape bone out of range"; return false; }
        } else if (s.kind == 1) {
            s.bone = r.get<std::uint16_t>();
            s.c = r.v3();
            for (float& x : s.R) x = r.get<float>();
            s.h = r.v3();
            s.r = r.get<float>();
            if (s.bone >= nb) { err = "shape bone out of range"; return false; }
        } else {
            err = "bad shape kind";
            return false;
        }
        d.shapes.push_back(s);
    }
    const std::uint32_t nr = r.get<std::uint32_t>();
    if (!r.ok || nr > 8) { err = "bad region count"; return false; }
    std::vector<std::uint8_t> seen(d.records, 0);
    for (std::uint32_t ri = 0; ri < nr && r.ok; ++ri) {
        Region R;
        R.name = r.str();
        R.side = static_cast<char>(r.get<std::uint8_t>());
        R.n = r.get<std::uint32_t>();
        if (!r.ok || R.n == 0 || R.n > 65535) { err = "bad node count"; return false; }
        R.p.resize(R.n);
        R.nrm.resize(R.n);
        R.depth.resize(R.n);
        R.fixed.resize(R.n);
        R.lbs0.resize(R.n);
        R.d0.resize(R.n);
        R.nl.resize(R.n);
        R.nd.resize(R.n);
        for (std::uint32_t j = 0; j < R.n && r.ok; ++j) {
            R.p[j] = r.v3();
            R.nrm[j] = r.v3();
            R.depth[j] = r.get<float>();
            R.fixed[j] = r.get<std::uint8_t>();
            R.nl[j] = r.get<std::uint8_t>();
            R.lbs0[j] = static_cast<std::uint32_t>(R.lbs.size());
            for (int k = 0; k < R.nl[j]; ++k) {
                Lbs l{r.get<std::uint16_t>(), r.get<float>()};
                if (l.bone >= nb || !d.hasBind.size() || (r.ok && !d.hasBind[l.bone])) { err = "node bone without a bind"; return false; }
                R.lbs.push_back(l);
            }
            R.nd[j] = r.get<std::uint8_t>();
            R.d0[j] = static_cast<std::uint32_t>(R.deltas.size());
            for (int k = 0; k < R.nd[j]; ++k) {
                Delta dl{r.get<std::uint16_t>(), r.v3()};
                if (dl.slider >= ns) { err = "node slider out of range"; return false; }
                R.deltas.push_back(dl);
            }
        }
        R.cols.resize(R.n);
        for (std::uint32_t j = 0; j < R.n && r.ok; ++j) {
            Column& c = R.cols[j];
            c.count = r.get<std::uint32_t>();
            c.inner = r.get<std::uint32_t>();
            if (!r.ok || c.count > R.n || c.inner > c.count) { err = "bad column"; return false; }
            c.at = static_cast<std::uint32_t>(R.idx.size());
            c.scale = r.get<float>();
            std::vector<std::uint16_t> ix;
            std::vector<std::int16_t> vv;
            r.arr(ix, c.count);
            r.arr(vv, static_cast<size_t>(c.count) * 9);
            if (!r.ok) break;
            for (std::uint32_t k = 0; k < c.count; ++k) {
                if (ix[k] >= R.n) { err = "column index out of range"; return false; }
                if (ix[k] == j) c.self = static_cast<std::int32_t>(c.at + k);
            }
            if (c.count && c.self < 0) { err = "a column without its own node"; return false; }
            R.idx.insert(R.idx.end(), ix.begin(), ix.end());
            R.vals.insert(R.vals.end(), vv.begin(), vv.end());
        }
        const std::uint32_t nt = r.get<std::uint32_t>();
        if (!r.ok || nt > 200000) { err = "bad triangle count"; return false; }
        r.arr(R.tri, static_cast<size_t>(nt) * 3);
        for (std::uint16_t t : R.tri)
            if (t >= R.n) { err = "triangle out of range"; return false; }
        R.nv = r.get<std::uint32_t>();
        if (!r.ok || R.nv > d.records) { err = "bad vertex count"; return false; }
        R.vnode.resize(R.nv);
        R.vrec.resize(R.nv);
        R.nfile.resize(R.nv);
        R.tfile.resize(R.nv);
        R.color.resize(R.nv);
        for (std::uint32_t v = 0; v < R.nv && r.ok; ++v) {
            R.vnode[v] = r.get<std::uint16_t>();
            R.vrec[v] = r.get<std::uint32_t>();
            R.nfile[v] = r.v3();
            R.tfile[v] = r.v3();
            R.color[v] = r.get<std::uint16_t>();
            if (!r.ok) break;
            if (R.vnode[v] >= R.n || R.vrec[v] >= d.records || seen[R.vrec[v]]) { err = "bad vertex record"; return false; }
            seen[R.vrec[v]] = 1;
        }
        const std::uint32_t nring = r.get<std::uint32_t>();
        if (!r.ok || nring > R.n) { err = "bad ring"; return false; }
        r.arr(R.ring, nring);
        r.arr(R.ringAng, nring);
        for (std::uint16_t q : R.ring)
            if (q >= R.n) { err = "ring out of range"; return false; }
        if (!r.ok) break;
        FaceNormals(R, R.p.data(), R.nRest);
        d.regions.push_back(std::move(R));
    }
    if (!r.ok) { err = "the file ends early"; return false; }
    if (r.at != size) { err = "bytes left over"; return false; }
    for (std::uint32_t k = 0; k < d.records; ++k)
        if (!seen[k]) { err = "a record no vertex writes"; return false; }
    return true;
}

// ---- the limb shapes
inline float Sign(float x) { return static_cast<float>((x > 0.0f) - (x < 0.0f)); }
// exact signed distance to a round cone (I. Quilez), as contact_morphs.sd_round_cone
inline float SdRoundCone(V3 p, V3 a, V3 b, float r1, float r2) {
    const V3 ba = Sub(b, a);
    const float l2 = Dot(ba, ba);
    const float rr = r1 - r2;
    const float a2 = l2 - rr * rr;
    const float il2 = 1.0f / l2;
    const V3 pa = Sub(p, a);
    const float y = Dot(pa, ba);
    const float z = y - l2;
    const V3 xv = Sub(Mul(pa, l2), Mul(ba, y));
    const float x2 = Dot(xv, xv);
    const float y2 = y * y * l2;
    const float z2 = z * z * l2;
    const float k = Sign(rr) * rr * rr * x2;
    if (Sign(z) * a2 * z2 > k) return std::sqrt(x2 + z2) * il2 - r2;
    if (Sign(y) * a2 * y2 < k) return std::sqrt(x2 + y2) * il2 - r1;
    return (std::sqrt(std::fmax(x2 * a2 * il2, 0.0f)) + y * rr) * il2 - r1;
}
inline float SdRoundBox(V3 q, V3 h, float r) {
    const V3 d = {std::fabs(q.x) - h.x + r, std::fabs(q.y) - h.y + r, std::fabs(q.z) - h.z + r};
    const V3 dm = {std::fmax(d.x, 0.0f), std::fmax(d.y, 0.0f), std::fmax(d.z, 0.0f)};
    return Len(dm) + std::fmin(std::fmax(d.x, std::fmax(d.y, d.z)), 0.0f) - r;
}
inline float SdShape(const WShape& s, V3 p) {
    if (s.kind == 0) return SdRoundCone(p, s.A, s.B, s.R1, s.R2);
    const V3 d = Sub(p, s.C);
    const V3 q = {d.x * s.Rw[0] + d.y * s.Rw[3] + d.z * s.Rw[6], d.x * s.Rw[1] + d.y * s.Rw[4] + d.z * s.Rw[7],
                  d.x * s.Rw[2] + d.y * s.Rw[5] + d.z * s.Rw[8]};
    return SdRoundBox(q, s.H, s.Rr);
}
inline bool WorldShape(const Shape& s, const Xf* world, const std::uint8_t* have, WShape& o) {
    o.kind = s.kind;
    if (s.kind == 0) {
        if (!have[s.boneA] || !have[s.boneB]) return false;
        const Xf& Wa = world[s.boneA];
        const float sc = Wa.scale;   // contact_morphs: both radii x bone a's scale
        o.A = Apply(Wa, s.a);
        o.B = Apply(world[s.boneB], s.b);
        o.R1 = s.r1 * sc;
        o.R2 = s.r2 * sc;
        o.holdBone = s.boneA;
        o.centre = Mul(Add(o.A, o.B), 0.5f);
        o.radius = 0.5f * Len(Sub(o.A, o.B)) + std::fmax(o.R1, o.R2);
        return Len(Sub(o.B, o.A)) > 1e-6f;
    }
    if (!have[s.bone]) return false;
    const Xf& W = world[s.bone];
    o.C = Apply(W, s.c);
    const M3 Rot = Axes(W);
    for (int r = 0; r < 3; ++r)       // Rw = (the bone's world axes as columns) x R
        for (int c = 0; c < 3; ++c)
            o.Rw[r * 3 + c] = Rot.m[r * 3] * s.R[c] + Rot.m[r * 3 + 1] * s.R[3 + c] + Rot.m[r * 3 + 2] * s.R[6 + c];
    o.H = Mul(s.h, W.scale);
    o.Rr = s.r * W.scale;
    o.holdBone = s.bone;
    o.centre = o.C;
    o.radius = Len(o.H);
    return true;
}

// ---- per frame
struct Stats {
    std::uint32_t candidates = 0, sweeps = 0, contacts = 0, held = 0, slipping = 0, capped = 0;
    float deepest = 0, rise = 0, inLimb = 0;   // m: the shown press at most, the shown rise at most, a node left in a limb
    float change = 0;                          // m: the most a shown node moved this frame
    bool limbNear = false;
};
struct Lin {   // a candidate's contact for one frame (SolveRegion)
    V3 uStart{}, nu0{}, rt0{};
    float phi0 = 0, slack0 = 0, wn = 0;
    int k0 = -1;
    bool anchored = false;
    M3 C{}, W{}, Winv{};
};
struct RegionState {
    std::vector<V3> F, u, show, pend;   // pend: force changes not yet spread to the near nodes (Push, Flush)
    std::vector<V3> frameF;             // this frame's force changes not yet spread to the far nodes (FinishColumn)
    std::vector<V3> lastY0;             // the nodes and the limb shapes last frame (relative to the first node): a still
    std::vector<float> lastShapes;      //     frame after kStillFrames still ones is not solved again
    int still = 0;
    int settled = 0;                    // ... once as many solves in a row moved no shown node more than kSettled
    std::vector<std::int32_t> anchorBone;
    std::vector<V3> anchorLoc;
    std::vector<std::uint8_t> slip, live;
    std::vector<V3> y0;
    std::vector<M3> A, Ainv;
    std::vector<int> cand, listAt, listN, lists;   // the candidates and, per candidate, the shapes it looks at
    std::vector<Lin> lin;                          // ... and its contact, linearised at the frame's start
    std::vector<float> lastSlack;
    std::vector<int> lastShape;
    std::vector<V3> P1, nDef;   // the records' scratch
    Stats stats;
    bool dirty = false;         // its shown displacement changed this frame (records to write)
    bool any = false;           // its shown displacement is not all zero
};
struct BoneMat { M3 m; V3 t; };   // a bone's mesh-to-world this frame (BoneMats)
struct State {
    std::vector<RegionState> r;
    std::vector<WShape> shapes;
    std::vector<int> nearShapes;   // (not 'near': windows.h defines it)
    std::vector<BoneMat> mats;
    void Init(const Data& d) {
        r.assign(d.regions.size(), RegionState{});
        for (size_t i = 0; i < d.regions.size(); ++i) {
            const std::uint32_t n = d.regions[i].n;
            RegionState& s = r[i];
            s.F.assign(n, V3{0, 0, 0});
            s.u.assign(n, V3{0, 0, 0});
            s.show.assign(n, V3{0, 0, 0});
            s.pend.assign(n, V3{0, 0, 0});
            s.frameF.assign(n, V3{0, 0, 0});
            s.anchorBone.assign(n, -1);
            s.anchorLoc.assign(n, V3{0, 0, 0});
            s.slip.assign(n, 0);
            s.live.assign(n, 0);
            s.lastSlack.assign(n, 0.0f);
            s.lastShape.assign(n, -1);
        }
    }
};

constexpr float kGripPress = 0.1f;  // N: a node pressed this hard is dragged the grip's full distance
constexpr float kStill = 5e-6f;     // m
constexpr float kSettled = 2e-5f;   // m
constexpr int kStillFrames = 10;
inline float SoftCap(float x, float c1, float c2) {
    if (x <= c1) return x;
    return c1 + (c2 - c1) * std::tanh((x - c1) / (c2 - c1));
}

// every bone's mesh-to-world this frame: Mb = W (scale R) x bind, its 3x3 = scale R r, its translation W.pos + scale R t
inline void BoneMats(const Data& d, const Xf* world, const std::uint8_t* have, std::vector<BoneMat>& out) {
    out.resize(d.bones.size());
    for (size_t b = 0; b < d.bones.size(); ++b) {
        if (!have[b] || !d.hasBind[b]) continue;
        const Xf& W = world[b];
        const Bind& bd = d.binds[b];
        BoneMat& o = out[b];
        o.m = MulMM(Axes(W), M3{{bd.r[0], bd.r[1], bd.r[2], bd.r[3], bd.r[4], bd.r[5], bd.r[6], bd.r[7], bd.r[8]}});
        for (float& e : o.m.m) e *= W.scale;
        o.t = Apply(W, V3{bd.t[0], bd.t[1], bd.t[2]});
    }
}

inline bool SkinNodes(const Region& R, const std::vector<BoneMat>& mats, const std::uint8_t* have, const float* sliders,
                      RegionState& st) {
    st.y0.resize(R.n);
    st.A.resize(R.n);
    st.Ainv.resize(R.n);
    for (std::uint32_t j = 0; j < R.n; ++j) {
        V3 p = R.p[j];
        for (int k = 0; k < R.nd[j]; ++k) {
            const Delta& dl = R.deltas[R.d0[j] + k];
            const float s = sliders ? sliders[dl.slider] : 0.0f;
            if (s != 0.0f) p = Add(p, Mul(dl.d, s));
        }
        V3 y{0, 0, 0};
        M3 A{};
        for (int k = 0; k < R.nl[j]; ++k) {
            const Lbs& l = R.lbs[R.lbs0[j] + k];
            if (!have[l.bone]) return false;
            const BoneMat& Mb = mats[l.bone];
            const V3 q = Add(MulM(Mb.m, p), Mb.t);
            y = Add(y, Mul(q, l.w));
            for (int e = 0; e < 9; ++e) A.m[e] += l.w * Mb.m.m[e];
        }
        st.y0[j] = y;
        st.A[j] = A;
        if (!Inverse(A, st.Ainv[j])) return false;
    }
    return true;
}

constexpr float kListMargin = 0.05f;   // m: a candidate keeps the shapes within cand + this of where it starts the frame
inline float UnionSd(const State& S, const int* list, int n, V3 p, int& k) {
    float best = 1e30f;
    k = -1;
    for (int q = 0; q < n; ++q) {
        const float v = SdShape(S.shapes[static_cast<size_t>(list[q])], p);
        if (v < best) { best = v; k = list[q]; }
    }
    return best;
}
// The limbs as one smooth surface for a frame's contact: the shapes' distances blended where two are within kBlend of
// each other (the polynomial smooth minimum), so the surface between two fingers turns smoothly instead of switching
// from one finger's normal to the other's (a node in the gap then rocked between them frame after frame).  primary:
// the nearest shape (the bone a node it holds keeps to); grad (optional): the blended normal, unit
constexpr float kBlend = 0.004f;   // m
inline V3 Grad(const WShape& s, V3 p, float h);
inline float SmoothUnion(const State& S, const int* list, int n, V3 p, float fd, int& primary, V3* grad) {
    float v[64];
    int id[64];
    const int m = n < 64 ? n : 64;
    for (int q = 0; q < m; ++q) {
        id[q] = list[q];
        v[q] = SdShape(S.shapes[static_cast<size_t>(list[q])], p);
        for (int r = q; r > 0 && v[r] < v[r - 1]; --r) {   // ascending (ties keep the list's order)
            std::swap(v[r], v[r - 1]);
            std::swap(id[r], id[r - 1]);
        }
    }
    primary = m ? id[0] : -1;
    if (!m) return 1e30f;
    float val = v[0];
    V3 g = grad ? Grad(S.shapes[static_cast<size_t>(id[0])], p, fd) : V3{0, 0, 0};
    for (int q = 1; q < m; ++q) {
        const float dlt = v[q] - val;
        if (dlt >= kBlend) break;
        const float h = (kBlend - dlt) / kBlend;
        val -= h * h * kBlend * 0.25f;
        if (grad) g = Add(Mul(g, 1.0f - 0.5f * h), Mul(Grad(S.shapes[static_cast<size_t>(id[q])], p, fd), 0.5f * h));
    }
    if (grad) {
        const float L = Len(g);
        *grad = L > 1e-12f ? Mul(g, 1.0f / L) : V3{0, 0, 1};
    }
    return val;
}
inline V3 Grad(const WShape& s, V3 p, float h) {
    V3 g;
    g.x = (SdShape(s, {p.x + h, p.y, p.z}) - SdShape(s, {p.x - h, p.y, p.z})) / (2 * h);
    g.y = (SdShape(s, {p.x, p.y + h, p.z}) - SdShape(s, {p.x, p.y - h, p.z})) / (2 * h);
    g.z = (SdShape(s, {p.x, p.y, p.z + h}) - SdShape(s, {p.x, p.y, p.z - h})) / (2 * h);
    const float L = Len(g);
    return L > 1e-12f ? Mul(g, 1.0f / L) : V3{0, 0, 1};
}

inline M3 SelfC(const Region& R, std::uint32_t j) {   // C[c][d] = G[j, j, d, c]: node j's response to its own force
    const Column& col = R.cols[j];
    const std::int16_t* v = &R.vals[static_cast<size_t>(col.self) * 9];
    M3 C{};
    for (int dd = 0; dd < 3; ++dd)
        for (int c = 0; c < 3; ++c) C.m[c * 3 + dd] = v[dd * 3 + c] * col.scale;
    return C;
}
// u += G[j]^T dF over entries [k0, k1) of j's column
inline void Spread(const Region& R, std::uint32_t j, V3 dF, std::vector<V3>& u, std::uint32_t k0, std::uint32_t k1) {
    const Column& col = R.cols[j];
    const float s = col.scale;
    const V3 f = Mul(dF, s);
    const std::uint16_t* ix = &R.idx[col.at];
    const std::int16_t* v = &R.vals[(static_cast<size_t>(col.at) + k0) * 9];
    FP_COUNT(k1 - k0);
#if defined(_M_X64) || defined(__SSE2__)
    // SSE2: each entry's three columns (the response to x, y, z) as int16 -> float lanes, x f, summed into u[i]
    const __m128 fx = _mm_set1_ps(f.x), fy = _mm_set1_ps(f.y), fz = _mm_set1_ps(f.z);
    for (std::uint32_t k = k0; k < k1; ++k, v += 9) {
        const __m128i a = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(v));       // v0 v1 v2 v3
        const __m128i b = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(v + 3));   // v3 v4 v5 v6
        const __m128i c = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(v + 5));   // v5 v6 v7 v8
        const __m128 c0 = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(a, a), 16));                       // v0 v1 v2 (v3)
        const __m128 c1 = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(b, b), 16));                       // v3 v4 v5 (v6)
        const __m128 c2 = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_unpacklo_epi16(_mm_srli_si128(c, 2), _mm_srli_si128(c, 2)), 16));   // v6 v7 v8
        const __m128 add = _mm_add_ps(_mm_add_ps(_mm_mul_ps(c0, fx), _mm_mul_ps(c1, fy)), _mm_mul_ps(c2, fz));
        alignas(16) float t[4];
        _mm_store_ps(t, add);
        V3& ui = u[ix[k]];
        ui.x += t[0];
        ui.y += t[1];
        ui.z += t[2];
    }
#else
    for (std::uint32_t k = k0; k < k1; ++k, v += 9) {
        V3& ui = u[ix[k]];
        ui.x += v[0] * f.x + v[3] * f.y + v[6] * f.z;
        ui.y += v[1] * f.x + v[4] * f.y + v[7] * f.z;
        ui.z += v[2] * f.x + v[5] * f.y + v[8] * f.z;
    }
#endif
}
inline void Spread(const Region& R, std::uint32_t j, V3 dF, std::vector<V3>& u) {   // the whole column
    Spread(R, j, dF, u, 0, R.cols[j].count);
}

// A force change at node j: its own displacement at once; the nodes near it (its column's inner part) once the
// change's own effect passes kLazy, or at the frame's end (Flush); the far part of its column once, at the frame's end,
// for the frame's whole change (FinishColumn): every frame ends with the full response
inline float g_lazy = 0.0f;   // m (the tests may set it)
inline void Flush(const Region& R, RegionState& st, std::uint32_t j, const M3& C) {
    const V3 p = st.pend[j];
    if (p.x == 0.0f && p.y == 0.0f && p.z == 0.0f) return;
    Spread(R, j, p, st.u, 0, R.cols[j].inner);   // the near nodes, itself again: that part taken back
    st.u[j] = Sub(st.u[j], MulM(C, p));
    st.frameF[j] = Add(st.frameF[j], p);
    st.pend[j] = {0, 0, 0};
}
inline void FinishColumn(const Region& R, RegionState& st, std::uint32_t j) {
    Flush(R, st, j, SelfC(R, j));
    const V3 g = st.frameF[j];
    if (g.x == 0.0f && g.y == 0.0f && g.z == 0.0f) return;
    Spread(R, j, g, st.u, R.cols[j].inner, R.cols[j].count);
    st.frameF[j] = {0, 0, 0};
}
inline void Push(const Region& R, RegionState& st, std::uint32_t j, V3 dF, const M3& C, const M3& A) {
    st.F[j] = Add(st.F[j], dF);
    st.u[j] = Add(st.u[j], MulM(C, dF));
    st.pend[j] = Add(st.pend[j], dF);
    if (Len(MulM(A, MulM(C, st.pend[j]))) > g_lazy) Flush(R, st, j, C);
}

// One breast's solve (Solver.solve_region)
inline void SolveRegion(const Data& d, const Region& R, RegionState& st, State& S, const Xf* world, float grip) {
    FP_START
    const Params& P = d.prm;
    Stats& stats = st.stats;
    stats = Stats{};
    stats.limbNear = !S.nearShapes.empty();
    const std::uint32_t n = R.n;
    std::vector<V3>& F = st.F;
    std::vector<V3>& u = st.u;
    st.cand.clear();
    st.listAt.clear();
    st.listN.clear();
    st.lists.clear();
    st.lin.clear();
    std::fill(st.slip.begin(), st.slip.end(), static_cast<std::uint8_t>(0));
    const float eps = P.reg;   // the contact's own give: a node rests eps x its force inside the limb
    for (std::uint32_t j = 0; j < n; ++j) {
        if (R.fixed[j]) continue;
        const bool hasF = F[j].x != 0.0f || F[j].y != 0.0f || F[j].z != 0.0f;
        const V3 y = Add(st.y0[j], MulM(st.A[j], u[j]));
        const int at = static_cast<int>(st.lists.size());
        bool within = false;
        for (int i : S.nearShapes) {   // a shape lies inside its bounding sphere: farther than cand there, farther in fact
            const WShape& w = S.shapes[static_cast<size_t>(i)];
            const float ds = Len(Sub(y, w.centre)) - w.radius;
            if (ds < P.cand + kListMargin) {
                st.lists.push_back(i);
                if (!within && ds < P.cand && SdShape(w, y) < P.cand) within = true;
            }
        }
        if (!(hasF || within)) {
            st.lists.resize(static_cast<size_t>(at));
            continue;
        }
        st.cand.push_back(static_cast<int>(j));
        st.listAt.push_back(at);
        st.listN.push_back(static_cast<int>(st.lists.size()) - at);
        // the contact linearised where the node starts the frame: the nearest shape's distance and normal, the cap's
        // slack (the press saturates softly from cap1 to cap2 of the tissue's depth), the anchor's target; within the
        // frame the constraint is phi0 + nu0 . (the node's move) + slack0 >= 0 (a fixed convex problem the sweeps settle)
        Lin L;
        L.uStart = u[j];
        int k = -1;
        L.phi0 = SmoothUnion(S, st.lists.data() + at, static_cast<int>(st.lists.size()) - at, y, P.fd, k, &L.nu0);
        L.k0 = k;
        if (k >= 0) {
            const float d0 = -Dot(R.nrm[j], u[j]);
            const float need = d0 - L.phi0;
            L.slack0 = need - SoftCap(need, P.cap1 * R.depth[j], P.cap2 * R.depth[j]);
            L.C = SelfC(R, j);
            L.W = MulMM(MulMM(st.A[j], L.C), Transpose(st.A[j]));
            L.wn = Dot(L.nu0, MulM(L.W, L.nu0)) + eps;
            L.anchored = st.anchorBone[j] >= 0 && grip > 0.0f;
            if (L.anchored) {
                // the hold's own give: eps along the normal, regT sideways (the held point pulls like a spring of
                // stiffness 1 / regT: stiff holds on nodes 7 mm apart made the solve blow up)
                M3 Weff = L.W;
                const V3 nn = L.nu0;
                const float et = P.regT;
                for (int r2 = 0; r2 < 3; ++r2)
                    for (int c2 = 0; c2 < 3; ++c2) {
                        const float nr = r2 == 0 ? nn.x : (r2 == 1 ? nn.y : nn.z), nc = c2 == 0 ? nn.x : (c2 == 1 ? nn.y : nn.z);
                        Weff.m[r2 * 3 + c2] += (eps - et) * nr * nc + (r2 == c2 ? et : 0.0f);
                    }
                if (!Inverse(Weff, L.Winv)) L.anchored = false;
                const Xf& Wb = world[st.anchorBone[j]];
                const V3 a = Apply(Wb, st.anchorLoc[j]);
                const V3 rr = Sub(a, y);
                L.rt0 = Sub(rr, Mul(L.nu0, Dot(rr, L.nu0)));   // the held point's sideways offset at the frame's start
                // the grip drags the skin at most `grip` m at a firm press (kGripPress or more on the node, its last
                // press), less at a lighter one (friction goes with the press): farther, the held point slides along
                const float press = Dot(MulMT(st.Ainv[j], F[j]), L.nu0);
                const float most = grip * std::clamp(press / kGripPress, 0.0f, 1.0f);
                const float off = Len(L.rt0);
                if (off > most) {
                    L.rt0 = off > 0.0f ? Mul(L.rt0, most / off) : L.rt0;
                    st.anchorLoc[j] = ToLocal(Wb, Add(Sub(a, rr), Add(Mul(L.nu0, Dot(rr, L.nu0)), L.rt0)));
                    st.slip[j] = 1;
                }
            }
        }
        st.lin.push_back(L);
    }
    stats.candidates = static_cast<std::uint32_t>(st.cand.size());
    FP(1)
    if (st.cand.empty()) return;
    if (S.nearShapes.empty()) {   // no limb near: every force lets go at once
        for (int j : st.cand) {
            Spread(R, static_cast<std::uint32_t>(j), Mul(F[static_cast<size_t>(j)], -1.0f), u);
            F[static_cast<size_t>(j)] = {0, 0, 0};
            st.anchorBone[static_cast<size_t>(j)] = -1;
        }
        return;
    }
    for (int j : st.cand) st.lastSlack[static_cast<size_t>(j)] = 0.0f;
    for (std::uint32_t sweep = 0; sweep < P.sweeps; ++sweep) {
        float big = 0.0f;
        for (size_t ci = 0; ci < st.cand.size(); ++ci) {
            const std::uint32_t j = static_cast<std::uint32_t>(st.cand[ci]);
            const Lin& L = st.lin[ci];
            const M3& A = st.A[j];
            const bool hasF = F[j].x != 0.0f || F[j].y != 0.0f || F[j].z != 0.0f;
            if (L.k0 < 0) {   // a held node no shape is near any more: it lets go
                if (!hasF) continue;
                const M3 C0 = SelfC(R, j);
                big = std::fmax(big, Len(MulM(A, MulM(C0, F[j]))));
                Push(R, st, j, Mul(F[j], -1.0f), C0, A);
                F[j] = {0, 0, 0};
                continue;
            }
            const V3 moved = MulM(A, Sub(u[j], L.uStart));   // the node's move this frame (world)
            const float ceff = L.phi0 + Dot(L.nu0, moved) + L.slack0;
            if (ceff >= 0.0f && !hasF) continue;
            const V3 nu = L.nu0;
            const V3 f = MulMT(st.Ainv[j], F[j]);   // world force
            V3 df;
            if (L.anchored) {
                const V3 rt = Sub(L.rt0, Sub(moved, Mul(nu, Dot(moved, nu))));   // the sideways way to the anchor left
                const float fn = Dot(f, nu);
                const V3 ftan = Sub(f, Mul(nu, fn));
                const V3 dy = Sub(Add(Mul(nu, -ceff), rt), Add(Mul(nu, eps * fn), Mul(ftan, P.regT)));
                df = MulM(L.Winv, dy);
            } else {
                df = Mul(nu, -(ceff + eps * Dot(f, nu)) / L.wn);
            }
            const float slack = L.slack0;
            const int k = L.k0;
            const M3& C = L.C;
            V3 f1 = Add(f, df);
            const float lam1 = Dot(f1, nu);
            if (lam1 <= 0.0f) f1 = {0, 0, 0};           // the skin leaves the limb
            else if (!L.anchored) f1 = Mul(nu, lam1);   // not held: the press alone
            const V3 dF = MulMT(A, Sub(f1, f));
            if (dF.x != 0.0f || dF.y != 0.0f || dF.z != 0.0f) {
                Push(R, st, j, dF, C, A);
                big = std::fmax(big, Len(MulM(A, MulM(C, dF))));
            }
            st.lastSlack[j] = slack;
            st.lastShape[j] = k;
        }
        stats.sweeps = sweep + 1;
        if (big < P.tol) break;
    }
    FP(2)
    for (int jj : st.cand) FinishColumn(R, st, static_cast<std::uint32_t>(jj));
    FP(3)
    // the frame's end: hold new touches, let go of released ones, move slipping ones
    for (size_t ci = 0; ci < st.cand.size(); ++ci) {
        const std::uint32_t j = static_cast<std::uint32_t>(st.cand[ci]);
        if (F[j].x == 0.0f && F[j].y == 0.0f && F[j].z == 0.0f) {   // no force: no contact (as lam = 0 below)
            st.anchorBone[j] = -1;
            continue;
        }
        const V3 y = Add(st.y0[j], MulM(st.A[j], u[j]));
        int k;
        V3 nu;
        const float phi = SmoothUnion(S, st.lists.data() + st.listAt[ci], st.listN[ci], y, P.fd, k, &nu);
        if (k < 0) {
            st.anchorBone[j] = -1;
            continue;
        }
        const WShape& ws = S.shapes[static_cast<size_t>(k)];
        const float lam = Dot(MulMT(st.Ainv[j], F[j]), nu);
        if (lam <= 1e-9f) {
            st.anchorBone[j] = -1;
            continue;
        }
        ++stats.contacts;
        if (st.lastSlack[j] > 0.0f) ++stats.capped;
        if (st.anchorBone[j] < 0) {   // a new touch: held where it touches, on the nearest shape's bone
            const V3 pt = Sub(y, Mul(nu, phi));
            st.anchorBone[j] = ws.holdBone;
            st.anchorLoc[j] = ToLocal(world[ws.holdBone], pt);
        } else {
            ++stats.held;
            stats.slipping += st.slip[j];   // (held, and slid along this frame: dragged its full `grip`)
        }
    }
    FP(4)
}

// The whole frame: returns true if any breast's shown displacement changed (its records want writing)
inline bool Frame(const Data& d, const Xf* world, const std::uint8_t* have, const float* sliders, float dt, float grip, State& S) {
    if (S.r.size() != d.regions.size()) S.Init(d);
    S.shapes.resize(d.shapes.size());
    std::vector<std::uint8_t> shapeOk(d.shapes.size(), 0);
    for (size_t i = 0; i < d.shapes.size(); ++i) shapeOk[i] = WorldShape(d.shapes[i], world, have, S.shapes[i]);
    bool changed = false;
    const float a = (d.prm.release > 0.0f && dt > 0.0f) ? 1.0f - std::exp(-dt / d.prm.release) : 1.0f;
    for (size_t ri = 0; ri < d.regions.size(); ++ri) {
        const Region& R = d.regions[ri];
        RegionState& st = S.r[ri];
        st.dirty = false;
        FP_START
        if (ri == 0) BoneMats(d, world, have, S.mats);
        if (!SkinNodes(R, S.mats, have, sliders, st)) {
            st.stats = Stats{};
            continue;
        }
        FP(0)
        V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const V3& y : st.y0) {
            lo = {std::fmin(lo.x, y.x), std::fmin(lo.y, y.y), std::fmin(lo.z, y.z)};
            hi = {std::fmax(hi.x, y.x), std::fmax(hi.y, y.y), std::fmax(hi.z, y.z)};
        }
        const float rch = d.prm.reach;
        lo = {lo.x - rch, lo.y - rch, lo.z - rch};
        hi = {hi.x + rch, hi.y + rch, hi.z + rch};
        S.nearShapes.clear();
        for (size_t i = 0; i < S.shapes.size(); ++i) {
            if (!shapeOk[i]) continue;
            const WShape& w = S.shapes[i];
            const V3 c = w.centre;
            const float rr = w.radius;
            if (c.x + rr > lo.x && c.y + rr > lo.y && c.z + rr > lo.z && c.x - rr < hi.x && c.y - rr < hi.y && c.z - rr < hi.z)
                S.nearShapes.push_back(static_cast<int>(i));
        }
        bool anyF = false;
        for (const V3& f : st.F) anyF = anyF || f.x != 0.0f || f.y != 0.0f || f.z != 0.0f;
        if (S.nearShapes.empty() && !anyF && !st.any) {
            st.stats = Stats{};
            st.still = 0;
            continue;   // nothing near, nothing held, nothing shown
        }
        // a still frame (the nodes and the shapes near them where they were, to kStill, relative to the first node):
        // after kStillFrames of them the solve is as settled as it gets; it is not run again until something moves
        {
            const V3 ref = st.y0[0];
            bool same = st.lastY0.size() == R.n;
            if (st.lastY0.size() != R.n) st.lastY0.assign(R.n, V3{0, 0, 0});
            for (std::uint32_t j = 0; j < R.n; ++j) {
                const V3 rel = Sub(st.y0[j], ref);
                const V3 dd = Sub(rel, st.lastY0[j]);
                if (same && (std::fabs(dd.x) > kStill || std::fabs(dd.y) > kStill || std::fabs(dd.z) > kStill)) same = false;
                st.lastY0[j] = rel;
            }
            std::vector<float> sh;
            sh.reserve(S.nearShapes.size() * 7);
            for (int i : S.nearShapes) {
                const WShape& w = S.shapes[static_cast<size_t>(i)];
                const V3 pa = Sub(w.kind == 0 ? w.A : w.C, ref), pb = Sub(w.kind == 0 ? w.B : Add(w.C, V3{w.Rw[0], w.Rw[3], w.Rw[6]}), ref);
                sh.insert(sh.end(), {static_cast<float>(i), pa.x, pa.y, pa.z, pb.x, pb.y, pb.z});
            }
            if (same && sh.size() == st.lastShapes.size()) {
                for (size_t q = 0; q < sh.size() && same; ++q) same = std::fabs(sh[q] - st.lastShapes[q]) <= kStill;
            } else {
                same = false;
            }
            st.lastShapes.swap(sh);
            st.still = same ? st.still + 1 : 0;
            // still, and the last solves moved nothing much: as settled as it gets (st.dirty stays false)
            if (st.still > kStillFrames && st.settled > kStillFrames) continue;
        }
        SolveRegion(d, R, st, S, world, grip);
        // the shown dent: a press at once, a release eased
        bool any = false;
        float change = 0.0f;
        for (std::uint32_t j = 0; j < R.n; ++j) {
            const V3 uj = st.u[j];
            V3& sh = st.show[j];
            const V3 before = sh;
            if (Dot(uj, uj) >= Dot(sh, sh)) sh = uj;
            else sh = Add(sh, Mul(Sub(uj, sh), a));
            if (Dot(sh, sh) < 1e-14f) sh = {0, 0, 0};   // below 0.1 um: gone
            if (sh.x != before.x || sh.y != before.y || sh.z != before.z) {
                st.dirty = true;
                change = std::fmax(change, Len(Sub(sh, before)));
            }
            any = any || sh.x != 0.0f || sh.y != 0.0f || sh.z != 0.0f;
        }
        st.any = any;
        st.settled = change < kSettled ? st.settled + 1 : 0;
        st.stats.change = change;
        changed = changed || st.dirty;
        // the shown press and rise (along the rest normals), for the log
        for (std::uint32_t j = 0; j < R.n; ++j) {
            const float in = -Dot(R.nrm[j], st.show[j]);
            st.stats.deepest = std::fmax(st.stats.deepest, in);
            st.stats.rise = std::fmax(st.stats.rise, -in);
        }
        if (!S.nearShapes.empty())
            for (size_t ci = 0; ci < st.cand.size(); ++ci) {
                const size_t j = static_cast<size_t>(st.cand[ci]);
                if (st.F[j].x == 0.0f && st.F[j].y == 0.0f && st.F[j].z == 0.0f) continue;   // (the nodes in contact)
                int k;
                const float phi = UnionSd(S, st.lists.data() + st.listAt[ci], st.listN[ci], Add(st.y0[j], MulM(st.A[j], st.u[j])), k);
                if (k >= 0) st.stats.inLimb = std::fmax(st.stats.inLimb, -phi);
            }
        FP(5)
    }
    return changed;
}

// ---- the travelling dent (the file's still one at theta 0)
inline V3 DentCentre(const Region& R, float theta, float spread) {
    double cx = 0, cy = 0, cz = 0, ws = 0;
    for (size_t k = 0; k < R.ring.size(); ++k) {
        double dd = std::fmod(static_cast<double>(R.ringAng[k]) - theta + 3.141592653589793, 6.283185307179586);
        if (dd < 0) dd += 6.283185307179586;
        dd -= 3.141592653589793;
        const double w = std::exp(-0.5 * (dd / spread) * (dd / spread));
        const V3 p = R.p[R.ring[k]];
        cx += p.x * w;
        cy += p.y * w;
        cz += p.z * w;
        ws += w;
    }
    if (!(ws > 0)) return R.p[0];
    return {static_cast<float>(cx / ws), static_cast<float>(cy / ws), static_cast<float>(cz / ws)};
}
inline void DentField(const Data& d, float theta, int region, State& S) {
    if (S.r.size() != d.regions.size()) S.Init(d);
    for (size_t ri = 0; ri < d.regions.size(); ++ri) {
        const Region& R = d.regions[ri];
        RegionState& st = S.r[ri];
        const bool on = static_cast<int>(ri) == region && !R.ring.empty();
        const V3 c = on ? DentCentre(R, theta, d.prm.dentSpread) : V3{0, 0, 0};
        bool any = false;
        for (std::uint32_t j = 0; j < R.n; ++j) {
            V3 v{0, 0, 0};
            if (on && !R.fixed[j]) {
                const float dd = Len(Sub(R.p[j], c)) / d.prm.dentSigma;
                const float g = d.prm.dentDepth * std::exp(-0.5f * dd * dd);
                v = Mul(R.nrm[j], -g);
            }
            st.show[j] = v;
            any = any || v.x != 0.0f || v.y != 0.0f || v.z != 0.0f;
        }
        st.dirty = true;
        st.any = any;
    }
}
inline void Clear(const Data& d, State& S) {   // every force and every shown dent gone (a mode change)
    S.Init(d);
    for (RegionState& st : S.r) st.dirty = true;
}

// ---- the records
inline std::uint16_t Half(float f) {   // float -> IEEE half, round to nearest even (numpy's astype('<f2'))
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const std::uint32_t absx = x & 0x7FFFFFFFu;
    if (absx >= 0x7F800000u) return static_cast<std::uint16_t>(sign | (absx > 0x7F800000u ? 0x7E00u : 0x7C00u));
    if (absx >= 0x477FF000u) return static_cast<std::uint16_t>(sign | 0x7C00u);   // overflow (rounds to inf)
    if (absx < 0x33000001u) return static_cast<std::uint16_t>(sign);               // below half the smallest subnormal
    const std::uint32_t exp = absx >> 23;
    std::uint32_t mant = absx & 0x7FFFFFu;
    if (exp < 113) {   // subnormal half
        mant |= 0x800000u;
        const std::uint32_t shift = 126 - exp;
        std::uint32_t h = mant >> shift;
        const std::uint32_t rem = mant & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (h & 1u))) ++h;
        return static_cast<std::uint16_t>(sign | h);
    }
    std::uint32_t h = ((exp - 112) << 10) | (mant >> 13);
    const std::uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
    return static_cast<std::uint16_t>(sign | h);
}
inline std::uint32_t Ten(V3 v) {   // 10:10:10 with w = 1: x bits 0-9, y 10-19, z 20-29 (mdat_io.encode_10_10_10)
    auto q = [](float c) {
        const double t = std::nearbyint(static_cast<double>(c) * 511.5 + 511.0);
        return static_cast<std::uint32_t>(t < 0 ? 0 : (t > 1023 ? 1023 : t));
    };
    return q(v.x) | (q(v.y) << 10) | (q(v.z) << 20) | (1u << 30);
}
inline V3 TurnVec(V3 a, V3 b, V3 x) {   // x turned by the rotation taking unit a to unit b (freemorph._turn)
    const V3 ax = Cross(a, b);
    const float sn = Len(ax);
    float cs = Dot(a, b);
    cs = cs < -1 ? -1 : (cs > 1 ? 1 : cs);
    if (!(sn > 1e-9f)) return x;
    const V3 k = Mul(ax, 1.0f / sn);
    return Add(Add(Mul(x, cs), Mul(Cross(k, x), sn)), Mul(k, Dot(k, x) * (1 - cs)));
}
// Fill the records of the regions marked dirty (16 bytes each, in record order) from their shown displacements
inline void Records(const Data& d, State& S, std::uint8_t* out) {
    for (size_t ri = 0; ri < d.regions.size(); ++ri) {
        const Region& R = d.regions[ri];
        RegionState& st = S.r[ri];
        if (!st.dirty) continue;
        st.P1.resize(R.n);
        for (std::uint32_t j = 0; j < R.n; ++j) st.P1[j] = Add(R.p[j], st.show[j]);
        FaceNormals(R, st.P1.data(), st.nDef);
        for (std::uint32_t v = 0; v < R.nv; ++v) {
            const std::uint16_t j = R.vnode[v];
            const V3 sh = st.show[j];
            const V3 dn = Sub(TurnVec(R.nRest[j], st.nDef[j], R.nfile[v]), R.nfile[v]);
            const V3 dt = Sub(TurnVec(R.nRest[j], st.nDef[j], R.tfile[v]), R.tfile[v]);
            std::uint8_t* rec = out + static_cast<size_t>(R.vrec[v]) * 16;
            const std::uint16_t hx = Half(sh.x), hy = Half(sh.y), hz = Half(sh.z), col = R.color[v];
            const std::uint32_t tn = Ten(dn), tt = Ten(dt);
            std::memcpy(rec + 0, &hx, 2);
            std::memcpy(rec + 2, &hy, 2);
            std::memcpy(rec + 4, &hz, 2);
            std::memcpy(rec + 6, &col, 2);
            std::memcpy(rec + 8, &tn, 4);
            std::memcpy(rec + 12, &tt, 4);
        }
    }
}

}  // namespace fm
