// EshHooks.cpp — Lane C (hooks + lifecycle), M1 hook set.
//
// Every ported behavior cites its AR (EarShotOpenAL) source line. All
// addresses are SA 1.0 US absolute VAs (re-audio-map.md / re-earshot-features.md).
//
// Hook idiom (proven in the previous src/Earshot.cpp, mirrors both reference
// mods): detour shape `__fastcall (T* self, void* unused-edx, args...)` for
// thiscall targets (EDX is the unused second-register arg — MSVC x86),
// `__cdecl` for CExplosion::AddExplosion, `__stdcall` for CWndProc.
// Install: subhook_new(@VA, detour, 0) + subhook_install (AR Loaders.cpp:925-960).
// Original call: subhook trampoline first-order (subhook README :86); fallback
// remove → raw call → reinstall (AR Main.cpp:271-273 idiom).
//
// Crash-parity rules applied throughout (re-earshot-features.md C.2):
//   C.2.1 every game pointer revalidated (pool-membership thunk + position
//         sanity) before deref; C.2.2 never MessageBox from hook context —
//         logging only, gated by the façade's debug-menu logging flag.
#include "hooks/EshHooks.h"

#include "plugin.h"                  // Events machinery, plugin::CallMethodAndReturn
#include <Windows.h>                 // OutputDebugStringA, HWND/WPARAM/LRESULT/WA_*
#include <cmath>                     // std::isfinite (position sanity check)
#include <cstdint>                   // uint8_t / uint32_t
#include <unordered_map>             // g_lastExplosionType (AR ourCommon.h:34)

#include "subhook.h"                 // vendored: external/subhook (SUBHOOK_STATIC)

#include "audio/EarshotAudio.h"      // FROZEN façade contract (+ EshSoundParams.h)
#include "engine/EshVoicePool.h"     // cross-layer include for the WndProc pause
                                     // call site — see EshHooks_HandleFocusChange
#include "loaders/EshSoundBank.h"    // INI-owned distant-shot thresholds (gap #4
                                     // wiring: s_distantGunshot/ExplosionDistance)

// --- game headers (plugin-sdk v1004, SA 1.0 US) ---
#include "CAEWeaponAudioEntity.h"    // WeaponFire/WeaponReload, m_pPed @0xA4
#include "CAEPedAudioEntity.h"       // HandlePedHit/HandlePedSwing, m_pPed @0x94
#include "CAEExplosionAudioEntity.h" // explosion audio entity (hook target class)
#include "CAudioEngine.h"            // ReportBulletHit / ReportFrontEndAudioEvent
#include "CAEAudioHardware.h"        // master/fader scaling factors
#include "CEntity.h"                 // IsEntityPointerValid decl, m_nType/m_nModelIndex
#include "CPhysical.h"               // CPhysical (victim args)
#include "CPed.h"                    // m_aWeapons/m_nSelectedWepSlot/GetWeapon
#include "CWeapon.h"                 // m_eWeaponType/m_nAmmoInClip
#include "CWeaponInfo.h"             // m_nAmmoClip
#include "CVector.h"                 // inline impl via CVectorImplementation.h
#include "CCamera.h"                 // TheCamera (defined in engine/EshListener.cpp)
#include "CGame.h"                   // currArea
#include "CTimer.h"                  // ms_fTimeScale
#include "CWaterLevel.h"             // GetWaterLevelNoWaves decl
#include "CExplosion.h"              // eExplosionType, AddExplosion decl
#include "CMenuManager.h"            // FrontEndMenuManager.m_bMenuActive
#include "eAudioEvents.h"            // AE_WEAPON_RELOAD_A/B, AE_PED_HIT_*, AE_FRONTEND_BULLET_PASS_*
#include "eSurfaceType.h"            // SURFACE_*, TOTAL_NUM_SURFACE_TYPES

using namespace plugin;

// ===========================================================================
// plugin-sdk linkage shims
// ===========================================================================
// E:/SDKs/plugin-sdk-master ships headers ONLY (premake5.lua:23-32 compiles
// just DynAddress/GameVersion/Patch). Symbols whose definitions live in PSDK
// .cpp files are re-provided here, each mirroring plugin-sdk's own definition
// verbatim (address + calling convention).

// PSDK CEntity.h:169 decl; CEntity.cpp:190-193 defines it as a thunk to the
// real pool-membership check in the game @0x533310 (gta-reversed-modern
// Entity.cpp:965 — validates the entity's pool slot by m_nType).
bool IsEntityPointerValid(CEntity* entity) {
    return ((bool (__cdecl*)(CEntity*))0x533310)(entity);
}

// PSDK CPed.h:536 decl; CPed.cpp:704-706 defines it as a thunk @0x5E8A30.
bool IsPedPointerValid(CPed* ped) {
    return ((bool (__cdecl*)(CPed*))0x5E8A30)(ped);
}

// PSDK CWeaponInfo.h:97 decl; CWeaponInfo.cpp:26-29 defines it as a thunk
// @0x743C60 (cdecl static).
static CWeaponInfo* EshGetWeaponInfo(eWeaponType weaponType, unsigned char skill) {
    return ((CWeaponInfo* (__cdecl*)(eWeaponType, unsigned char))0x743C60)(weaponType, skill);
}

// PSDK CPed.h:402 decl; CPed.cpp:626-628 defines it as a thunk @0x5E6580
// (thiscall).
static char EshGetWeaponSkill(CPed* ped) {
    return ((char (__thiscall*)(CPed*))0x5E6580)(ped);
}

// PSDK CWaterLevel.h:80 decl; CWaterLevel.cpp:55-56 defines it as a thunk
// @0x6E8580 (cdecl static).
static bool EshGetWaterLevelNoWaves(float x, float y, float z, float* outLevel) {
    return ((bool (__cdecl*)(float, float, float, float*))0x6E8580)(x, y, z, outLevel);
}

// Game globals referenced through PSDK declarations whose definitions live in
// non-compiled PSDK .cpp files — definitions mirrored verbatim:
float& CTimer::ms_fTimeScale = *(float*)0xB7CB64;              // PSDK CTimer.cpp:18
int&   CGame::currArea       = *(int*)0xB72914;                // PSDK CGame.cpp:14 (1.0 US)
CAEAudioHardware& AEAudioHardware = *(CAEAudioHardware*)0xB5F8B8;   // PSDK CAEAudioHardware.cpp:9
CMenuManager& FrontEndMenuManager = *(CMenuManager*)0xBA6748;       // PSDK CMenuManager.cpp:11
// NOTE for other lanes: do NOT re-define these four in your TU — they are
// provided here exactly once. (TheCamera is defined in engine/EshListener.cpp:42.)

// ===========================================================================
// small utilities
// ===========================================================================

static void EshLogRaw(const char* msg) {
    OutputDebugStringA(msg);
}

// Debug-gated log (AR logging.cpp model): hook-path logging only when the
// façade's debug-menu flag is on (façade contract: EarshotAudio_IsLogging).
static void EshLog(const char* msg) {
    if (EarshotAudio_IsLogging()) {
        OutputDebugStringA(msg);
    }
}

// NaN/inf or beyond ±50000 world units → garbage/dangling read. Kept verbatim
// from the previous src/Earshot.cpp (crash-parity C.2.1 companion guard).
static bool IsInsanePosition(const CVector& v) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        return true;
    }
    const float kWorldLimit = 50000.0f;
    return v.x < -kWorldLimit || v.x > kWorldLimit
        || v.y < -kWorldLimit || v.y > kWorldLimit
        || v.z < -kWorldLimit || v.z > kWorldLimit;
}

