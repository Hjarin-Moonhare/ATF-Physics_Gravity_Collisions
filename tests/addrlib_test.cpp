// Offline test of the game addresses through the Address Library: the plugin's own database reader
// (ReadAddressLibrary), address resolution (ResolveAddresses) and start-up checks (CheckGame, CheckSizeReading), on fake
// versionlib files and a fake game image laid out here with the bytes read from Starfield.exe 1.16.244:
//   - lookups give the RVAs written, from the path SFSE itself checks;
//   - a missing file, another format, another game version, a cut-short file, an ID past the end or with no address,
//     and an address outside the image all fail and leave nothing resolved;
//   - every check passes on the fake image, also with every function moved and reordered (the operands of the calls,
//     the load and the lea then change: they are checked by where they land), and each one fails when one byte of what
//     it checks changes; an address in unreadable memory fails its check without a crash;
//   - SFSE 0.2.21's own rule takes the plugin as version independent.
// With the game installed (the folder given as the first argument, else the STARFIELD_DIR environment variable, else
// the usual Steam folders) it also resolves through the real database for the installed Starfield.exe and runs every
// check on the exe itself, read from disk and laid out as the loader does (read only): on 1.16.244 every address must
// be the known one (kKnownRva).  Without the game those cases are skipped, not failed.
// Build and run: tests\run_tests.bat
#include "../src/plugin.cpp"

#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

static int g_fails = 0;
static char g_dir[MAX_PATH];   // build\tests

static void Report(const char* what, const char* detail, bool ok) {
    printf("%-44s %s  %s\n", what, detail, ok ? "OK" : "FAIL");
    g_fails += !ok;
}

static bool NoneResolved() {
    for (std::uintptr_t a : g_addr)
        if (a) return false;
    return true;
}
static void ClearResolved() { std::memset(g_addr, 0, sizeof(g_addr)); }

// ------------------------------------------------------------------ fake databases
struct FakeDb {
    std::int32_t format = 5;
    std::uint32_t version[4] = {1, 16, 244, 0};
    std::uint32_t pointerSize = 8;
    std::uint32_t count = 0;            // IDs the header says it has
    std::vector<std::uint32_t> rvas;    // written after the header (fewer than count: cut short)
};

static std::string WriteDb(const char* name, const FakeDb& d) {
    const std::string path = std::string(g_dir) + "\\" + name;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) {
        printf("cannot write %s\n", path.c_str());
        exit(1);
    }
    std::uint8_t h[0x60] = {};
    memcpy(h, &d.format, 4);
    memcpy(h + 4, d.version, 16);
    memcpy(h + 0x14, "Starfield.exe", 13);
    memcpy(h + 0x54, &d.pointerSize, 4);
    memcpy(h + 0x5C, &d.count, 4);
    fwrite(h, 1, sizeof(h), f);
    if (!d.rvas.empty()) fwrite(d.rvas.data(), 4, d.rvas.size(), f);
    fclose(f);
    return path;
}

// ------------------------------------------------------------------ a fake game image
// Every ID's function or global in its own 0x2000-byte slot, the bytes the checks look for written where each GameAddr
// lands (as read from 1.16.244), the RTTI and the triangle's names after the slots, and one page reserved but not
// committed at the end (unreadable).  `moved`: the slots in reverse order, each shifted by a different amount, so every
// distance between code and data differs from the plain layout.
struct Layout {
    char* image = nullptr;
    std::uint64_t size = 0, unreadable = 0;   // the image's span; the RVA of its unreadable page
    std::vector<std::uint32_t> rvas;          // per ID (the index), 0 = none
    std::uintptr_t at[kNumGameAddrs] = {};    // where each GameAddr lands
    std::string db;                           // its versionlib file
};

static void Put(std::uintptr_t at, std::initializer_list<int> bytes) {
    for (int b : bytes) *reinterpret_cast<std::uint8_t*>(at++) = static_cast<std::uint8_t>(b);
}
static void Put64(std::uintptr_t at, std::uint64_t v) { memcpy(reinterpret_cast<void*>(at), &v, 8); }
static void PutRel(std::uintptr_t disp, std::uintptr_t next, std::uintptr_t target) {   // a rip-relative operand
    const std::int32_t rel = static_cast<std::int32_t>(static_cast<std::int64_t>(target - next));
    memcpy(reinterpret_cast<void*>(disp), &rel, 4);
}

