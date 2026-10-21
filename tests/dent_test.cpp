// Dents (src/dents_core.h) against the design model (tools/dent_model.py): each case in tests/cases_dent (written by
// tests/export_dent_cases.py) holds a body's bones in a pose with the designed pushes and the dents the model gives;
// the plugin's solve must give the same dent bone moves, and on a rig whose regions tilt the same tilts.  Also: a
// cut-short or wrong file is turned away, a missing bone leaves its region undented, a dent never passes its largest
// move, a tilt's turn is a rotation, and the time a solve takes.
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/dents_core.h"

static char g_tests[MAX_PATH];

static bool ReadFileAll(const char* path, std::vector<std::uint8_t>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    const size_t got = n > 0 ? fread(out.data(), 1, out.size(), f) : 0;
    fclose(f);
    return got == out.size();
}

struct Case {
    std::string rig;
    float scale = 1.0f;
    std::map<std::string, float> morph;
    std::vector<dents::Xf> world;
    std::vector<std::uint8_t> have;
    std::map<std::string, std::vector<float>> expect;
    std::map<std::string, std::vector<float>> expectW;   // a tilting region's tilts (3 per dent bone)
};

static bool ReadCase(const char* path, size_t nBones, Case& c) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return false;
    c.world.assign(nBones, dents::Xf{});
    c.have.assign(nBones, 0);
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char* save = nullptr;
        char* tok = strtok_s(line, " \r\n", &save);
        if (!tok) continue;
        if (!strcmp(tok, "rig")) {
            c.rig = strtok_s(nullptr, " \r\n", &save);
        } else if (!strcmp(tok, "scale")) {
            c.scale = static_cast<float>(atof(strtok_s(nullptr, " \r\n", &save)));
        } else if (!strcmp(tok, "morph")) {
            const std::string n = strtok_s(nullptr, " \r\n", &save);
            c.morph[n] = static_cast<float>(atof(strtok_s(nullptr, " \r\n", &save)));
        } else if (!strcmp(tok, "B")) {
            const int i = atoi(strtok_s(nullptr, " \r\n", &save));
            strtok_s(nullptr, " \r\n", &save);   // the name
            const int h = atoi(strtok_s(nullptr, " \r\n", &save));
            if (i < 0 || i >= static_cast<int>(nBones)) { fclose(f); return false; }
            if (!h) continue;
            float v[13];
            for (float& x : v) x = static_cast<float>(atof(strtok_s(nullptr, " \r\n", &save)));
            dents::Xf& x = c.world[i];
            for (int k = 0; k < 3; ++k) x.ax[k] = {v[3 * k], v[3 * k + 1], v[3 * k + 2]};
            x.pos = {v[9], v[10], v[11]};
            x.scale = v[12];
            c.have[i] = 1;
        } else if (!strcmp(tok, "D")) {
            const std::string region = strtok_s(nullptr, " \r\n", &save);
            const int n = atoi(strtok_s(nullptr, " \r\n", &save));
            std::vector<float> d(n);
            for (float& x : d) x = static_cast<float>(atof(strtok_s(nullptr, " \r\n", &save)));
            c.expect[region] = d;
        } else if (!strcmp(tok, "W")) {
            const std::string region = strtok_s(nullptr, " \r\n", &save);
            const int n = atoi(strtok_s(nullptr, " \r\n", &save));
            std::vector<float> w(3 * static_cast<size_t>(n));
            for (float& x : w) x = static_cast<float>(atof(strtok_s(nullptr, " \r\n", &save)));
            c.expectW[region] = w;
        }
    }
    fclose(f);
    return true;
}

