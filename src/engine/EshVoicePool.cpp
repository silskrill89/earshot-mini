// EshVoicePool.cpp — path-keyed buffer cache + bounded voice pool.
//
// AR parity map (external/EarShotOpenAL/source/AudioManager.cpp unless noted):
//   * gBufferMap path→buffer cache, decode once per path ... :418-432
//     (here: one cached ma_sound per path, MA_SOUND_FLAG_DECODE; per-play
//     voices are ma_sound_init_copy clones that share the decoded PCM —
//     ma_resource_manager_data_buffer_init_copy re-acquires the SAME decoded
//     buffer node by hashed name, miniaudio.h:71296-71308, so a clone is a
//     refcount acquire, not a PCM copy).
//   * PlaySource (3D one-shot) ............................. :561-640
//   * PlaySource2D (relative 2D, AL_SOURCE_RELATIVE) ....... :1567-1586
//   * stereo warning "!!!NO 3D SPATIALIZATION!!!" .......... :484
//   * PauseSource/ResumeSource (offset-preserving) ......... :1507-1523
//   * per-frame finished-source sweep ...................... Main.cpp:3244-3253
//
// miniaudio facts used (external/miniaudio/miniaudio.h, v0.11.25):
//   * ma_sound_init_copy :11393 (impl :78601) — flags=0 keeps spatialization
//     AND pitch; the flag enum only has disable switches (:11119-11133), and
//     the "no pitch" init flag would also kill doppler (docs :431-433).
//   * ma_attenuation_model_inverse == AL_INVERSE_DISTANCE_CLAMPED (:5155);
//     ma_attenuation_inverse :51664 clamps distance to [min,max] and returns
//     gain 1 when min >= max — the OpenAL degenerate guard
//     (re-openal-parity.md §3, alc/alu.cpp:1795-1809). No manual gain math
//     needed on our side.
//   * ma_positioning_relative :5161 + ma_sound_set_positioning :11430;
//     ma_sound_set_spatialization_enabled :11416 (2D, docs :1342-1352).
//   * A ma_sound IS a node: ma_node_attach_output_bus :10803 re-attaches
//     without an explicit detach (docs :2143); the two buses' channel counts
//     must match or it returns MA_INVALID_OPERATION (:74892-74894). Attach
//     BEFORE ma_sound_start (examples/engine_effects.c:85-91).
//   * ma_sound_stop :11401 only flips the node state — the data-source cursor
//     survives, so pause = get_cursor + stop, resume = seek + start.
//   * ma_sound_uninit waits for the audio thread to finish with the node's
//     buses (docs :2419-2460) — safe from the game thread.
#include "engine/EshVoicePool.h"

#include <Windows.h>

#include <new>
#include <stdio.h>   // _snprintf (CRT secure warnings disabled in premake5.lua)
#include <string>
#include <unordered_map>

#include "CVector.h"                 // plugin-sdk shared/game (struct CVector)
#include "audio/EshSoundParams.h"    // frozen per-play struct (premake includedirs has "src")

#include "engine/EshAudioEngine.h"
#include "engine/EshDelayQueue.h"