static constexpr std::uint32_t kMaxId = 936960;   // the largest ID the plugin uses (the frame time)

static Layout MakeLayout(bool moved, const char* dbName) {
    Layout L;
    std::vector<std::uint32_t> ids;   // the distinct IDs, in table order
    for (const AddrRef& r : kAddrRef) {
        bool seen = false;
        for (std::uint32_t id : ids) seen |= id == r.id;
        if (!seen) ids.push_back(r.id);
    }
    const std::uint32_t n = static_cast<std::uint32_t>(ids.size());
    const std::uint32_t extra = 0x10000 + 0x2000 * (n + 1);   // RTTI and names
    L.unreadable = extra + 0x1000;
    L.size = L.unreadable + 0x1000;
    L.image = static_cast<char*>(VirtualAlloc(nullptr, L.size, MEM_RESERVE, PAGE_NOACCESS));
    VirtualAlloc(L.image, L.unreadable, MEM_COMMIT, PAGE_READWRITE);
    L.rvas.assign(kMaxId + 1, 0);
    for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint32_t slot = moved ? n - 1 - k : k;
        L.rvas[ids[k]] = 0x10000 + 0x2000 * slot + (moved ? 0x40 * ((k * 7) % 11) + 0x10 : 0);
    }
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(L.image);
    for (int i = 0; i < kNumGameAddrs; ++i) L.at[i] = base + L.rvas[kAddrRef[i].id] + kAddrRef[i].off;
    const std::uintptr_t* at = L.at;

    // the functions' first bytes
    Put(at[kModifierUpdate], {0x48, 0x85, 0xC9, 0x0F, 0x84, 0xDF, 0x00, 0x00, 0x00});
    Put(at[kGetModifierExtra], {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC});
    Put(at[kRigFind], {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24});
    Put(at[kRigRelease], {0x48, 0x85, 0xC9, 0x0F, 0x84, 0xA8, 0x00, 0x00, 0x00});
    Put(at[kPoseDeformerUpdate], {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89});
    Put(at[kSpringBoneUpdate], {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x81, 0xEC});
    // the two modifier classes' update slots
    Put64(at[kPoseDeformerVtbl] + 8, at[kPoseDeformerUpdate]);
    Put64(at[kSpringBoneVtbl] + 8, at[kSpringBoneUpdate]);
    // vmovss xmm10,[rip+..] -> the frame time
    Put(at[kFrameTimeLoad], {0xC5, 0x7A, 0x10, 0x15});
    PutRel(at[kFrameTimeLoad] + 4, at[kFrameTimeLoad] + 8, at[kFrameTime]);
    const float dt = 1.0f / 60;
    memcpy(reinterpret_cast<void*>(at[kFrameTime]), &dt, 4);
    // the three calls of the modifier loop, and the blocks before B and C
    for (int i = 0; i < 3; ++i) {
        Put(at[kCallSiteA + i], {0xE8});
        PutRel(at[kCallSiteA + i] + 1, at[kCallSiteA + i] + 5, at[kModifierUpdate]);
    }
    Put(at[kCallSiteB] - 16, {0x49, 0x8B, 0xCC, 0xFF, 0x90, 0x58, 0x05, 0x00, 0x00, 0x90, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0x08});
    const std::uintptr_t blockC = at[kCallSiteC] - 15;
    Put(blockC, {0x48, 0x8B, 0xCE, 0xE8, 0, 0, 0, 0, 0x90, 0x48, 0x8B, 0xD3, 0x48, 0x8B, 0x08});
    PutRel(blockC + 4, blockC + 8, at[kGet3D]);
    // the size reading: the morph-table loader's instructions, TESNPC::Load in slot 17, the classes' RTTI
    Put(at[kMorphLea448], {0x48, 0x8D, 0xB9, 0x48, 0x04, 0x00, 0x00});
    Put(at[kMorphCapacity], {0x48, 0x8B, 0x53, 0x20});
    Put(at[kMorphEntries], {0x48, 0x8B, 0x43, 0x18});
    Put(at[kMorphWeight], {0xC4, 0x81, 0x7A, 0x10, 0x44, 0xCA, 0x08});
    Put64(at[kNpcVtbl] + 8 * 17, at[kNpcLoad]);
    const GameAddr cls[3] = {kActorVtbl, kPlayerVtbl, kNpcVtbl};
    const char* clsName[3] = {".?AVActor@@", ".?AVPlayerCharacter@@", ".?AVTESNPC@@"};
    for (int c = 0; c < 3; ++c) {
        const std::uint32_t col = extra + 0x100 * c, td = extra + 0x400 + 0x100 * c;
        Put64(at[cls[c]] - 8, base + col);
        const std::uint32_t sig = 1;
        memcpy(L.image + col, &sig, 4);
        memcpy(L.image + col + 0x0C, &td, 4);
        strcpy_s(L.image + td + 0x10, 0x80, clsName[c]);
    }
    // the triangle: vmovss xmm6,[rsi+r15+0x350], lea r14,[rip+..] -> the name table -> Thin, Strong, Overweight
    Put(at[kTriWeightLoad], {0xC4, 0xA1, 0x7A, 0x10, 0xB4, 0x3E, 0x50, 0x03, 0x00, 0x00});
    Put(at[kTriNamesLea], {0x4C, 0x8D, 0x35});
    PutRel(at[kTriNamesLea] + 3, at[kTriNamesLea] + 7, at[kTriNameTable]);
    const char* names[3] = {"Thin", "Strong", "Overweight"};
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t s = extra + 0x800 + 0x20 * i;
        strcpy_s(L.image + s, 0x20, names[i]);
        Put64(at[kTriNameTable] + 8 * i, base + s);
    }
    FakeDb d;
    d.count = kMaxId + 1;
    d.rvas = L.rvas;
    L.db = WriteDb(dbName, d);
    return L;
}