// Slow-motion pitch clamp — AR applies `Clamp(CTimer::ms_fTimeScale, 0, 1)`
// everywhere (Main.cpp:388, 727; re-earshot-features.md B.3). The façade
// multiplies the .earshot class pitch on top (EshSoundParams.h:9-11 contract:
// "resolved by the caller, engine does not touch CTimer").
static float SlowMoPitch() {
    float ts = CTimer::ms_fTimeScale;
    if (ts < 0.0f) ts = 0.0f;
    if (ts > 1.0f) ts = 1.0f;
    return ts;
}

// ===========================================================================
// source motion → EshSoundParams::velocity / ::forward (doppler + incidence)
// ===========================================================================
// UNITS — SA's m_vecMoveSpeed is per-TIMESTEP, not per-second:
//   * CPhysical::ApplyMoveSpeed integrates
//         GetPosition() += CTimer::GetTimeStep() * m_vecMoveSpeed
//     (gta-reversed-modern game_sa/Entity/Physical.cpp:796-801,
//      ApplyMoveSpeed @0x542DD0, installed at Physical.cpp:39)
//   * CTimer::GetTimeStepInSeconds() = ms_fTimeStep / TIMESTEP_PER_SECOND with
//     TIMESTEP_PER_SECOND = 50 (gta-reversed Timer.h:17,84) → one timestep is
//     1/50 s ⇒ world units/second = m_vecMoveSpeed × 50.
//   * Cross-check inside the game's OWN audio code, where both conventions
//     meet in a single expression (AEVehicleAudioEntity.cpp:2417):
//         bIsNotSimple ? vehicle->m_autoPilot.m_speed / 50.0f
//                      : DotProduct(physical->m_vecMoveSpeed,
//                                   physical->m_matrix->GetForward())
//     m_autoPilot.m_speed is units/second; ÷50 lands exactly on
//     m_vecMoveSpeed's per-timestep units — the same ×50 factor inverted.
//   This conversion is load-bearing: doppler fed RAW m_vecMoveSpeed would
//   report every source 50× too slow and the effect would effectively vanish.
//
// FORWARD — SA keeps the facing direction in the matrix's `up` row:
//   CMatrix::GetForward() returns `up` (plugin-sdk CMatrix.h:73-74), and the
//   game projects motion onto exactly that row for its own audio
//   (AEVehicleAudioEntity.cpp:2417 above; Cam.cpp:611
//   `m_vecFront = entityWorldMat.GetForward().Normalized()`).
//   CPlaceable::GetForward() (CPlaceable.h) dereferences m_matrix with NO null
//   check, so test m_matrix first; a placeable without a matrix has no
//   reliable facing and we leave forward at {0,0,0} (omni → effect skipped).
//
// SLOW-MO — deliberately NOT scaled by CTimer::ms_fTimeScale: the façade
//   already folds ms_fTimeScale into the playback pitch (SlowMoPitch above),
//   and doppler is fed the game's LOGICAL motion so the two don't double-dip.
//   (Nuance: SA multiplies the frame delta by ms_fTimeScale before deriving
//   ms_fTimeStep — Timer.cpp:256-258 — so a strict wall-clock velocity would
//   carry the factor too; keeping the raw value is a deliberate choice.)
static void FillSourceMotion(CPhysical* entity, EshSoundParams& ov) {
    if (!entity || !IsEntityPointerValid(entity)) {
        return;   // velocity/forward stay {0,0,0}: static + omni
    }

    // velocity: per-timestep → world units/second (see header).
    CVector vel = entity->m_vecMoveSpeed;
    vel *= 50.0f;
    if (std::isfinite(vel.x) && std::isfinite(vel.y) && std::isfinite(vel.z)) {
        // Garbage guard: nothing legitimate in SA moves near 1000 u/s, and a
        // doppler input that large would be nonsense anyway.
        const float kMaxSpeed = 1000.0f;
        if (vel.MagnitudeSqr() > kMaxSpeed * kMaxSpeed) {
            vel = CVector(0.0f, 0.0f, 0.0f);
        }
        ov.velocity = vel;
    }

    // forward: normalised facing (the engine re-normalises defensively).
    if (entity->m_matrix) {
        CVector fwd = entity->m_matrix->GetForward();   // COPY — ref aliases the live matrix
        if (fwd.MagnitudeSqr() > 1e-8f) {
            fwd.Normalize();
            ov.forward = fwd;
        }
    }
}

// INI-owned distant-shot thresholds — LIVE values loaded from the MAIN INI
// section by the loader lane ("Distant gunshot distance" / "Distant explosion
// distance", B.8 defaults 50/100, ourCommon.h:31-32). Reconciliation gap #4:
// hooks read EshSoundBank statics instead of stale local constants so a
// user-edited INI actually changes the distant-shot branch decisions.
static const float kSpeedOfSound         = 343.3f;   // AR Main.cpp:520
static const float kMaxSoundDelaySec     = 5.0f;     // AR AudioManager.cpp:229 scheduler cap

// ===========================================================================
// B.6 surface taxonomy resolver (AR ourCommon.h:718-864)
// ===========================================================================
// SurfaceInfos_c game singleton @0xB79538 (AR ourCommon.h:725-726). The class
// layout is irrelevant — the taxonomy predicates are invoked as raw thiscalls
// via plugin::CallMethodAndReturn (header-only, PluginBase.h:184-187), exactly
// AR's idiom (ourCommon.h:729-767).

static void* const kSurfaceInfos = (void*)0xB79538;