namespace {

constexpr unsigned kDefaultVoiceCount = 32;   // task: Init(0) → 32
constexpr unsigned kMaxVoiceCount     = 128;  // bound the per-slot node allocations

// Per-slot air-absorption filter geometry (see PlayNow for the mapping).
constexpr float kAirAbsMinCutoffHz = 400.0f;
constexpr float kAirAbsRefCutoffHz = 20000.0f;   // cutoff at factor 1.0

struct EshVoiceSlot {
    ma_sound    sound{};          // bound per play via ma_sound_init_copy
    ma_lpf_node lpf{};            // persistent per-slot air-absorption filter
    bool        lpfReady = false;
    bool        bound    = false;   // `sound` holds an initialized ma_sound
    bool        inUse    = false;   // played, not yet swept back to free
    bool        paused   = false;
    bool        is2D     = false;
    ma_uint64   pauseCursor = 0;
};

EshVoiceSlot* g_voices     = nullptr;
unsigned      g_voiceCount = 0;
unsigned      g_stealCursor = 0;
bool          g_ready      = false;
bool          g_shutdown   = false;   // Shutdown is final (game exit only)

std::unordered_map<std::string, ma_sound*> g_bufferCache;   // AR gBufferMap :422-432

void PoolLog(const char* msg) {
    OutputDebugStringA(msg);
}

// Transparent LPF cutoff kept strictly below Nyquist: a biquad LPF pushed to
// sampleRate/2 degenerates (alpha → 0, division by zero in the coefficient
// math), so the "no air absorption" ceiling is 0.45 × device rate.
double TransparentCutoffHz(ma_uint32 sampleRate) {
    return 0.45 * (double)sampleRate;
}

// --- buffer cache (AR CreateOpenALBufferFromAudioFile :418-432) -----------
// Returns the cached source sound for `path`, decoding it on first use.
// The first load is synchronous (AR DecodeWAV :251 is synchronous too) —
// short one-shot wavs only; later plays hit the cache.
ma_sound* GetOrLoadSource(const char* path) {
    std::unordered_map<std::string, ma_sound*>::const_iterator it = g_bufferCache.find(path);
    if (it != g_bufferCache.end()) {
        return it->second;   // AR :422-427 cache hit
    }

    EshAudioEngine& eng = EshAudioEngine::Instance();
    ma_sound* src = new (std::nothrow) ma_sound{};
    if (!src) {
        PoolLog("[Earshot] VoicePool: out of memory for buffer cache entry\n");
        return nullptr;
    }

    // MA_SOUND_FLAG_DECODE: fully-decoded PCM in RAM — correct for short
    // one-shots (re-miniaudio.md §5, docs :1258-1262).
    ma_result result = ma_sound_init_from_file(eng.GetEngine(), path,
                                               MA_SOUND_FLAG_DECODE, NULL, NULL, src);   // :11391
    if (result != MA_SUCCESS) {
        PoolLog("[Earshot] VoicePool: ma_sound_init_from_file failed for \"");
        PoolLog(path);
        PoolLog("\": ");
        PoolLog(ma_result_description(result));
        PoolLog("\n");
        delete src;
        return nullptr;
    }

    // Mono validation warn (AR :484 logs "!!!NO 3D SPATIALIZATION!!!" for
    // stereo) — stereo still plays; 2D routing is the caller's job (pos==NULL).
    ma_uint32 channels = 0;
    if (ma_sound_get_data_format(src, NULL, &channels, NULL, NULL, 0) == MA_SUCCESS   // :11467
        && channels != 1) {
        PoolLog("[Earshot] WARNING: STEREO — no 3D spatialization for \"");
        PoolLog(path);
        PoolLog("\"\n");
    }

    g_bufferCache.emplace(path, src);
    return src;
}

// --- voice selection -------------------------------------------------------
// 1) a slot not in use; 2) an in-use slot whose sound already finished (the
// Tick sweep hasn't run yet); 3) round-robin steal of a playing voice — AR
// deletes and recreates sources in its sweep too (Main.cpp:3244-3253).
// Paused voices are skipped in passes 1-3 where possible: they hold a resume
// cursor (AR PauseSource :1507-1514). Returns -1 only when EVERY voice is
// paused.
int FindFreeVoice() {
    for (unsigned i = 0; i < g_voiceCount; ++i) {
        if (!g_voices[i].inUse) {
            return (int)i;
        }
    }
    for (unsigned i = 0; i < g_voiceCount; ++i) {
        const EshVoiceSlot& v = g_voices[i];
        if (v.bound && !v.paused
            && (ma_sound_at_end(&v.sound) || !ma_sound_is_playing(&v.sound))) {   // :11464 / :11459
            return (int)i;
        }
    }
    for (unsigned n = 0; n < g_voiceCount; ++n) {
        unsigned i = (g_stealCursor + n) % g_voiceCount;
        if (!g_voices[i].paused) {
            g_stealCursor = (i + 1) % g_voiceCount;
            return (int)i;
        }
    }
    return -1;
}

// --- immediate play (AR PlaySource :561-640 / PlaySource2D :1567-1586) -----
bool PlayNow(const char* path, const EshSoundParams& p, const CVector* pos) {
    if (!g_ready || g_shutdown || !path || !path[0]) {
        return false;
    }
    EshAudioEngine& eng = EshAudioEngine::Instance();
    if (!eng.IsReady()) {
        return false;
    }
    ma_engine* engine = eng.GetEngine();

    ma_sound* src = GetOrLoadSource(path);
    if (!src) {
        return false;
    }

    const int idx = FindFreeVoice();
    if (idx < 0) {
        PoolLog("[Earshot] VoicePool: every voice is paused — shot dropped\n");
        return false;
    }
    EshVoiceSlot& v = g_voices[idx];

    // (Re)bind the slot: release the previous clone, then clone the cached
    // source. flags = 0 → spatialization + pitch stay ON (they are required
    // for 3D positioning, per-shot pitch and doppler; see file header).
    if (v.bound) {
        ma_sound_uninit(&v.sound);   // :11397 — waits for the audio thread (docs :2419-2460)
        v.bound = false;
    }
    ma_result result = ma_sound_init_copy(engine, src, 0, NULL, &v.sound);   // :11393
    if (result != MA_SUCCESS) {
        PoolLog("[Earshot] VoicePool: ma_sound_init_copy failed: ");
        PoolLog(ma_result_description(result));
        PoolLog("\n");
        return false;
    }
    v.bound = true;

    // --- per-play params, AR PlaySource order :576-594 ----------------------
    ma_sound_set_volume(&v.sound, p.gain);                              // :11408 (AR SetSourceGain :578)
    ma_sound_set_pitch(&v.sound, p.pitch > 0.0f ? p.pitch : 1.0f);      // :11414 (AR :583-588; pitch must be > 0)
    ma_sound_set_looping(&v.sound, p.looping ? MA_TRUE : MA_FALSE);     // :11462 (AR :589)
    ma_sound_set_attenuation_model(&v.sound, ma_attenuation_model_inverse);   // :11428 == AL_INVERSE_DISTANCE_CLAMPED
    ma_sound_set_min_distance(&v.sound, p.refDist);                     // :11438 (AR SetSourceRefDist :590)
    ma_sound_set_max_distance(&v.sound, p.maxDist);                     // :11440 (AR SetSourceMaxDist :591)
    ma_sound_set_rolloff(&v.sound, p.rolloff);                          // :11432 (AR SetSourceRolloffFactor :592)

    // --- 2D vs 3D ------------------------------------------------------------
    if (pos == NULL || p.is2D) {
        // AR PlaySource2D :1580 (AL_SOURCE_RELATIVE). miniaudio: relative
        // positioning (:5161/:11430) pinned to the origin plus spatialization
        // off (:11416, docs :1342-1352) — the stereo image is preserved and
        // no distance attenuation applies. No per-frame pinning needed.
        ma_sound_set_positioning(&v.sound, ma_positioning_relative);
        ma_sound_set_position(&v.sound, 0.0f, 0.0f, 0.0f);
        ma_sound_set_spatialization_enabled(&v.sound, MA_FALSE);
        v.is2D = true;
    } else {
        ma_sound_set_position(&v.sound, pos->x, pos->y, pos->z);        // :11422 (AR :577)
        v.is2D = false;
    }

    // --- air absorption (AR SetSourceAirAbsorptionFactor :579 → :1612) ------
    // DIVERGENCE (documented): OpenAL applies a per-DISTANCE HF shelf
    // (re-openal-parity.md §1.2, alc/alu.cpp:1944-1948); the frozen per-play
    // param is a single resolved factor, so we approximate with a static
    // per-voice LPF: cutoff = 20 kHz × factor, clamped to
    // [400 Hz, 0.45×SR]. factor >= 1 → the voice bypasses the filter node
    // entirely (transparent path, no coloration).
    if (p.airAbsorption < 1.0f && v.lpfReady) {
        float cutoff = kAirAbsRefCutoffHz * p.airAbsorption;
        if (cutoff < kAirAbsMinCutoffHz) {
            cutoff = kAirAbsMinCutoffHz;
        }
        const double ceiling = TransparentCutoffHz(ma_engine_get_sample_rate(engine));   // :11361
        if ((double)cutoff > ceiling) {
            cutoff = (float)ceiling;
        }

        // Re-init the slot's filter coefficients for this play. Order and
        // channels are fixed at Init (ma_lpf_reinit rejects changing them,
        // miniaudio.h:48268-48281); only the cutoff moves. The voice is not
        // playing yet, and ma_lpf_node_reinit is the documented live-sweep
        // entry point (:10939).
        ma_lpf_config lcfg = ma_lpf_config_init(ma_format_f32,
                                                ma_engine_get_channels(engine),   // :11360
                                                ma_engine_get_sample_rate(engine),
                                                (double)cutoff, 2 /* order */);
        if (ma_lpf_node_reinit(&lcfg, &v.lpf) == MA_SUCCESS) {   // :10939
            ma_node_attach_output_bus(&v.sound, 0, &v.lpf, 0);   // voice → LPF (→ reverb → endpoint)
        } else {
            ma_node_attach_output_bus(&v.sound, 0, eng.GetVoiceAttachTarget(), 0);
        }
    } else {
        // Default attachment after init_copy is the endpoint (:78442);
        // re-attach to the voice bus target (reverb node or endpoint).
        ma_node_attach_output_bus(&v.sound, 0, eng.GetVoiceAttachTarget(), 0);
    }

    // --- start (engine_effects.c:88-91: attach BEFORE start) -----------------
    result = ma_sound_start(&v.sound);   // :11400
    if (result != MA_SUCCESS) {
        PoolLog("[Earshot] VoicePool: ma_sound_start failed: ");
        PoolLog(ma_result_description(result));
        PoolLog("\n");
        ma_sound_uninit(&v.sound);
        v.bound = false;
        return false;
    }

    v.inUse   = true;
    v.paused  = false;
    return true;
}

// --- finished-voice sweep (AR Main.cpp:3244-3253) ---------------------------
// AR erases every source whose state is neither PLAYING nor PAUSED. Here the
// slot's clone is uninited and the slot returns to the free state. Paused
// voices are never swept (they hold a resume cursor).
void SweepFinishedVoices() {
    for (unsigned i = 0; i < g_voiceCount; ++i) {
        EshVoiceSlot& v = g_voices[i];
        if (!v.inUse || v.paused || !v.bound) {
            continue;
        }
        if (ma_sound_at_end(&v.sound) || !ma_sound_is_playing(&v.sound)) {
            ma_sound_uninit(&v.sound);
            v.bound  = false;
            v.inUse  = false;
            v.is2D   = false;
        }
    }
}

} // namespace

