// EarshotAudio.h — façade over the loader/bank + engine layers. This header is
// the FROZEN M1 contract between the hook code (src/Earshot.cpp, src/hooks/*)
// and the implementation (src/audio/EarshotAudio.cpp + src/loaders/*).
// Do not change public signatures without orchestrator sign-off.
//
// Suppression model (AR parity, re-earshot-features.md B.5 step 6): every
// On* hook event returns true when a custom stem was played — the hook then
// SKIPS the original vanilla function. false = no custom content for that
// event — the hook calls the original (vanilla passthrough).
#pragma once

#include "EshSoundParams.h"

struct CVector;   // plugin-sdk shared/game/CVector.h (complete type in .cpp)

// --- lifecycle (AR init flow, re-earshot-features.md B.1) ---
// Stage 1 (initRwEvent.after): read EarShot.ini MAIN section, boot the
// miniaudio engine + voice pool, load generic categories.
bool  EarshotAudio_Init();
// Stage 2 (initGameEvent.after): recursive .earshot scan + weapon folder
// stems + INI attenuation/pitch sections. Call once, after Init.
bool  EarshotAudio_PostInit();
// Full teardown (shutdownGameEvent): stop all voices, free bank, uninit engine.
void  EarshotAudio_Shutdown();
// Per-frame (gameProcessEvent): listener update, delayed-sound scheduler,
// finished-voice sweep, pause-state checks.
void  EarshotAudio_OnFrame();
// Debug menu: stop all sounds, free everything, re-read INI + re-scan folders.
void  EarshotAudio_ReloadFolders();

// --- playback primitives (called by hooks) ---
// Play a weapon-folder stem class ("shoot", "after", "distant", "dryfire",
// "low_ammo", "reload", "reload_one", "reload_two", "hit", "hitmetal",
// "hitwood", "stomp", "swing", "martial_punch", "martial_kick", "projfire")
// for the weapon registered as (weaponType, modelId). Random alternative pick
// (stem0..9 then unindexed fallback) happens inside. The .earshot attenuation
// set for that stem class is merged under `overrides` (overrides win).
// Returns false when the weapon/class has no stems (caller falls back to
// vanilla).
bool EarshotAudio_PlayWeaponStem(unsigned weaponType, int modelId,
                                 const char* stemClass, const CVector& pos,
                                 const EshSoundParams& overrides);

// Play a generic-category stem. category: "explosions" (subKey = explosion
// type id, -1 = generic fallback), "ricochet" (subKey = surface index),
// "gunshells" (subKey = surface index), "bullet_whizz" (subKey = quadrant
// 0..3), "tankcannon" (subKey ignored). pos == nullptr → 2D playback
// (bullet whizz is stereo 2D). Returns false when no stems exist.
bool EarshotAudio_PlayGenericStem(const char* category, int subKey,
                                  const char* stemClass, const CVector* pos,
                                  const EshSoundParams& overrides);

// Suppression decision helper: does ANY custom stem set exist for this
// weapon (two-tier lookup: exact (type, modelId) then (type, -1))?
bool EarshotAudio_WeaponHasStems(unsigned weaponType, int modelId);

// --- debug menu state (B.9) ---
bool EarshotAudio_IsLogging();
void EarshotAudio_SetLogging(bool on);
