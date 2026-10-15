// EshSoundParams.h — per-play playback request struct shared by the loader
// layer (src/loaders), the façade (src/audio/EarshotAudio.cpp) and the engine
// layer (src/engine). Mirrors the AR (EarShotOpenAL) PlaySource parameter
// surface — re-earshot-features.md B.3, re-openal-parity.md §1.2/§1.5.
#pragma once

// CVector must be COMPLETE here: the two motion fields below carry default
// member initializers, and plugin-sdk's CVector declares user constructors
// (shared/game/CVector.h:34-40 — CVector(), CVector(float,float,float)), so
// `= {0,0,0}` resolves to the 3-float ctor instead of aggregate init. The
// façade/engine headers only FORWARD-declare `struct CVector` for by-ref/by-ptr
// parameters (EarshotAudio.h:14, EshVoicePool.h:15) — that pattern cannot back
// an NSDM, so this header pulls the real one (inline impl at the bottom of
// CVector.h:121 via CVectorImplementation.h; no .cpp symbol is emitted).
#include "CVector.h"   // plugin-sdk shared/game (on premake5.lua's include path)

struct EshSoundParams {
    float gain          = 1.0f;   // AL_GAIN
    float pitch         = 1.0f;   // FINAL playback pitch (slow-mo clamp × .earshot
                                  // class pitch — resolved by the caller, engine
                                  // does not touch CTimer)
    float refDist       = 3.0f;   // AL_REFERENCE_DISTANCE
    float maxDist       = 150.0f; // AL_MAX_DISTANCE
    float rolloff       = 1.0f;   // AL_ROLLOFF_FACTOR
    float airAbsorption = 1.0f;   // AL_ABSORPTION (1 = none; <1 = more HF loss)
    bool  looping       = false;  // AL_LOOPING (fire/missile/ambience loops)
    bool  is2D          = false;  // AL_SOURCE_RELATIVE — stereo/whizz/goggles,
                                  // no world position, always at the listener
    float delaySec      = 0.0f;   // speed-of-sound propagation delay; the engine
                                  // scheduler caps this at 5 s (AR AudioManager
                                  // .cpp:229)
    bool  reverb        = true;   // global reverb send (AR attaches the one EFX
                                  // reverb to EVERY playing source each frame)

    // --- realism inputs (doppler + incidence coloration) --------------------
    // PASS-THROUGH fields: caller-provided only. There is NO INI key and NO
    // .earshot-bank default for either one — the hooks lane (EshHooks.cpp
    // FillSourceMotion) writes them per play and the façade copies them
    // verbatim into the play params (`EshSoundParams p = overrides;` in
    // EarshotAudio.cpp PlayWeaponStem/PlayGenericStem), exactly like
    // pitch/delaySec ride along untouched by ApplyAttenuation. Leaving them at
    // the defaults below is always legal and means "no effect".
    CVector velocity = {0,0,0};    // source velocity, WORLD UNITS/SECOND.
                                  // ma_sound_set_velocity (miniaudio.h:11426)
                                  // → doppler. {0,0,0} = static source; the
                                  // engine skips the set and doppler stays at
                                  // the pitch-1 baseline. SA's m_vecMoveSpeed
                                  // is per-timestep (1/50 s), so hooks convert
                                  // ×50 — see EshHooks.cpp FillSourceMotion.
    CVector forward  = {0,0,0};    // source FACING direction at spawn, world
                                  // axes; normalised by the engine if needed.
                                  // Drives incidence coloration (listener in
                                  // FRONT = bright, BEHIND = muffled).
                                  // {0,0,0} = omni-directional → effect skipped.
};