static bool IsAudioConcrete(uint32_t id) {   // 0x55EA30
    return CallMethodAndReturn<bool, 0x55EA30, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsAudioGrass(uint32_t id) {      // 0x55EA50
    return CallMethodAndReturn<bool, 0x55EA50, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsAudioSand(uint32_t id) {       // 0x55EA70
    return CallMethodAndReturn<bool, 0x55EA70, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsAudioGravel(uint32_t id) {     // 0x55EA90
    return CallMethodAndReturn<bool, 0x55EA90, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsAudioWood(uint32_t id) {       // 0x55EAB0
    return CallMethodAndReturn<bool, 0x55EAB0, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsAudioWater(uint32_t id) {      // 0x55EAD0
    return CallMethodAndReturn<bool, 0x55EAD0, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsAudioMetal(uint32_t id) {      // 0x55EAF0
    return CallMethodAndReturn<bool, 0x55EAF0, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsAudioLongGrass(uint32_t id) {  // 0x55EB10
    return CallMethodAndReturn<bool, 0x55EB10, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsAudioTile(uint32_t id) {       // 0x55EB30
    return CallMethodAndReturn<bool, 0x55EB30, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsWater(uint32_t id) {           // 0x55E710 (AR ourCommon.h:826-829)
    return CallMethodAndReturn<bool, 0x55E710, void*, uint32_t>(kSurfaceInfos, id);
}
static bool IsShallowWater(uint32_t id) {    // 0x55E730 (AR ourCommon.h:832-835)
    return CallMethodAndReturn<bool, 0x55E730, void*, uint32_t>(kSurfaceInfos, id);
}

// Hard-coded wood list (AR ourCommon.h:778-796 IsWood).
static bool IsWood(uint32_t id) {
    switch (id) {
    case SURFACE_WOOD_CRATES:        // 42
    case SURFACE_WOOD_SOLID:         // 43
    case SURFACE_WOOD_THIN:          // 44
    case SURFACE_WOOD_BENCH:         // 70
    case SURFACE_FLOORBOARD:         // 72
    case SURFACE_STAIRSWOOD:         // 73
    case SURFACE_P_WOODLAND:         // 83
    case SURFACE_P_WOODDENSE:        // 84
    case SURFACE_P_FORESTSTUMPS:     // 128
    case SURFACE_P_FORESTSTICKS:     // 129
    case SURFACE_P_FORRESTLEAVES:    // 130
    case SURFACE_P_FORRESTDRY:       // 132
    case SURFACE_WOOD_PICKET_FENCE:  // 172
    case SURFACE_WOOD_SLATTED_FENCE: // 173
    case SURFACE_WOOD_RANCH_FENCE:   // 174
        return true;
    default:
        return false;
    }
}

// Hard-coded flesh list (AR ourCommon.h:799-812 IsFleshy) — "default" counts
// as fleshy there too.
static bool IsFleshy(uint32_t id) {
    switch (id) {
    case SURFACE_PED:      // 62
    case SURFACE_GORE:     // 177
    case SURFACE_DEFAULT:
        return true;
    default:
        return false;
    }
}

// --- ricochet material buckets (CAudioEngine::ReportBulletHit, AR :398-447) ---
// The frozen façade takes `subKey = surface index` for the "ricochet" category.
// Bug #4 (gravel-on-vehicle → metal) needs the victim ENTITY, so the hook must
// resolve the bucket itself; the resolved bucket ordinal is passed as subKey.
// LOADER-LANE MAPPING (keep in sync with generic/ricochet/<surface>/ folders,
// AR Loaders.cpp:1329-1377):
//   0 default  1 flesh  2 glass  3 metal  4 water  5 wood  6 sand  7 stone  8 dirt
enum EshRicochetSurface {
    ESH_RICOCHET_DEFAULT = 0,
    ESH_RICOCHET_FLESH,
    ESH_RICOCHET_GLASS,
    ESH_RICOCHET_METAL,
    ESH_RICOCHET_WATER,
    ESH_RICOCHET_WOOD,
    ESH_RICOCHET_SAND,
    ESH_RICOCHET_STONE,
    ESH_RICOCHET_DIRT,
};

[[maybe_unused]] static EshRicochetSurface EshResolveRicochetSurface(CEntity* victim, uint8_t surface) {
    switch (surface) {                                   // AR :407-419
    case SURFACE_PED:                                    // 62
    case SURFACE_GORE:                                   // 177
        return ESH_RICOCHET_FLESH;
    case SURFACE_GLASS:                                  // 45
    case SURFACE_GLASS_WINDOWS_LARGE:                    // 46
    case SURFACE_GLASS_WINDOWS_SMALL:                    // 47
    case SURFACE_UNBREAKABLE_GLASS:                      // 175
        return ESH_RICOCHET_GLASS;
    default:
        break;                                           // keep checking (AR :420)
    }

    // Metal — OR gravel-on-vehicle counts as metal, because vehicles can carry
    // gravel surfaces (bug #4, AR :423-427).
    if (IsAudioMetal(surface) ||
        (victim && victim->m_nType == ENTITY_TYPE_VEHICLE && surface == SURFACE_GRAVEL)) {
        return ESH_RICOCHET_METAL;
    }
    if (IsAudioWater(surface) || IsWater(surface) || IsShallowWater(surface)) {  // AR :428-429
        return ESH_RICOCHET_WATER;
    }
    if (IsAudioWood(surface)) {                          // AR :430-431
        return ESH_RICOCHET_WOOD;
    }
    if (IsAudioSand(surface)) {                          // AR :432-433
        return ESH_RICOCHET_SAND;
    }
    if (IsAudioConcrete(surface)) {                      // AR :434-435 (concrete → "stone")
        return ESH_RICOCHET_STONE;
    }
    if (IsAudioGrass(surface) || IsAudioLongGrass(surface) || IsAudioGravel(surface)) {  // AR :436-440
        return ESH_RICOCHET_DIRT;
    }
    return ESH_RICOCHET_DEFAULT;                         // AR :402 ("default" start value)
}

// ===========================================================================
// hook typedefs + original-call helpers
// ===========================================================================
// Signatures from PSDK thunks / gta-reversed-modern (re-audio-map.md §1, §6):
//   WeaponFire  0x504F80 void thiscall(T*, eWeaponType, CPhysical*, int)
//               (PSDK CAEWeaponAudioEntity.cpp:21-23; NOTSA AEWeaponAudioEntity.cpp:65)
//   WeaponReload 0x503690 same shape (PSDK CAEWeaponAudioEntity.cpp:14-16)
//   HandlePedHit 0x4E1CC0 void thiscall(T*, int, CPhysical*, uint8_t, float, uint32_t)
//               (AR Main.cpp:311-314; plugin::CallMethod<0x4E1CC0,...> AR :373-375)
//   HandlePedSwing 0x4E1A40 char thiscall(T*, int, int, int) (AR :491-494)
//   CAEExplosionAudioEntity::AddAudioEvent 0x4DCBE0 void thiscall(T*, int, CVector*, float)
//               (AR Main.cpp:516-520)
//   ReportBulletHit 0x506EC0 int thiscall(CAudioEngine*, CEntity*, uint8_t, const CVector&, float)
//               (AR Main.cpp:386-390)
//   ReportFrontEndAudioEvent 0x506EA0 CReference* thiscall(CAudioEngine*, int, float, float)
//               (PSDK CAudioEngine.cpp:45 — returned pointer carried as void*)
//   CWndProc 0x747EB0 LRESULT stdcall(HWND, UINT, WPARAM, LPARAM) (AR :2903-2906)
//   CExplosion::AddExplosion 0x736A50 bool cdecl(CEntity*, CEntity*, eExplosionType,
//               CVector, unsigned, unsigned char, float, unsigned char)
//               (PSDK CExplosion.h:65; AR :1404-1409)

using WeaponFireFn        = void   (__thiscall*)(CAEWeaponAudioEntity*, eWeaponType, CPhysical*, int);
using WeaponReloadFn      = void   (__thiscall*)(CAEWeaponAudioEntity*, eWeaponType, CPhysical*, int);
using HandlePedHitFn      = void   (__thiscall*)(CAEPedAudioEntity*, int, CPhysical*, uint8_t, float, uint32_t);
using HandlePedSwingFn    = char   (__thiscall*)(CAEPedAudioEntity*, int, int, int);
using ExplosionAudioFn    = void   (__thiscall*)(CAEExplosionAudioEntity*, int, CVector*, float);
using ReportBulletHitFn   = int    (__thiscall*)(CAudioEngine*, CEntity*, uint8_t, const CVector&, float);
using ReportFrontEndFn    = void*  (__thiscall*)(CAudioEngine*, int, float, float);
using WndProcFn           = LRESULT (__stdcall*)(HWND, UINT, WPARAM, LPARAM);
using AddExplosionFn      = bool   (__cdecl*)(CEntity*, CEntity*, eExplosionType, CVector,
                                              unsigned int, unsigned char, float, unsigned char);

enum EshHookId {
    H_WEAPON_FIRE,        // 0x504F80
    H_WEAPON_RELOAD,      // 0x503690
    H_PED_HIT,            // 0x4E1CC0
    H_PED_SWING,          // 0x4E1A40
    H_EXPLOSION_AUDIO,    // 0x4DCBE0
    H_BULLET_HIT,         // 0x506EC0
    H_FRONTEND_AUDIO,     // 0x506EA0
    H_WNDPROC,            // 0x747EB0
    H_ADD_EXPLOSION,      // 0x736A50 (type tracker)
    H_COUNT
};

static subhook_t g_hooks[H_COUNT] = {};

// Call the original implementation for a hooked address: subhook trampoline
// first-order; when no trampoline could be built, unpatch → raw call → repatch
// (AR Main.cpp:271-273 idiom). One tiny helper per hook keeps the calling
// conventions explicit and C++-plain.
static void CallOriginalWeaponFire(CAEWeaponAudioEntity* self, eWeaponType weaponType,
                                   CPhysical* victim, int audioEventId) {
    const WeaponFireFn orig = (WeaponFireFn)0x504F80;
    subhook_t h = g_hooks[H_WEAPON_FIRE];
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            ((WeaponFireFn)tramp)(self, weaponType, victim, audioEventId);
            return;
        }
        subhook_remove(h);
        orig(self, weaponType, victim, audioEventId);
        subhook_install(h);
        return;
    }
    orig(self, weaponType, victim, audioEventId);
}

static void CallOriginalWeaponReload(CAEWeaponAudioEntity* self, eWeaponType weaponType,
                                     CPhysical* entity, int audioEventId) {
    const WeaponReloadFn orig = (WeaponReloadFn)0x503690;
    subhook_t h = g_hooks[H_WEAPON_RELOAD];
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            ((WeaponReloadFn)tramp)(self, weaponType, entity, audioEventId);
            return;
        }
        subhook_remove(h);
        orig(self, weaponType, entity, audioEventId);
        subhook_install(h);
        return;
    }
    orig(self, weaponType, entity, audioEventId);
}

static void CallOriginalHandlePedHit(CAEPedAudioEntity* self, int audioEvent, CPhysical* victim,
                                     uint8_t surface, float volume, uint32_t maxVolume) {
    const HandlePedHitFn orig = (HandlePedHitFn)0x4E1CC0;
    subhook_t h = g_hooks[H_PED_HIT];
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            ((HandlePedHitFn)tramp)(self, audioEvent, victim, surface, volume, maxVolume);
            return;
        }
        subhook_remove(h);
        orig(self, audioEvent, victim, surface, volume, maxVolume);
        subhook_install(h);
        return;
    }
    orig(self, audioEvent, victim, surface, volume, maxVolume);
}

static char CallOriginalHandlePedSwing(CAEPedAudioEntity* self, int a2, int a3, int a4) {
    const HandlePedSwingFn orig = (HandlePedSwingFn)0x4E1A40;
    subhook_t h = g_hooks[H_PED_SWING];
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            return ((HandlePedSwingFn)tramp)(self, a2, a3, a4);
        }
        subhook_remove(h);
        const char r = orig(self, a2, a3, a4);
        subhook_install(h);
        return r;
    }
    return orig(self, a2, a3, a4);
}

static void CallOriginalExplosionAudio(CAEExplosionAudioEntity* self, int audioEvent,
                                       CVector* posn, float volume) {
    const ExplosionAudioFn orig = (ExplosionAudioFn)0x4DCBE0;
    subhook_t h = g_hooks[H_EXPLOSION_AUDIO];
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            ((ExplosionAudioFn)tramp)(self, audioEvent, posn, volume);
            return;
        }
        subhook_remove(h);
        orig(self, audioEvent, posn, volume);
        subhook_install(h);
        return;
    }
    orig(self, audioEvent, posn, volume);
}