static void ResetSizeReading() {
    g_actorVt = g_playerVt = g_npcVt = 0;
    g_triangleOk = false;
}

// Changes the byte at `at` (xor 0x5A), runs `check`, puts the byte back; true if the check said no meanwhile.
template <class F> static bool FailsWhenChanged(std::uintptr_t at, F check) {
    auto* p = reinterpret_cast<std::uint8_t*>(at);
    *p ^= 0x5A;
    const bool failed = !check();
    *p ^= 0x5A;
    return failed;
}

// ------------------------------------------------------------------ the real game, read from disk
// Starfield.exe laid out the way the loader maps it (sections at their RVAs) and relocated to where it lands here.
static char* MapExe(const char* path, std::uint64_t& imageSize) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return nullptr;
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    std::vector<char> file(static_cast<size_t>(size));
    const bool read = fread(file.data(), 1, file.size(), f) == file.size();
    fclose(f);
    if (!read || size < 0x1000) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    imageSize = nt->OptionalHeader.SizeOfImage;
    char* img = static_cast<char*>(VirtualAlloc(nullptr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!img) return nullptr;
    memcpy(img, file.data(), nt->OptionalHeader.SizeOfHeaders);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        size_t n = sec[i].SizeOfRawData < sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize;
        if (sec[i].VirtualAddress + n > imageSize) n = static_cast<size_t>(imageSize - sec[i].VirtualAddress);
        if (sec[i].PointerToRawData + n > file.size()) n = file.size() - sec[i].PointerToRawData;
        memcpy(img + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, n);
    }
    const IMAGE_DATA_DIRECTORY& rd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    const std::uint64_t delta = reinterpret_cast<std::uint64_t>(img) - nt->OptionalHeader.ImageBase;
    for (std::uint32_t pos = 0; pos + sizeof(IMAGE_BASE_RELOCATION) <= rd.Size;) {
        const auto* blk = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(img + rd.VirtualAddress + pos);
        if (blk->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
        const auto* e = reinterpret_cast<const WORD*>(blk + 1);
        const size_t count = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
        for (size_t k = 0; k < count; ++k) {
            const std::uint64_t at = blk->VirtualAddress + (e[k] & 0xFFFu);
            if ((e[k] >> 12) == IMAGE_REL_BASED_DIR64 && at + 8 <= imageSize) *reinterpret_cast<std::uint64_t*>(img + at) += delta;
        }
        pos += blk->SizeOfBlock;
    }
    return img;
}

static bool ExeVersion(const char* path, std::uint32_t& major, std::uint32_t& minor, std::uint32_t& build) {
    DWORD ignored = 0;
    const DWORD n = GetFileVersionInfoSizeA(path, &ignored);
    if (!n) return false;
    std::vector<char> info(n);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoA(path, 0, n, info.data()) || !VerQueryValueA(info.data(), "\\", reinterpret_cast<void**>(&fixed), &len) ||
        !fixed)
        return false;
    major = HIWORD(fixed->dwFileVersionMS);
    minor = LOWORD(fixed->dwFileVersionMS);
    build = HIWORD(fixed->dwFileVersionLS);
    return true;
}

