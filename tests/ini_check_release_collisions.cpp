// Reads TFPhysics.ini (next to the exe) with the A-Body 1.5 release build's own LoadSettings and prints what it got.
// Built by tests\run_ini_check_release_collisions.bat with TFP_RELEASE + TFP_RELEASE_COLLISIONS.
#include "../src/plugin.cpp"

int main() {
    LoadSettings();
    std::printf("kDiag %d kCollisions %d | ini %s\n", kDiag, kCollisions, g_iniPath);
    std::printf("collision %d measureOnly %d others %d othersMeasureOnly %d deadBodies %d glutes %d innerThighs %d\n",
                g_cfg.collision, g_cfg.measureOnly, g_cfg.others, g_cfg.othersMeasureOnly, g_cfg.deadBodies, g_cfg.glutes,
                g_cfg.innerThighs);
    std::printf("softScale %.2f maxPush %.3f %.3f %.3f %.3f other %.3f m\n", g_cfg.softScale, g_cfg.maxPush[0],
                g_cfg.maxPush[1], g_cfg.maxPush[2], g_cfg.maxPush[3], g_cfg.maxPushOther);
    std::printf("bodyShape %d stretchGuard %d gravity %d | world proxies %d layer %d steer %d snapshots %d\n", g_cfg.shape,
                g_cfg.stretchGuard, g_cfg.enabled, g_cfg.proxies, g_cfg.proxyLayer, g_cfg.steer, g_cfg.snapshots);
    std::printf("breast mass %d x%.2f, glute mass %d x%.2f, relative damping %d | FreeMorph %d key %d start %d grip %.1f "
                "give %.1f cap x%.2f own pushes %d\n", g_cfg.massOn, g_cfg.massScale, g_cfg.gluteOn, g_cfg.gluteScale,
                g_cfg.relDamping, g_cfg.fmEnabled, g_cfg.fmModeKey, g_cfg.fmStartMode, g_cfg.fmGrip, g_cfg.fmHoldGive,
                g_cfg.fmCapScale, g_cfg.fmOwnPushes);
    const bool ok = kCollisions && !kDiag && g_cfg.collision && g_cfg.others && !g_cfg.othersMeasureOnly &&
                    !g_cfg.deadBodies && !g_cfg.measureOnly && !g_cfg.proxies && !g_cfg.proxyLayer && !g_cfg.steer &&
                    g_cfg.massOn && g_cfg.gluteOn && g_cfg.relDamping && g_cfg.fmEnabled && g_cfg.fmModeKey == 0 &&
                    g_cfg.fmStartMode == 2 && !g_cfg.fmOwnPushes;
    std::printf(ok ? "OK\n" : "FAIL\n");
    return ok ? 0 : 1;
}
