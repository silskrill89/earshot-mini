// EshDelayQueue.h — game-thread scheduler for speed-of-sound delayed shots.
//
// Port of AR CAudioManager::ScheduleDelayedSound / ProcessScheduledSounds
// (external/EarShotOpenAL/source/AudioManager.cpp:226-249; 5 s cap at :229).
// CTimer::m_snTimeInMilliseconds → timeGetTime() (winmm): the engine layer
// must not touch CTimer (hooks lane owns game-time access).
#pragma once

struct CVector;
struct EshSoundParams;

// Enqueue one delayed play. delaySec is capped at 5.0 s (AR :229) and
// negatives are treated as 0. Copies path/params/pos — the caller's storage
// may vanish before the due time.
void EshDelayQueue_Enqueue(const char* path, const EshSoundParams& params,
                           const CVector* pos, float delaySec);

// Play every due entry through EshVoicePool_PlayFile (with delaySec forced
// to 0). Called once per frame from EshVoicePool_Tick (AR :235-249).
void EshDelayQueue_ProcessDue();

// Drop all pending entries (StopAll / Shutdown).
void EshDelayQueue_Clear();
