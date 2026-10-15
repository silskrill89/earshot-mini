// EshSoundParams.h — per-play playback request struct shared by the loader
// layer (src/loaders), the façade (src/audio/EarshotAudio.cpp) and the engine
// layer (src/engine). Mirrors the AR (EarShotOpenAL) PlaySource parameter
// surface — re-earshot-features.md B.3, re-openal-parity.md §1.2/§1.5.
#pragma once

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
};
