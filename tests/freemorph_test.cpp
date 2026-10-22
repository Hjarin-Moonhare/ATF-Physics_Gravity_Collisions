// FreeMorph's offline test: the plugin's solve (src/freemorph_core.h) against the design model's
// (A-Body's freemorph.py, Solver) on its test vectors: the same FreeMorph.bin, the grab's frames (both
// hands closing on the breasts, then held), frame by frame: each breast's shown displacement and its contacts; the
// records after the last frame; the travelling dent's records.  Also times the frames.
// freemorph_test <FreeMorph.bin> <vectors.bin>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define FM_PROF
#include "freemorph_core.h"

static std::vector<std::uint8_t> ReadAll(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static float HalfToFloat(std::uint16_t h) {
    const std::uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    std::uint32_t x;
    if (e == 0) {
        if (m == 0) x = s;
        else {   // subnormal
            float v = std::ldexp(static_cast<float>(m), -24);
            return s ? -v : v;
        }
    } else if (e == 31) x = s | 0x7F800000u | (m << 13);
    else x = s | ((e + 112) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}

struct RecDiff { double pos = 0; int ten = 0; int tenFields = 0; int colour = 0; int bytesDiffer = 0; };
static RecDiff CompareRecords(const std::uint8_t* a, const std::uint8_t* b, std::uint32_t n) {
    RecDiff r;
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint8_t* p = a + 16 * k;
        const std::uint8_t* q = b + 16 * k;
        if (std::memcmp(p, q, 16)) ++r.bytesDiffer;
        for (int c = 0; c < 3; ++c) {
            std::uint16_t hp, hq;
            std::memcpy(&hp, p + 2 * c, 2);
            std::memcpy(&hq, q + 2 * c, 2);
            r.pos = std::fmax(r.pos, std::fabs(static_cast<double>(HalfToFloat(hp)) - HalfToFloat(hq)));
        }
        if (std::memcmp(p + 6, q + 6, 2)) ++r.colour;
        for (int w = 0; w < 2; ++w) {
            std::uint32_t tp, tq;
            std::memcpy(&tp, p + 8 + 4 * w, 4);
            std::memcpy(&tq, q + 8 + 4 * w, 4);
            for (int c = 0; c < 3; ++c) {
                const int vp = static_cast<int>((tp >> (10 * c)) & 1023u), vq = static_cast<int>((tq >> (10 * c)) & 1023u);
                if (vp != vq) ++r.tenFields;
                r.ten = std::max(r.ten, std::abs(vp - vq));
            }
            if ((tp >> 30) != 1u || (tq >> 30) != 1u) r.ten = 9999;
        }
    }
    return r;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: freemorph_test <FreeMorph.bin> <vectors.bin>\n"); return 2; }
    const auto bin = ReadAll(argv[1]);
    const auto vec = ReadAll(argv[2]);
    fm::Data d;
    std::string err;
    const auto l0 = std::chrono::high_resolution_clock::now();
    if (!fm::Load(bin.data(), bin.size(), d, err)) { std::printf("FAIL load: %s\n", err.c_str()); return 1; }
    const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - l0).count();
    size_t entries = 0;
    for (const auto& R : d.regions) entries += R.idx.size();
    std::printf("loaded %zu bytes in %.0f ms: morph %s, %u body vertices, %u records, %zu bones, %zu sliders, %zu shapes, "
                "%zu regions, %zu table entries\n", bin.size(), loadMs, d.morph.c_str(), d.bodyVerts, d.records, d.bones.size(),
                d.sliders.size(), d.shapes.size(), d.regions.size(), entries);
    fm::Reader r{vec.data(), vec.size()};
    if (vec.size() < 8 || std::memcmp(vec.data(), "TFFV0001", 8)) { std::printf("FAIL: not a vectors file\n"); return 1; }
    r.at = 8;
    const std::uint32_t nf = r.get<std::uint32_t>();
    float grip = r.get<float>();
    // tune <sweeps> <reg mm/N> <grip>: the vectors' frames replayed with these, then held 10 s; no comparison
    if (argc > 6 && !std::strcmp(argv[3], "tune")) {
        d.prm.sweeps = static_cast<std::uint32_t>(std::atoi(argv[4]));
        d.prm.reg = static_cast<float>(std::atof(argv[5])) * 1e-3f;
        grip = static_cast<float>(std::atof(argv[6])) * 1e-3f;
        if (argc > 7) d.prm.regT = static_cast<float>(std::atof(argv[7])) * 1e-3f;
        std::printf("tune: the hold's sideways give %.1f mm/N\n", d.prm.regT * 1e3);
        std::printf("tune: %u sweeps a frame, the contact's give %.2f mm/N, the grip drags up to %.1f mm\n", d.prm.sweeps,
                    d.prm.reg * 1e3, grip * 1e3);
        fm::State S;
        S.Init(d);
        std::vector<fm::Xf> world(d.bones.size());
        std::vector<std::uint8_t> have(d.bones.size(), 1);
        std::vector<float> sliders;
        double sum = 0, worst = 0;
        int frames = 0;
        std::vector<std::vector<fm::Xf>> kept;   // every frame's bones (the opening and the second grab replay them)
        auto line = [&](const char* what, int f, double ms) {
            std::printf("%s %3d (%.2f ms):", what, f, ms);
            for (size_t ri = 0; ri < d.regions.size(); ++ri) {
                const fm::Stats& st = S.r[ri].stats;
                std::printf(" %s %u sw, %u contacts (%u held, %u sliding), in %.1f out %.1f, %.2f in a limb, moved %.3f;",
                            d.regions[ri].name.c_str(), st.sweeps, st.contacts, st.held, st.slipping, st.deepest * 1000,
                            st.rise * 1000, st.inLimb * 1000, st.change * 1000);
            }
            std::printf("\n");
        };
        for (std::uint32_t f = 0; f < nf; ++f) {
            const float dt = r.get<float>();
            const std::uint32_t nb = r.get<std::uint32_t>();
            for (std::uint32_t b = 0; b < nb; ++b) {
                fm::Xf& x = world[b];
                for (int k = 0; k < 3; ++k) x.ax[k] = r.v3();
                x.pos = r.v3();
                x.scale = r.get<float>();
            }
            const std::uint32_t ns = r.get<std::uint32_t>();
            sliders.resize(ns);
            for (std::uint32_t s = 0; s < ns; ++s) sliders[s] = r.get<float>();
            for (size_t ri = 0; ri < d.regions.size(); ++ri) {
                const std::uint32_t n = r.get<std::uint32_t>();
                r.at += static_cast<size_t>(n) * 12 + 8;
            }
            const auto t0 = std::chrono::high_resolution_clock::now();
            fm::Frame(d, world.data(), have.data(), sliders.data(), dt, grip, S);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
            sum += ms;
            worst = std::fmax(worst, ms);
            ++frames;
            kept.push_back(world);
            if (f % 6 == 0 || f + 1 == nf) line("frame", static_cast<int>(f), ms);
        }
        auto run = [&](const std::vector<fm::Xf>& w, const char* what, int f, bool show) {
            const auto t0 = std::chrono::high_resolution_clock::now();
            fm::Frame(d, w.data(), have.data(), sliders.data(), 1.0f / 60.0f, grip, S);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
            sum += ms;
            worst = std::fmax(worst, ms);
            ++frames;
            if (show) line(what, f, ms);
        };
        for (int f = 0; f < 600; ++f) run(world, "held", f + 1, f == 0 || f == 59 || f == 179 || f == 599);
        // the hand opens again (the closing frames backwards, each shown 4 frames), stays open 1 s, closes again, held 2 s
        const int nk = static_cast<int>(kept.size());
        for (int f = nk - 1, q = 0; f >= 0; --f)
            for (int rep = 0; rep < 4; ++rep, ++q) run(kept[static_cast<size_t>(f)], "opening", q, rep == 3 && (f % 6 == 0));
        for (int f = 0; f < 60; ++f) run(kept[0], "open", f + 1, f == 59);
        for (int f = 0, q = 0; f < nk; ++f)
            for (int rep = 0; rep < 4; ++rep, ++q) run(kept[static_cast<size_t>(f)], "closing again", q, rep == 3 && (f % 6 == 0 || f + 1 == nk));
        for (int f = 0; f < 120; ++f) run(kept.back(), "held again", f + 1, f == 0 || f == 119);
        std::printf("tune: %.2f ms a frame on average, %.2f ms at most\n", sum / frames, worst);
        std::printf("profile (ms in all): skin %.1f, candidates %.1f, sweeps %.1f, far columns %.1f, frame end %.1f, the rest "
                    "(incl. the solve) %.1f; table entries spread %.2f M\n", fm::g_prof[0] / 1e3, fm::g_prof[1] / 1e3,
                    fm::g_prof[2] / 1e3, fm::g_prof[3] / 1e3, fm::g_prof[4] / 1e3, fm::g_prof[5] / 1e3, fm::g_spread / 1e6);
        return 0;
    }
    fm::State S;
    S.Init(d);
    std::vector<fm::Xf> world(d.bones.size());
    std::vector<std::uint8_t> have(d.bones.size(), 1);
    std::vector<float> sliders;
    double worstShow = 0, totalMs = 0, worstMs = 0;
    double earlyWorst = 0, pressDiff = 0;   // while few nodes touch the two agree exactly; later the press and the rise
    int contactDiff = 0, heldDiff = 0;
    std::vector<double> worstPerRegion(d.regions.size(), 0.0);
    for (std::uint32_t f = 0; f < nf; ++f) {
        const float dt = r.get<float>();
        const std::uint32_t nb = r.get<std::uint32_t>();
        if (nb != d.bones.size()) { std::printf("FAIL: frame %u has %u bones\n", f, nb); return 1; }
        for (std::uint32_t b = 0; b < nb; ++b) {
            fm::Xf& x = world[b];
            for (int k = 0; k < 3; ++k) x.ax[k] = r.v3();
            x.pos = r.v3();
            x.scale = r.get<float>();
        }
        const std::uint32_t ns = r.get<std::uint32_t>();
        sliders.resize(ns);
        for (std::uint32_t s = 0; s < ns; ++s) sliders[s] = r.get<float>();
        const auto t0 = std::chrono::high_resolution_clock::now();
        fm::Frame(d, world.data(), have.data(), sliders.data(), dt, grip, S);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
        totalMs += ms;
        worstMs = std::fmax(worstMs, ms);
        char line[512];
        size_t ln = 0;
        line[0] = 0;
        for (size_t ri = 0; ri < d.regions.size(); ++ri) {
            const std::uint32_t n = r.get<std::uint32_t>();
            if (n != d.regions[ri].n) { std::printf("FAIL: frame %u region %zu has %u nodes\n", f, ri, n); return 1; }
            double worst = 0, inE = 0, outE = 0, inG = 0, outG = 0;
            for (std::uint32_t j = 0; j < n; ++j) {
                const fm::V3 e = r.v3();
                const fm::V3 g = S.r[ri].show[j];
                worst = std::fmax(worst, std::fabs(static_cast<double>(e.x) - g.x));
                worst = std::fmax(worst, std::fabs(static_cast<double>(e.y) - g.y));
                worst = std::fmax(worst, std::fabs(static_cast<double>(e.z) - g.z));
                const fm::V3 nr = d.regions[ri].nrm[j];
                const double ie = -(nr.x * e.x + nr.y * e.y + nr.z * e.z), ig = -(nr.x * g.x + nr.y * g.y + nr.z * g.z);
                inE = std::fmax(inE, ie), outE = std::fmax(outE, -ie), inG = std::fmax(inG, ig), outG = std::fmax(outG, -ig);
            }
            pressDiff = std::fmax(pressDiff, std::fmax(std::fabs(inE - inG), std::fabs(outE - outG)));
            const std::uint32_t contacts = r.get<std::uint32_t>(), held = r.get<std::uint32_t>();
            if (contacts < 20) earlyWorst = std::fmax(earlyWorst, worst);
            const fm::Stats& st = S.r[ri].stats;
            contactDiff = std::max(contactDiff, std::abs(static_cast<int>(contacts) - static_cast<int>(st.contacts)));
            heldDiff = std::max(heldDiff, std::abs(static_cast<int>(held) - static_cast<int>(st.held)));
            worstPerRegion[ri] = std::fmax(worstPerRegion[ri], worst);
            worstShow = std::fmax(worstShow, worst);
            ln += std::snprintf(line + ln, sizeof(line) - ln, " %s: %u cand, %u sweeps, %u contacts (py %u), %u held (py %u), in %.1f "
                                "out %.1f mm, off %.3f mm;", d.regions[ri].name.c_str(), st.candidates, st.sweeps, st.contacts,
                                contacts, st.held, held, st.deepest * 1000, st.rise * 1000, worst * 1000);
        }
        if (f % 6 == 0 || f + 1 == nf) std::printf("frame %2u (%.2f ms):%s\n", f, ms, line);
    }
    const std::uint32_t nrb = r.get<std::uint32_t>();
    if (nrb != d.records * 16u) { std::printf("FAIL: %u record bytes, want %u\n", nrb, d.records * 16u); return 1; }
    std::vector<std::uint8_t> want(nrb), got(nrb, 0);
    std::memcpy(want.data(), vec.data() + r.at, nrb);
    r.at += nrb;
    for (auto& st : S.r) st.dirty = true;
    fm::Records(d, S, got.data());
    const RecDiff rd = CompareRecords(want.data(), got.data(), d.records);
    const float theta = r.get<float>();
    const std::uint32_t ndb = r.get<std::uint32_t>();
    if (ndb != nrb) { std::printf("FAIL: dent record bytes %u\n", ndb); return 1; }
    std::vector<std::uint8_t> dwant(ndb), dgot(ndb, 0);
    std::memcpy(dwant.data(), vec.data() + r.at, ndb);
    r.at += ndb;
    fm::State S2;
    fm::DentField(d, theta, 0, S2);
    fm::Records(d, S2, dgot.data());
    const RecDiff dd = CompareRecords(dwant.data(), dgot.data(), d.records);
    if (!r.ok || r.at != vec.size()) { std::printf("FAIL: the vectors file read %zu of %zu bytes\n", r.at, vec.size()); return 1; }
    std::printf("frames: %u, %.2f ms each on average, %.2f ms at most (this machine, one thread)\n", nf, totalMs / nf, worstMs);
    std::printf("shown displacement: at most %.3f mm off the model (", worstShow * 1000);
    for (size_t ri = 0; ri < d.regions.size(); ++ri) std::printf("%s %.3f%s", d.regions[ri].name.c_str(), worstPerRegion[ri] * 1000,
                                                                 ri + 1 < d.regions.size() ? ", " : "");
    std::printf("); contacts off by at most %d, held by %d; while under 20 nodes touch at most %.4f mm off; the press and "
                "the rise at most %.3f mm off\n", contactDiff, heldDiff, earlyWorst * 1000, pressDiff * 1000);
    std::printf("records after the grab: %d of %u differ in bytes; position at most %.4f mm off, 10-bit fields at most %d "
                "steps off (%d fields), colours %d\n", rd.bytesDiffer, d.records, rd.pos * 1000, rd.ten, rd.tenFields, rd.colour);
    std::printf("travelling dent (theta %.2f): %d of %u differ in bytes; position at most %.4f mm, 10-bit at most %d steps, "
                "colours %d\n", theta, dd.bytesDiffer, d.records, dd.pos * 1000, dd.ten, dd.colour);
    // the grab held on (the last frame's bones) for 10 s at 60 fps: does the solve settle, how long does a frame take
    // (argv[3]: the sweeps per frame for this part, to weigh speed against quality)
    if (argc > 3 && std::atoi(argv[3]) > 0) {
        d.prm.sweeps = static_cast<std::uint32_t>(std::atoi(argv[3]));
        std::printf("held with %u sweeps a frame\n", d.prm.sweeps);
    }
    {
        double worst = 0, sum = 0;
        int frames = 0;
        for (int f = 0; f < 600; ++f) {
            const auto t0 = std::chrono::high_resolution_clock::now();
            fm::Frame(d, world.data(), have.data(), sliders.data(), 1.0f / 60.0f, grip, S);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
            worst = std::fmax(worst, ms);
            sum += ms;
            ++frames;
            if (f == 0 || f == 59 || f == 179 || f == 599) {
                std::printf("held %3d frames (%.2f ms):", f + 1, ms);
                for (size_t ri = 0; ri < d.regions.size(); ++ri) {
                    const fm::Stats& st = S.r[ri].stats;
                    std::printf(" %s %u sweeps, %u contacts (%u held, %u sliding), in %.1f out %.1f mm, %.2f mm in a limb;",
                                d.regions[ri].name.c_str(), st.sweeps, st.contacts, st.held, st.slipping, st.deepest * 1000,
                                st.rise * 1000, st.inLimb * 1000);
                }
                std::printf("\n");
            }
        }
        std::printf("held 10 s: %.2f ms a frame on average, %.2f ms at most\n", sum / frames, worst);
    }
    // the two solve the same steps; once many nodes touch, float and double part ways a little in a frame's unsettled
    // sweeps (friction-like holds are path-dependent): exact while few touch, the press and the rise within 1 mm after,
    // no node more than 5 mm off; the records' coding exact (the dent's)
    const bool pass = earlyWorst < 5e-5 && pressDiff < 1e-3 && worstShow < 5e-3 && rd.pos < 5e-3 && rd.colour == 0 &&
                      dd.pos < 1e-5 && dd.ten <= 1 && dd.colour == 0 && contactDiff <= 12;
    std::printf("%s\n", pass ? "ALL PASSED" : "FAILED");
    return pass ? 0 : 1;
}
