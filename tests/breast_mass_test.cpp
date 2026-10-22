// Offline check of src/breast_mass.h: the breast mass on made-up chest motion (standing,
// steady motion, a walk's and a run's bounce, a sudden stop, a teleport) at 30, 60 and 144 fps, and the relative
// damping on a model of the game's spring (damping on the bone's world motion).
// Build: tests\build_breast_mass_test.bat
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "breast_mass.h"

static const float kAxes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // chest X up the spine, Y forward, Z left = the world's here

struct Motion { float bobCm, bobHz, swayCm, swayHz, speed, stopAt; };

// the anchor's world position at time t: forward at speed (stopping at stopAt over 0.15 s), a vertical bob, a sway
static void Where(const Motion& m, double t, float p[3]) {
    double fwd;
    if (t < m.stopAt) fwd = m.speed * t;
    else {
        const double u = t - m.stopAt;
        fwd = m.speed * m.stopAt + (u < 0.15 ? m.speed * (u - u * u / 0.3) : m.speed * 0.075);
    }
    const double moving = t < m.stopAt ? 1.0 : 0.0;
    p[0] = static_cast<float>(moving * m.bobCm / 100.0 * std::sin(6.283185307 * m.bobHz * t));
    p[1] = static_cast<float>(fwd);
    p[2] = static_cast<float>(moving * m.swayCm / 100.0 * std::sin(6.283185307 * m.swayHz * t));
}

struct Result { float peak[3], lo[3], settleS, last; bool finite; };

