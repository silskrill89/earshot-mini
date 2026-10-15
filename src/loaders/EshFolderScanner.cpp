// EshFolderScanner.cpp — filesystem walking + registration + marker parsing.
// Every ported behavior cites its AR source line (external/EarShotOpenAL/source).
#include "loaders/EshFolderScanner.h"
#include "loaders/EshSoundBank.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cfloat>
#include <charconv>   // std::from_chars (AR Loaders.cpp:1061)
#include <optional>
#include <string>
#include <vector>

#include <eWeaponType.h>   // eWeaponType / WEAPONTYPE_UNARMED (PSDK enums dir)

namespace {

// ---------------------------------------------------------------------------
// Small string/fs helpers (Win32 A-functions, MBCS)
// ---------------------------------------------------------------------------
std::string ToUpper(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Join with single backslashes (paths here are always MBCS char strings).
std::string Join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '\\' || a.back() == '/')
        return a + b;
    return a + "\\" + b;
}

std::string ParentDir(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool DirExists(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::string& path) {
    if (DirExists(path))
        return true;
    return CreateDirectoryA(path.c_str(), nullptr) != 0 || DirExists(path);
}

// AR extensions order (AudioManager.h:19): .wav .mp3 .flac .ogg
const char* const kExtensions[] = { ".wav", ".mp3", ".flac", ".ogg" };

// Resolve "<dir>/<stem>.<ext>" trying kExtensions in order — port of AR
// AudioStream::audioPath extension loop (AudioManager.cpp:1652-1664).
bool FindStemFile(const std::string& dir, const std::string& stem, std::string& out) {
    for (const char* ext : kExtensions) {
        std::string candidate = Join(dir, stem + ext);
        DWORD attr = GetFileAttributesA(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            out = candidate;
            return true;
        }
    }
    return false;
}

// Contiguous indexed scan (prefix0, prefix1, ...) + unindexed fallback ONLY
// when no indexed file exists — the pattern every AR generic loader uses
// (e.g. ricochet Loaders.cpp:1343-1372, gunshells :1394-1433, whizz :420-450,
// tank cannon :10-45, explosions :1798-1827).
bool LoadIndexedSet(const std::string& dir, const char* prefix, std::vector<std::string>& out) {
    int idx = 0;
    bool indexed = false;
    while (true) {
        std::string p;
        if (!FindStemFile(dir, std::string(prefix) + std::to_string(idx), p))
            break;
        indexed = true;
        out.push_back(std::move(p));
        ++idx;
    }
    if (!indexed) {
        std::string p;
        if (FindStemFile(dir, prefix, p))
            out.push_back(std::move(p));
    }
    return !out.empty();
}

// Recursive walk (FindFirstFileA; std::filesystem avoided per the MBCS
// char* rule). Callback(fullPath, stem, extensionWithDot, isDirectory).
template <typename Fn>
void RecursiveWalk(const std::string& dir, const Fn& cb) {
    std::string pattern = Join(dir, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        std::string full = Join(dir, fd.cFileName);
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir) {
            RecursiveWalk(full, cb);
        } else {
            std::string name(fd.cFileName);
            size_t dot = name.find_last_of('.');
            std::string stem = (dot == std::string::npos) ? name : name.substr(0, dot);
            std::string ext  = (dot == std::string::npos) ? std::string() : name.substr(dot);
            cb(full, stem, ext, false);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------------------
// Game thunks (plugin-sdk is headers-only — raw SA 1.0 US addresses)
// ---------------------------------------------------------------------------
// CWeaponInfo::FindWeaponType(char*) @0x743D10 (PSDK CWeaponInfo.cpp:31-34)
eWeaponType FindWeaponTypeGame(const std::string& upperName) {
    return ((eWeaponType(__cdecl*)(char*))0x743D10)(const_cast<char*>(upperName.c_str()));
}

// CModelInfo::GetModelInfo(const char*, int*) @0x4C5940 (PSDK CModelInfo.cpp:68-70)
void* GetModelInfoGame(const char* name, int* modelId) {
    return ((void* (__cdecl*)(const char*, int*))0x4C5940)(name, modelId);
}

// nameType with the HZ rule: type must be > WEAPONTYPE_UNARMED.
// AR's nameType accepts >= UNARMED (ourCommon.h:91 — always true, bug #9);
// HZ required > UNARMED (HZ.cpp:153-160). We rebuild with the HZ rule.
bool NameToWeaponType(const std::string& weaponName, eWeaponType& out) {
    out = FindWeaponTypeGame(ToUpper(weaponName));   // AR nameType uppercases (ourCommon.h:89)
    return out > WEAPONTYPE_UNARMED;
}

// ---------------------------------------------------------------------------
// .earshot marker tuning parse (AR Loaders.cpp:1029-1199)
// ---------------------------------------------------------------------------
// Loose key=value line reader — port of AR readOptFromLine (Loaders.cpp:1043-1064):
// substring key find, '=' after it, skip whitespace, parse float until
// whitespace / ';' / '#'.
std::optional<float> ReadOptFromLine(const std::string& line, const std::string& key) {
    size_t pos = line.find(key);
    if (pos == std::string::npos)
        return std::nullopt;
    size_t eq = line.find('=', pos + key.size());
    if (eq == std::string::npos)
        return std::nullopt;
    size_t i = eq + 1;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= line.size())
        return std::nullopt;
    size_t j = i;
    while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j])) &&
           line[j] != ';' && line[j] != '#')
        ++j;
    float value = 0.0f;
    auto res = std::from_chars(line.data() + i, line.data() + j, value);
    if (res.ec == std::errc())
        return value;
    return std::nullopt;
}

