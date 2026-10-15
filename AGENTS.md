# AGENTS.md — Earshot_reminid

## Project

Reverse-engineering rebuild of **EarShot** — a GTA San Andreas 1.0 US weapon-audio ASI plugin by **HzanRsxa2959**. EarShot adds NEW weapon sounds WITHOUT replacing vanilla audio: dynamic gunshots, after-shot echo, reverb, doppler, and 3D spatialization (mono WAVs; stereo only for 2D sounds like bullet whizz). The original shipped on deprecated irrKlang; a community revamp (`EarShotOpenAL` by theartemmaps1) moved it to OpenAL and added the debug menu.

**Goal**: recreate EarShot's weapon-audio behavior on the **miniaudio** engine — no OpenAL runtime install required. OpenAL-parity features (EFX reverb, filters, doppler, distance models) are ported onto miniaudio by reading openal-soft source. The MiniAudioO3DE gem's architecture (listener/playback/bank/node split) is salvaged as our engine layer, lifted out of the AZ/EBus framework onto plain miniaudio.

- **Project name**: Earshot_reminid / workspace `earshot_reminid`

**Language policy (binding)**: pure C/C++ only — no Rust. miniaudio's C API idiom governs the engine layer; plugin-sdk/game-facing code matches SA-era conventions.

**Engine policy (binding, user directive 2026-10-13)**: miniaudio is THE shipping backend (`external/miniaudio`, vendored single header; `MINIAUDIO_IMPLEMENTATION` defined in exactly ONE TU: `src/miniaudio_impl.cpp`). OpenAL is never linked, loaded, or required at runtime. `external/openal-soft` is a READING REFERENCE only. The Hzan original is feature truth; the artem revamp is implementation reference.

**Naming policy (binding)**:
- Original mod sources are fully readable (`external/EarShotOpenAL/source/`, `original_files/Earshot-main_HzanRsxa2959.zip`): reuse the author's symbol names wherever recovered (AudioManager, Loaders, Main hook wiring, INI keys).
- SA engine-side names come from gta-reversed-modern (`CAEWeaponAudioEntity::PlayGunSounds`/`::WeaponFire`, `CAEAudioEntity`, `CAESound`, `CAudioEngine`, …).
- Unnamed/RE'd constructs get semantic `Esh*`/`Earshot*` names; the raw Ghidra name + RVA survives only as a trailing comment (`// DAT_xxx @ RVA 0x…`) for grep-ability. Log all findings in `.serena/memories/re-*.md`.

- **Plugin target**: `earshot_reminid` (built/deployed as `earshot_reminid.asi`)
- **Known parity bugs to avoid** (v1.0.x crash reports): rocket launcher, rain/thunder — treat as test cases, not mysteries.
- **Reference sources**: gta-reversed-modern `E:\dev(dave)\releases\GTASource\GTA SA RE\source` (authoritative for SA audio entity classes), gta-reversed-master (cross-check), `E:\SDKs\plugin-sdk-master` v1004 (SA 1.0 addresses/structs for hooks), `E:\dev(dave)\GTA SA Reverse Engineering Documentation` (Obsidian RE vault: SND sound banks, audio stream docs). See PLAN.md §3.

## Repo Layout

| Path | Purpose | Owner |
|------|---------|-------|
| `original_files/` | READ-ONLY RE material: Hzan original source + artem packaged mod bundle | RE (read-only) |
| `src/` | Implementation (Earshot.cpp entry, miniaudio_impl.cpp, engine layer) | orchestrator |
| `external/miniaudio/` | Shipping runtime — vendored single header + examples/extras | orchestrator |
| `external/EarShotOpenAL/` | Mod skeleton reference: hooks/loading/debugmenu/INI | read-only ref |
| `external/MiniAudioO3DE/` | Engine architecture salvage source (O3DE gem) | read-only ref |
| `external/openal-soft/` | OpenAL-parity feature reference (EFX/filters/doppler) | read-only ref |
| `external/injector/` | injector submodule (hooking) | orchestrator |
| `premake5.lua` / `premake5.cmd` / `b.bat` | Build definition + dispatcher | orchestrator |
| `tools/` | premake5.exe (local), memory-startup.py, wrappers | orchestrator |
| `PLAN.md` | Master plan | orchestrator |
| `AGENTS.md`, `memory-bank/`, `.sadie/`, `.serena/`, `.opencode/`, `.harness-memory/`, `.lsai/`, `.obsidian/` | Harness | harness |

## Build