static Result Run(const Motion& m, float fps, float seconds, const bm::Params& P = bm::Params{}) {
    bm::State s;
    Result r{{0, 0, 0}, {0, 0, 0}, -1.0f, 0.0f, true};
    const float dt = 1.0f / fps;
    float out[3];
    for (int f = 0; f * dt < seconds; ++f) {
        const double t = f * dt;
        float p[3];
        Where(m, t, p);
        bm::Step(s, p, kAxes, 1.0f, 1, dt, P, out);
        float mag = 0;
        for (int k = 0; k < 3; ++k) {
            r.finite &= std::isfinite(out[k]);
            if (t > 1.0 && t < m.stopAt) {   // after the start-up, while moving
                r.peak[k] = std::fmax(r.peak[k], out[k]);
                r.lo[k] = std::fmin(r.lo[k], out[k]);
            }
            mag = std::fmax(mag, std::fabs(out[k]));
        }
        if (t > m.stopAt && mag > 0.001f) r.settleS = static_cast<float>(t - m.stopAt);
        r.last = mag;
    }
    return r;
}

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        fails += !ok;
    };
    const Motion still{0, 1, 0, 1, 0, 1e9f}, steady{0, 1, 0, 1, 5.0f, 1e9f};
    const Motion walk{2.0f, 1.9f, 1.0f, 0.95f, 1.4f, 6.0f}, run{4.0f, 2.8f, 1.5f, 1.4f, 4.0f, 6.0f};
    Result a = Run(still, 60, 3), b = Run(steady, 60, 3);
    std::printf("still: %.3f mm; steady 5 m/s: %.3f mm\n", a.last * 1000, b.last * 1000);
    check(a.finite && a.last < 1e-6f, "standing still: at rest");
    check(b.finite && b.last < 1e-4f, "steady motion: at rest (no lag)");
    for (const float fps : {30.0f, 60.0f, 144.0f}) {
        Result w = Run(walk, fps, 8), r = Run(run, fps, 8);
        std::printf("%5.0f fps  walk: spine %+.1f / %+.1f, fwd %+.1f / %+.1f, side %+.1f / %+.1f mm, settles %.2f s after "
                    "stopping | run: spine %+.1f / %+.1f, fwd %+.1f / %+.1f, side %+.1f / %+.1f mm, settles %.2f s\n", fps,
                    w.peak[0] * 1000, w.lo[0] * 1000, w.peak[1] * 1000, w.lo[1] * 1000, w.peak[2] * 1000, w.lo[2] * 1000,
                    w.settleS, r.peak[0] * 1000, r.lo[0] * 1000, r.peak[1] * 1000, r.lo[1] * 1000, r.peak[2] * 1000,
                    r.lo[2] * 1000, r.settleS);
        char what[160];
        snprintf(what, sizeof(what), "%.0f fps: walk bounces 5-25 mm along the spine", fps);
        check(w.finite && w.peak[0] - w.lo[0] > 0.005f && w.peak[0] - w.lo[0] < 0.025f * 2, what);
        snprintf(what, sizeof(what), "%.0f fps: run bounces more than the walk, never at the hard stops", fps);
        bm::Params P;
        check(r.finite && (r.peak[0] - r.lo[0]) > (w.peak[0] - w.lo[0]) && r.peak[0] < 0.97f * P.hard * P.up &&
                  r.lo[0] > -0.97f * P.hard * P.down, what);   // (never at the hard stop: no snap)
        snprintf(what, sizeof(what), "%.0f fps: settles within 0.8 s of a stop", fps);
        check(w.settleS < 0.8f && r.settleS < 0.8f, what);
    }
    {   // the glutes (bm::GluteParams): the hips bob like the chest
        const bm::Params G = bm::GluteParams();
        for (const float fps : {30.0f, 60.0f, 144.0f}) {
            Result w = Run(walk, fps, 8, G), r = Run(run, fps, 8, G);
            std::printf("%5.0f fps  glute walk: up %+.1f / %+.1f, side %+.1f / %+.1f mm, settles %.2f s | run: up %+.1f / %+.1f, "
                        "side %+.1f / %+.1f mm, settles %.2f s\n", fps, w.peak[0] * 1000, w.lo[0] * 1000, w.peak[2] * 1000,
                        w.lo[2] * 1000, w.settleS, r.peak[0] * 1000, r.lo[0] * 1000, r.peak[2] * 1000, r.lo[2] * 1000, r.settleS);
            char what[160];
            snprintf(what, sizeof(what), "%.0f fps: the glute's walk bounce 5-30 mm, its run never at a hard stop, settles", fps);
            check(w.finite && r.finite && w.peak[0] - w.lo[0] > 0.005f && w.peak[0] - w.lo[0] < 0.030f &&
                      r.peak[0] < 0.97f * G.hard * G.up && r.lo[0] > -0.97f * G.hard * G.down && w.settleS < 0.8f &&
                      r.settleS < 0.8f, what);
        }
    }
    {   // frame-rate independence of the run's bounce
        Result r30 = Run(run, 30, 8), r144 = Run(run, 144, 8);
        const float s30 = r30.peak[0] - r30.lo[0], s144 = r144.peak[0] - r144.lo[0];
        std::printf("run bounce 30 fps %.1f mm, 144 fps %.1f mm\n", s30 * 1000, s144 * 1000);
        check(std::fabs(s30 - s144) < 0.2f * s144, "the run's bounce within 20% at 30 and 144 fps");
    }
    {   // a long fall (a speed limit would reset the mass every frame of it): 5 s at 1 g (49 m/s), then a
        // landing; the mass must stay continuous (no reset), float up in the fall, never pass its hard stops, settle
        for (const bool glute : {false, true}) {
            for (const float fps : {30.0f, 60.0f, 144.0f}) {
                const bm::Params P = glute ? bm::GluteParams() : bm::Params{};
                bm::State s;
                const float dt = 1.0f / fps;
                float out[3], prev = 0.0f, worstStep = 0.0f, fallUp = 0.0f, settle = -1.0f, hi = 0.0f, lo = 0.0f;
                double x = 0.0, v = 0.0;
                bool finite = true;
                for (int f = 0; f * dt < 8.0f; ++f) {
                    const double t = f * dt;
                    if (t < 5.0) v -= 9.81 * dt;
                    else v = 0.0;   // the landing: the fall stops at once
                    x += v * dt;
                    const float p[3] = {static_cast<float>(x), 0.0f, 0.0f};
                    bm::Step(s, p, kAxes, 1.0f, 1, dt, P, out);
                    finite &= std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
                    if (t > 0.5 && t < 5.0) {
                        worstStep = std::fmax(worstStep, std::fabs(out[0] - prev));
                        fallUp = out[0];
                    }
                    hi = std::fmax(hi, out[0]);
                    lo = std::fmin(lo, out[0]);
                    if (t > 5.0 && std::fabs(out[0]) > 0.001f) settle = static_cast<float>(t - 5.0);
                    prev = out[0];
                }
                std::printf("%s fall %3.0f fps: in the fall %+.1f mm (most change a frame %.2f mm); range %+.1f / %+.1f mm; "
                            "settles %.2f s after landing\n", glute ? "glute " : "breast", fps, fallUp * 1000, worstStep * 1000,
                            hi * 1000, lo * 1000, settle);
                char what[160];
                snprintf(what, sizeof(what), "%s, %.0f fps: a long fall stays smooth, floats up, lands, settles",
                         glute ? "glute" : "breast", fps);
                check(finite && worstStep < 0.002f && fallUp > 0.005f && hi <= P.hard * P.up + 1e-6f &&
                          lo >= -P.hard * P.down - 1e-6f && settle < 1.2f, what);
            }
        }
    }
    {   // a teleport: the mass starts again from rest
        bm::Params P;
        bm::State s;
        float out[3], p[3] = {0, 0, 0}, worst = 0;
        for (int f = 0; f < 240; ++f) {
            if (f == 120) p[1] = 30.0f;
            bm::Step(s, p, kAxes, 1.0f, 1, 1.0f / 60, P, out);
            for (int k = 0; k < 3; ++k) worst = std::fmax(worst, std::fabs(out[k]));
        }
        check(worst < 1e-6f, "a teleport moves nothing");
    }
    {   // relative damping on a model of the game's spring: x = goal - sim, sim'' = S (goal - sim) - c sim'
        const float S = 72.0f, ratio = 0.18f, c = 2 * ratio * std::sqrt(S), v = 4.0f, dt = 1.0f / 60;
        for (const bool kick : {false, true}) {
            float x = 0, xv = 0;   // offset and its velocity
            for (int f = 0; f < 600; ++f) {
                const float gv = v;   // the goal moves steadily
                if (kick) {
                    const float vg[3] = {gv, 0, 0}, cc[3] = {c, 0, 0};
                    float dv[3];
                    bm::RelativeKick(vg, cc, dt, dv);
                    xv += dv[0];
                }
                for (int sub = 0; sub < 4; ++sub) {   // the game's: goal'' = 0, x'' = -S x + c (goal' - x')
                    const float h = dt / 4;
                    const float acc = -S * x + c * (gv - xv) * (sub == 0 && kick ? 1.0f : 1.0f);
                    xv += acc * h;
                    x += xv * h;
                }
            }
            std::printf("model spring at %.0f m/s, relative damping %s: steady lag %.1f mm (c v / S = %.1f mm)\n", v,
                        kick ? "on" : "off", x * 1000, c * v / S * 1000);
            if (!kick) check(std::fabs(x - c * v / S) < 0.002f, "the model spring lags c v / S without it");
            else check(std::fabs(x) < 0.25f * c * v / S, "relative damping takes most of the lag off");
        }
    }
    std::printf("%s: %d failures\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