// The known 1.16.244 RVAs, in GameAddr order
static const std::uint32_t kKnownRva[kNumGameAddrs] = {
    0x872770, 0x131f684, 0x18f1fbd, 0x1a2da05, 0x872550, 0x320ef0, 0xcec8a0, 0x4b50ea0, 0x874990, 0x4b512e8, 0x877910,
    0x5FBF9A8, 0x8779B9, 0x1a3ec60, 0x4cb9248, 0x4cbc448, 0x4bd5a90, 0xcd2160, 0xce79d2, 0xce7a91, 0xce7a9c, 0xce7ade,
    0x34ed97, 0x34edc0, 0x4b69760};

static bool LogHas(const char* text) {
    FlushLog();
    FILE* f = _fsopen(g_logPath, "rb", _SH_DENYNO);   // shared: the log's writer thread keeps it open
    if (!f) return false;
    std::string all;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) all.append(buf, n);
    fclose(f);
    return all.find(text) != std::string::npos;
}

int main(int argc, char** argv) {
    char root[MAX_PATH];
    GetModuleFileNameA(nullptr, root, MAX_PATH);
    for (int up = 0; up < 3; ++up)
        if (char* slash = strrchr(root, '\\')) *slash = 0;
    snprintf(g_dir, sizeof(g_dir), "%s\\build\\tests", root);
    snprintf(g_logPath, sizeof(g_logPath), "%s\\addrlib_test.log", g_dir);
    FILE* lf = nullptr;
    if (fopen_s(&lf, g_logPath, "w") == 0 && lf) fclose(lf);
    // the game folder: the argument, else STARFIELD_DIR, else the first of the usual Steam folders that holds the exe
    static char envDir[MAX_PATH];
    const char* gameDir = nullptr;
    const DWORD envLen = GetEnvironmentVariableA("STARFIELD_DIR", envDir, MAX_PATH);
    if (argc > 1) gameDir = argv[1];
    else if (envLen > 0 && envLen < MAX_PATH) gameDir = envDir;
    else {
        static const char* const kUsual[] = {"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Starfield",
                                             "D:\\SteamLibrary\\steamapps\\common\\Starfield",
                                             "E:\\SteamLibrary\\steamapps\\common\\Starfield"};
        gameDir = kUsual[0];
        for (const char* dir : kUsual) {
            char probe[MAX_PATH];
            snprintf(probe, sizeof(probe), "%s\\Starfield.exe", dir);
            if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
                gameDir = dir;
                break;
            }
        }
    }
    char d[512];
    std::uint32_t ids[kNumGameAddrs], rvas[kNumGameAddrs];
    for (int i = 0; i < kNumGameAddrs; ++i) ids[i] = kAddrRef[i].id;

    // ================================================================ the plugin's SFSE declaration and the path
    {
        const SFSEPluginVersionData& v = SFSEPlugin_Version;
        // SFSE 0.2.21, PluginManager::checkPluginCompatibility: version independent = (signatures or Address Library
        // v2) and (no structs or the 1.14.70 layout); with Address Library (v1 or v2) it needs the running game's file
        const bool addressFree = v.addressIndependence & ((1u << 0) | (1u << 2));
        const bool layoutFree = v.structureIndependence & ((1u << 0) | (1u << 3));
        const bool needsDb = v.addressIndependence & ((1u << 1) | (1u << 2));
        snprintf(d, sizeof(d), "version %u, address bits 0x%X, layout bits 0x%X, list {0x%08X, 0x%X}", v.pluginVersion,
                 v.addressIndependence, v.structureIndependence, v.compatibleVersions[0], v.compatibleVersions[1]);
        Report("SFSE: loads on any game with its database", d,
               v.dataVersion == 1 && v.pluginVersion == 18 && addressFree && layoutFree && needsDb && !v.reservedBreaking &&
                   v.compatibleVersions[0] == kRuntime_1_16_244 && !v.compatibleVersions[1] && !strcmp(v.name, "TF Physics"));
        char p1[MAX_PATH], p2[MAX_PATH];
        AddressLibraryPath(kRuntime_1_16_244, p1, sizeof(p1));
        AddressLibraryPath(MakeExeVersion(1, 17, 9), p2, sizeof(p2));
        snprintf(d, sizeof(d), "%s, %s", p1, p2);
        Report("database path (as SFSE checks it)", d,
               !strcmp(p1, "Data\\SFSE\\Plugins\\versionlib-1-16-244-0.bin") &&
                   !strcmp(p2, "Data\\SFSE\\Plugins\\versionlib-1-17-9-0.bin"));
    }

    // ================================================================ lookups, and what fails
    Layout plain = MakeLayout(false, "fake_versionlib.bin");
    {
        char why[512] = {};
        const bool ok = ReadAddressLibrary(plain.db.c_str(), kRuntime_1_16_244, ids, kNumGameAddrs, rvas, why, sizeof(why));
        bool same = ok;
        for (int i = 0; i < kNumGameAddrs && ok; ++i) same &= rvas[i] == plain.rvas[ids[i]] && rvas[i] != 0;
        snprintf(d, sizeof(d), "%d IDs, e.g. ID %u -> RVA 0x%X", kNumGameAddrs, ids[0], rvas[0]);
        Report("lookups: the RVAs written", d, same);
    }
    struct Bad { const char* what; FakeDb db; std::uint32_t runtime; const char* why; bool missingFile; };
    std::vector<Bad> bad;
    {
        FakeDb good;
        good.count = kMaxId + 1;
        good.rvas = plain.rvas;
        Bad b{"missing file", good, kRuntime_1_16_244, "not found", true};
        bad.push_back(b);
        b = {"format 2 (the old library)", good, kRuntime_1_16_244, "not an Address Library database of format 5", false};
        b.db.format = 2;
        bad.push_back(b);
        b = {"another game version", good, kRuntime_1_16_244, "is for game 1.16.242, not 1.16.244", false};
        b.db.version[2] = 242;
        bad.push_back(b);
        b = {"32-bit pointers", good, kRuntime_1_16_244, "not an Address Library database of format 5", false};
        b.db.pointerSize = 4;
        bad.push_back(b);
        b = {"cut short", good, kRuntime_1_16_244, "is cut short", false};
        b.db.rvas.resize(1000);
        bad.push_back(b);
        b = {"an ID past the end", good, kRuntime_1_16_244, "ID 936960 is not in", false};
        b.db.count = kMaxId;
        b.db.rvas.resize(kMaxId);
        bad.push_back(b);
        b = {"an ID with no address", good, kRuntime_1_16_244, "ID 409163 has no address", false};
        b.db.rvas[409163] = 0;
        bad.push_back(b);
        b = {"running another game version", good, MakeExeVersion(1, 16, 250), "is for game 1.16.244, not 1.16.250", false};
        bad.push_back(b);
    }
    for (const Bad& b : bad) {
        const std::string path = b.missingFile ? std::string(g_dir) + "\\no_such_versionlib.bin" : WriteDb("bad_versionlib.bin", b.db);
        char why[512] = {};
        const bool read = ReadAddressLibrary(path.c_str(), b.runtime, ids, kNumGameAddrs, rvas, why, sizeof(why));
        ClearResolved();
        g_base = reinterpret_cast<std::uintptr_t>(plain.image);
        const bool resolved = ResolveAddresses(path.c_str(), b.runtime, plain.size);
        std::string shown = why;   // without the folder
        for (size_t at; (at = shown.find(std::string(g_dir) + "\\")) != std::string::npos;) shown.erase(at, strlen(g_dir) + 1);
        snprintf(d, sizeof(d), "\"%s\"", shown.c_str());
        Report((std::string("fails: ") + b.what).c_str(), d,
               !read && !resolved && strstr(why, b.why) && NoneResolved());
    }
    {
        ClearResolved();
        g_base = reinterpret_cast<std::uintptr_t>(plain.image);
        const bool resolved = ResolveAddresses(plain.db.c_str(), kRuntime_1_16_244, plain.unreadable - 0x10000);
        Report("fails: an address outside the image", "the image cut short: IDs past its end", !resolved && NoneResolved() &&
               LogHas("is outside the game"));
        const bool missingLogged = LogHas("no_such_versionlib.bin not found; nothing patched (install or update Address Library");
        Report("log: why nothing was patched", "\"Address Library: ...not found; nothing patched (install or update...\"",
               missingLogged);
    }

    // ================================================================ the checks on the fake image
    auto resolve = [](const Layout& L) {
        ClearResolved();
        ResetSizeReading();
        g_siteActorOk[0] = g_siteActorOk[1] = g_siteActorOk[2] = false;
        g_base = reinterpret_cast<std::uintptr_t>(L.image);
        return ResolveAddresses(L.db.c_str(), kRuntime_1_16_244, L.size);
    };
    auto allChecks = [](const Layout& L, char* out, size_t cap) {
        bool where = true;
        for (int i = 0; i < kNumGameAddrs; ++i) where &= g_addr[i] == L.at[i];
        const bool installs = CheckGame();
        ResetSizeReading();
        CheckSizeReading();
        snprintf(out, cap, "addresses %s, CheckGame %d (paths B %d, C %d), size reading %d, triangle %d", where ? "right" : "WRONG",
                 installs, g_siteActorOk[1], g_siteActorOk[2], g_actorVt == g_addr[kActorVtbl] && g_actorVt, g_triangleOk);
        return where && installs && !g_siteActorOk[0] && g_siteActorOk[1] && g_siteActorOk[2] && g_actorVt == g_addr[kActorVtbl] &&
               g_playerVt == g_addr[kPlayerVtbl] && g_npcVt == g_addr[kNpcVtbl] && g_actorVt && g_triangleOk;
    };
    {
        snprintf(d, sizeof(d), "not resolved");
        const bool all = resolve(plain) && allChecks(plain, d, sizeof(d));
        Report("fake game: every check passes", d, all);
        Report("log: the start-up line", "\"Address Library: all 25 game addresses resolved through ...\"",
               LogHas("Address Library: all 25 game addresses resolved through ") && kNumGameAddrs == 25);
    }
    Layout moved = MakeLayout(true, "fake_versionlib_moved.bin");
    {
        snprintf(d, sizeof(d), "not resolved");
        const bool all = resolve(moved) && allChecks(moved, d, sizeof(d));
        Report("moved code: every check passes", d, all);
        // the operands' bytes differ between the two layouts: comparing them as bytes would have failed here
        std::int32_t a = 0, b = 0, c = 0, e = 0;
        memcpy(&a, reinterpret_cast<void*>(plain.at[kFrameTimeLoad] + 4), 4);
        memcpy(&b, reinterpret_cast<void*>(moved.at[kFrameTimeLoad] + 4), 4);
        memcpy(&c, reinterpret_cast<void*>(plain.at[kCallSiteC] + 1), 4);
        memcpy(&e, reinterpret_cast<void*>(moved.at[kCallSiteC] + 1), 4);
        snprintf(d, sizeof(d), "frame-time load %+d -> %+d, call C %+d -> %+d", a, b, c, e);
        Report("moved code: the operands changed", d, a != b && c != e);
    }

    // ================================================================ each check fails when what it checks changes
    resolve(plain);
    auto checkGame = [] { return CheckGame(); };
    {
        int n = 0, failed = 0;
        for (const FnCheck& c : kFnChecks)
            for (size_t k = 0; k < sizeof(c.bytes); ++k, ++n) failed += FailsWhenChanged(g_addr[c.at] + k, checkGame);
        snprintf(d, sizeof(d), "%d of %d one-byte changes fail CheckGame", failed, n);
        Report("changed: a function's first bytes", d, failed == n && n == 54);
    }
    {
        const bool pd = FailsWhenChanged(g_addr[kPoseDeformerVtbl] + 8, checkGame);
        const bool sb = FailsWhenChanged(g_addr[kSpringBoneVtbl] + 8, checkGame);
        Report("changed: a modifier class's update slot", pd && sb ? "PoseDeformer and SpringBone fail" : "missed", pd && sb);
        int failed = 0;
        for (int k = 0; k < 8; ++k) failed += FailsWhenChanged(g_addr[kFrameTimeLoad] + k, checkGame);
        snprintf(d, sizeof(d), "%d of 8 (4 opcode bytes, 4 operand bytes: it reads elsewhere)", failed);
        Report("changed: the frame-time load", d, failed == 8);
        failed = 0;
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 5; ++k) failed += FailsWhenChanged(g_addr[kCallSiteA + i] + k, checkGame);
        snprintf(d, sizeof(d), "%d of 15 (A, B, C: not a call, or a call elsewhere = another mod's hook)", failed);
        Report("changed: a call site", d, failed == 15);
    }
    {
        int n = 0, missed = 0;
        for (int s = 1; s < 3; ++s) {
            const SiteActor& sa = kSiteActor[s];
            const std::uintptr_t block = g_addr[kCallSiteA + s] - sa.blockLen;
            for (size_t k = 0; k < sa.blockLen; ++k, ++n) {
                auto* p = reinterpret_cast<std::uint8_t*>(block + k);
                *p ^= 0x5A;
                const bool installs = CheckGame();
                missed += !installs || g_siteActorOk[s];
                *p ^= 0x5A;
            }
        }
        const bool back = CheckGame() && g_siteActorOk[1] && g_siteActorOk[2];
        snprintf(d, sizeof(d), "%d one-byte changes: that path loses its character, the hooks still go in (%d missed)", n,
                 missed);
        Report("changed: the character's register blocks", d, !missed && n == 31 && back);
    }
    auto size = [] {
        ResetSizeReading();
        CheckSizeReading();
        return g_actorVt != 0;
    };
    {
        int n = 0, failed = 0;
        const GameAddr code[4] = {kMorphLea448, kMorphCapacity, kMorphEntries, kMorphWeight};
        const size_t len[4] = {7, 4, 4, 7};
        for (int c = 0; c < 4; ++c)
            for (size_t k = 0; k < len[c]; ++k, ++n) failed += FailsWhenChanged(g_addr[code[c]] + k, size);
        failed += FailsWhenChanged(g_addr[kNpcVtbl] + 8 * 17, size);   // TESNPC::Load not in slot 17
        ++n;
        for (GameAddr vt : {kActorVtbl, kPlayerVtbl, kNpcVtbl}) {
            std::uintptr_t col = 0;
            memcpy(&col, reinterpret_cast<void*>(g_addr[vt] - 8), 8);
            std::uint32_t td = 0;
            memcpy(&td, reinterpret_cast<void*>(col + 0x0C), 4);
            failed += FailsWhenChanged(col, size);                    // not a complete object locator
            failed += FailsWhenChanged(g_base + td + 0x10 + 6, size);  // another class name
            n += 2;
        }
        snprintf(d, sizeof(d), "%d of %d (the loader's 4 instructions, slot 17, the 3 classes' RTTI)", failed, n);
        Report("changed: what the size reading reads", d, failed == n && n == 29 && size());
    }
    {
        auto triangle = [] {
            ResetSizeReading();
            CheckSizeReading();
            return g_triangleOk || !g_actorVt;   // fails only if the triangle alone is turned away
        };
        int n = 0, failed = 0;
        for (int k = 0; k < 10; ++k, ++n) failed += FailsWhenChanged(g_addr[kTriWeightLoad] + k, triangle);
        for (int k = 0; k < 7; ++k, ++n) failed += FailsWhenChanged(g_addr[kTriNamesLea] + k, triangle);
        for (int i = 0; i < 3; ++i, ++n) {
            const char* name = nullptr;
            memcpy(&name, reinterpret_cast<void*>(g_addr[kTriNameTable] + 8 * i), 8);
            failed += FailsWhenChanged(reinterpret_cast<std::uintptr_t>(name) + 1, triangle);
        }
        snprintf(d, sizeof(d), "%d of %d (the load, the lea and where it points, the 3 names); the size reading stays", failed, n);
        Report("changed: what the triangle reading reads", d, failed == n && n == 20);
    }
    {
        // a database pointing the SpringBone vtable, then the modifier loop, at memory that can't be read
        FakeDb u;
        u.count = kMaxId + 1;
        u.rvas = plain.rvas;
        u.rvas[kAddrRef[kSpringBoneVtbl].id] = static_cast<std::uint32_t>(plain.unreadable);
        const std::string p1 = WriteDb("unreadable_versionlib.bin", u);
        ClearResolved();
        g_base = reinterpret_cast<std::uintptr_t>(plain.image);
        const bool r1 = ResolveAddresses(p1.c_str(), kRuntime_1_16_244, plain.size);
        const bool g1 = CheckGame();
        u.rvas = plain.rvas;
        u.rvas[kAddrRef[kModifierUpdate].id] = static_cast<std::uint32_t>(plain.unreadable + 0x100);
        const std::string p2 = WriteDb("unreadable_versionlib.bin", u);
        ClearResolved();
        const bool r2 = ResolveAddresses(p2.c_str(), kRuntime_1_16_244, plain.size);
        const bool g2 = CheckGame();
        snprintf(d, sizeof(d), "resolved %d/%d, CheckGame %d/%d, no crash", r1, r2, g1, g2);
        Report("unreadable address: the check fails", d, r1 && r2 && !g1 && !g2);
    }

    // ================================================================ the real game (read only)
    char exe[MAX_PATH];
    snprintf(exe, sizeof(exe), "%s\\Starfield.exe", gameDir);
    std::uint32_t major = 0, minor = 0, build = 0;
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES || !ExeVersion(exe, major, minor, build)) {
        printf("%-44s %s  SKIPPED\n", "real game", exe);
    } else {
        const std::uint32_t runtime = MakeExeVersion(major, minor, build);
        char rel[MAX_PATH], db[MAX_PATH];
        AddressLibraryPath(runtime, rel, sizeof(rel));
        snprintf(db, sizeof(db), "%s\\%s", gameDir, rel);
        char why[512] = {};
        const bool read = ReadAddressLibrary(db, runtime, ids, kNumGameAddrs, rvas, why, sizeof(why));
        snprintf(d, sizeof(d), "Starfield.exe %u.%u.%u, %s", major, minor, build, read ? strrchr(db, '\\') + 1 : why);
        Report("real database: every ID found", d, read);
        if (read && runtime == kRuntime_1_16_244) {
            int same = 0;
            for (int i = 0; i < kNumGameAddrs; ++i) same += rvas[i] + kAddrRef[i].off == kKnownRva[i];
            snprintf(d, sizeof(d), "%d of %d equal the known 1.16.244 RVAs", same, kNumGameAddrs);
            Report("real database: the known addresses", d, same == kNumGameAddrs);
            const bool other = !ReadAddressLibrary(db, MakeExeVersion(1, 16, 242), ids, kNumGameAddrs, rvas, why, sizeof(why)) &&
                               strstr(why, "is for game 1.16.244, not 1.16.242");
            Report("real database: refused for another game", "1.16.244's file while running 1.16.242", other);
        }
        for (std::uint32_t older : {242u, 236u}) {   // the same IDs in the two patches before 1.16.244
            const std::uint32_t rt = MakeExeVersion(1, 16, older);
            char orel[MAX_PATH], odb[MAX_PATH];
            AddressLibraryPath(rt, orel, sizeof(orel));
            snprintf(odb, sizeof(odb), "%s\\%s", gameDir, orel);
            if (GetFileAttributesA(odb) == INVALID_FILE_ATTRIBUTES || rt == runtime) continue;
            const bool ok = ReadAddressLibrary(odb, rt, ids, kNumGameAddrs, rvas, why, sizeof(why));
            snprintf(d, sizeof(d), "%s: every ID there, modifier loop at RVA 0x%X", strrchr(odb, '\\') + 1, ok ? rvas[0] : 0);
            Report((std::string("older database: 1.16.") + std::to_string(older)).c_str(), d, ok);
        }
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
        std::uint64_t imageSize = 0;
        char* img = read ? MapExe(exe, imageSize) : nullptr;
        QueryPerformanceCounter(&t1);
        if (img) {
            ClearResolved();
            ResetSizeReading();
            g_base = reinterpret_cast<std::uintptr_t>(img);
            const bool resolved = ResolveAddresses(db, runtime, imageSize);
            const bool installs = resolved && CheckGame();
            ResetSizeReading();
            if (resolved) CheckSizeReading();
            g_deadOk = false;
            if (resolved) CheckDeadReading();   // Actor::IsDead through slot 269 of both vtables
            g_proxyOk = false;
            if (resolved) ResolveWorldAddresses(db, runtime, imageSize);   // the proxy watch's classes
            snprintf(d, sizeof(d), "resolved %d, CheckGame %d (paths B %d, C %d), size reading %d, triangle %d, life state %d, "
                     "world proxies %d (writes %d, motion tables %d) (mapped in %.1f s)", resolved, installs, g_siteActorOk[1],
                     g_siteActorOk[2], g_actorVt != 0, g_triangleOk, g_deadOk, g_proxyOk, g_proxyWriteOk, g_proxyMotionOk,
                     static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(fq.QuadPart));
            Report("real Starfield.exe: every check passes", d,
                   installs && g_siteActorOk[1] && g_siteActorOk[2] && g_actorVt && g_triangleOk && g_deadOk && g_proxyOk &&
                       g_proxyWriteOk && g_proxyMotionOk);
            VirtualFree(img, 0, MEM_RELEASE);
        } else {
            Report("real Starfield.exe: every check passes", "could not read it", false);
        }
    }
    FlushLog();
    printf("%s\n", g_fails ? "SOME CASES FAILED" : "all cases passed");
    return g_fails;
}