bool EshVoicePool_IsReady() {
    return g_ready;
}

bool EshVoicePool_Init(unsigned voiceCount) {
    if (g_ready) {
        return true;
    }
    if (g_shutdown) {
        return false;   // shut down for good (game exit) — no re-init
    }

    EshAudioEngine& eng = EshAudioEngine::Instance();
    if (!eng.IsReady()) {
        PoolLog("[Earshot] EshVoicePool_Init: engine not ready\n");
        return false;
    }
    ma_engine* engine = eng.GetEngine();

    if (voiceCount == 0) {
        voiceCount = kDefaultVoiceCount;
    }
    if (voiceCount > kMaxVoiceCount) {
        PoolLog("[Earshot] EshVoicePool_Init: voice count clamped to 128\n");
        voiceCount = kMaxVoiceCount;
    }

    g_voices = new (std::nothrow) EshVoiceSlot[voiceCount];
    if (!g_voices) {
        PoolLog("[Earshot] EshVoicePool_Init: out of memory for voice slots\n");
        return false;
    }
    g_voiceCount = voiceCount;

    // Per-slot air-absorption LPF nodes, created upfront and pre-attached to
    // the voice bus target: voice → [LPF] → reverb → endpoint. The cutoff is
    // re-inited per play (PlayNow); when a play doesn't need the filter the
    // voice bypasses the node entirely.
    const ma_uint32 channels   = ma_engine_get_channels(engine);      // :11360
    const ma_uint32 sampleRate = ma_engine_get_sample_rate(engine);   // :11361
    ma_node* target = eng.GetVoiceAttachTarget();
    for (unsigned i = 0; i < g_voiceCount; ++i) {
        ma_lpf_node_config cfg = ma_lpf_node_config_init(channels, sampleRate,
                                                         TransparentCutoffHz(sampleRate),
                                                         2 /* order */);   // :10929
        ma_result r = ma_lpf_node_init(ma_engine_get_node_graph(engine), &cfg, NULL,
                                       &g_voices[i].lpf);   // :10938
        if (r == MA_SUCCESS) {
            g_voices[i].lpfReady = true;
            if (target) {
                ma_node_attach_output_bus(&g_voices[i].lpf, 0, target, 0);   // :10803
            }
        } else {
            // The filter is optional: the slot still plays, just without the
            // air-absorption coloration.
            PoolLog("[Earshot] EshVoicePool_Init: ma_lpf_node_init failed for a voice slot: ");
            PoolLog(ma_result_description(r));
            PoolLog("\n");
        }
    }

    g_ready = true;
    PoolLog("[Earshot] EshVoicePool ready: voices=");
    char buf[16];
    _snprintf(buf, sizeof(buf), "%u", (unsigned)g_voiceCount);
    buf[sizeof(buf) - 1] = '\0';
    PoolLog(buf);
    PoolLog(eng.HasReverb() ? " (global reverb inline)\n" : " (no reverb node)\n");
    return true;
}

