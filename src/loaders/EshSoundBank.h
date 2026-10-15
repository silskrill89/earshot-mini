// EshSoundBank.h — loaded-sound registry, two-tier lookup, random alternative
// pick, attenuation sets. Data owner of the loader lane; the filesystem
// walking lives in EshFolderScanner, the lifecycle in src/audio/EarshotAudio.cpp.
//
// Ported from the AR (EarShotOpenAL) reference:
//   * weapon registry key (eWeaponType, eModelID) — registeredweapons map
//     (Loaders.h:63, registerWeapon Loaders.h:65-117)
//   * two-tier lookup — the façade contract (exact (type, modelId) first, then
//     (type, -1) ped-generic). NOTE: AR's AUDIOCALL macro tries ped-generic
//     FIRST (ourCommon.h:707-708); the frozen façade header specifies
//     exact-first, which is implemented here.
//   * random alternative pick — RandomIntegers mt19937 (ourCommon.h:881-901)
//   * per-class attenuation — AR AudioPlay slot selection (AudioManager.cpp:790-822)
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include "audio/EshSoundParams.h"

// AR Attenuation (AudioManager.h:71-77)
struct EshAttenuation {
    float maxDist       = 0.0f;
    float refDist       = 0.0f;
    float rolloffFactor = 0.0f;
    float airAbsorption = 0.0f;
};

// Per-weapon tuning parsed from the .earshot marker file. Slots mirror AR's
// AttenuationSet weapon members (AudioManager.h:79-87) + Pitch weapon members
// (AudioManager.h:31-39); ctor defaults = AttenuationSet() ctor values
// (AudioManager.h:119-144) — see EshSoundBank.cpp.
struct EshWeaponTuning {
    EshAttenuation shoot;     // AR "base"
    EshAttenuation reload;
    EshAttenuation after;
    EshAttenuation lowAmmo;
    EshAttenuation distant;
    EshAttenuation vehicle;
    EshAttenuation melee;     // AR "quieter"
    std::optional<float> pitchShoot, pitchReload, pitchAfter, pitchLowAmmo,
                         pitchDistant, pitchVehicle, pitchMelee;

    EshWeaponTuning();
};

// One registered weapon: the .earshot marker + its stem folder.
struct EshWeaponEntry {
    unsigned    weaponType = 0;
    int         modelId    = -1;    // -1 = ped-generic (AR MODELUNDEFINED, ourCommon.h:709)
    std::string name;                // .earshot stem (logging)
    std::string folder;              // folder holding the stem files
    // stem class (lowercase, e.g. "shoot") -> alternative file paths
    // (stem0..stem9 then unindexed fallback — AudioManager.cpp:1668-1709)
    std::unordered_map<std::string, std::vector<std::string>> stems;
    EshWeaponTuning tuning;
};

// One generic-category stem set (explosion class, ricochet bucket, gunshell
// bucket, whizz quadrant, tank cannon). Paths + the INI tuning that governs
// them (B.8 defaults — see EshSoundBank.cpp for the exact table).
struct EshGenericEntry {
    std::vector<std::string> paths;
    EshAttenuation att{};
    std::optional<float> pitch;
    bool is2D = false;               // bullet whizz (stereo 2D, B.11)
};

class EshSoundBank {
public:
    static constexpr int kMaxSoundAlternatives = 10;  // AR AudioManager.h:15
    static constexpr int kMaxExplosionTypes    = 12;  // AR AudioManager.h:17

    // MAIN INI distances (AR Loaders.cpp:465-466; read by the façade, consumed
    // by the hook lane for the distant-shot/explosion thresholds).
    static float s_distantGunshotDistance;    // default 50.0f
    static float s_distantExplosionDistance;  // default 100.0f

    void Clear();

    // --- weapon registry (key = (weaponType, modelId)) ---
    // false = key already registered (AR emplace no-overwrite, Loaders.cpp:106-109)
    bool RegisterWeapon(unsigned weaponType, int modelId, std::string name, std::string folder);
    // Marker-file tuning store (AR gWeaponAttenuations/gWeaponPitches keyed by
    // filename, Loaders.cpp:1197-1198 — keyed by registry entry here).
    void SetWeaponTuning(unsigned weaponType, int modelId, const EshWeaponTuning& t);
    EshWeaponEntry*       GetRegistered(unsigned weaponType, int modelId);
    // Two-tier: exact (type, modelId) first, then (type, -1) ped-generic.
    const EshWeaponEntry* FindWeapon(unsigned weaponType, int modelId) const;
    bool                  WeaponHasStems(unsigned weaponType, int modelId) const;
    size_t                WeaponCount() const { return m_weapons.size(); }