static int CallOriginalReportBulletHit(CAudioEngine* engine, CEntity* victim, uint8_t surface,
                                       const CVector& posn, float angleWithColPointNorm) {
    const ReportBulletHitFn orig = (ReportBulletHitFn)0x506EC0;
    subhook_t h = g_hooks[H_BULLET_HIT];
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            return ((ReportBulletHitFn)tramp)(engine, victim, surface, posn, angleWithColPointNorm);
        }
        subhook_remove(h);
        const int r = orig(engine, victim, surface, posn, angleWithColPointNorm);
        subhook_install(h);
        return r;
    }
    return orig(engine, victim, surface, posn, angleWithColPointNorm);
}

static void* CallOriginalReportFrontEndAudio(CAudioEngine* engine, int eventId,
                                             float volumeChange, float speed) {
    const ReportFrontEndFn orig = (ReportFrontEndFn)0x506EA0;
    subhook_t h = g_hooks[H_FRONTEND_AUDIO];
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            return ((ReportFrontEndFn)tramp)(engine, eventId, volumeChange, speed);
        }
        subhook_remove(h);
        void* r = orig(engine, eventId, volumeChange, speed);
        subhook_install(h);
        return r;
    }
    return orig(engine, eventId, volumeChange, speed);
}

static LRESULT CallOriginalWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    const WndProcFn orig = (WndProcFn)0x747EB0;
    subhook_t h = g_hooks[H_WNDPROC];
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            return ((WndProcFn)tramp)(window, message, wParam, lParam);
        }
        subhook_remove(h);
        const LRESULT r = orig(window, message, wParam, lParam);
        subhook_install(h);
        return r;
    }
    return orig(window, message, wParam, lParam);
}

static bool CallOriginalAddExplosion(CEntity* victim, CEntity* creator, eExplosionType type,
                                     CVector posn, unsigned int time, unsigned char makeSound,
                                     float camShake, unsigned char visibility) {
    const AddExplosionFn orig = (AddExplosionFn)0x736A50;
    subhook_t h = g_hooks[H_ADD_EXPLOSION];
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            return ((AddExplosionFn)tramp)(victim, creator, type, posn, time, makeSound, camShake, visibility);
        }
        subhook_remove(h);
        const bool r = orig(victim, creator, type, posn, time, makeSound, camShake, visibility);
        subhook_install(h);
        return r;
    }
    return orig(victim, creator, type, posn, time, makeSound, camShake, visibility);
}

// ===========================================================================
// explosion type tracking (AR Main.cpp:1403-1452)
// ===========================================================================
// CExplosion::AddExplosion records the latest explosion type per creator; the
// CAEExplosionAudioEntity::AddAudioEvent hook (which the game calls FROM
// INSIDE AddExplosion when makeSound) reads it back. AR stores it in
// g_lastExplosionType (ourCommon.h:34), cleared at every AddExplosion entry
// and erased at exit — at audio-hook time the map holds exactly one entry.
// Tank-cannon pre-play (AR :1416-1441, CarGunJustDown + cannon_fire buffers)
// is S6 scope — NOT ported in M1.
static std::unordered_map<CEntity*, int> g_lastExplosionType;

static bool __cdecl HookedAddExplosion(CEntity* victim, CEntity* creator, eExplosionType type,
                                       CVector posn, unsigned int time, unsigned char makeSound,
                                       float camShake, unsigned char visibility) {
    static bool s_inside = false;   // AR :1404 isInside re-entrancy guard
    if (s_inside) {
        // AR :1450: re-entrant call passes through the original untouched
        // (the outer entry's type stays visible to the audio hook — AR quirk,
        // mirrored).
        return CallOriginalAddExplosion(victim, creator, type, posn, time, makeSound, camShake, visibility);
    }
    g_lastExplosionType.clear();                    // AR :1406
    g_lastExplosionType[creator] = (int)type;       // AR :1407
    s_inside = true;
    const bool result = CallOriginalAddExplosion(victim, creator, type, posn, time, makeSound, camShake, visibility);
    s_inside = false;
    g_lastExplosionType.erase(creator);             // AR :1447
    return result;
}

