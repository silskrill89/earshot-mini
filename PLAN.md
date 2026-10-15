# Earshot_reminid — Master Plan (EarShot RE → miniaudio rebuild)

> Status: infrastructure done (S0, 2026-10-13 — scaffold cloned, references fetched).
> RE not started. Plan owner: orchestrator. This file is the authoritative
> deployment + RE roadmap. Session rules live in `AGENTS.md`.

## 1. Mission

1. **Reverse** EarShot's weapon-audio behavior — combining the original
   HzanRsxa2959 source (`original_files/`, feature truth), the EarShotOpenAL
   revamp (`external/EarShotOpenAL/`, readable implementation), and SA audio
   entity internals (gta-reversed-modern).
2. **Rebuild** it as a clean, compilable SA 1.0 US ASI plugin
   (`earshot_reminid.asi`) on the **miniaudio** engine — no OpenAL runtime
   install, ever.
3. **Port** OpenAL-parity features (EFX reverb, filters, doppler, distance
   models) onto miniaudio by reading openal-soft — "best of both worlds: no
   need to install OpenAL, all same functions and more thanks to miniaudio."

## 2. Hard constraints (user directives)

| # | Constraint |
|---|------------|
| C1 | **miniaudio is the only audio backend** — vendored `external/miniaudio`; never link/load/install OpenAL at runtime. |
| C2 | **OpenAL-parity features still required** — EFX reverb, filters, doppler, distance models ported onto miniaudio; `external/openal-soft` is the reading reference. |
| C3 | **EarShotOpenAL skeleton is kept** — mod entry/hook wiring, INI reader, debug menu (logging toggle, reload audio folders), logging; OpenAL calls are cut at the seams and re-routed to our miniaudio engine layer. |
| C4 | **MiniAudioO3DE gem is salvaged** — listener/playback/bank/node architecture lifted out of AZ/EBus as our engine layer on plain miniaudio. |
| C5 | **x86 only** (SA 1.0 US, `gta_sa.exe`), static CRT (/MT), MBCS-era conventions. |
| C6 | **`original_files/` read-only**; every RE claim cites readable source or `binary @ RVA`. |
| C7 | **Mono WAVs for 3D sounds** — stereo allowed only for 2D sounds (bullet whizz etc.); loader validates and routes accordingly. |
| C8 | **Hzan original = feature truth**; artem revamp = implementation reference; v1.0.x crash reports (rocket launcher, rain/thunder) = parity test cases. |

## 3. RE targets & materials

### Mod-side (readable source — no binary RE needed to start)

| Material | Path | Note |
|----------|------|------|
| Hzan original | `original_files/Earshot-main_HzanRsxa2959.zip` → `Earshot-main/SOURCE/repo/EarShot/` | 104 KB; single-ASI VS project; feature truth |
| EarShotOpenAL | `external/EarShotOpenAL/source/` | AudioManager.cpp/.h, Loaders.cpp/.h, Main.cpp hook wiring, IniReader.h, debugmenu_public.h, logging; vendored subhook-0.8.2, dr_libs, stb_vorbis |

### Engine-side (reversed SA source)

| Material | Path | Note |
|----------|------|------|
| gta-reversed-modern | `E:/dev(dave)/releases/GTASource/GTA SA RE/source` | authoritative for audio entity classes |
| gta-reversed-master | `E:/dev(dave)/releases/GTASource/gta-reversed-master` | cross-check (~50-60% coverage) |
| plugin-sdk v1004 | `E:/SDKs/plugin-sdk-master` | SA 1.0 addresses/structs for hooks |
| RE docs vault | `E:/dev(dave)/GTA SA Reverse Engineering Documentation` | SND sound banks, audio stream, SA audio toolkit docs |

### Engine libraries

| Library | Path | Role |
|---------|------|------|
| miniaudio | `external/miniaudio` | shipping backend; `examples/` = API tutorials; `extras/nodes` = node graph |
| MiniAudioO3DE | `external/MiniAudioO3DE/MiniAudio/` | salvage source (listener/playback/bank/node, de-EBus) |
| openal-soft | `external/openal-soft` | feature reference: `al/`, `alc/`, `core/` (EFX, filters, doppler, distance models) |

## 4. Architecture (salvage map)