    // --- read-only iteration for the debug menu (src/ui/EshUiMenu.cpp) ---
    // The live bank instance is file-local to the façade TU (src/audio/
    // EarshotAudio.cpp:41 sits inside an anonymous namespace — dumpbin on the
    // object file shows `anonymous namespace'::s_bank), so the UI builds its
    // own probe bank through EshFolderScanner and iterates it via these.
    const std::unordered_map<uint64_t, EshWeaponEntry>& RegisteredWeapons() const { return m_weapons; }
    const std::unordered_map<std::string, EshGenericEntry>& GenericEntries() const { return m_generic; }

    // --- generic storage (flat map; typed key builders below) ---
    // Create-on-demand WITHOUT touching an existing entry's tuning (the
    // scanner fills paths; the façade's INI pass fills tuning).
    EshGenericEntry&       Generic(const std::string& key);
    const EshGenericEntry* FindGeneric(const std::string& key) const;

    static std::string ExplosionKey(int typeId /* -1 = generic fallback */, const char* stemClass);
    static std::string RicochetKey(const char* bucket);
    static std::string GunshellKey(const char* bucket, bool shotgun);
    static std::string WhizzKey(int quadrant /* 0..3 */);
    static const char* TankCannonKey();

    // --- surface-id -> folder bucket resolvers ---
    // Ricochet: AR Main.cpp:397-440 (ReportBulletHit). Gunshell: AR
    // AudioManager.cpp:645-661 (PlayGunshellSound). Both call the game's
    // SurfaceInfos_c predicates (global @0xB79538) — play-time only.
    // KNOWN GAP: AR's veh+gravel->metal special case (Main.cpp:418-421, C.2 #4)
    // needs the victim entity, which the frozen façade signature has no
    // channel for — not ported.
    static const char* RicochetBucketForSurface(unsigned surfaceId);
    static const char* GunshellBucketForSurface(unsigned surfaceId);

    // --- per-surface-id ricochet tuning (INI RICOCHET_SURFACE_TYPE_i, AR
    // gAttenuationSettings.ricochet[i] / gPitches.ricochet[i], consumed at
    // Main.cpp:460-469) ---
    void SetRicochetSurfaceTuning(unsigned surfaceId, const EshAttenuation& att, std::optional<float> pitch);
    const EshAttenuation&    RicochetAttenuation(unsigned surfaceId) const;
    const std::optional<float>& RicochetPitch(unsigned surfaceId) const;

    // --- play-time helpers ---
    // Random alternative pick (AR RandomIntegers, ourCommon.h:881-901: static
    // mt19937 seeded from random_device, uniform over the set size).
    static const std::string* PickRandom(const std::vector<std::string>& alternatives);

    // Stem class -> attenuation slot / pitch slot (AR AudioPlay
    // AudioManager.cpp:790-822 + NeedsToBeQuieter list :743-752).
    static const EshAttenuation& ClassAttenuation(const EshWeaponEntry& e, const char* stemClass);
    static std::optional<float>  ClassPitch(const EshWeaponEntry& e, const char* stemClass);

    // --- file logger (port of AR logging.cpp; lives in this lane because of
    // file-ownership bounds). Renamed to EarshotReminid.log so a co-installed
    // EarShotOpenAL's log is never clobbered (B.10). ---
    static void EshLog(const char* fmt, ...);
    static void EshLogClearFile();                       // AR logging.cpp:77-101
    static void EshLogSetConfig(bool enabled, unsigned long long maxBytes);
    static bool EshLogEnabled() { return s_logging; }
    // Debug-lane writer (src/debug/EshLog.h): append ONE pre-formatted line
    // (its own [HH:MM:SS.mmm] prefix — no AR date prefix) + '\n'. Same
    // Logging gate + byte cap as EshLog; no farewell line (EshLog owns that).
    static void EshLog_WriteLine(const char* line);
    // Rotate the current log to <name>.prev.log (MoveFileEx, CopyFile
    // fallback — skygfx diagnostics.cpp:373-390). Call ONCE from Init BEFORE
    // EshLogClearFile so the previous boot's evidence survives the truncate.
    static void EshLogRotateFile();
    // Exact path the file logger writes to (single source of truth — the
    // debug-menu Status panel must not re-derive it). Returns a static buffer.
    static const char* EshLogFilePath();

private:
    static uint64_t Key(unsigned weaponType, int modelId) {
        // high 32 = modelId (two's complement, -1 = ped-generic), low 32 = type
        return (static_cast<uint64_t>(static_cast<uint32_t>(modelId)) << 32)
             | static_cast<uint32_t>(weaponType);
    }

    std::unordered_map<uint64_t, EshWeaponEntry> m_weapons;
    std::unordered_map<std::string, EshGenericEntry> m_generic;
    // sized to TOTAL_NUM_SURFACE_TYPES in Clear() (eSurfaceType.h)
    std::vector<EshAttenuation>     m_ricochetAtt;
    std::vector<std::optional<float>> m_ricochetPitch;

    static bool  s_logging;
    static unsigned long long s_maxBytesInLog;
    static unsigned long long s_numBytesInLog;
    static void* s_logFile;          // FILE* (kept void* to keep the header light)
};
