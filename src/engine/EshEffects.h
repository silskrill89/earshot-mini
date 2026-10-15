// EshEffects.h — "coloration" realism effects: occlusion muffling (line of
// sight blocked by buildings/vehicles/objects) + incidence-angle coloration
// (source facing away from the listener is muffled), plus their INI-backed
// runtime config.
//
// Both effects collapse into ONE number — the target low-pass cutoff in Hz for
// the per-voice coloration ma_lpf_node owned by EshVoicePool (chain:
// voice → [air-absorption LPF] → [coloration LPF] → reverb/endpoint).
//
// Config ownership: statics live HERE (engine layer), NOT in src/loaders, so a
// future ImGui settings panel can flip them at runtime through the setters
// below without touching the loader lane. The [MAIN] INI keys are read once by
// EarshotAudio.cpp ReadIniStage1 and pushed through the same setters
// (reloaded by EarshotAudio_ReloadFolders).
//
//   INI [MAIN] key     setter                        default
//   Occlusion          EshEffects_SetOcclusion...    1      (0/1 enable)
//   Occlusion cutoff   EshEffects_SetOcclusionCutoff 700 Hz (blocked cutoff)
//   Incidence          EshEffects_SetIncidence...    1      (0/1 enable)
//   Incidence cutoff   EshEffects_SetIncidenceCutoff 1200 Hz (rear cutoff)
//
// Game-thread only: EshEffects_ComputeColorationCutoff performs a world
// line-of-sight raycast (CWorld::GetIsLineOfSightClear @0x56A490, 1.0 US).
// EshVoicePool_Tick throttles it to ~200 ms per voice.
#pragma once

struct CVector;   // plugin-sdk shared/game/CVector.h (complete type in callers)

// --- enable toggles (INI "Occlusion" / "Incidence", both default on) --------
void  EshEffects_SetOcclusionEnabled(bool on);
bool  EshEffects_OcclusionEnabled();
void  EshEffects_SetIncidenceEnabled(bool on);
bool  EshEffects_IncidenceEnabled();

// --- muffling cutoffs (INI "Occlusion cutoff" 700 / "Incidence cutoff" 1200).
// Values are clamped into [kEshEffectsMinCutoffHz, kEshEffectsOpenCutoffHz].
void  EshEffects_SetOcclusionCutoff(float hz);
float EshEffects_OcclusionCutoff();
void  EshEffects_SetIncidenceCutoff(float hz);
float EshEffects_IncidenceCutoff();

// "Effectively open" cutoff — used for the clear-LOS / listener-in-front end
// of both effects, and as the voice node's resting cutoff.
constexpr float kEshEffectsOpenCutoffHz = 20000.0f;
// Floor for the configured cutoffs (a degenerate 0 Hz cutoff would make the
// 10%-change detector in EshVoicePool divide by ~0 and could starve the
// biquad coefficient math).
constexpr float kEshEffectsMinCutoffHz  = 50.0f;

// Compute the target coloration cutoff (Hz) for ONE 3D voice.
//   listenerPos — current listener (camera) world position
//   voicePos    — the voice's world position at play time
//   forward     — the source's facing vector (EshSoundParams::forward);
//                 {0,0,0} / degenerate = omni → incidence contributes "open"
// Returns min(occlusionCutoff, incidenceCutoff), already inside
// [kEshEffectsMinCutoffHz, kEshEffectsOpenCutoffHz].
//
// Semantics:
//   occlusion — LOS listener→voice. Blocked = configured cutoff, clear = open.
//               Skipped (→ open) when the two positions coincide, when the
//               caller's enable flag is off, and beyond kEshOcclusionTestMaxDist
//               (raycast cost guard — far sources are already attenuated).
//   incidence — cosθ of (source→listener) vs normalised forward:
//               +1 listener in FRONT → open, -1 BEHIND → configured cutoff,
//               smoothstep(softstep) lerp in between, clamped.
float EshEffects_ComputeColorationCutoff(const CVector& listenerPos,
                                         const CVector& voicePos,
                                         const CVector& forward);

// Raycast budget guard: beyond this world-unit distance the LOS test is
// skipped and occlusion contributes "open" (documented divergence — see the
// return notes). 300 units ≈ 2× the default maxDist (150).
constexpr float kEshOcclusionTestMaxDist = 300.0f;
