// Prints how TF Physics reads a TFPhysics.ini (its gravity settings and its [Tuning] section) and the tuning files next
// to it (the TFPhysics folder beside the INI, every *.ini in name order), with the plugin's own readers: one line per
// setting.  A checker for tuning files and for installers that write them.
// Usage: tune_dump.exe <path to TFPhysics.ini>   (Build: tests\run_tests.bat)
#include "../src/plugin.cpp"

static const char* LevelText(int l) {
    return l == kLevelPreset ? "Preset" : l == kLevelOff ? "Off" : (l >= 0 && l < kNumLevels ? kLevelName[l] : "?");
}
static void Parts(const float f[kNumParts]) {
    for (int p = 0; p < kNumParts; ++p) printf(" %s %.4f", kPartSection[p], f[p]);
    printf("\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: tune_dump <path to TFPhysics.ini>\n");
        return 2;
    }
    if (!GetFullPathNameA(argv[1], MAX_PATH, g_iniPath, nullptr) || GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        printf("no such file: %s\n", argv[1]);
        return 2;
    }
    ReadSettings();
    TuneSettings& t = g_tunes[0];
    LoadTuning(t);
    printf("ini %s\n", g_iniPath);
    const Settings& cfg = g_cfg;
    printf("gravity enabled %d level %.4f back %.4f side %.4f hang %.4f down %.4f maxlevel %.4f size %d shapes %.4f\n",
           cfg.enabled ? 1 : 0, cfg.gravityLevel, cfg.maxBack, cfg.maxSide, cfg.maxHang, cfg.maxDown, cfg.maxLevel,
           cfg.sizeScaling ? 1 : 0, cfg.gravityShape);
    const float sag[kNumParts] = {cfg.springGravity, cfg.springGravityButt, cfg.springGravityBelly, cfg.springGravityThighs,
                                  cfg.springGravityHips, cfg.springGravityCalves, cfg.springGravityArms, cfg.springGravityTorso};
    printf("springs");
    Parts(sag);
    printf("collision enabled %d couple %d\n", cfg.collision ? 1 : 0, cfg.couplePushes ? 1 : 0);
    printf("files %d %s\n", t.files, t.fileNames);
    printf("reload %d\n", g_cfg.tuneReload ? 1 : 0);
    printf("planet %d %.4f\n", t.planet ? 1 : 0, t.planetAmount);
    printf("outfits %d\n", t.outfitsOn ? 1 : 0);
    printf("all %s\n", LevelText(t.allLevel));
    for (int p = 0; p < kNumParts; ++p) {
        const PartTune& pt = t.part[p];
        printf("part %s level %s match %d swing %.4f side %.4f frontback %.4f updown %.4f settle %.4f bounce %.4f sag %.4f "
               "limit %.4f gravity %.4f back %.4f sideways %.4f hang %.4f down %.4f shape %.4f\n",
               kPartSection[p], LevelText(pt.level), pt.match ? 1 : 0, pt.swing, pt.dir[0], pt.dir[1], pt.dir[2], pt.settle,
               pt.bounce, pt.sag, pt.limit, pt.gravLevel, pt.gravPose[0], pt.gravPose[1], pt.gravPose[2], pt.gravPose[3],
               pt.shape);
    }
    for (int b = 0; b < kNumPresetBones; ++b)
        if (t.bone[b] != 1.0f) printf("bone %s %.4f\n", kPresetBones[b].name, t.bone[b]);
    for (int c = 0; c < t.nChars; ++c) {
        printf("character %s %08X", t.chars[c].player ? "player" : "npc", t.chars[c].id);
        Parts(t.chars[c].part);
    }
    for (int o = 0; o < t.nOutfits; ++o) {
        const OutfitTune& ot = t.outfits[o];
        printf("outfit [%s] form %08X slots", ot.name, ot.form);
        for (int s = 0; s < 64; ++s)
            if (ot.slots >> s & 1) printf(" %d", s);
        printf(" |");
        Parts(ot.part);
    }
    printf("changes springs %d swing %d damping %d\n", t.springs ? 1 : 0, t.scaled ? 1 : 0, t.damping ? 1 : 0);
    return 0;
}
