// EshSoundBank.cpp — registry/lookup/pick implementation + the loader-lane
// file logger. Every ported behavior cites its AR source line.
#include "loaders/EshSoundBank.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cfloat>    // FLT_MAX
#include <random>
#include <algorithm>

#include <eSurfaceType.h>   // SURFACE_* enum values + TOTAL_NUM_SURFACE_TYPES (PSDK)

// ---------------------------------------------------------------------------
// SurfaceInfos_c predicates (game global @0xB79538; AR ourCommon.h:716-828).
// Pure id->bool thiscall queries — safe in play (hook) context.
// ---------------------------------------------------------------------------
namespace {

constexpr uintptr_t kSurfaceInfos = 0xB79538;   // AR ourCommon.h:716

bool SurfPred(unsigned addr, unsigned id) {
    return ((bool(__thiscall*)(void*, unsigned))addr)((void*)kSurfaceInfos, id);
}
bool IsAudioConcrete(unsigned id)  { return SurfPred(0x55EA30, id); }  // ourCommon.h:718
bool IsAudioGrass(unsigned id)     { return SurfPred(0x55EA50, id); }  // :724
bool IsAudioSand(unsigned id)      { return SurfPred(0x55EA70, id); }  // :730
bool IsAudioGravel(unsigned id)    { return SurfPred(0x55EA90, id); }  // :736
bool IsAudioWood(unsigned id)      { return SurfPred(0x55EAB0, id); }  // :742
bool IsAudioWater(unsigned id)     { return SurfPred(0x55EAD0, id); }  // :748
bool IsAudioMetal(unsigned id)     { return SurfPred(0x55EAF0, id); }  // :754
bool IsAudioLongGrass(unsigned id) { return SurfPred(0x55EB10, id); }  // :760
bool IsAudioTile(unsigned id)      { return SurfPred(0x55EB30, id); }  // :766
bool IsWater(unsigned id)          { return SurfPred(0x55E710, id); }  // :820
bool IsShallowWater(unsigned id)   { return SurfPred(0x55E730, id); }  // :826

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------
float EshSoundBank::s_distantGunshotDistance   = 50.0f;   // AR Loaders.cpp:465
float EshSoundBank::s_distantExplosionDistance = 100.0f;  // AR Loaders.cpp:466

bool              EshSoundBank::s_logging = false;
unsigned long long EshSoundBank::s_maxBytesInLog = 9000000;  // INI default (Loaders.cpp:459)
unsigned long long EshSoundBank::s_numBytesInLog = 0;
void*             EshSoundBank::s_logFile = nullptr;

// ---------------------------------------------------------------------------
// EshWeaponTuning — ctor defaults copied EXACTLY from AR AttenuationSet()
// (AudioManager.h:119-144). These seed the .earshot marker parse
// (AR Loaders.cpp:1035 `AttenuationSet aset = gAttenuationSettings;`).
// ---------------------------------------------------------------------------
EshWeaponTuning::EshWeaponTuning() {
    shoot   = EshAttenuation{ FLT_MAX, 1.5f, 1.0f, 2.0f };  // base: FLT_MAX/1.5/1.0/2.0 (:121-124)
    reload  = EshAttenuation{ FLT_MAX, 2.3f, 6.0f, 2.0f };  // :126-129
    after   = EshAttenuation{ FLT_MAX, 2.3f, 6.0f, 2.0f };  // :131-134
    vehicle = EshAttenuation{ 125.0f, 1.5f, 0.5f, 1.5f };   // :136-139
    melee   = EshAttenuation{ 3000.0f, 2.0f, 3.0f, 1.0f };  // quieter :141-144
    lowAmmo = EshAttenuation{ FLT_MAX, 4.5f, 1.0f, 2.0f };  // :233
    distant = EshAttenuation{ 500.0f, 2.0f, 1.0f, 2.0f };   // :234
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void EshSoundBank::Clear() {
    m_weapons.clear();
    m_generic.clear();
    // Ricochet per-surface arrays seeded with the B.8 RICOCHET defaults
    // (AR AttenuationSet ctor AudioManager.h:147-152) so play-time lookups
    // never see zeroed attenuation before the INI pass runs.
    m_ricochetAtt.assign(TOTAL_NUM_SURFACE_TYPES, EshAttenuation{ 50.0f, 3.5f, 5.0f, 3.0f });
    m_ricochetPitch.assign(TOTAL_NUM_SURFACE_TYPES, std::optional<float>{});
}

// ---------------------------------------------------------------------------
// Weapon registry
// ---------------------------------------------------------------------------
bool EshSoundBank::RegisterWeapon(unsigned weaponType, int modelId, std::string name, std::string folder) {
    uint64_t key = Key(weaponType, modelId);
    if (m_weapons.count(key))
        return false;   // AR emplace no-overwrite (Loaders.cpp:106-109)
    EshWeaponEntry& e = m_weapons[key];
    e.weaponType = weaponType;
    e.modelId    = modelId;
    e.name       = std::move(name);
    e.folder     = std::move(folder);
    return true;
}

EshWeaponEntry* EshSoundBank::GetRegistered(unsigned weaponType, int modelId) {
    auto it = m_weapons.find(Key(weaponType, modelId));
    return it != m_weapons.end() ? &it->second : nullptr;
}

void EshSoundBank::SetWeaponTuning(unsigned weaponType, int modelId, const EshWeaponTuning& t) {
    auto it = m_weapons.find(Key(weaponType, modelId));
    if (it != m_weapons.end())
        it->second.tuning = t;
}

const EshWeaponEntry* EshSoundBank::FindWeapon(unsigned weaponType, int modelId) const {
    // Two-tier (frozen façade contract): exact (type, modelId) first, then
    // (type, -1) ped-generic. AR's AUDIOCALL macro orders ped-generic first
    // (ourCommon.h:707-708) — the façade header specifies exact-first here.
    if (modelId >= 0) {
        auto it = m_weapons.find(Key(weaponType, modelId));
        if (it != m_weapons.end())
            return &it->second;
    }
    auto it = m_weapons.find(Key(weaponType, -1));
    return it != m_weapons.end() ? &it->second : nullptr;
}

bool EshSoundBank::WeaponHasStems(unsigned weaponType, int modelId) const {
    const EshWeaponEntry* e = FindWeapon(weaponType, modelId);
    if (!e)
        return false;
    for (const auto& kv : e->stems) {
        if (!kv.second.empty())
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Generic storage
// ---------------------------------------------------------------------------
EshGenericEntry& EshSoundBank::Generic(const std::string& key) {
    return m_generic[key];   // create-on-demand; existing tuning untouched
}

const EshGenericEntry* EshSoundBank::FindGeneric(const std::string& key) const {
    auto it = m_generic.find(key);
    return it != m_generic.end() ? &it->second : nullptr;
}

std::string EshSoundBank::ExplosionKey(int typeId, const char* stemClass) {
    if (typeId < 0)
        return std::string("explosion/generic/") + ToLower(stemClass);
    return "explosion/" + std::to_string(typeId) + "/" + ToLower(stemClass);
}
std::string EshSoundBank::RicochetKey(const char* bucket) {
    return std::string("ricochet/") + ToLower(bucket);
}
std::string EshSoundBank::GunshellKey(const char* bucket, bool shotgun) {
    return std::string(shotgun ? "shotgunshell/" : "gunshell/") + ToLower(bucket);
}
std::string EshSoundBank::WhizzKey(int quadrant) {
    return "whizz/" + std::to_string(quadrant);
}
const char* EshSoundBank::TankCannonKey() {
    return "tankcannon";
}

// ---------------------------------------------------------------------------
// Surface-id -> bucket resolvers (play-time; call the game's SurfaceInfos_c)
// ---------------------------------------------------------------------------
const char* EshSoundBank::RicochetBucketForSurface(unsigned surfaceId) {
    // Port of AR HookedCAudioEngine__ReportBulletHit resolution
    // (Main.cpp:397-440). Order matters — do not reorder.
    switch (surfaceId) {
    case SURFACE_PED:                                // Main.cpp:401
    case SURFACE_GORE:                               // :402
        return "flesh";
    case SURFACE_GLASS:                              // :406
    case SURFACE_GLASS_WINDOWS_LARGE:                // :407
    case SURFACE_GLASS_WINDOWS_SMALL:                // :408
    case SURFACE_UNBREAKABLE_GLASS:                  // :409
        return "glass";
    default:
        break;
    }
    // Main.cpp:418-421 — veh+gravel->metal special case NOT ported (needs the
    // victim entity; the frozen façade signature has no channel for it).
    if (IsAudioMetal(surfaceId))                                            return "metal";   // :420
    if (IsAudioWater(surfaceId) || IsWater(surfaceId) || IsShallowWater(surfaceId)) return "water"; // :423
    if (IsAudioWood(surfaceId))                                             return "wood";    // :426
    if (IsAudioSand(surfaceId))                                             return "sand";    // :429
    if (IsAudioConcrete(surfaceId))                                         return "stone";   // :432
    if (IsAudioGrass(surfaceId) || IsAudioLongGrass(surfaceId) || IsAudioGravel(surfaceId))
                                                                            return "dirt";    // :435-437
    return "default";   // Main.cpp:397 initial value
}

const char* EshSoundBank::GunshellBucketForSurface(unsigned surfaceId) {
    // Port of AR CAudioManager::PlayGunshellSound resolution
    // (AudioManager.cpp:645-661).
    switch (surfaceId) {
    case SURFACE_PED:                                // :647
    case SURFACE_GORE:                               // :648
        return "flesh";
    case SURFACE_GLASS:                              // :649
    case SURFACE_GLASS_WINDOWS_LARGE:                // :650
        return "glass";
    default:
        break;
    }
    if (IsAudioGrass(surfaceId))   return "grass";     // :652
    if (IsAudioWood(surfaceId))    return "wood";      // :653
    if (IsAudioMetal(surfaceId))   return "metal";     // :654
    if (IsAudioSand(surfaceId))    return "sand";      // :655
    if (IsAudioGravel(surfaceId))  return "dirt";      // :656
    if (IsAudioConcrete(surfaceId))return "pavement";  // :657
    if (IsAudioWater(surfaceId) || IsWater(surfaceId) || IsShallowWater(surfaceId))
                                   return "water";     // :658
    if (IsAudioTile(surfaceId))    return "tile";      // :659
    return "default";                                  // :645
}

// ---------------------------------------------------------------------------
// Per-surface ricochet tuning
// ---------------------------------------------------------------------------
void EshSoundBank::SetRicochetSurfaceTuning(unsigned surfaceId, const EshAttenuation& att, std::optional<float> pitch) {
    if (surfaceId < m_ricochetAtt.size()) {
        m_ricochetAtt[surfaceId]   = att;
        m_ricochetPitch[surfaceId] = std::move(pitch);
    }
}

const EshAttenuation& EshSoundBank::RicochetAttenuation(unsigned surfaceId) const {
    static const EshAttenuation kFallback{ 50.0f, 3.5f, 5.0f, 3.0f };  // B.8 RICOCHET defaults
    return surfaceId < m_ricochetAtt.size() ? m_ricochetAtt[surfaceId] : kFallback;
}

const std::optional<float>& EshSoundBank::RicochetPitch(unsigned surfaceId) const {
    static const std::optional<float> kNone;
    return surfaceId < m_ricochetPitch.size() ? m_ricochetPitch[surfaceId] : kNone;
}

// ---------------------------------------------------------------------------
// Random pick — AR RandomIntegers (ourCommon.h:881-901): static mt19937
// seeded from random_device, uniform_int_distribution over the set size.
// ---------------------------------------------------------------------------
const std::string* EshSoundBank::PickRandom(const std::vector<std::string>& alternatives) {
    if (alternatives.empty())
        return nullptr;
    static std::mt19937 generator(std::random_device{}());   // ourCommon.h:890-891
    std::uniform_int_distribution<size_t> dist(0, alternatives.size() - 1);
    return &alternatives[dist(generator)];
}

// ---------------------------------------------------------------------------
// Stem class -> tuning slot (AR AudioPlay, AudioManager.cpp:790-822)
// ---------------------------------------------------------------------------
const EshAttenuation& EshSoundBank::ClassAttenuation(const EshWeaponEntry& e, const char* stemClass) {
    const EshWeaponTuning& t = e.tuning;
    if (_stricmp(stemClass, "distant") == 0)  return t.distant;   // :794-797
    if (_stricmp(stemClass, "reload") == 0 ||
        _stricmp(stemClass, "reload_one") == 0 ||
        _stricmp(stemClass, "reload_two") == 0)                return t.reload;  // Reloads :740-741, :802-805
    if (_stricmp(stemClass, "after") == 0)    return t.after;     // :806-809
    if (_stricmp(stemClass, "low_ammo") == 0) return t.lowAmmo;   // :814-817
    // AR NeedsToBeQuieter list (AudioManager.cpp:743-752): hit, swing, stomp,
    // martial_kick, martial_punch (+indexed variants). hitmetal/hitwood are
    // NOT in it and fall through to base — ported faithfully.
    if (_stricmp(stemClass, "hit") == 0 ||
        _stricmp(stemClass, "swing") == 0 ||
        _stricmp(stemClass, "stomp") == 0 ||
        _stricmp(stemClass, "martial_punch") == 0 ||
        _stricmp(stemClass, "martial_kick") == 0)              return t.melee;   // :810-813
    if (_stricmp(stemClass, "projfire") == 0 && e.modelId >= 0)
        return t.vehicle;   // AR: vehicle entity -> vehicle slot (:798-801); projfire is only played on vehicles (Main.cpp:1249)
    return t.shoot;   // base: shoot, dryfire, hitmetal, hitwood, ped projfire (:818-822)
}

std::optional<float> EshSoundBank::ClassPitch(const EshWeaponEntry& e, const char* stemClass) {
    const EshWeaponTuning& t = e.tuning;
    if (_stricmp(stemClass, "distant") == 0)  return t.pitchDistant;
    if (_stricmp(stemClass, "reload") == 0 ||
        _stricmp(stemClass, "reload_one") == 0 ||
        _stricmp(stemClass, "reload_two") == 0)                return t.pitchReload;
    if (_stricmp(stemClass, "after") == 0)    return t.pitchAfter;
    if (_stricmp(stemClass, "low_ammo") == 0) return t.pitchLowAmmo;
    if (_stricmp(stemClass, "hit") == 0 ||
        _stricmp(stemClass, "swing") == 0 ||
        _stricmp(stemClass, "stomp") == 0 ||
        _stricmp(stemClass, "martial_punch") == 0 ||
        _stricmp(stemClass, "martial_kick") == 0)              return t.pitchMelee;
    if (_stricmp(stemClass, "projfire") == 0 && e.modelId >= 0) return t.pitchVehicle;
    return t.pitchShoot;
}

// ---------------------------------------------------------------------------
// File logger — port of AR logging.cpp (B.10), renamed to EarshotReminid.log
// so a co-installed EarShotOpenAL's EarShotOpenAL.log is never clobbered.
// Gated by the Logging bool; byte counter vs "Max bytes in log"; self-truncating.
// ---------------------------------------------------------------------------
namespace {
const char* LogFilePath() {
    // PLUGIN_PATH/EarshotReminid.log — plugin dir = dir of this ASI
    // (plugin-sdk Paths.cpp:13-38 semantics, reimplemented because premake5.lua
    // does not compile shared/extensions/Paths.cpp).
    static char path[MAX_PATH];
    static bool init = false;
    if (!init) {
        init = true;
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
            else modPath[0] = '\0';
            _snprintf(path, MAX_PATH, "%sEarshotReminid.log", modPath);
            path[MAX_PATH - 1] = '\0';
        }
    }
    return path;
}
} // namespace

// Status-panel accessor: the exact path EshLog()/EshLog_WriteLine() write to.
// Thin forwarder so the debug menu never re-derives (and can never drift from)
// the logger's own path.
const char* EshSoundBank::EshLogFilePath() {
    return LogFilePath();
}

void EshSoundBank::EshLog(const char* fmt, ...) {
    FILE* f = static_cast<FILE*>(s_logFile);

    if (!s_logging) {
        // Logging disabled: write a farewell line and close for good
        // (AR logging.cpp:18-29).
        if (f) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f, "[%02d/%02d/%d %02d:%02d:%02d] [LOG]: Logging is disabled. Closing log file...\n",
                    st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
            fclose(f);
            s_logFile = nullptr;
        }
        return;
    }

    if (!f) {
        f = fopen(LogFilePath(), "a");   // AR logging.cpp:11
        if (!f)
            return;   // never block hook context (C.2 #2 — no dialogs)
        s_logFile = f;
    }

    va_list args;
    va_start(args, fmt);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d/%02d/%d %02d:%02d:%02d] ",
            st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
    s_numBytesInLog += static_cast<unsigned long long>(vfprintf(f, fmt, args)) + 2;
    va_end(args);

    // Truncate the log when over budget (AR logging.cpp:57-59)
    if (s_numBytesInLog >= s_maxBytesInLog)
        EshLogClearFile();

    // freopen keeps the FILE* object valid, but the not-open-before branch of
    // EshLogClearFile swaps in a fresh handle — re-read it.
    f = static_cast<FILE*>(s_logFile);
    if (f) {
        fprintf(f, "\n");
        fflush(f);   // AR logging.cpp:62
    }
}

void EshSoundBank::EshLogClearFile() {
    // AR logging.cpp:77-101: reset the byte counter, (re)open for truncation.
    s_numBytesInLog = 0;
    FILE* f = static_cast<FILE*>(s_logFile);
    if (!f) {
        f = fopen(LogFilePath(), "w");
        if (!f)
            return;
    } else {
        f = freopen(LogFilePath(), "w", f);
        if (!f)
            return;
    }
    s_logFile = f;
}

void EshSoundBank::EshLogSetConfig(bool enabled, unsigned long long maxBytes) {
    s_logging = enabled;
    if (maxBytes > 0)
        s_maxBytesInLog = maxBytes;
}

void EshSoundBank::EshLog_WriteLine(const char* line) {
    // Debug-lane writer (src/debug/EshLog.h): one pre-formatted line + '\n',
    // no AR date prefix (the caller carries its own [HH:MM:SS.mmm]). Same
    // Logging gate + byte cap as EshLog; the farewell line stays EshLog's job.
    if (!s_logging || !line)
        return;
    FILE* f = static_cast<FILE*>(s_logFile);
    if (!f) {
        f = fopen(LogFilePath(), "a");
        if (!f)
            return;   // never block the caller (C.2 #2 — no dialogs)
        s_logFile = f;
    }
    s_numBytesInLog += static_cast<unsigned long long>(fprintf(f, "%s\n", line));
    if (s_numBytesInLog >= s_maxBytesInLog) {
        EshLogClearFile();
        return;
    }
    fflush(f);
}

void EshSoundBank::EshLogRotateFile() {
    // Boot rotation (skygfx diagnostics.cpp:373-390): move the previous
    // boot's log to <name>.prev.log BEFORE EshLogClearFile truncates, so a
    // crash's evidence survives the next launch. MoveFileEx replaces the
    // older rotation (stays one boot behind); CopyFile is the locked-reader
    // fallback. Failure is harmless — an absent source means nothing to keep.
    const char* src = LogFilePath();
    if (!src[0])
        return;
    char dst[MAX_PATH];
    _snprintf(dst, MAX_PATH, "%s", src);
    dst[MAX_PATH - 1] = '\0';
    // "<dir>\EarshotReminid.log" -> "<dir>\EarshotReminid.prev.log"; strip
    // only the FILENAME's extension (dots in the directory must survive).
    char* slash = strrchr(dst, '\\');
    if (!slash) slash = strrchr(dst, '/');
    char* dot = strrchr(dst, '.');
    if (dot && (!slash || dot > slash))
        *dot = '\0';
    strncat(dst, ".prev.log", MAX_PATH - strlen(dst) - 1);
    if (!MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING))
        CopyFileA(src, dst, FALSE);
}