bool EshVoicePool_PlayFile(const char* path, const EshSoundParams& p, const CVector* pos) {
    if (!g_ready || g_shutdown) {
        return false;
    }
    // Speed-of-sound propagation delay (AR ScheduleDelayedSound :226-233).
    if (p.delaySec > 0.0f) {
        EshDelayQueue_Enqueue(path, p, pos, p.delaySec);
        return true;
    }
    return PlayNow(path, p, pos);
}

void EshVoicePool_StopAll() {
    if (!g_ready) {
        return;
    }
    EshDelayQueue_Clear();   // scheduled-but-unplayed shots die with the scene

    for (unsigned i = 0; i < g_voiceCount; ++i) {
        EshVoiceSlot& v = g_voices[i];
        v.paused = false;
        v.inUse  = false;
        v.is2D   = false;
        if (v.bound) {
            ma_sound_stop(&v.sound);     // :11401
            ma_sound_uninit(&v.sound);   // :11397
            v.bound = false;
        }
    }
}

void EshVoicePool_PauseAll() {
    if (!g_ready) {
        return;
    }
    for (unsigned i = 0; i < g_voiceCount; ++i) {
        EshVoiceSlot& v = g_voices[i];
        if (!v.inUse || v.paused || !v.bound || !ma_sound_is_playing(&v.sound)) {
            continue;
        }
        // Offset-preserving pause (AR PauseSource :1507-1514: read
        // AL_SEC_OFFSET, then alSourcePause). ma_sound_stop only flips the
        // node state — the data-source cursor survives (:78774-78781).
        ma_sound_get_cursor_in_pcm_frames(&v.sound, &v.pauseCursor);   // :11468
        ma_sound_stop(&v.sound);                                       // :11401
        v.paused = true;
    }
}