```bash
b.bat           # premake → msbuild dispatcher
# or manually:
premake5.cmd    # premake5.lua → build/earshot_reminid.sln (VS2022)
msbuild build/earshot_reminid.sln /p:Configuration=Release /p:Platform=Win32
```

- **MSBuild**: VS2022; **SDK**: Windows 10 SDK 26100 (pinned in `Directory.Build.targets`)
- **Platform**: x86 (Win32) — SA 1.0 US is 32-bit; static CRT (/MT)
- **plugin-sdk**: `E:/SDKs/plugin-sdk-master` v1004 — include lines mirrored EXACTLY in `premake5.lua` (`plugin_sa/game_sa` + `shared`); hardcoded absolute paths, no env var.
- **miniaudio**: includedirs `external/miniaudio`; implementation TU = `src/miniaudio_impl.cpp` ONLY.
- **Output**: `earshot_reminid.asi`
- **Deploy**: set env `SA_GAME_DIR` → postbuild copies the .asi there. Unset → no copy step.
- **Host**: GTA San Andreas 1.0 US (`gta_sa.exe`, x86)

## Domain Concepts

| Concept | Status |
|---------|--------|
| `CAEWeaponAudioEntity` (PlayGunSounds @0x503CE0, WeaponFire @0x504F80) | RE'd 2026-10-13 — bundle of 3–8 CAESounds/shot, bank 143 slot 5, per-weapon pitch table; `re-audio-map.md` |
| `CAEAudioEntity` / `CAESound` / `CAudioEngine` | RE'd — pull-based 3D: Service @0x507750 → UpdateParameters @0x4EFF50 re-reads positions; `re-audio-map.md` |
| `CPhysical` / `CWeapon` / `CWorld` spatial sources | RE'd — `CWeapon::Fire` @0x742300 → AddAudioEvent(145); ped weapon AE at CPed+0x394 |
| miniaudio engine API (ma_engine, listener, **ma_sound pool**) | RE'd — `ma_engine_play_sound()` CANNOT do 3D (no position, forces NO_SPATIALIZATION/NO_PITCH); pool via `ma_sound_init_from_file`+`ma_sound_set_position`; SA Z-up → `set_world_up(0,0,1)`; `re-miniaudio.md` |
| O3DE salvage: ListenerBus → `ma_engine_listener_set_*` (TheCamera); PlaybackBus → ma_sound pool at ped/weapon | de-EBus proposal `EshAudioEngine`/`EshListener`/`EshSoundBank`/`EshPlayback`/`EshVoicePool`; `re-o3de-salvage.md` |
| OpenAL-parity features to port: EFX reverb, low-pass/high-pass filters, doppler, distance models | RE'd — EFX 23-param contract + 113 presets + exact formulas; 15-item S6 checklist; `re-openal-parity.md` |
| Mono WAV requirement | 3D sounds MUST be mono; stereo won't localize (channel-convert runs BEFORE positional pan) — loader validates |
| INI config + debug menu (logging toggle, reload audio folders) | Inventory done — full key taxonomy; README `[WEAPONSOUND_n]` never parsed by revamp (do not port); `re-earshot-features.md` |

## Rules

### Critical RE Rules
- **Never modify `original_files/`** — pristine RE material. Copy out anything you need to analyze.
- **Never edit `external/*` clones** — they are references; our code lives in `src/`.
- **Cite source for every claim** — reversed-source file/method, EarShotOpenAL source file, or binary `@ RVA`. Unattributed addresses/claims are worthless.
- **Log RE findings into `.serena/memories/re-*.md`** — `re-func-names.md` (seed list), `re-audio-map.md` (filled entity/address map), `re-miniaudio.md` / `re-openal-parity.md` / `re-o3de-salvage.md` / `re-earshot-features.md` (S1/S2 notes), `re-daily.md` (daily log).
- **Reversed source = encyclopedia first** — verify SA variable/behavior from gta-reversed-modern before hooking; plugin-sdk headers carry the addresses.
- **x86 assumptions** — SA 1.0 US is 32-bit; all addresses/RVAs are 32-bit. Watch MBCS strings and stdcall/cdecl conventions.

### Safety Rules
- **Null Pointer Guards**: check pointers from game functions; check every `ma_*` return (`ma_result`).
- **Audio-thread discipline**: miniaudio runs its own audio thread — never uninit the engine or do heavy file I/O from hook context without care; game thread does only enqueue/start.
- **CRT**: static link (/MT) — the plugin must not depend on the user's CRT runtime.
- **Crash parity**: rocket launcher + rain/thunder crashes in original v1.0.x are test cases — find the original bug, avoid it.

