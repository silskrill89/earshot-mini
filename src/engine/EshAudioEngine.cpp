// EshAudioEngine.cpp — ma_engine lifecycle + global reverb node for
// Earshot_reminid.
//
// References:
//   * .serena/memories/re-miniaudio.md §1 — engine bootstrap recipe, world-up.
//   * .serena/memories/re-o3de-salvage.md §5 — de-EBus class split.
//   * AR reverb preset: external/EarShotOpenAL/source/AudioManager.cpp
//     :1467-1486 (InitReverb), attach-per-source :1541-1552 + Main.cpp:3172.
#include "engine/EshAudioEngine.h"

#include <Windows.h>
#include <math.h>   // exp

namespace {

// Tiny log helper — OutputDebugStringA only; never blocks (re-earshot-features
// C.2.2: no MessageBox/log-dialog from hook context).
void EngineLog(const char* msg) {
    OutputDebugStringA(msg);
}

// Invert the verblib decay-time formula (extras/nodes/ma_reverb_node/
// verblib.h:678-690) to find the roomSize whose Freeverb tail best matches an
// EFX AL_REVERB_DECAY_TIME:
//   decayFrames = (80 / |−20·ln(1/roomsize1)|) × (combR7.bufsize × 2)
//   roomsize1   = 0.28·roomSize + 0.7            (verblib_set_room_size :599-603)
//   combR7.bufsize ≈ 1640·SR/44100               (verblib_combtuningR7 1617+23, :157-171)
// Result clamps to [0,1]. For AR's 0.6 s the clamp binds at 0.0 — verblib's
// shortest possible decay is ~0.83 s (roomSize 0 → comb feedback 0.7), longer
// than AR's 0.6 s; the heavy damping below shortens the audible HF tail
// toward AR's intent (divergence documented at the call site).
float RoomSizeFromDecaySeconds(float decaySec, ma_uint32 sampleRate) {
    if (decaySec <= 0.0f || sampleRate == 0) {
        return 0.0f;
    }
    const double combR7x2     = 2.0 * (1617.0 + 23.0) * (double)sampleRate / 44100.0;
    const double targetFrames = (double)decaySec * (double)sampleRate;
    const double lnTerm       = 80.0 * combR7x2 / targetFrames;   // = 20·ln(1/roomsize1)
    const double roomsize1    = exp(-lnTerm / 20.0);
    double roomSize = (roomsize1 - 0.7) / 0.28;
    if (roomSize < 0.0) roomSize = 0.0;
    if (roomSize > 1.0) roomSize = 1.0;
    return (float)roomSize;
}

} // namespace

EshAudioEngine& EshAudioEngine::Instance() {
    static EshAudioEngine s_instance;
    return s_instance;
}