struct MarkerSlot {
    const char* prefix;          // .earshot key prefix (Loaders.cpp:1069-1074)
    EshAttenuation* att;         // EshWeaponTuning slot
    std::optional<float>* pitch; // .pitch slot
};

// M1 marker prefixes: the 7 weapon classes (B.8: shoot, reload, after,
// low_ammo, distant, vehicle, melee × maxDist/refDist/rolloffFactor/
// airAbsorption + .pitch each). AR's loop-weapon prefixes (minigun.*, chainsaw.*,
// fireExtinguisher.*, sprayCan.*) are S7 material — not parsed in M1.
void ParseMarkerLine(const std::string& line, EshWeaponTuning& t) {
    MarkerSlot slots[] = {
        { "shoot",    &t.shoot,   &t.pitchShoot   },
        { "reload",   &t.reload,  &t.pitchReload  },
        { "after",    &t.after,   &t.pitchAfter   },
        { "low_ammo", &t.lowAmmo, &t.pitchLowAmmo },
        { "distant",  &t.distant, &t.pitchDistant },
        { "vehicle",  &t.vehicle, &t.pitchVehicle },
        { "melee",    &t.melee,   &t.pitchMelee   },
    };
    for (const MarkerSlot& s : slots) {
        std::string prefix = s.prefix;
        if (auto v = ReadOptFromLine(line, prefix + ".maxDist"))       s.att->maxDist = *v;
        if (auto v = ReadOptFromLine(line, prefix + ".refDist"))       s.att->refDist = *v;
        if (auto v = ReadOptFromLine(line, prefix + ".rolloffFactor")) s.att->rolloffFactor = *v;
        if (auto v = ReadOptFromLine(line, prefix + ".airAbsorption")) s.att->airAbsorption = *v;
        if (auto v = ReadOptFromLine(line, prefix + ".pitch"))        *s.pitch = *v;
    }
}

EshWeaponTuning ReadMarkerTuning(const std::string& path) {
    // Defaults = AR `AttenuationSet aset = gAttenuationSettings;` (Loaders.cpp:1035)
    EshWeaponTuning t;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        EshSoundBank::EshLog("Failed to open marker file: %s", path.c_str());   // AR :1031
        return t;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        ParseMarkerLine(line, t);
    }
    fclose(f);
    return t;
}

// ---------------------------------------------------------------------------
// Weapon-folder stem scan (folder next to the .earshot)
// ---------------------------------------------------------------------------
// M1 stem classes (canonical list, re-earshot-features.md B.11). Loop stems
// (minigun_*, chainsaw_*, flamethrower_*, spraycan_*, extinguisher_*, camera_*,
// goggles_*) are S7 material — not scanned in M1.
const char* const kM1StemClasses[] = {
    "shoot", "after", "distant", "dryfire", "low_ammo",
    "reload", "reload_one", "reload_two",
    "hit", "hitmetal", "hitwood", "stomp", "swing",
    "martial_punch", "martial_kick", "projfire",
};

