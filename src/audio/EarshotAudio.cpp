// EarshotAudio.cpp — façade implementation over the loader/bank layer
// (src/loaders/*) and the engine layer (src/engine/*, another lane).
//
// Lifecycle mirrors the AR init flow (re-earshot-features.md B.1):
//   Init     = stage 1 (initRwEvent.after): EarShot.ini MAIN + engine boot +
//              generic loaders (AR Main.cpp:3047-3051)
//   PostInit = stage 2 (initGameEvent.after): .earshot scan + INI
//              attenuation/pitch sections (AR Main.cpp:3053-3058)
//   Shutdown = shutdownGameEvent full teardown (AR Main.cpp:3381-3384)
//   ReloadFolders = debug-menu hot swap (AR Loaders.cpp:1213-1285)
#include "audio/EarshotAudio.h"

#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include <cctype>
#include <cfloat>
#include <cstdarg>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <eSurfaceType.h>   // TOTAL_NUM_SURFACE_TYPES (PSDK enums dir)

#include "CVector.h"   // plugin-sdk (complete type; fwd-declared in the façade header)

#include "loaders/EshIniReader.h"
#include "loaders/EshFolderScanner.h"
#include "loaders/EshSoundBank.h"
#include "engine/EshAudioEngine.h"
#include "engine/EshListener.h"
#include "engine/EshVoicePool.h"