bool EshAudioEngine::Init() {
    if (m_ready) {
        return true;
    }

    // Default config: 1 listener, device-native channels/rate, device-owned
    // mixing thread (re-miniaudio.md §1). Spatialization is ON by default for
    // every sound — no "enable 3D" flag exists.
    //
    // DOPPLER stays enabled by these defaults (verified against miniaudio.h
    // v0.11.25 — nothing below turns it off):
    //   * ma_engine_init(NULL, ...) → ma_engine_config_init() keeps the default
    //     linear pitchResampling (:77462-77464) — the resampler the engine node
    //     uses to apply dopplerPitch (:76607-76616).
    //   * every sound's spatializer gets dopplerFactor 1 from
    //     ma_spatializer_config_init (:52070; 0 would disable it).
    //   * speed of sound 343.3 (OpenAL parity) is set explicitly below
    //     (default already 343.3, :51758).
    // Per-voice/listener velocities are pushed by EshVoicePool (source) and
    // EshListener (listener).
    ma_result result = ma_engine_init(NULL, &m_engine);
    if (result != MA_SUCCESS) {
        EngineLog("[Earshot] FATAL: ma_engine_init failed: ");
        EngineLog(ma_result_description(result));
        EngineLog("\n");
        return false;   // re-miniaudio.md §7.8: init can fail (MA_OUT_OF_MEMORY) — never assume it's live
    }

    // GTA SA is a Z-up world; miniaudio defaults to OpenGL-style +Y up
    // (re-miniaudio.md §1, miniaudio.h:51754). Set ONCE after init:
    // listener up = +Z. Signature: (ma_engine*, listenerIndex, x, y, z)
    // (miniaudio.h:11380).
    ma_engine_listener_set_world_up(&m_engine, 0, 0.0f, 0.0f, 1.0f);

    // Speed of sound has NO engine-level wrapper — reach into the public
    // listener struct (re-miniaudio.md §2). Default is already 343.3 m/s,
    // same as OpenAL; set it explicitly so the intent is documented.
    if (m_engine.listenerCount > 0) {
        ma_spatializer_listener_set_speed_of_sound(&m_engine.listeners[0], 343.3f);
    }

    m_ready = true;
    EngineLog("[Earshot] EshAudioEngine ready (miniaudio engine up, world_up=+Z)\n");

    // --- global reverb node (AR InitReverb, AudioManager.cpp:1467-1486) ------
    // AR attaches ONE global EFX reverb to EVERY playing source each frame
    // (AttachReverbToSource :1541-1552, called per instance in the Main.cpp
    // sweep :3172). miniaudio has no auxiliary-send graph, so the same
    // audible result is an INLINE node: every voice → reverb → endpoint.
    //
    // verblib limits: 1-2 channels (verblib.h:422-425) and sample rate
    // >= 22050 (:426-429). Setups outside that skip the reverb (logged) and
    // voices attach straight to the endpoint.
    const ma_uint32 channels   = ma_engine_get_channels(&m_engine);      // :11360
    const ma_uint32 sampleRate = ma_engine_get_sample_rate(&m_engine);   // :11361
    if ((channels == 1 || channels == 2) && sampleRate >= 22050) {
        ma_reverb_node_config cfg = ma_reverb_node_config_init(channels, sampleRate);   // ma_reverb_node.h:28
        //
        // AR preset (AudioManager.cpp:1475-1483) → Freeverb/verblib mapping.
        // DIVERGENCE (documented): EFX reverb is an early/late model with
        // density/diffusion/room-rolloff knobs; verblib is Freeverb (8 combs
        // + 4 allpasses, verblib.h:134-135) with roomSize/damping/width/wet/
        // dry. Knob-shape parity, not DSP parity:
        //   wet     = 0.3  ← AL_REVERB_GAIN 0.3 (:1477, overall reverb level)
        //   dry     = 0.7  ← the inline node must pass the dry signal (AR's
        //                    EFX send is parallel: dry 1.0 + wet added on top;
        //                    dry 0.7 compensates the added wet energy so the
        //                    combined mix level stays sane)
        //   roomSize       ← AL_REVERB_DECAY_TIME 0.6 s (:1476) via the
        //                    inverted verblib decay formula (helper above);
        //                    clamps to 0.0 → tail ≈ 0.83 s vs AR's 0.6 s
        //   damping = 0.7  ← AL_REVERB_GAINHF 0.3 (:1478, heavy HF damping),
        //                    linear inversion (1 − gainHF). EFX gainHF is a
        //                    shelf gain, Freeverb damping a comb-feedback LPF
        //                    coefficient — intent-level match, not 1:1.
        //   width   = 1.0  ← verblib default (plain EFX reverb has no width)
        //   mode    = 0    ← normal (no freeze)
        //   Dropped — no verblib equivalent: DENSITY 0.2 (:1475),
        //   DECAY_HFRATIO 0.3 (:1479), REFLECTIONS_GAIN 0.4 (:1480),
        //   LATE_REVERB_GAIN 0.3 (:1481), LATE_REVERB_DELAY 5 ms (:1482),
        //   ROOM_ROLLOFF_FACTOR 0.3 (:1483); their level intent is folded
        //   into wet/dry.
        cfg.wetVolume = 0.3f;
        cfg.dryVolume = 0.7f;
        cfg.roomSize  = RoomSizeFromDecaySeconds(0.6f, sampleRate);
        cfg.damping   = 0.7f;
        cfg.width     = 1.0f;
        cfg.mode      = 0.0f;

        result = ma_reverb_node_init(ma_engine_get_node_graph(&m_engine), &cfg, NULL,
                                     &m_reverb);   // ma_reverb_node.h:37
        if (result == MA_SUCCESS) {
            // reverb → endpoint (examples/engine_effects.c:72 pattern).
            ma_node_attach_output_bus((ma_node*)&m_reverb, 0,
                                      ma_engine_get_endpoint(&m_engine), 0);   // :10803 / :11353
            m_reverbReady = true;
            EngineLog("[Earshot] global reverb node up (Freeverb inline, wet .3 dry .7 damp .7)\n");
        } else {
            EngineLog("[Earshot] ma_reverb_node_init failed: ");
            EngineLog(ma_result_description(result));
            EngineLog(" — voices will bypass reverb\n");
        }
    } else {
        EngineLog("[Earshot] reverb node skipped (device channels/rate outside verblib 1-2ch @>=22050Hz)\n");
    }

    return true;
}

ma_node* EshAudioEngine::GetVoiceAttachTarget() {
    if (!m_ready) {
        return NULL;
    }
    if (m_reverbReady) {
        return (ma_node*)&m_reverb;
    }
    return ma_engine_get_endpoint(&m_engine);   // :11353
}

void EshAudioEngine::SetGlobalVolume(float linear) {
    m_globalVolume = linear;
    if (m_ready) {
        ma_engine_set_volume(&m_engine, linear);   // miniaudio.h:11365
    }
}

float EshAudioEngine::GetGlobalVolume() const {
    return m_globalVolume;
}
