// EshEffects.cpp — occlusion + incidence coloration (target-cutoff math and
// the INI-backed runtime config).
//
// Citations
//   * Occlusion raycast — CWorld::GetIsLineOfSightClear(origin, target,
//       buildings, vehicles, peds, objects, dummies, doSeeThroughCheck,
//       doCameraIgnoreCheck):
//         plugin-sdk CWorld.h:152 (declaration, static, PUBLIC — the class has
//           no access specifier between :42 and :159),
//         plugin-sdk CWorld.cpp:410-413 (definition = cdecl thunk @0x56A490),
//         gta-reversed-modern game_sa/World.h:184 + World.cpp:2522 (impl),
//         World.cpp:102 `RH_ScopedInstall(GetIsLineOfSightClear, 0x56A490)`.
//       The definition lives in PSDK CWorld.cpp, which premake5.lua does NOT
//       compile (headers-only SDK; only DynAddress/GameVersion/Patch), so the
//       call is issued through plugin::CallAndReturn against the raw 1.0 US
//       address — the same idiom plugin-sdk itself uses there.
//   * Incidence axis — CMatrix::GetForward() returns the matrix's `up` row
//       (plugin-sdk CMatrix.h:73-74); the game projects motion onto exactly
//       that row for its own audio (gta-reversed AEVehicleAudioEntity.cpp:2417
//       `DotProduct(physical->m_vecMoveSpeed, physical->m_matrix->GetForward())`).
//   * CVector helpers — shared/game/CVector.h:69 Dot, :73 MagnitudeSqr,
//       :87 Normalize (inline impl via CVectorImplementation.h).
#include "engine/EshEffects.h"

#include "plugin.h"        // plugin::CallAndReturn (header-only thunk machinery)

#include "CVector.h"       // complete type (also pulled by EshSoundParams.h)
#include "CWorld.h"        // documents the callee; its .cpp is NOT linked

namespace {

// [MAIN] INI-backed state (defaults = EarshotAudio.cpp ReadIniStage1 reads).
bool  s_occlusionEnabled  = true;
bool  s_incidenceEnabled  = true;
float s_occlusionCutoffHz = 700.0f;    // INI "Occlusion cutoff"
float s_incidenceCutoffHz = 1200.0f;   // INI "Incidence cutoff"

// Clamp a user-supplied cutoff into the usable biquad range.
float ClampCutoffHz(float hz) {
    if (!(hz > kEshEffectsMinCutoffHz)) {          // NaN-safe: NaN fails the test
        return kEshEffectsMinCutoffHz;
    }
    if (hz > kEshEffectsOpenCutoffHz) {
        return kEshEffectsOpenCutoffHz;
    }
    return hz;
}

// One world line-of-sight test, listener → voice.
// Flag choice (simplest correct call, gta-reversed semantics):
//   buildings=true    — walls/roofs block (the point of the effect)
//   vehicles=true     — a truck between source and listener muffles
//   peds=false        — a stray ped must not muffle the sound; also keeps the
//                       test cheap and avoids self-occlusion in crowds
//   objects=true      — containers, fences, mappers' statics
//   dummies=true      — GTA "dummy" collision objects (World.cpp:2522 passes
//                       them straight to GetIsLineOfSightSectorClear)
//   doSeeThroughCheck=false — glass counts as a blocker (a closed window does
//                       muffle HF; keeps the flag list at the documented seven)
//   doCameraIgnoreCheck=false — no camera-ignored entity special case
bool IsLineOfSightClear(const CVector& origin, const CVector& target) {
    return plugin::CallAndReturn<bool, 0x56A490,
                                 const CVector&, const CVector&,
                                 bool, bool, bool, bool, bool, bool, bool>(
        origin, target, true, true, false, true, true, false, false);
}

} // namespace

// --- config ----------------------------------------------------------------
void EshEffects_SetOcclusionEnabled(bool on) { s_occlusionEnabled = on; }
bool EshEffects_OcclusionEnabled()           { return s_occlusionEnabled; }

void EshEffects_SetIncidenceEnabled(bool on) { s_incidenceEnabled = on; }
bool EshEffects_IncidenceEnabled()           { return s_incidenceEnabled; }

void  EshEffects_SetOcclusionCutoff(float hz) { s_occlusionCutoffHz = ClampCutoffHz(hz); }
float EshEffects_OcclusionCutoff()            { return s_occlusionCutoffHz; }

void  EshEffects_SetIncidenceCutoff(float hz) { s_incidenceCutoffHz = ClampCutoffHz(hz); }
float EshEffects_IncidenceCutoff()            { return s_incidenceCutoffHz; }

// --- per-voice target cutoff ------------------------------------------------
float EshEffects_ComputeColorationCutoff(const CVector& listenerPos,
                                         const CVector& voicePos,
                                         const CVector& forward) {
    if (!s_occlusionEnabled && !s_incidenceEnabled) {
        return kEshEffectsOpenCutoffHz;   // both effects off → skip every game call
    }

    // --- occlusion: LOS listener → voice ------------------------------------
    float occlusionCutoff = kEshEffectsOpenCutoffHz;
    if (s_occlusionEnabled) {
        const float distSqr = (voicePos - listenerPos).MagnitudeSqr();
        if (distSqr <= 1e-6f) {
            // Degenerate: listener sitting on the source — no ray to cast.
        } else if (distSqr > kEshOcclusionTestMaxDist * kEshOcclusionTestMaxDist) {
            // Cost guard: raycast work grows with the sector span it crosses,
            // and past kEshOcclusionTestMaxDist the source is already hammered
            // by distance attenuation + air absorption. Treated as OPEN
            // (documented divergence, see the header).
        } else if (!IsLineOfSightClear(listenerPos, voicePos)) {
            occlusionCutoff = s_occlusionCutoffHz;
        }
    }

    // --- incidence: listener position relative to the source's facing --------
    float incidenceCutoff = kEshEffectsOpenCutoffHz;
    if (s_incidenceEnabled) {
        CVector front = forward;
        CVector toListener = listenerPos - voicePos;   // SOURCE → listener
        const float frontMag = front.Magnitude();
        const float distMag  = toListener.Magnitude();
        if (frontMag > 1e-4f && distMag > 1e-4f) {
            front.Normalize();
            toListener.Normalize();

            float cosTheta = toListener.Dot(front);    // +1 = listener in FRONT
            if (cosTheta >  1.0f) cosTheta =  1.0f;    // (normalised inputs, but
            if (cosTheta < -1.0f) cosTheta = -1.0f;     //  clamp anyway)

            const float t = (cosTheta + 1.0f) * 0.5f;  // 0 = behind, 1 = front
            const float s = t * t * (3.0f - 2.0f * t); // smoothstep ("softstep")
            incidenceCutoff = s_incidenceCutoffHz
                            + (kEshEffectsOpenCutoffHz - s_incidenceCutoffHz) * s;
        }
        // Degenerate forward (omni) or coincident positions → leave "open".
    }

    return (occlusionCutoff < incidenceCutoff) ? occlusionCutoff
                                               : incidenceCutoff;
}
