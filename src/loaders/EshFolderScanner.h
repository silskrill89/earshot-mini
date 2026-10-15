// EshFolderScanner.h — filesystem walking for the loader lane: mod-folder
// resolve, recursive .earshot scan + weapon registration + marker tuning
// parse + weapon-folder stem scan, and the M1 generic category loaders.
//
// Ported from the AR reference (external/EarShotOpenAL/source/Loaders.cpp):
//   * folder resolve — Main.cpp:3035-3036 + ourCommon.h:58-60
//   * .earshot scan / registerWeapon — Loaders.cpp:1006-1210, Loaders.h:65-117
//   * marker tuning parse — Loaders.cpp:1029-1199
//   * generic loaders — tank cannon :7-46, bullet whizz :401-452,
//     ricochet :1329-1377, gunshells :1379-1454, explosions :1783-1910
#pragma once
#include <string>
class EshSoundBank;

namespace EshFolderScanner {

// plugin-sdk Paths.cpp semantics (shared/extensions/Paths.cpp:13-57),
// reimplemented here because premake5.lua does not compile
// shared/extensions/Paths.cpp (headers-only SDK — see src/Earshot.cpp shim note).
std::string PluginDirA();   // dir of this ASI, trailing backslash
std::string GameDirA();     // dir of gta_sa.exe, trailing backslash

// Mod root (AR Main.cpp:3035-3036 + ourCommon.h:58-60):
// "<game>/data/EarShot" preferred; else "<plugin dir>/EarShot" (created).
std::string ResolveModFolder();

// Stage 2: recursive .earshot scan -> registerWeapon (HZ rule: weapon type
// must be > WEAPONTYPE_UNARMED — bug #9 fix, ourCommon.h:91 vs HZ.cpp:153-160)
// + marker tuning parse + weapon-folder stem scan (M1 classes).
// Returns the number of newly registered weapons.
int ScanWeapons(const char* modFolder, EshSoundBank& bank);

// Stage 1 generic loaders (M1 set). Each logs and no-ops when its folder is
// missing (AR loader behavior). Tuning defaults are seeded here; the façade's
// INI stage-2 pass overwrites them (AR order: AudioManager.Initialize at
// stage 1, InitializeIniFile(2) at stage 2 — Main.cpp:3047-3058).
void LoadExplosions(const char* modFolder, EshSoundBank& bank);    // Loaders.cpp:1783-1910
void LoadRicochet(const char* modFolder, EshSoundBank& bank);      // Loaders.cpp:1329-1377
void LoadGunshells(const char* modFolder, EshSoundBank& bank);     // Loaders.cpp:1379-1454
void LoadBulletWhizz(const char* modFolder, EshSoundBank& bank);   // Loaders.cpp:401-452
void LoadTankCannon(const char* modFolder, EshSoundBank& bank);    // Loaders.cpp:7-46

} // namespace EshFolderScanner