int main() {
    GetModuleFileNameA(nullptr, g_tests, MAX_PATH);
    for (int up = 0; up < 3; ++up)
        if (char* slash = strrchr(g_tests, '\\')) *slash = 0;
    strcat_s(g_tests, "\\tests");
    int fails = 0;
    std::map<std::string, dents::Rig> rigs;
    for (const char* tag : {"A", "B", "C"}) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\data\\DentRig_%s.bin", g_tests, tag);
        std::vector<std::uint8_t> buf;
        dents::Rig rig;
        std::string err;
        const bool ok = ReadFileAll(path, buf) && dents::Load(buf.data(), buf.size(), rig, err);
        int samples = 0;
        for (const auto& R : rig.regions) samples += static_cast<int>(R.samples.size());
        printf("%-34s rig %s: %d dent bones in %zu regions, %d samples, %zu shapes  %s\n", "load", tag, rig.totalDent,
               rig.regions.size(), samples, rig.colliders.size(), ok ? "OK" : "FAIL");
        if (!ok) {
            printf("    %s\n", err.c_str());
            ++fails;
            continue;
        }
        // a file cut short, or another file, is turned away with a reason (and nothing read half)
        int refused = 0;
        for (size_t cut : {size_t{4}, size_t{200}, buf.size() / 2, buf.size() - 1}) {
            dents::Rig r2;
            std::string e2;
            refused += !dents::Load(buf.data(), cut, r2, e2) && !e2.empty();
        }
        std::vector<std::uint8_t> bad = buf;
        bad[3] = 'X';
        dents::Rig r3;
        std::string e3;
        refused += !dents::Load(bad.data(), bad.size(), r3, e3);
        printf("%-34s rig %s: %d of 5 cut-short or wrong files turned away  %s\n", "load: refused", tag, refused,
               refused == 5 ? "OK" : "FAIL");
        fails += refused != 5;
        rigs[tag] = std::move(rig);
    }
    // every case: the plugin's dents against the model's
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\cases_dent\\*.txt", g_tests);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    std::vector<std::string> names;
    if (h != INVALID_HANDLE_VALUE) {
        do names.push_back(fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::sort(names.begin(), names.end());
    dents::Work wk;
    double solveUs = 0;
    int solves = 0, contactSolves = 0;
    double contactUs = 0;
    for (const std::string& name : names) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\cases_dent\\%s", g_tests, name.c_str());
        Case c;
        const std::string tag = name.substr(0, 1);
        if (!rigs.count(tag) || !ReadCase(path, rigs[tag].bones.size(), c)) {
            printf("%-34s unread  FAIL\n", name.c_str());
            ++fails;
            continue;
        }
        const dents::Rig& rig = rigs[tag];
        std::vector<float> morphW(rig.morphs.size(), 0.0f);
        for (size_t m = 0; m < rig.morphs.size(); ++m)
            if (c.morph.count(rig.morphs[m])) morphW[m] = c.morph[rig.morphs[m]];
        float worst = 0, worstW = 0, deepest = 0, tiltMax = 0;
        int contacts = 0;
        bool shape = true, tilts = false;
        for (const dents::Region& R : rig.regions) {
            std::vector<float> D(R.dent.size()), W(3 * R.dent.size());
            dents::RegionStats st;
            const auto t0 = std::chrono::steady_clock::now();
            dents::SolveRegion(rig, R, c.world.data(), c.have.data(), morphW.data(), 1.0f, c.scale, D.data(), &st, wk,
                               W.data());
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            solveUs += us;
            ++solves;
            if (st.contacts) {
                contactUs += us;
                ++contactSolves;
            }
            contacts += st.contacts;
            deepest = std::fmax(deepest, st.dent);
            tiltMax = std::fmax(tiltMax, st.tilt);
            const auto it = c.expect.find(R.name);
            if (it == c.expect.end() || it->second.size() != D.size()) {
                shape = false;
                continue;
            }
            for (size_t j = 0; j < D.size(); ++j) worst = std::fmax(worst, std::fabs(D[j] - it->second[j]));
            if (R.flags & 1) {   // a tilting region: its tilts too
                tilts = true;
                const auto iw = c.expectW.find(R.name);
                if (iw == c.expectW.end() || iw->second.size() != W.size()) {
                    shape = false;
                    continue;
                }
                for (size_t j = 0; j < W.size(); ++j) worstW = std::fmax(worstW, std::fabs(W[j] - iw->second[j]));
            }
        }
        const bool ok = shape && worst < 0.0005f && worstW < 0.005f;
        if (tilts)
            printf("%-34s %4d skin samples in contact, deepest dent %.2f cm, worst difference %.3f mm; tilt up to %.1f deg, "
                   "worst difference %.4f  %s\n", name.c_str(), contacts, deepest * 100, worst * 1000,
                   tiltMax * 57.29578f, worstW, ok ? "OK" : "FAIL");
        else
            printf("%-34s %4d skin samples in contact, deepest dent %.2f cm, worst difference %.3f mm  %s\n", name.c_str(),
                   contacts, deepest * 100, worst * 1000, ok ? "OK" : "FAIL");
        fails += !ok;
    }
    if (names.empty()) {
        printf("%-34s no cases (python tests\\export_dent_cases.py)  FAIL\n", "cases");
        ++fails;
    }
    // a tilt's turn: three orthonormal axes, right-handed, and to first order I + [w]x
    {
        const float ws[3][3] = {{0.3f, -0.2f, 0.1f}, {0.0f, 0.0f, 0.9f}, {1e-3f, 0.0f, 0.0f}};
        float off = 0, firstOrder = 0;
        for (const auto& w : ws) {
            dents::V3 ax[3];
            dents::TiltAxes(w, ax);
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) off = std::fmax(off, std::fabs(dents::Dot(ax[i], ax[j]) - (i == j ? 1.0f : 0.0f)));
            const dents::V3 c = {ax[0].y * ax[1].z - ax[0].z * ax[1].y, ax[0].z * ax[1].x - ax[0].x * ax[1].z,
                                 ax[0].x * ax[1].y - ax[0].y * ax[1].x};
            off = std::fmax(off, dents::Len(dents::Sub(c, ax[2])));
        }
        dents::V3 ax[3];
        dents::TiltAxes(ws[2], ax);
        firstOrder = dents::Len(dents::Sub(ax[1], dents::V3{0.0f, 1.0f, ws[2][0]}));
        const bool okT = off < 1e-5f && firstOrder < 1e-5f;
        printf("%-34s off a rotation by %.1e, a small tilt off I + [w]x by %.1e  %s\n", "tilt turn", off, firstOrder,
               okT ? "OK" : "FAIL");
        fails += !okT;
    }
    // a missing bone (a dent bone's parent) leaves its region undented; the depth setting scales the dents; a dent
    // bone never moves past its largest move
    for (auto& [tag, rig] : rigs) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\cases_dent\\%s_folded.txt", g_tests, tag.c_str());
        Case c;
        if (!ReadCase(path, rig.bones.size(), c)) continue;
        const dents::Region& R = rig.regions[0];
        std::vector<float> D(R.dent.size()), D2(R.dent.size()), D0(R.dent.size());
        std::vector<std::uint8_t> miss = c.have;
        miss[R.dent[0].parent] = 0;
        dents::SolveRegion(rig, R, c.world.data(), miss.data(), nullptr, 1.0f, 1.0f, D.data(), nullptr, wk);
        float any = 0;
        for (float x : D) any = std::fmax(any, x);
        printf("%-34s rig %s, %s without %s: largest dent bone move %.3f cm  %s\n", "missing bone", tag.c_str(),
               R.name.c_str(), rig.bones[R.dent[0].parent].name.c_str(), any * 100, any == 0 ? "OK" : "FAIL");
        fails += any != 0;
        dents::SolveRegion(rig, R, c.world.data(), c.have.data(), nullptr, 0.0f, 1.0f, D0.data(), nullptr, wk);
        dents::SolveRegion(rig, R, c.world.data(), c.have.data(), nullptr, 2.0f, 1.0f, D2.data(), nullptr, wk);
        float z = 0, most = 0;
        for (size_t j = 0; j < D.size(); ++j) {
            z = std::fmax(z, D0[j]);
            most = std::fmax(most, D2[j]);
        }
        const bool okS = z == 0 && most <= R.dMax * 2.0f + 1e-6f;
        printf("%-34s rig %s, %s: depth x0 largest move %.3f cm, x2 %.2f cm (at most %.1f)  %s\n", "depth setting",
               tag.c_str(), R.name.c_str(), z * 100, most * 100, R.dMax * 200, okS ? "OK" : "FAIL");
        fails += !okS;
    }
    printf("%-34s %d solves, %.1f us each (%.1f us with a contact)\n", "time", solves, solves ? solveUs / solves : 0.0,
           contactSolves ? contactUs / contactSolves : 0.0);
#ifdef DENTS_PROFILE
    printf("profile (us in all): place %.0f, needs %.0f, wanted %.0f, fit build %.0f, sweeps %.0f, rest %.0f\n",
           dents::g_prof[0], dents::g_prof[1], dents::g_prof[2], dents::g_prof[3], dents::g_prof[4], dents::g_prof[5]);
#endif
    printf("%s\n", fails ? "SOME CASES FAILED" : "all cases passed");
    return fails ? 1 : 0;
}
