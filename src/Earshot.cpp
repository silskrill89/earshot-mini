// Earshot_reminid — GTA San Andreas 1.0 US ASI plugin.
// Recreates the EarShot weapon-audio mod on the miniaudio engine.
//
// Plugin entry (thin, Lane C split): subscribes the plugin-sdk lifecycle
// events that drive the AR init flow (re-earshot-features.md B.1) and
// delegates everything else:
//   * game hooks + game-lifecycle events → src/hooks/EshHooks.{h,cpp}
//   * audio engine/bank                   → frozen façade src/audio/EarshotAudio.h
//
// Event availability (plugin-sdk shared/Events.h, GTASA section — verified):
//   initRwEvent      :368  (CdeclEvent @0x5BF3A1 H_CALL, PRIORITY_AFTER)
//   initGameEvent    :405  (CdeclEvent @0x748CFB H_CALL, PRIORITY_AFTER)
//   gameProcessEvent :404  (CdeclEvent @0x53E981 H_CALL, PRIORITY_AFTER)
// shutdownGameEvent / ClearForRestart / ClearExcitingStuffFromArea /
// MakePlayerSafe / WorldRemoveProj do NOT exist in plugin-sdk — EshHooks.cpp
// declares them with the same event machinery (AR ourCommon.h:22-26).
#include "plugin.h"

#include "audio/EarshotAudio.h"
#include "hooks/EshHooks.h"

using namespace plugin;

// ---------------------------------------------------------------------------
// Plugin entry
// ---------------------------------------------------------------------------
class EarshotPlugin {
public:
    EarshotPlugin() {
        // Stage 1 (AR Main.cpp:3047-3051, initRwEvent.after): EarShot.ini MAIN
        // section + miniaudio engine boot + generic-category load.
        Events::initRwEvent += []() {
            if (!EarshotAudio_Init()) {
                // Never MessageBox from init either (crash-parity C.2.2). The
                // façade logs every ma_result failure internally; hooks still
                // install and every façade play call will return false, so
                // each hook falls back to the vanilla audio path.
                OutputDebugStringA("[Earshot] audio init failed — hooks still install, "
                                   "all custom playback disabled (vanilla audio unaffected)\n");
            }
        };

        // Stage 2 (AR Main.cpp:3053-3058, initGameEvent.after): weapon-folder
        // scan + INI attenuation sections (façade PostInit), then all game
        // hooks + lifecycle events (EshHooks_InstallAll).
        Events::initGameEvent += []() {
            OutputDebugStringA("Earshot_reminid initialized (miniaudio engine)\n");
            EarshotAudio_PostInit();
            EshHooks_InstallAll();
        };

        // Per-frame (AR Main.cpp:3060+ gameProcessEvent): listener update,
        // delayed-sound scheduler, finished-voice sweep, pause-state checks.
        Events::gameProcessEvent += []() {
            EarshotAudio_OnFrame();
        };

        // Shutdown + restart/area-clear/projectile-removal events are owned by
        // EshHooks (see EshHooks_InstallAll) — they are not plugin-sdk events.
    }
} earshotPlugin;