void ScanStemFolder(EshWeaponEntry* e) {
    if (!e)
        return;
    for (const char* stemClass : kM1StemClasses) {
        std::vector<std::string> alts;
        // Alternatives: stem0..stem9, contiguous from 0 — AR audioPlay
        // (AudioManager.cpp:1673-1683: first missing index breaks the loop).
        for (int i = 0; i < EshSoundBank::kMaxSoundAlternatives; ++i) {
            std::string p;
            if (FindStemFile(e->folder, std::string(stemClass) + std::to_string(i), p))
                alts.push_back(std::move(p));
            else
                break;
        }
        // Unindexed fallback ONLY when no indexed alternative exists
        // (AudioManager.cpp:1686-1695).
        if (alts.empty()) {
            std::string p;
            if (FindStemFile(e->folder, stemClass, p))
                alts.push_back(std::move(p));
        }
        if (!alts.empty())
            e->stems[stemClass] = std::move(alts);
    }
}

// ---------------------------------------------------------------------------
// One .earshot registration (AR registerWeapon, Loaders.h:65-117)
// ---------------------------------------------------------------------------
void RegisterOneEarshot(const std::string& earshotPath, const std::string& stem, EshSoundBank& bank, int& registered) {
    std::string weaponName = stem;
    int modelId = -1;   // AR eModelID modelid = MODELUNDEFINED (Loaders.h:74)

    if (weaponName.empty()) {
        EshSoundBank::EshLog("Failed to register weapon sound for file %s", earshotPath.c_str());  // AR :70
        return;
    }

    // Vehicle guns: "<WEAPONNAME> <modelDFF>" (AR Loaders.cpp:77-95)
    size_t sep = weaponName.find(' ');
    if (sep != std::string::npos) {
        std::string modelName = weaponName.substr(sep + 1);
        int modelfound = -1;
        void* info = GetModelInfoGame(modelName.c_str(), &modelfound);
        if (info && modelfound > -1) {   // AR: modelfound > MODELUNDEFINED (:81)
            modelId = modelfound;
        } else {
            EshSoundBank::EshLog("Skipped weapon '%s': model '%s' was not found",
                                 weaponName.c_str(), modelName.c_str());   // AR :85
            return;
        }
        weaponName = weaponName.substr(0, sep);   // keep only weapon part (:89)
    }

    // Resolve weapon type from name — HZ rule (> UNARMED, bug #9 fix)
    eWeaponType weaponType = WEAPONTYPE_UNARMED;
    if (!NameToWeaponType(weaponName, weaponType)) {
        EshSoundBank::EshLog("Skipped weapon '%s': unknown weapon name", weaponName.c_str());  // AR :100
        return;
    }

    if (!bank.RegisterWeapon(static_cast<unsigned>(weaponType), modelId, stem, ParentDir(earshotPath))) {
        EshSoundBank::EshLog("Weapon already registered type=%d(%s) model=%d -> skipping",
                             static_cast<int>(weaponType), weaponName.c_str(), modelId);   // AR :107
        return;
    }

    // Marker contents -> per-class attenuation/pitch (AR Loaders.cpp:1029-1199).
    // NOTE: AR parses the marker even for skipped/duplicate weapons and keys
    // the result by filename; we key by registry entry — equivalent for M1
    // (1:1 registration) and cheaper.
    bank.SetWeaponTuning(static_cast<unsigned>(weaponType), modelId, ReadMarkerTuning(earshotPath));

    // Weapon folder stem scan (folder next to the .earshot)
    ScanStemFolder(bank.GetRegistered(static_cast<unsigned>(weaponType), modelId));

    ++registered;
    EshSoundBank::EshLog("Registered weapon type=%d(%s) model=%d path=%s",
                         static_cast<int>(weaponType), weaponName.c_str(), modelId, earshotPath.c_str());  // AR :115-116
}