// ===========================================================================
// hook 1 — CAEWeaponAudioEntity::WeaponFire @0x504F80 (AR Main.cpp:176-274)
// ===========================================================================
// Selection semantics (B.5): valid m_pPed else victim (heli AI guns);
// low_ammo when ammoInClip < clip/3, dryfire additionally when ammo < 2;
// distant when dist >= 50; shoot{i}/after{i} alternatives (façade picks the
// random alternative internally — one call covers AR's whole i<10 loop);
// after ONLY outdoors (CGame::currArea <= 0); no stems at all → vanilla.
static void __fastcall HookedWeaponFire(CAEWeaponAudioEntity* self, void* /*unused-edx*/,
                                        eWeaponType weaponType, CPhysical* victim,
                                        int audioEventId) {
    if (!self) {
        // thiscall with null this should never happen; bail without touching
        // anything (crash-parity: never deref unvalidated game pointers).
        EshLogRaw("[Earshot] WeaponFire hook: null 'this' — skipped\n");
        return;
    }

    // AR :180-185: weapon + weapon-info from the owning ped (clip math below).
    CWeapon* weap = nullptr;
    CWeaponInfo* info = nullptr;
    CPed* ownerPed = self->m_pPed;                       // PSDK CAEWeaponAudioEntity.h:34 (0xA4)
    if (ownerPed && IsPedPointerValid(ownerPed)) {
        weap = ownerPed->GetWeapon();                    // CPed.h:436-437 (&m_aWeapons[slot])
        if (weap) {
            info = EshGetWeaponInfo(weap->m_eWeaponType, EshGetWeaponSkill(ownerPed));
        }
    }

    // AR :196-213: entity selection — valid m_pPed first, else the victim
    // CPhysical* (AI heli guns only have a valid victim).
    CPhysical* entity = nullptr;
    if (ownerPed && IsPedPointerValid(ownerPed)) {
        entity = ownerPed;
    } else if (victim && IsEntityPointerValid(victim)) {
        entity = victim;
    }
    if (!entity || !IsEntityPointerValid(entity)) {
        EshLog("[Earshot] WeaponFire: no valid entity — vanilla fallback\n");
        CallOriginalWeaponFire(self, weaponType, victim, audioEventId);
        return;
    }

    CVector pos = entity->GetPosition();                 // CPlaceable.h:53-54
    if (IsInsanePosition(pos)) {
        EshLog("[Earshot] WeaponFire: insane/dangling position — vanilla fallback\n");
        CallOriginalWeaponFire(self, weaponType, victim, audioEventId);
        return;
    }

    const unsigned wt = (unsigned)weaponType;
    const int modelId = entity->m_nModelIndex;           // CEntity.h:66 (0x22)

    // Fast path: no custom stems registered for this weapon at all → vanilla
    // (two-tier lookup (type, modelId) then (type, -1) happens inside).
    if (!EarshotAudio_WeaponHasStems(wt, modelId)) {
        CallOriginalWeaponFire(self, weaponType, victim, audioEventId);
        return;
    }

    // Attenuation stays façade-owned (INI + .earshot class set); the hook only
    // contributes the slow-mo pitch clamp + the source motion (doppler
    // velocity / incidence forward) for the entity the shot is attributed to
    // (shooter ped, or the victim CPhysical for AI heli guns — AR :196-213).
    EshSoundParams ov;
    ov.pitch = SlowMoPitch();
    FillSourceMotion(entity, ov);

    const float dist = CVector::Distance(TheCamera.GetPosition(), pos);
    const bool farAway = dist >= EshSoundBank::s_distantGunshotDistance;   // AR :216

    bool emptyPlayed = false;
    bool alternatePlayed = false;

    // AR :221-235: low ammo + dryfire (near shots, ped-held guns only).
    if (!farAway && entity->m_nType == ENTITY_TYPE_PED && weap && info) {
        const unsigned int ammo = weap->m_nAmmoInClip;       // CWeapon.h:33 (0x8)
        const unsigned short ammoClip = info->m_nAmmoClip;   // CWeaponInfo.h:61 (0x20)
        const unsigned int left = (unsigned int)(ammoClip / 3);
        if (ammo < left) {
            if (EarshotAudio_PlayWeaponStem(wt, modelId, "low_ammo", pos, ov)) {
                emptyPlayed = true;
            }
            if (ammo < 2) {
                if (EarshotAudio_PlayWeaponStem(wt, modelId, "dryfire", pos, ov)) {
                    emptyPlayed = true;
                }
            }
        }
    }

    // AR :237-243: distant gunshot. If no distant stems exist, AR falls through
    // to the regular shoot path — mirrored.
    if (farAway) {
        if (EarshotAudio_PlayWeaponStem(wt, modelId, "distant", pos, ov)) {
            return;                                          // suppress vanilla
        }
    }

    // AR :246-264: shoot + after. The façade picks the random alternative
    // (shoot0..9 → unindexed shoot) internally, so a single "shoot" call covers
    // AR's whole alternative loop. "after" only outdoors (currArea <= 0) and
    // only when a shoot stem actually played (AR never plays after alone).
    // NOTE (reported gap): AR pairs shoot{i} with after{i}; two independent
    // façade calls can pick different alternative indexes.
    if (EarshotAudio_PlayWeaponStem(wt, modelId, "shoot", pos, ov)) {
        alternatePlayed = true;
        if (CGame::currArea <= 0) {                          // AR :251-253, PSDK CGame.h:20
            EarshotAudio_PlayWeaponStem(wt, modelId, "after", pos, ov);
        }
    }

    if (emptyPlayed || alternatePlayed) {
        return;                                              // suppress vanilla
    }

    // AR :270-273: fallback to the original.
    EshLog("[Earshot] WeaponFire: no custom stem played — vanilla fallback\n");
    CallOriginalWeaponFire(self, weaponType, victim, audioEventId);
}

// ===========================================================================
// hook 2 — CAEWeaponAudioEntity::WeaponReload @0x503690 (AR Main.cpp:276-308)
// ===========================================================================
// reload_one / reload_two / reload by audioEventId (AE_WEAPON_RELOAD_A=0x92 /
// AE_WEAPON_RELOAD_B=0x93, PSDK eAudioEvents.h:158-159). Same suppression rule.
static void __fastcall HookedWeaponReload(CAEWeaponAudioEntity* self, void* /*unused-edx*/,
                                          eWeaponType weaponType, CPhysical* entity,
                                          int audioEventId) {
    if (self && entity && IsEntityPointerValid(entity)) {
        CVector pos = entity->GetPosition();
        if (!IsInsanePosition(pos)) {
            const unsigned wt = (unsigned)weaponType;
            const int modelId = entity->m_nModelIndex;
            const char* stemClass;
            switch (audioEventId) {
            case AE_WEAPON_RELOAD_A: stemClass = "reload_one"; break;   // AR :284-289
            case AE_WEAPON_RELOAD_B: stemClass = "reload_two"; break;   // AR :290-295
            default:                 stemClass = "reload";     break;   // AR :296-301
            }
            if (EarshotAudio_WeaponHasStems(wt, modelId)) {
                EshSoundParams ov;
                ov.pitch = SlowMoPitch();
                FillSourceMotion(entity, ov);   // reloading ped/vehicle motion
                if (EarshotAudio_PlayWeaponStem(wt, modelId, stemClass, pos, ov)) {
                    return;                                          // suppress vanilla
                }
            }
        }
    }
    EshLog("[Earshot] WeaponReload: vanilla fallback\n");
    CallOriginalWeaponReload(self, weaponType, entity, audioEventId);
}

