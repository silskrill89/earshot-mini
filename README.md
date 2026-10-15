# EarShot mini — EarShot + miniaudio

GTA San Andreas (1.0 US) weapon-audio ASI plugin. A rebuild of **EarShot**
(HzanRsxa2959) on the **[miniaudio](https://github.com/mackron/miniaudio)**
engine — dynamic gunshots, after-shot echo, reverb, 3D spatialization and
distance attenuation, with **no OpenAL runtime required** (no OpenAL32.dll,
no alsoft.ini, no installers).

The folder layout and `EarShot.ini` keys are the same as **EarShotOpenAL**,
so existing sound packs (VC weapon pack, Manhunt, Driv3r, CS1.6, ...) work
unmodified.

## Install (for players)

1. **Remove** the old `EarShot.asi` (EarShotOpenAL) — do not run both mods at
   once. `OpenAL32.dll` / `alsoft.ini` are no longer needed by EarShot.
2. Copy everything from **[`dist/earshot_reminid_v0.1.0-alpha1/`](dist/earshot_reminid_v0.1.0-alpha1/)**
   (`earshot_reminid.asi`, `EarShot.ini`) into your game folder — next to
   `gta_sa.exe`, or into `scripts\`, or a ModLoader subfolder.
3. Sound content folder: `GameFolder\data\EarShot\` is used automatically if
   it exists; otherwise `GameFolder\EarShot\` (next to the .asi) is created
   on first run — drop your packs in there.
4. Start the game.

Full instructions, file-structure reference and troubleshooting:
**[dist/earshot_reminid_v0.1.0-alpha1/readme.txt](dist/earshot_reminid_v0.1.0-alpha1/readme.txt)**
(same format as the old EarShotOpenAL readme).

The log file is `EarshotReminid.log` next to the .asi. Set
`Logging = true` in `EarShot.ini` for full load/playback tracing.

## What works in v0.1.0-alpha1

- Weapon sounds: shoot (+randomized `shoot0..9`), after-shot, distant shots
  (speed-of-sound delay), reload / reload_one / reload_two, dryfire,
  low_ammo, melee hits (flesh/metal/wood/stomp), swings, martial arts,
  vehicle projectile fire
- Explosions: per-type (`explosionTypes/<id>/`), shared generic, underwater,
  debris, distant with propagation delay + HF loss
- Ricochet (per-surface), gunshells / shotgun shells (per-surface),
  bullet whizz (2D), rhino tank cannon
- `EarShot.ini` + `.earshot` marker tuning identical to EarShotOpenAL
- EFX-style reverb (global Freeverb node) + per-voice air absorption on the
  miniaudio engine; focus-loss pause; clean restart/shutdown handling

Not yet ported (later stages): footsteps, landing/collapse, missiles,
fire, car jacking, grenade bounce, vehicle SFX, ambience / map ambience,
looped weapons (minigun, chainsaw, flamethrower, spray can, extinguisher).

## Build (for developers)

Requirements: Visual Studio 2022, Windows 10/11 SDK (10.0.26100 pinned),
x86 (Win32) target — SA 1.0 US is 32-bit.

```bat
git clone --recurse-submodules https://github.com/silskrill89/earshot-mini.git
cd earshot-mini
tools\premake5.exe vs2022
msbuild build\earshot_reminid.sln /p:Configuration=Release /p:Platform=Win32
```

Output: `bin\Release\earshot_reminid.asi`.

- Engine: miniaudio (vendored submodule `external/miniaudio`, single-TU
  implementation in `src/miniaudio_impl.cpp`)
- Hooking: subhook (vendored, `external/subhook`) + injector (submodule)
- Game structs/addresses: plugin-sdk v1004 headers (SA 1.0 US)
- Reference sources (not built): EarShotOpenAL, openal-soft, MiniAudioO3DE
  — clone them next to the repo if you want to read the RE references

## Credits

- **HzanRsxa2959** — original EarShot
- **wuzimu7171_ (TheArtemMaps)** — EarShotOpenAL revamp (implementation reference)
- **Ben Rayner** — miniaudio
- **openal-soft** — EFX/filter/doppler parity reference
- GTA SA reverse-engineering community (gta-reversed, plugin-sdk)
