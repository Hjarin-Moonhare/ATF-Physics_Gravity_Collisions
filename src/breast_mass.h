// Breast and glute mass, and the springs' relative damping.  A breast moves as one net rooted at the chest (as Skyrim's
// GT Softbody builds it): its mass falls off toward the tip, its travel is lopsided along the breast, and its parts
// damp each other.  Two things, both pure functions so tests/breast_mass_test.cpp runs the same code:
//
// 1. Mass: each whole breast is one damped mass on its anchor bone (L/R_Pecs), simulated in the chest's own axes
//    (X along the spine, Y forward, Z to the left): it answers the anchor's acceleration only (standing still or moving
//    steadily it sits at rest; a step's bounce, a stop, a landing swing it), with its own stiffness and damping per
//    axis and lopsided soft limits (it drops further than it rises, swings further out than in).  The 4 TF_Breast
//    springs, the tips and the nipple hang under the anchor, so they all ride on it: the breast moves as one, and the
//    game's springs add the shape's follow-through on top.
// 2. Relative damping: the game damps a spring against its bone's world motion, so moving steadily at v leaves a lag
//    of c v / S (c the damping, S the stiffness): running, the soft springs sit pinned at their travel limit, held
//    back.  Before the game's update each spring's offset velocity gets + c x (the goal's velocity) back (x' -= c v dt,
//    x = goal - simulated), so the damping acts against the motion relative to the body; accelerations still swing it.
#pragma once
#include <cmath>