// ===========================================================================
// hook 3 — CAEPedAudioEntity::HandlePedHit @0x4E1CC0 (AR Main.cpp:310-383)
// ===========================================================================
// martial_punch / martial_kick by event, else hitmetal / hitwood / stomp / hit
// by surface via the B.6 resolver (door models 1532/1500/1478/1432 count as
// wood, AR :326). Bug #5 (C.2.5): surface id can exceed TOTAL_NUM_SURFACE_TYPES
// — range-check every surface classification.
static void __fastcall HookedHandlePedHit(CAEPedAudioEntity* self, void* /*unused-edx*/,
                                          int audioEvent, CPhysical* victim, uint8_t surface,
                                          float volume, uint32_t maxVolume) {
    if (!self) {
        EshLogRaw("[Earshot] HandlePedHit hook: null 'this' — skipped\n");
        return;
    }

    // AR :316 reads the ped's selected weapon unguarded — crash-parity fix:
    // validate the ped first, otherwise there is no weapon key → vanilla.
    CPed* ped = self->m_pPed;                            // PSDK CAEPedAudioEntity.h:25 (0x94)
    if (!ped || !IsPedPointerValid(ped)) {
        CallOriginalHandlePedHit(self, audioEvent, victim, surface, volume, maxVolume);
        return;
    }
    const eWeaponType weaponType = ped->m_aWeapons[ped->m_nSelectedWepSlot].m_eWeaponType;
                                                         // CPed.h:243/247 (0x5A0/0x718)

    // Bug #5: range-check before any surface classification (AR :322-326).
    const bool surfaceInRange = surface <= TOTAL_NUM_SURFACE_TYPES;
    const bool metalSurface = surfaceInRange && (IsAudioMetal(surface) || surface == SURFACE_GLASS);
    const bool woodSurface = surfaceInRange &&
        (IsAudioWood(surface) ||
         (victim && (victim->m_nModelIndex == 1532 || victim->m_nModelIndex == 1500 ||
                     victim->m_nModelIndex == 1478 || victim->m_nModelIndex == 1432)) ||
         IsWood(surface));

    bool handled = false;
    if (victim && IsEntityPointerValid(victim)) {
        CVector pos = victim->GetPosition();
        if (!IsInsanePosition(pos)) {
            const unsigned wt = (unsigned)weaponType;
            const int modelId = victim->m_nModelIndex;
            EshSoundParams ov;
            ov.pitch = SlowMoPitch();
            FillSourceMotion(victim, ov);   // the VICTIM's motion (task: ped hit)

            switch (audioEvent) {                        // AR :330-338 event set
            case AE_PED_HIT_HIGH:
            case AE_PED_HIT_LOW:
            case AE_PED_HIT_GROUND:
            case AE_PED_HIT_GROUND_KICK:
            case AE_PED_HIT_HIGH_UNARMED:
            case AE_PED_HIT_LOW_UNARMED:
            case AE_PED_HIT_MARTIAL_PUNCH:
            case AE_PED_HIT_MARTIAL_KICK:
                // Martial attacks first (AR :341-349).
                if (audioEvent == AE_PED_HIT_MARTIAL_PUNCH &&
                    EarshotAudio_PlayWeaponStem(wt, modelId, "martial_punch", pos, ov)) {
                    handled = true;
                }
                if (!handled && audioEvent == AE_PED_HIT_MARTIAL_KICK &&
                    EarshotAudio_PlayWeaponStem(wt, modelId, "martial_kick", pos, ov)) {
                    handled = true;
                }
                // Surface-specific (AR :352-362).
                if (!handled && metalSurface &&
                    EarshotAudio_PlayWeaponStem(wt, modelId, "hitmetal", pos, ov)) {
                    handled = true;
                }
                if (!handled && woodSurface &&
                    EarshotAudio_PlayWeaponStem(wt, modelId, "hitwood", pos, ov)) {
                    handled = true;
                }
                if (!handled && (audioEvent == AE_PED_HIT_GROUND || audioEvent == AE_PED_HIT_GROUND_KICK) &&
                    EarshotAudio_PlayWeaponStem(wt, modelId, "stomp", pos, ov)) {
                    handled = true;
                }
                // Fallback for hitting flesh (AR :365-369).
                if (!handled && !woodSurface && victim->m_nType == ENTITY_TYPE_PED &&
                    IsFleshy(surface) &&
                    EarshotAudio_PlayWeaponStem(wt, modelId, "hit", pos, ov)) {
                    handled = true;
                }
                break;
            default:
                break;                                   // AR :371-373 unhandled events
            }
        }
    }

    if (!handled) {
        EshLog("[Earshot] HandlePedHit: vanilla fallback\n");
        CallOriginalHandlePedHit(self, audioEvent, victim, surface, volume, maxVolume);
    }
}

// ===========================================================================
// hook 4 — CAEPedAudioEntity::HandlePedSwing @0x4E1A40 (AR Main.cpp:490-514)
// ===========================================================================
// swing stem; ped && ped->GetWeapon() null-guarded (AR :500). Returns char(-1)
// to suppress the vanilla swing sound (AR :507).
static char __fastcall HookedHandlePedSwing(CAEPedAudioEntity* self, void* /*unused-edx*/,
                                            int a2, int a3, int a4) {
    if (self) {
        CPed* ped = self->m_pPed;
        if (ped && IsPedPointerValid(ped) && ped->GetWeapon()) {   // AR :498-503
            const unsigned wt = (unsigned)ped->GetWeapon()->m_eWeaponType;
            const int modelId = ped->m_nModelIndex;
            if (EarshotAudio_WeaponHasStems(wt, modelId)) {
                CVector pos = ped->GetPosition();
                if (!IsInsanePosition(pos)) {
                    EshSoundParams ov;
                    ov.pitch = SlowMoPitch();
                    FillSourceMotion(ped, ov);   // swinging ped's motion
                    if (EarshotAudio_PlayWeaponStem(wt, modelId, "swing", pos, ov)) {
                        return (char)-1;                          // AR :507 suppress
                    }
                }
            }
        }
    }
    return CallOriginalHandlePedSwing(self, a2, a3, a4);
}

// ===========================================================================
// hook 5 — CAEExplosionAudioEntity::AddAudioEvent @0x4DCBE0 (AR Main.cpp:515-693)
// ===========================================================================
// main / debris (debris IN ADDITION to main) / underwater (z < waterLevel-3) /
// distant (dist >= 100) with speed-of-sound delay (dist/343.3, capped 5 s) and
// aggressive HF loss (airAbsorption 0.2) for distant; molotov suppresses the
// generic fallback (notAMolotov, AR :540/:564).
//
// AR's chooseBuffers (Main.cpp:536-560) approximated over the façade's bool
// contract: try the type-specific set first (subKey = type id); if it misses
// and the type is not molotov, retry the generic set (subKey = -1). AR's extra
// rule — a type entry that EXISTS but loaded empty also blocks the generic
// fallback — is not expressible over the bool contract (reported gap).
// AR's handled accumulation is last-write-wins (debris overwrites main's
// result, Main.cpp:627-649) which double-plays vanilla under custom main when
// debris stems are absent — treated as an AR bug here: ANY custom stem
// suppresses vanilla.
static bool EshPlayExplosionClass(const char* stemClass, int explosionType, bool notAMolotov,
                                  const CVector& pos, const EshSoundParams& ov) {
    if (EarshotAudio_PlayGenericStem("explosions", explosionType, stemClass, &pos, ov)) {
        return true;
    }
    if (notAMolotov && EarshotAudio_PlayGenericStem("explosions", -1, stemClass, &pos, ov)) {
        return true;
    }
    return false;
}

