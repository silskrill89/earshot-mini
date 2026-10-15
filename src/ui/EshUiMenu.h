// EshUiMenu.h — Earshot debug/editor menu (M1) public surface.
//
// Self-installing module: src/ui/EshUiMenu.cpp owns its own render hook and
// installs it from a CRT static constructor (precedent:
// src/debug/EshCrashHandler.cpp:652-668), so no other translation unit has to
// know this module exists. Nothing in src/hooks/, src/audio/ or src/Earshot.cpp
// is touched by the UI.
//
// M1 scope: Ctrl+5 toggle, three read-only/low-risk panels (Status, Settings,
// Sounds). Future work (load new sound files from this UI, mute individual
// sounds) slots into the "Sound file loading (coming soon)" section of the
// Settings panel.
#pragma once

// Toggle menu visibility. The render hook polls Ctrl+5 itself (edge detect,
// skygfx src/core/main.cpp:2568-2586 uses Ctrl+4); this is the seam future
// callers (INI key binding, other hooks) will use.
void EshUi_Toggle();

// True while the menu is open. The render hook only builds/draws ImGui frames
// for the menu when this is true.
bool EshUi_IsOpen();
