// EshListener.h — per-frame listener update (game thread → miniaudio listener 0).
//
// Salvaged from MiniAudioO3DE's MiniAudioListenerComponentController
// (re-o3de-salvage.md §2) — but driven by a per-frame poll of TheCamera
// instead of an AZ transform event (§6: the game has no change events worth
// subscribing to).
#pragma once

struct CVector;   // plugin-sdk shared/game/CVector.h (complete type in callers)

// Called once per frame from the game thread (wired to
// Events::gameProcessEvent in src/Earshot.cpp). Pushes listener position,
// forward direction and velocity into miniaudio listener 0
// (ma_engine_listener_set_position :11372 / _set_direction :11374 /
//  _set_velocity :11376).
//
// No-op until EshAudioEngine::Init() has succeeded.
void EshListener_Update();

// Listener world position captured by the most recent EshListener_Update —
// the "start" point for the occlusion raycast (EshEffects). Reference aliases
// the engine layer's own storage; copy it out if you need to keep it (same
// rule as CMatrix::GetForward). Returns {0,0,0} until the first update has
// run (game thread only).
const CVector& EshListener_GetPosition();