static void __fastcall HookedExplosionAddAudioEvent(CAEExplosionAudioEntity* self,
                                                    void* /*unused-edx*/, int audioEvent,
                                                    CVector* posn, float volume) {
    if (!self || !posn) {
        CallOriginalExplosionAudio(self, audioEvent, posn, volume);
        return;
    }
    if (IsInsanePosition(*posn)) {
        EshLog("[Earshot] ExplosionAudio: insane position — vanilla fallback\n");
        CallOriginalExplosionAudio(self, audioEvent, posn, volume);
        return;
    }

    const float dist = CVector::Distance(TheCamera.GetPosition(), *posn);   // AR :521
    const bool farAway = dist >= EshSoundBank::s_distantExplosionDistance;  // AR :527

    int lastExplosionType = 0;
    if (!g_lastExplosionType.empty()) {
        lastExplosionType = g_lastExplosionType.begin()->second;            // AR :537-539
    }
    const bool notAMolotov = lastExplosionType != EXPLOSION_MOLOTOV;        // AR :540

    float waterLevel = 0.0f;
    const bool isUnderWater = EshGetWaterLevelNoWaves(posn->x, posn->y, posn->z, &waterLevel);
                                                                            // AR :541

    EshSoundParams ov;
    ov.pitch = SlowMoPitch();                                              // AR :526
    // velocity/forward stay {0,0,0} on purpose: an explosion is a static,
    // omni-directional source (task: no wiring for explosions).

    bool handled = false;
    if (!farAway) {
        if (isUnderWater && posn->z < waterLevel - 3.0f) {                 // AR :580
            handled = EshPlayExplosionClass("underwater", lastExplosionType, notAMolotov, *posn, ov);
            if (!handled) {
                // AR :584: no underwater buffers → the main explosion still plays.
                handled = EshPlayExplosionClass("explosion", lastExplosionType, notAMolotov, *posn, ov);
            }
        } else {
            // Main explosion (AR :563-594).
            handled = EshPlayExplosionClass("explosion", lastExplosionType, notAMolotov, *posn, ov);
        }
        // Debris plays IN ADDITION to main (AR :627-649).
        if (EshPlayExplosionClass("debris", lastExplosionType, notAMolotov, *posn, ov)) {
            handled = true;
        }
    } else {
        // Distant explosion: speed-of-sound propagation delay + aggressive HF
        // loss (AR :651-683 — AL_LOWPASS_GAINHF 0.20 → airAbsorption 0.2 here;
        // the engine scheduler caps the delay at 5 s, AudioManager.cpp:229).
        EshSoundParams dov = ov;
        float delay = dist / kSpeedOfSound;
        if (delay > kMaxSoundDelaySec) delay = kMaxSoundDelaySec;
        dov.delaySec = delay;
        dov.airAbsorption = 0.2f;
        handled = EshPlayExplosionClass("distant", lastExplosionType, notAMolotov, *posn, dov);
    }

    if (!handled) {
        EshLog("[Earshot] ExplosionAudio: vanilla fallback\n");
        CallOriginalExplosionAudio(self, audioEvent, posn, volume);
    }
}

// ===========================================================================
// hook 6 — CAudioEngine::ReportBulletHit @0x506EC0 (AR Main.cpp:386-487)
// ===========================================================================
// Per-material ricochet via the B.6 resolver (gravel-on-vehicle → metal, bug
// #4). Returning -1 suppresses the vanilla ricochet when a custom stem played;
// otherwise the original runs and its return value is forwarded.
static int __fastcall HookedReportBulletHit(CAudioEngine* engine, int /*unused-edx*/,
                                            CEntity* victim, uint8_t surface,
                                            const CVector& posn, float angleWithColPointNorm) {
    if (engine && surface <= TOTAL_NUM_SURFACE_TYPES && !IsInsanePosition(posn)) {
        // Reconciliation gap #2: pass the RAW surface id — the façade resolves
        // the bucket itself (EshSoundBank::RicochetBucketForSurface, port of AR
        // Main.cpp:407-419) and indexes the per-surface RICOCHET_SURFACE_TYPE_i
        // INI arrays by surface id, matching the frozen contract ("subKey =
        // surface index"). Divergence: AR bug #4 (gravel-on-vehicle → metal,
        // Main.cpp:418-421) is NOT applied here — the facade has no victim
        // entity — gravel-on-vehicle plays the gravel bucket. Deferred to S6.
        EshSoundParams ov;
        ov.pitch = SlowMoPitch();
        // velocity/forward stay {0,0,0}: an impact point is static and omni
        // (doppler/incidence wiring is deliberately PED-motion only).
        // Attenuation (maxDist 50 / refDist 3.5 / rolloff 5 / airAbs 3, AR
        // :452-456) is façade-owned (RICOCHET INI section, B.8).
        if (EarshotAudio_PlayGenericStem("ricochet", (int)surface, "ricochet", &posn, ov)) {
            return -1;                                   // AR :479 suppress vanilla
        }
    }
    EshLog("[Earshot] ReportBulletHit: vanilla fallback\n");
    return CallOriginalReportBulletHit(engine, victim, surface, posn, angleWithColPointNorm);
}

// ===========================================================================
// hook 7 — CAudioEngine::ReportFrontEndAudioEvent @0x506EA0 (AR Main.cpp:1454-1494)
// ===========================================================================
// Bullet whizz: the 4 quadrant events map to subKey 0..3 and play as 2D
// stereo (pos = nullptr per the façade contract). Every other frontend event
// passes through untouched.
static void* __fastcall HookedReportFrontEndAudio(CAudioEngine* engine, int /*unused-edx*/,
                                                  int eventId, float volumeChange, float speed) {
    int quadrant = -1;                                   // AR :1462-1477 buffer lists
    switch (eventId) {
    case AE_FRONTEND_BULLET_PASS_LEFT_REAR:   quadrant = 0; break;  // 0x1B
    case AE_FRONTEND_BULLET_PASS_LEFT_FRONT:  quadrant = 1; break;  // 0x1C
    case AE_FRONTEND_BULLET_PASS_RIGHT_REAR:  quadrant = 2; break;  // 0x1D
    case AE_FRONTEND_BULLET_PASS_RIGHT_FRONT: quadrant = 3; break;  // 0x1E
    default:
        break;
    }

    if (quadrant >= 0) {
        EshSoundParams ov;
        ov.is2D = true;                                  // 2D stereo (AR PlaySource2D)
        ov.pitch = SlowMoPitch();
        // velocity/forward stay {0,0,0} on purpose: whizz is 2D (no spatializer
        // consumes velocity, and there is no world position for incidence).
        if (EarshotAudio_PlayGenericStem("bullet_whizz", quadrant, "", nullptr, ov)) {
            return nullptr;                              // suppress vanilla
        }
    }

    return CallOriginalReportFrontEndAudio(engine, eventId, volumeChange, speed);
}

// ===========================================================================
// hook 8 — CWndProc @0x747EB0 (AR Main.cpp:2902-2931)
// ===========================================================================
// WM_ACTIVATE / WM_ACTIVATEAPP / WM_KILLFOCUS / WM_SETFOCUS → focus tracking;
// WM_ACTIVATEAPP drives the pause/resume-all decision (with the pause-menu
// check). M1: the pause/resume call site is flagged — see
// EshHooks_HandleFocusChange for the contract gap.

static bool g_eshFocusLost = false;

// Reconciliation gap #1 RESOLVED: the engine lane shipped offset-preserving
// EshVoicePool_PauseAll/EshVoicePool_ResumeAll (cursor snapshot + stop / seek +
// start) — wired directly here (cross-layer call sanctioned for this case).
static void EshHooks_HandleFocusChange(bool shouldPause) {
    if (shouldPause) {
        EshLog("[Earshot] focus lost / menu active — pausing voices\n");
        EshVoicePool_PauseAll();
    } else {
        EshLog("[Earshot] focus gained — resuming voices\n");
        EshVoicePool_ResumeAll();
    }
}