// ---------------------------------------------------------------------------
// Generic loader tuning defaults (B.8 — the INI-reader defaults that AR's
// stage-2 pass would set anyway; the façade overwrites them from EarShot.ini)
// ---------------------------------------------------------------------------
void SeedExplosionDefaults(EshSoundBank& bank) {
    struct ClassDef { const char* name; EshAttenuation att; };
    // AR Loaders.cpp:543-546 defaults (B.8): main distExp/10/0.7/0.6;
    // distant 4000/10/0.5/1.0; debris 150/1/1.5/1; underwater 4000/15/0.2/3.
    ClassDef defs[] = {
        { "explosion",  { EshSoundBank::s_distantExplosionDistance, 10.0f, 0.7f, 0.6f } },
        { "distant",    { 4000.0f, 10.0f, 0.5f, 1.0f } },
        { "debris",     { 150.0f, 1.0f, 1.5f, 1.0f } },
        { "underwater", { 4000.0f, 15.0f, 0.2f, 3.0f } },
    };
    for (const ClassDef& d : defs) {
        EshGenericEntry& e = bank.Generic(EshSoundBank::ExplosionKey(-1, d.name));
        if (e.att.maxDist == 0.0f && e.att.refDist == 0.0f)
            e.att = d.att;
    }
}

} // namespace

namespace EshFolderScanner {

// ---------------------------------------------------------------------------
// Path bases (plugin-sdk Paths.cpp:13-57 semantics, reimplemented)
// ---------------------------------------------------------------------------
std::string PluginDirA() {
    static std::string dir;
    if (dir.empty()) {
        HMODULE h = nullptr;
        static int dummy;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&dummy), &h)) {
            char modPath[MAX_PATH];
            GetModuleFileNameA(h, modPath, MAX_PATH);
            char* slp = strrchr(modPath, '\\');
            if (!slp) slp = strrchr(modPath, '/');
            if (slp) slp[1] = '\0';   // keep trailing separator (Paths.cpp:30-36)
            dir = modPath;
        }
    }
    return dir;
}

std::string GameDirA() {
    static std::string dir;
    if (dir.empty()) {
        char modPath[MAX_PATH];
        GetModuleFileNameA(nullptr, modPath, MAX_PATH);   // Paths.cpp:47-55
        char* slp = strrchr(modPath, '\\');
        if (!slp) slp = strrchr(modPath, '/');
        if (slp) slp[1] = '\0';
        dir = modPath;
    }
    return dir;
}

std::string ResolveModFolder() {
    // AR Main.cpp:3035-3036 + ourCommon.h:58-60:
    //   folderdata = GAME_PATH("") / "data" / "EarShot"
    //   foldermod  = PLUGIN_PATH("") / "EarShot"
    //   if exists(folderdata) foldermod = folderdata; else create foldermod
    std::string dataDir = Join(Join(GameDirA(), "data"), "EarShot");
    if (DirExists(dataDir))
        return dataDir;
    std::string pluginDir = Join(PluginDirA(), "EarShot");
    EnsureDir(pluginDir);   // AR :3036 create_directories
    return pluginDir;
}

// ---------------------------------------------------------------------------
// Stage 2: .earshot scan (AR RegisterAllWeapons, Loaders.cpp:1006-1210)
// ---------------------------------------------------------------------------
int ScanWeapons(const char* modFolder, EshSoundBank& bank) {
    int registered = 0;
    EshSoundBank::EshLog("File(s):");   // AR :1017
    RecursiveWalk(modFolder, [&](const std::string& full, const std::string& stem,
                                 const std::string& ext, bool /*isDir*/) {
        if (_stricmp(ext.c_str(), ".earshot") != 0)   // modextension (ourCommon.h:55)
            return;
        RegisterOneEarshot(full, stem, bank, registered);
    });
    if (registered == 0)
        EshSoundBank::EshLog("No file(s) found. Stopping work for this session.");   // AR :1205
    else
        EshSoundBank::EshLog("Total registered weapons: %d", registered);            // AR :1208-1209
    return registered;
}