### Token Efficiency Rules
- **AGGRESSIVE COMPRESSION**: compress early, compress often. Every 5-8 tool calls, evaluate stale ranges. Do NOT wait for the max context warning.
- **LSAI First**: use LSAI/graph tools for all symbol lookup; never grep/glob for symbol search when a graph tool exists.
- **Specialist Routing**: mechanical edits → @fixer (1/2 cost); symbol lookup → @explorer (2x faster); external research → @librarian; architecture/RE strategy → @oracle.
- **Parallel Execution**: independent lanes in parallel background tasks; never parallelize overlapping write scopes.
- **Minimal Context**: reference paths/lines instead of pasting files.

## Specialist Routing

| Agent | Use for |
|-------|---------|
| @explorer | Symbol lookup in reversed sources, miniaudio examples, EarShotOpenAL skeleton |
| @librarian | miniaudio/OpenAL docs, GTA audio modding research, wav-format edge cases |
| @fixer | Mechanical edits, harness work |
| @oracle | RE strategy, engine architecture decisions |
| RE agents | Binary analysis lanes (Ghidra/IDA) if source proves insufficient — log to `.serena/memories/re-*.md` |

## Knowledge System

### Session Startup

```bash
python tools/memory-startup.py --compact
```

Loads 5 layers: sadie → serena → memory-bank → harness-memory → vault.

### Memory Layers (7 total)

| # | Layer | Location | Purpose |
|---|-------|----------|---------|
| 1 | Conversation | Current session | Working context |
| 2 | Memory Bank | `memory-bank/*.md` (+ `.opencode/memory-bank/` mirror) | Structured project state |
| 3 | Serena | `.serena/memories/*.md` | Curated knowledge notes + RE findings |
| 4 | Sadie | `.sadie/sadie.db` | Queryable SQLite index of all layers |
| 5 | Harness-memory | `.harness-memory/memory.sqlite` | Dream-consolidated memories |
| 6 | Obsidian Vault | `E:/dev(dave)/GTA SA Reverse Engineering Documentation` | External RE knowledge vault (audio docs inside) |
| 7 | Harness commands | `.opencode/commands/harness-memory-*.md` | Memory CLI workflows |

### Sadie Knowledge Librarian

```bash
python .sadie/sadie.py init             # rebuild sadie.db — run after RE findings or scaffold changes
python .sadie/sadie.py search "<term>"  # search items by title/content/tags
python .sadie/sadie.py query "<sql>"    # run SQL
python .sadie/sadie.py stats            # database statistics
python .sadie/sadie.py gotchas          # critical items
# NOTE: sadie_extract.py is the Maxo shader-register scanner (domain-mismatched here — unused)
```

` sadie.db` and `__pycache__/` are local-only (not in git) — rebuild after cloning.

### Obsidian Vault (external)

- Vault root: `E:/dev(dave)/GTA SA Reverse Engineering Documentation` — NO project-internal docs/ vault (unlike the Maxo template).
- Audio-relevant entries: `File Formats/SND - Sound Banks.md`, `GTAMods Wiki/Audio stream.md`, `GTAMods Wiki/San Andreas Audio Toolkit.md`, `Projects/` analyses.
- MCP: `obsidian-mcp` via `tools/obsidian-mcp-wrapper.ps1` (configured in `.opencode/opencode.json`); sync into sadie with `python .sadie/sadie_vault_sync.py`.

### Skills

| Skill | Purpose |
|-------|---------|
| `hybrid-re-setup` | RE harness setup/workflow |
| `memory-octree` | Layered memory retrieval protocol |
| `token-efficiency` | Context management rules |
| `obsidian-skills` | Vault operations |

## Gotchas

- **Paths contain parentheses** (`E:\dev(dave)`) — ALWAYS quote paths in shell commands; backslash-escaping in shell variables bites (use forward slashes).
- **`MINIAUDIO_IMPLEMENTATION` in more than one TU → duplicate-symbol link errors.** Only `src/miniaudio_impl.cpp` defines it.
- EarShot 3D sounds require **MONO wavs**; stereo is 2D-only. The loader must validate and route accordingly.
- `plugin-sdk-master` (v1004, canonical) vs `plugin-sdk-sa` fork (E:/SDKs/plugin-sdk-sa) may drift — use the fork only to cross-check addresses/API.
- Deploy copy happens ONLY when `SA_GAME_DIR` is set (guarded postbuildcommand).
- SA 1.0 US exe addresses differ between retail variants — always record which exe base an address was found against.
- Original mod sources are READABLE — exhaust them before touching Ghidra/IDA; binary RE is only for behavior not visible in source.
