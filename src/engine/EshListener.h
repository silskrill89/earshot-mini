// EshListener.h — per-frame listener update (game thread → miniaudio listener 0).
//
// Salvaged from MiniAudioO3DE's MiniAudioListenerComponentController
// (re-o3de-salvage.md §2) — but driven by a per-frame poll of TheCamera
// instead of an AZ transform event (§6: the game has no change events worth
// subscribing to).
#pragma once

// Called once per frame from the game thread (wired to
// Events::gameProcessEvent in src/Earshot.cpp). Pushes listener position,
// forward direction and velocity into miniaudio listener 0.
//
// No-op until EshAudioEngine::Init() has succeeded.
void EshListener_Update();