namespace bm {

struct Params {
    float hz[3] = {3.0f, 2.8f, 2.0f};            // natural frequency per chest axis (along the spine, forward, sideways)
    float zeta[3] = {0.45f, 0.40f, 0.35f};       // damping ratio
    float up = 0.020f, down = 0.030f;            // m: soft limits along the spine (it drops further than it rises)
    float fwd = 0.012f, back = 0.015f;           // forward / back
    float out = 0.020f, in = 0.012f;             // sideways, away from / toward the middle
    float hard = 1.35f;                          // the limit x this is a hard stop
    float limitStiff = 25.0f;                    // past a soft limit the spring is this many times stiffer
    float maxAccel = 80.0f;                      // m/s2: a bigger jump in the anchor's motion is a cut or a teleport
    float maxJump = 3.0f;                        // m: the anchor this far from where its last velocity puts it is a cut, a
                                                 // teleport or a load and resets the mass (a fall at any speed is where its
                                                 // velocity says; a speed limit would reset it in every long fall)
    float substep = 1.0f / 240.0f;               // s
};

struct State {
    float x[3] = {0, 0, 0}, v[3] = {0, 0, 0};    // chest axes, m and m/s
    float p[3] = {0, 0, 0}, pv[3] = {0, 0, 0};   // the anchor's last world position and velocity
    int have = 0;                                // 0 nothing yet, 1 a position, 2 a velocity too
};

// The glutes (as GT Softbody's glute: one chain per side held at the hip and the fold, heavy in the
// middle, travel lopsided down 7 / up 3, strong fore-aft damping): the same mass on L/R_Butt, in the body's axes (up,
// forward, sideways), heavier and softer than a breast, dropping much further than it rises, held fore and aft
inline Params GluteParams() {
    Params P;
    P.hz[0] = 3.0f, P.hz[1] = 2.6f, P.hz[2] = 2.2f;
    P.zeta[0] = 0.60f, P.zeta[1] = 0.55f, P.zeta[2] = 0.40f;
    P.up = 0.016f, P.down = 0.024f;
    P.fwd = 0.010f, P.back = 0.010f;
    P.out = 0.012f, P.in = 0.008f;
    return P;
}

inline void Reset(State& s) {
    for (int k = 0; k < 3; ++k) s.x[k] = s.v[k] = s.pv[k] = 0.0f;
    s.have = 0;
}

// One frame.  pos: the anchor's attachment point in the world (m); axes: the chest's three axes in the world (rows, unit);
// upSign: +1 if the chest's X axis points up the body (toward the head), else -1; left: 1 for the left breast (its
// outside is +Z), 0 for the right.  out: the offset to add to the anchor, in the chest's axes (m).
inline void Step(State& s, const float pos[3], const float axes[9], float upSign, int left, float dt, const Params& P,
                 float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    if (!(dt > 0.0f && dt < 0.25f)) return;
    float vw[3];
    if (s.have >= 1) {
        float miss = 0.0f;   // from where the last velocity puts it (no velocity yet: 0, the next frame tells)
        for (int k = 0; k < 3; ++k) {
            vw[k] = (pos[k] - s.p[k]) / dt;
            const float e = s.have == 2 ? pos[k] - s.p[k] - s.pv[k] * dt : 0.0f;
            miss += e * e;
        }
        if (miss > P.maxJump * P.maxJump) {   // a cut, a teleport, a load: start again from rest
            Reset(s);
            for (int k = 0; k < 3; ++k) s.p[k] = pos[k];
            s.have = 1;
            return;
        }
    }
    if (s.have == 0) {
        for (int k = 0; k < 3; ++k) s.p[k] = pos[k];
        s.have = 1;
        return;
    }
    if (s.have == 1) {
        for (int k = 0; k < 3; ++k) {
            s.pv[k] = vw[k];
            s.p[k] = pos[k];
        }
        s.have = 2;
        return;
    }
    float aw[3], an = 0.0f;
    for (int k = 0; k < 3; ++k) {
        aw[k] = (vw[k] - s.pv[k]) / dt;
        an += aw[k] * aw[k];
        s.pv[k] = vw[k];
        s.p[k] = pos[k];
    }
    an = std::sqrt(an);
    if (an > P.maxAccel)
        for (int k = 0; k < 3; ++k) aw[k] *= P.maxAccel / an;
    float a[3];   // the anchor's acceleration in the chest's axes
    for (int k = 0; k < 3; ++k) a[k] = aw[0] * axes[3 * k] + aw[1] * axes[3 * k + 1] + aw[2] * axes[3 * k + 2];
    // the limits per axis, positive and negative side (X: up is +upSign; Z: the outside is +Z on the left breast)
    const float hi[3] = {upSign > 0 ? P.up : P.down, P.fwd, left ? P.out : P.in};
    const float lo[3] = {upSign > 0 ? P.down : P.up, P.back, left ? P.in : P.out};
    int n = static_cast<int>(std::ceil(dt / P.substep));
    n = n < 1 ? 1 : (n > 16 ? 16 : n);
    const float h = dt / static_cast<float>(n);
    for (int it = 0; it < n; ++it)
        for (int k = 0; k < 3; ++k) {
            const float w = 6.28318531f * P.hz[k];
            float acc = -w * w * s.x[k] - 2.0f * P.zeta[k] * w * s.v[k] - a[k];
            if (s.x[k] > hi[k]) acc -= P.limitStiff * w * w * (s.x[k] - hi[k]);
            if (s.x[k] < -lo[k]) acc -= P.limitStiff * w * w * (s.x[k] + lo[k]);
            s.v[k] += acc * h;
            s.x[k] += s.v[k] * h;
            if (s.x[k] > P.hard * hi[k]) {
                s.x[k] = P.hard * hi[k];
                if (s.v[k] > 0) s.v[k] = 0;
            }
            if (s.x[k] < -P.hard * lo[k]) {
                s.x[k] = -P.hard * lo[k];
                if (s.v[k] < 0) s.v[k] = 0;
            }
        }
    for (int k = 0; k < 3; ++k) out[k] = s.x[k];
}

// Relative damping for one spring: the velocity change to its offset (driver axes) for the goal's world velocity
// vg (m/s) given in the same axes and its damping per axis c (the game's, 2 x ratio x sqrt(strength)).
inline void RelativeKick(const float vg[3], const float c[3], float dt, float dv[3]) {
    for (int k = 0; k < 3; ++k) dv[k] = -c[k] * vg[k] * dt;
}

}  // namespace bm
