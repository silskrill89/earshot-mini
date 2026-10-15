// EshHooks.h — Lane C: subhook install table + the M1 hook set (8 game hooks +
// the CExplosion::AddExplosion type tracker) + the game lifecycle events that
// plugin-sdk's shared/Events.h does not provide (verified 2026-10-15: no
// ClearForRestart / ClearExcitingStuff / MakePlayerSafe / WorldRemoveProj /
// shutdownGame events exist there — they are declared in EshHooks.cpp with the
// same plugin-sdk event machinery the AR revamp used, ourCommon.h:22-26).
//
// Hook targets (SA 1.0 US absolute VAs — re-earshot-features.md B.2, AR
// Loaders.cpp:923-960):
//   0x504F80 CAEWeaponAudioEntity::WeaponFire          (AR Main.cpp:176-274)
//   0x503690 CAEWeaponAudioEntity::WeaponReload        (AR Main.cpp:276-308)
//   0x4E1CC0 CAEPedAudioEntity::HandlePedHit           (AR Main.cpp:310-383)
//   0x4E1A40 CAEPedAudioEntity::HandlePedSwing         (AR Main.cpp:490-514)
//   0x4DCBE0 CAEExplosionAudioEntity::AddAudioEvent    (AR Main.cpp:515-693)
//   0x506EC0 CAudioEngine::ReportBulletHit             (AR Main.cpp:386-487)
//   0x506EA0 CAudioEngine::ReportFrontEndAudioEvent    (AR Main.cpp:1454-1494)
//   0x747EB0 CWndProc                                  (AR Main.cpp:2902-2931)
//   0x736A50 CExplosion::AddExplosion (cdecl tracker)  (AR Main.cpp:1403-1452)
#pragma once

// Install every M1 hook and subscribe the lifecycle events. Call ONCE from
// Events::initGameEvent.after (AR B.1 stage 2: Loaders::InstallHooks,
// Main.cpp:3053-3058). Safe to call again — already-installed hooks are kept.
void EshHooks_InstallAll();

// Uninstall every hook (subhook_remove + subhook_free). Call from the
// shutdownGameEvent handler BEFORE EarshotAudio_Shutdown so no detour remains
// while the engine tears down.
void EshHooks_RemoveAll();

// WndProc focus state (AR MainWndProcHOOK Main.cpp:2902-2931 tracks
// WM_ACTIVATE / WM_ACTIVATEAPP / WM_KILLFOCUS / WM_SETFOCUS). The M1 façade
// contract (src/audio/EarshotAudio.h) exposes no PauseAll/ResumeAll and
// src/engine/EshVoicePool.h has no pause/resume pair either (reported contract
// gap), so M1 only FLAGS the state here; a later façade OnFrame pause check
// can consume it without any contract change.
bool EshHooks_IsFocusLost();