void EshVoicePool_ResumeAll() {
    if (!g_ready) {
        return;
    }
    for (unsigned i = 0; i < g_voiceCount; ++i) {
        EshVoiceSlot& v = g_voices[i];
        if (!v.inUse || !v.paused || !v.bound) {
            continue;
        }
        // AR ResumeSource :1516-1523: restore AL_SEC_OFFSET, then play.
        ma_sound_seek_to_pcm_frame(&v.sound, v.pauseCursor);   // :11465
        ma_sound_start(&v.sound);                              // :11400
        v.paused = false;
    }
}

void EshVoicePool_Tick() {
    if (!g_ready || g_shutdown) {
        return;
    }
    EshDelayQueue_ProcessDue();   // AR ProcessScheduledSounds :235-249
    SweepFinishedVoices();        // AR Main.cpp:3244-3253
}

void EshVoicePool_Shutdown() {
    if (g_shutdown) {
        return;   // second call safe (task §8)
    }
    g_shutdown = true;

    if (g_ready) {
        // Stop + release every voice clone first (they hold refcounts on the
        // cached decoded buffers).
        for (unsigned i = 0; i < g_voiceCount; ++i) {
            EshVoiceSlot& v = g_voices[i];
            if (v.bound) {
                ma_sound_stop(&v.sound);
                ma_sound_uninit(&v.sound);
                v.bound = false;
            }
            v.inUse = false;
            v.paused = false;
            if (v.lpfReady) {
                ma_lpf_node_uninit(&v.lpf, NULL);   // :10940
                v.lpfReady = false;
            }
        }
        delete[] g_voices;
        g_voices     = nullptr;
        g_voiceCount = 0;

        // Release the buffer cache (AR frees its buffers at shutdown :210-213).
        for (std::unordered_map<std::string, ma_sound*>::iterator it = g_bufferCache.begin();
             it != g_bufferCache.end(); ++it) {
            ma_sound_uninit(it->second);
            delete it->second;
        }
        g_bufferCache.clear();
    }

    g_ready = false;
    PoolLog("[Earshot] EshVoicePool shut down (ma_engine left running — process lifetime)\n");
}
