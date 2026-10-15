// EshVoicePool.h — path-keyed buffer cache + bounded voice pool for weapon
// sounds (miniaudio port of the AR playback layer).
//
// AR references (external/EarShotOpenAL/source/AudioManager.cpp):
//   * gBufferMap path→buffer cache ......... :418-432 (decode once per path)
//   * PlaySource (3D one-shot) ............. :561-640
//   * PlaySource2D (relative 2D) ........... :1567-1586
//   * ScheduleDelayedSound (5 s cap) ....... :226-249
//   * PauseSource/ResumeSource ............. :1507-1523 (offset-preserving)
//
// FROZEN ENGINE API — src/audio/EarshotAudio.cpp (other lane) calls exactly
// these; do not change signatures.
#pragma once

struct CVector;          // plugin-sdk shared/game/CVector.h (struct CVector)
struct EshSoundParams;   // src/audio/EshSoundParams.h (frozen struct)

// Create the voice pool. voiceCount == 0 → default 32 (clamped to 128).
// Idempotent: a second call while initialized returns true unchanged.
bool  EshVoicePool_Init(unsigned voiceCount);

// Stop everything, free voices + the path→source buffer cache, drop pending
// delayed sounds. Only safe at game exit (audio-thread discipline: the
// ma_engine itself is NEVER uninited mid-game). A second call is a no-op.
void  EshVoicePool_Shutdown();

// Play `path` once (or looping per params). pos == NULL → 2D (relative to the
// listener, spatialization off — AR PlaySource2D). p.delaySec > 0 (capped at
// 5 s) defers the play to EshVoicePool_Tick (AR ScheduleDelayedSound).
// The FIRST play of a path decodes it synchronously (AR gBufferMap parity);
// later plays reuse the cached decoded source.
bool  EshVoicePool_PlayFile(const char* path, const EshSoundParams& p, const CVector* pos);

void  EshVoicePool_StopAll();
void  EshVoicePool_PauseAll();    // offset-preserving: cursor + stop
void  EshVoicePool_ResumeAll();   // seek back + start

// Per-frame: play due delayed sounds, sweep finished one-shots back to free.
void  EshVoicePool_Tick();

// True between successful Init and Shutdown. (Extra beyond the frozen
// contract — kept for the façade boot path.)
bool  EshVoicePool_IsReady();

// Read-only voice census for the debug-menu Status panel (src/ui/
// EshUiMenu.cpp): *outActive = slots holding a live, un-paused voice,
// *outTotal = pool size. Either out pointer may be NULL. Const query only —
// no other engine change.
void  EshVoicePool_GetStats(unsigned* outActive, unsigned* outTotal);