static LRESULT CALLBACK HookedWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ACTIVATE:
        g_eshFocusLost = (LOWORD(wParam) == WA_INACTIVE);            // AR :2908-2910
        break;

    case WM_ACTIVATEAPP:
        g_eshFocusLost = (wParam == FALSE);                          // AR :2912-2913
        EshHooks_HandleFocusChange(g_eshFocusLost || FrontEndMenuManager.m_bMenuActive);
        break;

    case WM_KILLFOCUS:
        g_eshFocusLost = true;                                       // AR :2915-2916
        break;

    case WM_SETFOCUS:
        g_eshFocusLost = false;                                      // AR :2918-2919
        break;

    default:
        break;
    }

    return CallOriginalWndProc(window, message, wParam, lParam);
}

// ===========================================================================
// game lifecycle events (AR Main.cpp:3381-3450, ourCommon.h:22-26)
// ===========================================================================
// NOT in plugin-sdk shared/Events.h (verified 2026-10-15 — no such events for
// GTASA), so they are declared here with the same plugin-sdk event machinery
// AR used (ourCommon.h:22-26). Addresses are SA 1.0 US call sites.
static CdeclEvent<AddressList<0x748E6B, H_CALL>, PRIORITY_BEFORE, ArgPickNone, void()> eshShutdownGameEvent;
static CdeclEvent<AddressList<0x564372, H_CALL>, PRIORITY_AFTER, ArgPickNone, void()> eshClearForRestartEvent;
static CdeclEvent<AddressList<0x56A46E, H_CALL>, PRIORITY_AFTER, ArgPickNone, void()> eshClearExcitingStuffFromAreaEvent;
static CdeclEvent<AddressList<0x56E975, H_CALL>, PRIORITY_AFTER, ArgPickNone, void()> eshMakePlayerSafeEvent;
static CdeclEvent<AddressList<0x738AFF, H_CALL, 0x73997A, H_CALL, 0x739A17, H_CALL, 0x739AD0, H_CALL>,
                  PRIORITY_AFTER, ArgPickN<CEntity*, 0>, void(CEntity* ent)> eshWorldRemoveProjEvent;

// Restart / area-clear / projectile-removal handlers (AR :3386-3450). M1 wiring
// (reconciliation gap #3): EshVoicePool_StopAll() stops every voice AND clears
// the delayed-sound queue (engine-lane divergence #5) — the closest M1
// equivalent of AR's scheduledSounds.clear() + entity-sound teardown. Per-
// ENTITY sound stop needs the S6 façade (voices are not entity-tracked in M1).
static void EshHooks_OnClearForRestart() {
    // AR :3391-3413: scheduledSounds.clear() + ambience timers + g_Buffers.ent.
    EshLog("[Earshot] ClearForRestart: stopping all voices + clearing delayed queue\n");
    EshVoicePool_StopAll();
}
static void EshHooks_OnClearExcitingStuff() {
    // AR :3415-3427: missile sources for the cleared area.
    EshLog("[Earshot] ClearExcitingStuffFromArea: stopping all voices\n");
    EshVoicePool_StopAll();
}
static void EshHooks_OnMakePlayerSafe() {
    // AR :3429-3441.
    EshLog("[Earshot] MakePlayerSafe: stopping all voices\n");
    EshVoicePool_StopAll();
}
static void EshHooks_OnWorldRemoveProj(CEntity* ent) {
    // AR :3443-3455: stop sounds attached to the removed projectile entity.
    // M1: voices are not entity-tracked, so per-entity stop has no engine API
    // — deliberately NOT calling StopAll here (would kill unrelated sounds on
    // every projectile despawn). S6 adds per-entity tracking.
    (void)ent;
    EshLog("[Earshot] WorldRemoveProj: per-entity stop deferred to S6 (no entity tracking yet)\n");
}

// ===========================================================================
// install / remove
// ===========================================================================
struct EshHookEntry {
    EshHookId  id;
    void*      target;
    void*      detour;
    const char* name;
};

static const EshHookEntry kHookTable[H_COUNT] = {
    { H_WEAPON_FIRE,     (void*)0x504F80, (void*)&HookedWeaponFire,             "WeaponFire @0x504F80" },
    { H_WEAPON_RELOAD,   (void*)0x503690, (void*)&HookedWeaponReload,           "WeaponReload @0x503690" },
    { H_PED_HIT,         (void*)0x4E1CC0, (void*)&HookedHandlePedHit,           "HandlePedHit @0x4E1CC0" },
    { H_PED_SWING,       (void*)0x4E1A40, (void*)&HookedHandlePedSwing,         "HandlePedSwing @0x4E1A40" },
    { H_EXPLOSION_AUDIO, (void*)0x4DCBE0, (void*)&HookedExplosionAddAudioEvent, "ExplosionAudioEvent @0x4DCBE0" },
    { H_BULLET_HIT,      (void*)0x506EC0, (void*)&HookedReportBulletHit,        "ReportBulletHit @0x506EC0" },
    { H_FRONTEND_AUDIO,  (void*)0x506EA0, (void*)&HookedReportFrontEndAudio,    "ReportFrontEndAudioEvent @0x506EA0" },
    { H_WNDPROC,         (void*)0x747EB0, (void*)&HookedWndProc,                "WndProc @0x747EB0" },
    { H_ADD_EXPLOSION,   (void*)0x736A50, (void*)&HookedAddExplosion,           "AddExplosion @0x736A50 (tracker)" },
};

void EshHooks_InstallAll() {
    for (const EshHookEntry& e : kHookTable) {
        if (g_hooks[e.id]) {
            continue;                                    // already installed
        }
        g_hooks[e.id] = subhook_new(e.target, e.detour, subhook_flags_t(0));
        if (!g_hooks[e.id] || subhook_install(g_hooks[e.id]) != 0) {
            // Unconditional visibility for install failures (never MessageBox — C.2.2).
            EshLogRaw("[Earshot] FATAL: hook install failed: ");
            EshLogRaw(e.name);
            EshLogRaw("\n");
            if (g_hooks[e.id]) {
                subhook_free(g_hooks[e.id]);
                g_hooks[e.id] = nullptr;
            }
            continue;
        }
        EshLogRaw("[Earshot] hook installed: ");
        EshLogRaw(e.name);
        EshLogRaw("\n");
    }

    // Lifecycle events (AR :3381-3450). shutdownGameEvent tears everything down
    // in reverse order: hooks first, then the audio engine.
    eshShutdownGameEvent += []() {
        EshHooks_RemoveAll();
        EarshotAudio_Shutdown();
    };
    eshClearForRestartEvent += []() {
        EshHooks_OnClearForRestart();
    };
    eshClearExcitingStuffFromAreaEvent += []() {
        EshHooks_OnClearExcitingStuff();
    };
    eshMakePlayerSafeEvent += []() {
        EshHooks_OnMakePlayerSafe();
    };
    eshWorldRemoveProjEvent += [](CEntity* ent) {
        EshHooks_OnWorldRemoveProj(ent);
    };
}

void EshHooks_RemoveAll() {
    for (subhook_t& h : g_hooks) {
        if (h) {
            subhook_remove(h);
            subhook_free(h);
            h = nullptr;
        }
    }
}

bool EshHooks_IsFocusLost() {
    return g_eshFocusLost;
}