namespace {

EshSoundBank s_bank;
std::string  s_modFolder;
std::string  s_iniPath;    // PLUGIN_PATH/EarShot.ini (AR Loaders.cpp:456)

// MAIN INI state (AR Loaders.cpp:458-467). Ambience intervals + stereo
// ambience volume are stored for the S7-S8 ambience lane; M1 consumes only
// the distances + logging keys.
uint32_t s_fireIntervalMin = 5000;
uint32_t s_fireIntervalMax = 10000;
uint32_t s_zoneIntervalMin = 5000;
uint32_t s_zoneIntervalMax = 10000;
float    s_stereoAmbienceVol = 0.3f;   // AR Loaders.cpp:467

// Voice pool size: vanilla submits a 3-8 CAESound bundle per shot
// (re-audio-map.md §1); M1 adds shoot+after (+distant/low_ammo/dryfire) per
// event across concurrent peds — 24 voices gives headroom.
constexpr unsigned kVoicePoolVoices = 24;

// ---------------------------------------------------------------------------
// Merge helpers — "overrides win": a caller field that differs from the
// EshSoundParams default replaces the class/entry value; otherwise the
// class/entry value applies. Pitch is multiplicative per the façade contract
// (overrides.pitch × class pitch).
// ---------------------------------------------------------------------------
float MergeAtt(float overrideVal, float paramDefault, float classVal) {
    return (overrideVal != paramDefault) ? overrideVal : classVal;
}

void ApplyAttenuation(EshSoundParams& p, const EshSoundParams& overrides,
                      const EshAttenuation& att) {
    p.maxDist       = MergeAtt(overrides.maxDist,       150.0f, att.maxDist);
    p.refDist       = MergeAtt(overrides.refDist,       3.0f,   att.refDist);
    p.rolloff       = MergeAtt(overrides.rolloff,       1.0f,   att.rolloffFactor);
    p.airAbsorption = MergeAtt(overrides.airAbsorption, 1.0f,   att.airAbsorption);
}

// ---------------------------------------------------------------------------
// INI passes (AR Loaders::InitializeIniFile, Loaders.cpp:454-570)
// ---------------------------------------------------------------------------
void ReadIniStage1() {
    EshIniReader ini(s_iniPath.c_str());
    // MAIN section (AR :458-467)
    bool logging = ini.ReadBool("MAIN", "Logging", false);
    unsigned long long maxBytes =
        static_cast<unsigned long long>(ini.ReadInt("MAIN", "Max bytes in log", 9000000));
    EshSoundBank::EshLogSetConfig(logging, maxBytes);

    s_fireIntervalMin = static_cast<uint32_t>(ini.ReadInt("MAIN", "Ambience interval min", 5000));
    s_fireIntervalMax = static_cast<uint32_t>(ini.ReadInt("MAIN", "Ambience interval max", 10000));
    s_zoneIntervalMin = static_cast<uint32_t>(ini.ReadInt("MAIN", "Zone ambience interval min", 5000));
    s_zoneIntervalMax = static_cast<uint32_t>(ini.ReadInt("MAIN", "Zone ambience interval max", 10000));
    EshSoundBank::s_distantGunshotDistance  = ini.ReadFloat("MAIN", "Distant gunshot distance", 50.0f);
    EshSoundBank::s_distantExplosionDistance = ini.ReadFloat("MAIN", "Distant explosion distance", 100.0f);
    s_stereoAmbienceVol = ini.ReadFloat("MAIN", "Stereo ambience volume", 0.3f);   // AR :467 (S7-S8 consumer)
}

// LoadAttenuation lambda port (AR Loaders.cpp:478-489): keyPrefix empty ->
// bare key names, else "<prefix>.<key>".
void LoadAttenuationIni(EshIniReader& ini, const char* section, const char* keyPrefix,
                        EshAttenuation& att, float defMax, float defRef,
                        float defRoll, float defAir) {
    std::string maxKey  = keyPrefix && *keyPrefix ? std::string(keyPrefix) + ".maxDist" : "maxDist";
    std::string refKey  = keyPrefix && *keyPrefix ? std::string(keyPrefix) + ".refDist" : "refDist";
    std::string rollKey = keyPrefix && *keyPrefix ? std::string(keyPrefix) + ".rolloffFactor" : "rolloffFactor";
    std::string airKey  = keyPrefix && *keyPrefix ? std::string(keyPrefix) + ".airAbsorption" : "airAbsorption";
    att.maxDist       = ini.ReadFloat(section, maxKey.c_str(), defMax);
    att.refDist       = ini.ReadFloat(section, refKey.c_str(), defRef);
    att.rolloffFactor = ini.ReadFloat(section, rollKey.c_str(), defRoll);
    att.airAbsorption = ini.ReadFloat(section, airKey.c_str(), defAir);
}

void ReadIniStage2() {
    EshIniReader ini(s_iniPath.c_str());

    // --- TANKCANNON (AR :491-492) ---
    {
        EshGenericEntry& e = s_bank.Generic(EshSoundBank::TankCannonKey());
        LoadAttenuationIni(ini, "TANKCANNON", "", e.att, 125.0f, 3.5f, 0.3f, 0.3f);
        float p;
        if (ini.ReadFloatOpt("TANKCANNON", "tankcannon.pitch", &p))
            e.pitch = p;
    }

    // --- GUNSHELLS (AR :523-524, :529-530) ---
    {
        EshGenericEntry& gun  = s_bank.Generic(EshSoundBank::GunshellKey("default", false));
        EshGenericEntry& sgun = s_bank.Generic(EshSoundBank::GunshellKey("default", true));
        LoadAttenuationIni(ini, "GUNSHELLS", "general_gunshell", gun.att,  FLT_MAX, 1.0f, 1.5f, 4.0f);
        LoadAttenuationIni(ini, "GUNSHELLS", "shotgun_shell",    sgun.att, FLT_MAX, 1.0f, 1.5f, 3.0f);
        float p;
        if (ini.ReadFloatOpt("GUNSHELLS", "gunshells.pitch", &p))      gun.pitch  = p;
        if (ini.ReadFloatOpt("GUNSHELLS", "shotgunshells.pitch", &p)) sgun.pitch = p;
        // Propagate the (per-category, not per-bucket — AR stores one shared
        // Attenuation) tuning to every bucket entry WITHOUT touching paths.
        for (const char* b : { "metal", "wood", "water", "dirt", "glass", "stone", "sand" }) {
            EshGenericEntry& eg  = s_bank.Generic(EshSoundBank::GunshellKey(b, false));
            eg.att = gun.att;
            eg.pitch = gun.pitch;
            EshGenericEntry& es  = s_bank.Generic(EshSoundBank::GunshellKey(b, true));
            es.att = sgun.att;
            es.pitch = sgun.pitch;
        }
    }

    // --- EXPLOSIONS + EXPLOSION_TYPE_0..11 (AR :533-555) ---
    for (int i = 0; i < EshSoundBank::kMaxExplosionTypes; i++) {
        std::string section = "EXPLOSION_TYPE_" + std::to_string(i);
        if (!ini.SectionExists(section.c_str())) {
            section = "EXPLOSIONS";   // AR :538-541 fallback
        }
        struct { const char* cls; const char* prefix; float dM, dR, dRo, dA; } defs[] = {
            { "explosion",  "explosion.main",      EshSoundBank::s_distantExplosionDistance, 10.0f, 0.7f, 0.6f },
            { "distant",    "explosion.distant",   4000.0f, 10.0f, 0.5f, 1.0f },
            { "debris",     "explosion.debris",    150.0f, 1.0f, 1.5f, 1.0f },
            { "underwater", "explosion.underwater", 4000.0f, 15.0f, 0.2f, 3.0f },
        };
        for (const auto& d : defs) {
            EshGenericEntry& e = s_bank.Generic(EshSoundBank::ExplosionKey(i, d.cls));
            LoadAttenuationIni(ini, section.c_str(), d.prefix, e.att, d.dM, d.dR, d.dRo, d.dA);
            std::string pitchKey = std::string(d.prefix) + ".pitch";
            float p;
            if (ini.ReadFloatOpt(section.c_str(), pitchKey.c_str(), &p))
                e.pitch = p;
        }
    }
    // Generic fallback set tuned from the EXPLOSIONS section (AR reads the
    // per-type sections with EXPLOSIONS as fallback; the generic buffers use
    // the same values — Loaders.cpp:543-546 with section=EXPLOSIONS).
    {
        struct { const char* cls; const char* prefix; float dM, dR, dRo, dA; } defs[] = {
            { "explosion",  "explosion.main",      EshSoundBank::s_distantExplosionDistance, 10.0f, 0.7f, 0.6f },
            { "distant",    "explosion.distant",   4000.0f, 10.0f, 0.5f, 1.0f },
            { "debris",     "explosion.debris",    150.0f, 1.0f, 1.5f, 1.0f },
            { "underwater", "explosion.underwater", 4000.0f, 15.0f, 0.2f, 3.0f },
        };
        for (const auto& d : defs) {
            EshGenericEntry& e = s_bank.Generic(EshSoundBank::ExplosionKey(-1, d.cls));
            LoadAttenuationIni(ini, "EXPLOSIONS", d.prefix, e.att, d.dM, d.dR, d.dRo, d.dA);
            std::string pitchKey = std::string(d.prefix) + ".pitch";
            float p;
            if (ini.ReadFloatOpt("EXPLOSIONS", pitchKey.c_str(), &p))
                e.pitch = p;
        }
    }

    // --- RICOCHET + RICOCHET_SURFACE_TYPE_0..N (AR :557-569) ---
    for (unsigned i = 0; i < TOTAL_NUM_SURFACE_TYPES; i++) {
        std::string section = "RICOCHET_SURFACE_TYPE_" + std::to_string(i);
        if (!ini.SectionExists(section.c_str()))
            section = "RICOCHET";   // AR :562-565 fallback
        EshAttenuation att;
        LoadAttenuationIni(ini, section.c_str(), "", att, 50.0f, 3.5f, 5.0f, 3.0f);   // AR :568
        float p;
        std::optional<float> pitch;
        if (ini.ReadFloatOpt(section.c_str(), "pitch", &p))   // AR :567
            pitch = p;
        s_bank.SetRicochetSurfaceTuning(i, att, pitch);
    }
}

// ---------------------------------------------------------------------------
// Scan + load orchestration
// ---------------------------------------------------------------------------
void LoadGenericFolders() {
    // M1 set (AR AudioManager.Initialize loads all categories at stage 1,
    // Main.cpp:3047-3051; S7-S8 categories intentionally absent).
    EshFolderScanner::LoadExplosions(s_modFolder.c_str(), s_bank);
    EshFolderScanner::LoadRicochet(s_modFolder.c_str(), s_bank);
    EshFolderScanner::LoadGunshells(s_modFolder.c_str(), s_bank);
    EshFolderScanner::LoadBulletWhizz(s_modFolder.c_str(), s_bank);
    EshFolderScanner::LoadTankCannon(s_modFolder.c_str(), s_bank);
}

// Play one resolved alternative through the voice pool.
bool PlayResolved(const std::string& path, const EshSoundParams& params, const CVector* pos) {
    bool ok = EshVoicePool_PlayFile(path.c_str(), params, pos);
    if (!ok)
        EshSoundBank::EshLog("PlayFile failed: %s", path.c_str());
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle (frozen façade contract)
// ---------------------------------------------------------------------------
bool EarshotAudio_Init() {
    // 0) Bank reset — sizes the per-surface ricochet arrays with B.8 defaults
    //    (required before ReadIniStage2's SetRicochetSurfaceTuning calls).
    s_bank.Clear();

    // 1) Log reset (AR AudioManager.cpp:50 ClearLogFile at init)
    EshSoundBank::EshLogClearFile();

    // 2) Paths: INI at PLUGIN_PATH/EarShot.ini; mod root per AR Main.cpp:3035-3036
    s_iniPath  = EshFolderScanner::PluginDirA() + "EarShot.ini";
    s_modFolder = EshFolderScanner::ResolveModFolder();

    // 3) MAIN INI (stage 1, AR Loaders.cpp:458-467) — enables logging first so
    //    the load report below is captured.
    ReadIniStage1();
    EshSoundBank::EshLog("Mod folder: %s", s_modFolder.c_str());

    // 4) Engine + voice pool boot
    if (!EshAudioEngine::Instance().Init()) {
        EshSoundBank::EshLog("EshAudioEngine init FAILED — custom sounds disabled");
        return false;
    }
    if (!EshVoicePool_Init(kVoicePoolVoices)) {
        EshSoundBank::EshLog("EshVoicePool init FAILED — custom sounds disabled");
        return false;
    }

    // 5) Generic category loaders (stage 1, AR Main.cpp:3047-3051)
    LoadGenericFolders();

    EshSoundBank::EshLog("EarshotAudio_Init complete (weapons registered at PostInit)");
    return true;
}

bool EarshotAudio_PostInit() {
    // 1) .earshot scan + weapon folder stems + marker tuning
    //    (AR RegisterAllWeapons, Main.cpp:3053-3058)
    int n = EshFolderScanner::ScanWeapons(s_modFolder.c_str(), s_bank);

    // 2) INI attenuation/pitch sections (AR InitializeIniFile(2))
    ReadIniStage2();

    EshSoundBank::EshLog("EarshotAudio_PostInit complete (%d weapons)", n);
    return true;
}

void EarshotAudio_Shutdown() {
    EshVoicePool_StopAll();
    EshVoicePool_Shutdown();
    s_bank.Clear();
    EshSoundBank::EshLog("EarshotAudio_Shutdown complete");
}

void EarshotAudio_OnFrame() {
    EshListener_Update();
    EshVoicePool_Tick();
}

void EarshotAudio_ReloadFolders() {
    // AR ReloadAudioFolders (Loaders.cpp:1213-1285): stop+delete sources ->
    // clear bank -> re-read INI (both stages) -> re-scan everything.
    EshVoicePool_StopAll();
    s_bank.Clear();
    ReadIniStage1();
    ReadIniStage2();   // tuning first, so loader-created entries keep it
    EshFolderScanner::ScanWeapons(s_modFolder.c_str(), s_bank);
    LoadGenericFolders();
    EshSoundBank::EshLog("EarshotAudio_ReloadFolders complete");
}

// ---------------------------------------------------------------------------
// Playback primitives (frozen façade contract)
// ---------------------------------------------------------------------------
bool EarshotAudio_PlayWeaponStem(unsigned weaponType, int modelId,
                                 const char* stemClass, const CVector& pos,
                                 const EshSoundParams& overrides) {
    const EshWeaponEntry* e = s_bank.FindWeapon(weaponType, modelId);   // two-tier
    if (!e)
        return false;

    std::string cls = stemClass ? stemClass : "";
    for (char& c : cls)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    auto it = e->stems.find(cls);
    if (it == e->stems.end() || it->second.empty())
        return false;

    const std::string* path = EshSoundBank::PickRandom(it->second);
    if (!path)
        return false;

    EshSoundParams p = overrides;
    ApplyAttenuation(p, overrides, EshSoundBank::ClassAttenuation(*e, cls.c_str()));
    if (auto cp = EshSoundBank::ClassPitch(*e, cls.c_str()))
        p.pitch = overrides.pitch * (*cp);   // overrides.pitch × class pitch

    EshSoundBank::EshLog("PlayWeaponStem type=%u model=%d class=%s -> %s",
                         weaponType, modelId, cls.c_str(), path->c_str());
    return PlayResolved(*path, p, &pos);
}

bool EarshotAudio_PlayGenericStem(const char* category, int subKey,
                                  const char* stemClass, const CVector* pos,
                                  const EshSoundParams& overrides) {
    const EshGenericEntry* entry = nullptr;
    EshAttenuation att{};
    std::optional<float> pitch;
    bool haveTuning = false;
    bool force2D = false;
    std::string key;

    if (_stricmp(category, "explosions") == 0) {
        // subKey = explosion type id, -1 = generic fallback. Molotov
        // suppression of the generic fallback is the HOOK lane's decision
        // (AR Main.cpp:540,564 notAMolotov) — missing sets simply return false.
        const char* cls = (stemClass && *stemClass) ? stemClass : "explosion";
        if (subKey >= 0) {
            // Per-type paths first (AR chooseBuffers Main.cpp:550-572); the
            // type entry "exists" for fallback purposes only when it has paths
            // (AR Loaders.cpp:1843 stores non-empty vectors only).
            const EshGenericEntry* typeEntry = s_bank.FindGeneric(EshSoundBank::ExplosionKey(subKey, cls));
            if (typeEntry && !typeEntry->paths.empty()) {
                entry = typeEntry;
            } else {
                // Generic fallback buffers; TUNING still comes from the
                // per-type slot (AR Main.cpp:611-615 uses
                // gAttenuationSettings.explosion[lastExplosionType] even when
                // playing generic fallback buffers).
                entry = s_bank.FindGeneric(EshSoundBank::ExplosionKey(-1, cls));
            }
            if (typeEntry) {
                att = typeEntry->att;
                pitch = typeEntry->pitch;
                haveTuning = true;
            } else if (entry) {
                att = entry->att;
                pitch = entry->pitch;
                haveTuning = true;
            }
        } else {
            entry = s_bank.FindGeneric(EshSoundBank::ExplosionKey(-1, cls));
            if (entry) { att = entry->att; pitch = entry->pitch; haveTuning = true; }
        }
    } else if (_stricmp(category, "ricochet") == 0) {
        // subKey = game surface id; bucket resolved here (AR Main.cpp:397-440);
        // tuning per surface id (AR Main.cpp:460-469).
        const char* bucket = EshSoundBank::RicochetBucketForSurface(static_cast<unsigned>(subKey));
        key = EshSoundBank::RicochetKey(bucket);
        entry = s_bank.FindGeneric(key);
        // Resolved bucket empty -> "default" bucket fallback (AR Main.cpp:444-450)
        if ((!entry || entry->paths.empty()) && _stricmp(bucket, "default") != 0) {
            key = EshSoundBank::RicochetKey("default");
            entry = s_bank.FindGeneric(key);
        }
        att = s_bank.RicochetAttenuation(static_cast<unsigned>(subKey));
        pitch = s_bank.RicochetPitch(static_cast<unsigned>(subKey));
        haveTuning = true;
    } else if (_stricmp(category, "gunshells") == 0) {
        // subKey = game surface id; stemClass "gunshell" | "shotgunshell"
        // (AR AudioManager.cpp:644-721).
        const char* bucket = EshSoundBank::GunshellBucketForSurface(static_cast<unsigned>(subKey));
        bool shotgun = stemClass && _stricmp(stemClass, "shotgunshell") == 0;
        key = EshSoundBank::GunshellKey(bucket, shotgun);
        entry = s_bank.FindGeneric(key);
        // Resolved bucket empty -> "default" bucket fallback (AR :674-678)
        if ((!entry || entry->paths.empty()) && _stricmp(bucket, "default") != 0) {
            key = EshSoundBank::GunshellKey("default", shotgun);
            entry = s_bank.FindGeneric(key);
        }
        if (entry) { att = entry->att; pitch = entry->pitch; haveTuning = true; }
    } else if (_stricmp(category, "bullet_whizz") == 0) {
        // subKey = quadrant 0..3 (left_rear, left_front, right_rear,
        // right_front — AR Loaders.cpp:413-418 order). Stereo 2D.
        if (subKey < 0 || subKey > 3)
            return false;
        key = EshSoundBank::WhizzKey(subKey);
        entry = s_bank.FindGeneric(key);
        if (entry) { att = entry->att; pitch = entry->pitch; haveTuning = true; }
        force2D = true;
    } else if (_stricmp(category, "tankcannon") == 0) {
        key = EshSoundBank::TankCannonKey();
        entry = s_bank.FindGeneric(key);
        if (entry) { att = entry->att; pitch = entry->pitch; haveTuning = true; }
    } else {
        EshSoundBank::EshLog("PlayGenericStem: unknown category '%s'", category);
        return false;
    }

    if (!entry || entry->paths.empty())
        return false;

    const std::string* path = EshSoundBank::PickRandom(entry->paths);
    if (!path)
        return false;

    EshSoundParams p = overrides;
    if (haveTuning)
        ApplyAttenuation(p, overrides, att);
    if (pitch)
        p.pitch = overrides.pitch * (*pitch);
    if (force2D || pos == nullptr)
        p.is2D = true;   // pos==nullptr -> 2D playback (façade contract)

    EshSoundBank::EshLog("PlayGenericStem %s subKey=%d -> %s", category, subKey, path->c_str());
    return PlayResolved(*path, p, p.is2D ? nullptr : pos);
}

bool EarshotAudio_WeaponHasStems(unsigned weaponType, int modelId) {
    return s_bank.WeaponHasStems(weaponType, modelId);
}

// ---------------------------------------------------------------------------
// Debug menu state (B.9)
// ---------------------------------------------------------------------------
bool EarshotAudio_IsLogging() {
    return EshSoundBank::EshLogEnabled();
}

void EarshotAudio_SetLogging(bool on) {
    // Toggling off writes the farewell line and closes the file
    // (AR logging.cpp:18-29) — EshLog performs that when gated off.
    EshSoundBank::EshLogSetConfig(on, 0);
    if (!on)
        EshSoundBank::EshLog("%s", "toggle");
}