- **Keep from EarShotOpenAL**: mod entry (Main.cpp hook wiring), INI reader,
  debug menu entries (logging toggle, reload audio folders), WAV loading
  discipline (mono validation for 3D), logging.
- **Cut**: every OpenAL call site (`alGen*`/`alSource*`/`alc*` + `OpenAL32.lib`
  linkage) — replaced by our engine layer in `src/`.
- **Salvage from MiniAudioO3DE** (de-EBus onto plain miniaudio — see `re-o3de-salvage.md`):
  - ListenerComponent → per-frame `ma_engine_listener_set_position` /
    `ma_engine_listener_set_direction`, fed from `TheCamera` position/forward;
    plus velocity + `ma_spatializer_listener_set_speed_of_sound` (the gem has
    neither). SA is Z-up → `ma_engine_listener_set_world_up(0,0,1)` + camera
    axis map (miniaudio is Y-up/-Z-forward).
  - PlaybackComponent → **ma_sound pool** (`ma_sound_init_from_file` +
    `ma_sound_set_position` at ped/weapon position, per shot) — NOT
    `ma_engine_play_sound()`, which cannot do 3D (no position param; forces
    NO_SPATIALIZATION|NO_PITCH|ASYNC, killing doppler too).
  - Bank/node architecture → sound asset management; decoders (dr_libs /
    stb_vorbis) already vendored on both sides.
- **Port from openal-soft** (S6+): EFX reverb (`core/effects`, filters),
  doppler shift math, distance/rolloff models — reimplemented on the miniaudio
  node graph (`extras/nodes`).
- **Hook seams (SA, verified 1.0 US — see `re-audio-map.md`)**:
  `CAEWeaponAudioEntity::WeaponFire` @0x504F80 (Hzan's primary hook site) and
  `::PlayGunSounds` @0x503CE0 (bundle of 3–8 CAESounds, bank 143 slot 5);
  rockets bypass WeaponFire → `CAEGlobalWeaponAudioEntity::ProjectileFire`
  @0x4DF060 via `CAudioEngine::ReportWeaponEvent` @0x506F40; event chain starts
  at `CWeapon::Fire` @0x742300 → `AddAudioEvent(AE_WEAPON_FIRE=145)`; ped weapon
  AE lives at `CPed+0x394`. Rain/thunder = `CAEWeatherAudioEntity` @0xC81360.

## 5. Build & deploy

- `premake5.cmd` → `build/earshot_reminid.sln` (VS2022, x86, /MT); `b.bat` = one-shot dispatcher.
- plugin-sdk include lines mirrored exactly from `skygfx_plus_expIV/premake5.lua:41-42` (hardcoded `E:/SDKs/plugin-sdk-master/...`).
- `MINIAUDIO_IMPLEMENTATION` in exactly one TU (`src/miniaudio_impl.cpp`).
- Output `earshot_reminid.asi`; deploy via `SA_GAME_DIR` env postbuild (unset = no copy).

## 6. Stages

| Stage | Goal | Status |
|-------|------|--------|
| S0 | Scaffold + references fetched (openal-soft, miniaudio, MiniAudioO3DE, EarShotOpenAL) | ✅ 2026-10-13 |
| S1 | Read: openal-soft feature map + miniaudio + O3DE salvage → `re-miniaudio.md`, `re-openal-parity.md`, `re-o3de-salvage.md`, `re-earshot-features.md` | ✅ 2026-10-13 |
| S2 | Read: gta-reversed-modern audio entities → fill `re-audio-map.md` | ✅ 2026-10-13 |
| S3 | Skeleton hook: log WeaponFire events in-game (no audio yet) | ✅ built 2026-10-14 — subhook detour @0x504F80; in-game verify pending |
| S4 | Engine bootstrap: ma_engine init inside ASI, device selection | ✅ built 2026-10-14 — EshAudioEngine, world_up=(0,0,1), speedOfSound 343.3; in-game verify pending |
| S5 | First 3D gunshot: ma_sound pool at weapon position (world_up=(0,0,1) axis map) | ✅ built 2026-10-14 — 16-voice pool + TheCamera listener per-frame; in-game verify pending |
| S6 | Parity ports: EFX reverb, doppler, distance models on miniaudio nodes | pending |
| S7 | INI + debug menu parity (logging toggle, reload audio folders) | pending |
| S8 | Crash-parity fixes + polish (rocket launcher, rain/thunder test cases) | pending |