// ---------------------------------------------------------------------------
// Generic loaders (M1 set)
// ---------------------------------------------------------------------------
void LoadExplosions(const char* modFolder, EshSoundBank& bank) {
    // AR LoadExplosionRelatedSounds (Loaders.cpp:1783-1910)
    const std::string expDir = Join(modFolder, "generic\\explosions");
    if (!DirExists(expDir)) {
        EshSoundBank::EshLog("Explosions folder wasn't found, '%s'", expDir.c_str());   // AR :1788
        return;
    }
    SeedExplosionDefaults(bank);

    // Per-explosion-type folders (AR :1830-1863): explosionTypes/<id>/ with
    // numeric directory names (stoi, non-numeric skipped).
    const std::string typesDir = Join(expDir, "explosionTypes");
    if (DirExists(typesDir)) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(Join(typesDir, "*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                    continue;
                const std::string name(fd.cFileName);
                int typeId = -1;
                auto res = std::from_chars(name.data(), name.data() + name.size(), typeId);
                if (res.ec != std::errc() || typeId < 0)
                    continue;   // AR stoi try/catch skip (:1836-1838)
                const std::string typeDir = Join(typesDir, name);
                struct { const char* cls; } classes[] = { { "explosion" }, { "debris" }, { "distant" }, { "underwater" } };
                for (const auto& c : classes) {
                    EshGenericEntry& e = bank.Generic(EshSoundBank::ExplosionKey(typeId, c.cls));
                    if (e.att.maxDist == 0.0f && e.att.refDist == 0.0f) {
                        // seed same defaults as the generic set (overwritten by INI stage 2)
                        if (strcmp(c.cls, "explosion") == 0)
                            e.att = { EshSoundBank::s_distantExplosionDistance, 10.0f, 0.7f, 0.6f };
                        else if (strcmp(c.cls, "distant") == 0)
                            e.att = { 4000.0f, 10.0f, 0.5f, 1.0f };
                        else if (strcmp(c.cls, "debris") == 0)
                            e.att = { 150.0f, 1.0f, 1.5f, 1.0f };
                        else
                            e.att = { 4000.0f, 15.0f, 0.2f, 3.0f };
                    }
                    LoadIndexedSet(typeDir, c.cls, e.paths);
                }
                EshSoundBank::EshLog("Loaded explosionType '%d' sounds from %s", typeId, typeDir.c_str());  // AR :1861
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }

    // Generic fallback (AR :1865-1909): ONE shared index across the 4 classes;
    // stop when NONE of the 4 was found at that index; unindexed fallback per
    // class only when nothing was indexed at all.
    struct GenClass { const char* cls; std::vector<std::string>* paths; };
    std::vector<std::string> explosion, debris, distant, underwater;
    GenClass gens[] = {
        { "explosion",  &explosion  },
        { "debris",     &debris     },
        { "distant",    &distant    },
        { "underwater", &underwater },
    };
    int idx = 0;
    bool anyIndexed = false;
    while (true) {
        bool loadedSomething = false;
        for (GenClass& g : gens) {
            std::string p;
            if (FindStemFile(expDir, std::string(g.cls) + std::to_string(idx), p)) {
                g.paths->push_back(std::move(p));
                loadedSomething = true;
                anyIndexed = true;
            }
        }
        if (!loadedSomething)
            break;
        ++idx;
    }
    if (!anyIndexed) {
        for (GenClass& g : gens) {
            std::string p;
            if (FindStemFile(expDir, g.cls, p))
                g.paths->push_back(std::move(p));
        }
    }
    for (GenClass& g : gens) {
        if (!g.paths->empty())
            bank.Generic(EshSoundBank::ExplosionKey(-1, g.cls)).paths = *g.paths;
    }
}

void LoadRicochet(const char* modFolder, EshSoundBank& bank) {
    // AR LoadRicochetSounds (Loaders.cpp:1329-1377): 9 surface folders.
    const std::string ricoDir = Join(modFolder, "generic\\ricochet");
    if (!DirExists(ricoDir)) {
        EshSoundBank::EshLog("Ricochet folder wasn't found, '%s'", ricoDir.c_str());   // AR :1333
        return;
    }
    const char* surfaces[] = { "default", "metal", "wood", "water", "dirt",
                               "glass", "stone", "sand", "flesh" };   // AR :1336-1338
    for (const char* surface : surfaces) {
        EshGenericEntry& e = bank.Generic(EshSoundBank::RicochetKey(surface));
        if (e.att.maxDist == 0.0f && e.att.refDist == 0.0f)
            e.att = { 50.0f, 3.5f, 5.0f, 3.0f };   // B.8 RICOCHET defaults (AR :568)
        if (LoadIndexedSet(Join(ricoDir, surface), "ricochet", e.paths))
            EshSoundBank::EshLog("Loaded %d ricochet sound(s) for surface: %s",
                                 static_cast<int>(e.paths.size()), surface);   // AR :1374
    }
}

void LoadGunshells(const char* modFolder, EshSoundBank& bank) {
    // AR LoadGunshellSounds (Loaders.cpp:1379-1454): 8 surface folders x
    // {gunshell, shotgunshell}.
    const std::string shellsDir = Join(modFolder, "generic\\gunshells");
    if (!DirExists(shellsDir)) {
        EshSoundBank::EshLog("Gunshells folder wasn't found, '%s'", shellsDir.c_str());   // AR :1384
        return;
    }
    const char* surfaces[] = { "default", "metal", "wood", "water", "dirt",
                               "glass", "stone", "sand" };   // AR :1388-1390
    for (const char* surface : surfaces) {
        const std::string surfaceDir = Join(shellsDir, surface);
        if (!DirExists(surfaceDir))
            continue;   // AR :1445-1449
        EshGenericEntry& gun = bank.Generic(EshSoundBank::GunshellKey(surface, false));
        if (gun.att.maxDist == 0.0f && gun.att.refDist == 0.0f)
            gun.att = { FLT_MAX, 1.0f, 1.5f, 4.0f };   // general_gunshell (AR :523)
        EshGenericEntry& sgun = bank.Generic(EshSoundBank::GunshellKey(surface, true));
        if (sgun.att.maxDist == 0.0f && sgun.att.refDist == 0.0f)
            sgun.att = { FLT_MAX, 1.0f, 1.5f, 3.0f };  // shotgun_shell (AR :524)
        LoadIndexedSet(surfaceDir, "gunshell", gun.paths);
        LoadIndexedSet(surfaceDir, "shotgunshell", sgun.paths);
        EshSoundBank::EshLog("Loaded %d gunshell + %d shotgunshell sound(s) from: %s",
                             static_cast<int>(gun.paths.size()),
                             static_cast<int>(sgun.paths.size()), surfaceDir.c_str());   // AR :1435-1438
    }
}

void LoadBulletWhizz(const char* modFolder, EshSoundBank& bank) {
    // AR LoadBulletWhizzSounds (Loaders.cpp:401-452): 4 quadrant buckets,
    // stereo 2D (B.11). Quadrant order = AR's entry order (:413-418).
    const std::string whizzDir = Join(modFolder, "generic\\bullet_whizz");
    if (!DirExists(whizzDir)) {
        EshSoundBank::EshLog("bullet whizz folder wasn't found, '%s'", whizzDir.c_str());   // AR :405
        return;
    }
    const char* quadrants[] = { "left_rear", "left_front", "right_rear", "right_front" };
    for (int q = 0; q < 4; ++q) {
        EshGenericEntry& e = bank.Generic(EshSoundBank::WhizzKey(q));
        e.is2D = true;   // stereo 2D — no world position (B.11 routing rule)
        if (e.att.maxDist == 0.0f && e.att.refDist == 0.0f)
            e.att = { 150.0f, 3.0f, 1.0f, 1.0f };   // EshSoundParams defaults (2D: unused)
        LoadIndexedSet(whizzDir, quadrants[q], e.paths);
    }
}

void LoadTankCannon(const char* modFolder, EshSoundBank& bank) {
    // AR LoadTankCannonSounds (Loaders.cpp:7-46): "Tank Cannon/cannon_fire[N]"
    // (folder name contains a space — hardcoded, AR :6).
    const std::string tankDir = Join(modFolder, "Tank Cannon");
    EshGenericEntry& e = bank.Generic(EshSoundBank::TankCannonKey());
    if (e.att.maxDist == 0.0f && e.att.refDist == 0.0f)
        e.att = { 125.0f, 3.5f, 0.3f, 0.3f };   // B.8 TANKCANNON defaults (AR :491)
    if (!DirExists(tankDir))
        return;   // AR probes files directly; a missing folder just yields no files
    LoadIndexedSet(tankDir, "cannon_fire", e.paths);
}

} // namespace EshFolderScanner
