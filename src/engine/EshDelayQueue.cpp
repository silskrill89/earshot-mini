// EshDelayQueue.cpp — delayed-sound scheduler (game thread only).
//
// AR parity map (external/EarShotOpenAL/source/AudioManager.cpp):
//   ScheduleDelayedSound .... :226-233  → EshDelayQueue_Enqueue
//     * 5 s cap ............... :229
//     * playAt = now + delay*1000 :230 (CTimer → timeGetTime)
//   ProcessScheduledSounds .. :235-249  → EshDelayQueue_ProcessDue
//     * erase-remove of due entries, PlaySource for each :239-248
#include "engine/EshDelayQueue.h"

#include <Windows.h>
#include <mmsystem.h>   // timeGetTime (winmm linked via pragma below)
#pragma comment(lib, "winmm.lib")

#include <string>
#include <vector>

#include "CVector.h"                 // plugin-sdk shared/game (struct CVector — complete type needed for the pos member)
#include "audio/EshSoundParams.h"
#include "engine/EshVoicePool.h"

namespace {

constexpr float kMaxDelaySec = 5.0f;   // AR AudioManager.cpp:229

struct EshDelayedEntry {
    std::string    path;
    EshSoundParams params;
    CVector        pos;
    bool           hasPos = false;
    unsigned       dueMs  = 0;   // timeGetTime() deadline
};

std::vector<EshDelayedEntry> g_queue;

} // namespace

void EshDelayQueue_Enqueue(const char* path, const EshSoundParams& params,
                           const CVector* pos, float delaySec) {
    if (!path || !path[0]) {
        return;
    }
    if (delaySec > kMaxDelaySec) {
        delaySec = kMaxDelaySec;   // AR :229 "cap at 5 s; beyond that the player won't notice"
    }
    if (delaySec < 0.0f) {
        delaySec = 0.0f;
    }

    EshDelayedEntry e;
    e.path   = path;
    e.params = params;
    e.hasPos = (pos != NULL);
    if (pos) {
        e.pos = *pos;
    }
    e.dueMs = timeGetTime() + static_cast<unsigned>(delaySec * 1000.0f);   // AR :230

    g_queue.push_back(e);   // AR :231
}

void EshDelayQueue_ProcessDue() {
    if (g_queue.empty()) {
        return;
    }

    const unsigned now = timeGetTime();   // AR :238 (CTimer::m_snTimeInMilliseconds)

    for (size_t i = 0; i < g_queue.size(); ) {
        EshDelayedEntry& e = g_queue[i];
        // Wrap-safe "now >= due": signed diff of unsigned millis stays correct
        // across the 49.7-day timeGetTime() wrap for spans < ~24.8 days.
        if (static_cast<int>(now - e.dueMs) >= 0) {
            EshSoundParams p = e.params;
            p.delaySec = 0.0f;   // delay consumed — play immediately
            EshVoicePool_PlayFile(e.path.c_str(), p, e.hasPos ? &e.pos : NULL);   // AR :243

            // Swap-pop erase (order between pending entries is irrelevant;
            // AR uses erase+remove_if :239-248). Do NOT advance i: slot i now
            // holds the previously-last entry and must be re-examined.
            g_queue[i] = g_queue.back();
            g_queue.pop_back();
        } else {
            ++i;
        }
    }
}

void EshDelayQueue_Clear() {
    g_queue.clear();
}
