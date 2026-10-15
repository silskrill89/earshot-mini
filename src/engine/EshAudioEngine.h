// EshAudioEngine.h — Earshot_reminid engine owner (ma_engine lifecycle +
// global reverb node).
//
// Salvaged from the MiniAudioO3DE gem's MiniAudioSystemComponent /
// MiniAudioRequests bus (see .serena/memories/re-o3de-salvage.md §1, §5):
// one ma_engine value-member on a singleton, no AZ/EBus machinery.
//
// Global reverb: AR (EarShotOpenAL) attaches ONE global EFX reverb to EVERY
// playing source each frame (AudioManager.cpp:1541-1552 AttachReverbToSource,
// called per instance from the Main.cpp sweep :3172; preset :1467-1486).
// miniaudio has no send/return graph, so the same audible result is ONE
// Freeverb-style node (extras/nodes/ma_reverb_node) inserted INLINE between
// every pool voice and the engine endpoint: voice → [LPF] → reverb → endpoint.
#pragma once

#include "miniaudio.h"
#include "extras/nodes/ma_reverb_node/ma_reverb_node.h"   // external/miniaudio on the include path

class EshAudioEngine {
public:
    // Process-lifetime singleton: never destroyed (see .cpp for the
    // audio-thread discipline rationale).
    static EshAudioEngine& Instance();

    // ma_engine_init(NULL, &engine) + SA coordinate-system setup + global
    // reverb node creation. Every ma_result failure is logged via
    // OutputDebugStringA before returning false (re-miniaudio.md §1:
    // "always check the result").
    bool Init();

    ma_engine* GetEngine() { return &m_engine; }
    bool IsReady() const { return m_ready; }

    // The reverb node (NULL until created). Voices attach here when
    // HasReverb() is true.
    ma_node* GetReverbNode() { return m_reverbReady ? (ma_node*)&m_reverb : NULL; }
    bool     HasReverb() const { return m_reverbReady; }

    // Where every pool voice's output bus attaches: the reverb node when it
    // exists, otherwise straight to the engine endpoint (device channel
    // count > 2 or sample rate < 22050 — verblib limits, verblib.h:9 and
    // :422-433). Returns NULL only before Init().
    ma_node* GetVoiceAttachTarget();

    // Global gain helpers (re-o3de-salvage.md §1, MiniAudioRequests::SetGlobalVolume).
    void SetGlobalVolume(float linear);
    float GetGlobalVolume() const;

private:
    EshAudioEngine() = default;

    ma_engine m_engine{};   // zero-init; only ma_engine_init may touch it
    bool m_ready = false;
    float m_globalVolume = 1.0f;

    ma_reverb_node m_reverb{};   // AR global EFX reverb → inline Freeverb node
    bool m_reverbReady = false;
};
