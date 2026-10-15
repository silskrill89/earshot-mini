// EshUiMenu.cpp — Earshot in-game debug/editor menu (M1), ImGui on miniaudio
// project (DX9 + Win32 backends).
//
// Integration model ported from skygfx_plus_expIV (READ-ONLY reference), every
// block citing the skygfx file:line it mirrors:
//   src/rw/gta.cpp:8                 — RW's global IDirect3DDevice9* @0xC97C28
//   src/core/debugmenu_ui.cpp:358-375 — WndProc subclass + input-swallow rule
//   src/core/debugmenu_ui.cpp:377-395 — lazy ImGui init (CreateContext → DX9
//                                        → GetCreationParameters → Win32 →
//                                        SetWindowLongPtr(GWL_WNDPROC))
//   src/core/debugmenu_ui.cpp:2205-2207 — per-frame NewFrame chain
//   src/core/debugmenu_ui.cpp:2431-2432 — Render + RenderDrawData
//   src/core/main.cpp:2568-2586      — Ctrl+<key> edge-detect toggle (Ctrl+4
//                                       there; we hardcode Ctrl+5 for M1)
//   src/core/main.cpp:4870           — skygfx hooks the 0x53EBE9 CALL SITE
//   src/render/postfx.cpp:1951-1956  — skygfx draws its own menu last, inside
//                                       its DrawFinalEffects replacement
//   src/core/main.cpp:1979-1986+4846 — skygfx chains CPostEffects::Render's
//                                       tail-jmp site 0x53E227 to the game
//                                       function (CPostEffects__Render_orig)
//
// ---------------------------------------------------------------------------
// HOOK ADDRESS — CPostEffects::Render @ 0x7046E0 (prologue, subhook trampoline)
// ---------------------------------------------------------------------------
// The task asked for the "DrawFinalEffects prologue"; verified against the
// binary that NO game function by that name exists:
//   * plugin-sdk CPostEffects.h and gta-reversed PostEffects.h both declare no
//     DrawFinalEffects (grep, 2026-10-15).
//   * The call at 0x53EBE9 (the site skygfx hooks) is `E8 52 5A 1F 00` ->
//     0x734640 = FlushObrsPrintfs (plugin-sdk common.cpp:111-114; gta-reversed
//     app/app_debug.cpp:55-58 `#ifndef FINAL`), whose retail body is a lone
//     `C3` (byte read of gta_sa.exe 1.0 US). skygfx's InjectHook at that site
//     REPLACES it wholesale with skygfx's own DrawFinalEffects_pf
//     (main.cpp:4536-4544), which never re-enters game code — so hooking that
//     site is a load-order race with skygfx, and hooking 0x734640's prologue
//     would never fire from it under skygfx.
//   * CPostEffects::Render @ 0x7046E0 IS the game's final-effects pass:
//       - gta-reversed PostEffects.cpp:59 `RH_ScopedInstall(Render, 0x7046E0)`
//         and `// 0x7046E0` at PostEffects.cpp:896;
//       - its prologue reads `A0 CF 02 C4 00` = mov al,[0xC402CF] =
//         CPostEffects::m_bDisableAllPostEffect (gta-reversed PostEffects.h:85);
//       - plugin-sdk CPostEffects.h:71 declares `static void Render();`
//       - a byte scan of .text finds exactly ONE direct reference: the tail
//         `jmp` at 0x53E227, which skygfx intercepts and CHAINS to 0x7046E0
//         (main.cpp:4846 + :1981-1986) — and skygfx's inner patches into this
//         function start at 0x704D48, never its prologue, so subhook here
//         does not collide with skygfx.
//
// Frame composition under skygfx_plus (verified by reading both sides):
//   CMain::Render -> RenderEffects(0x53EAD3) -> jmp@0x53E227 ->
//   skygfx CPostEffects__Render_hook -> orig(0x7046E0) -> OUR detour ->
//   trampoline (original body + skygfx's inner postfx hooks) -> OUR ImGui draw
//   -> return -> skygfx effectsForceAlphaBlock("PostFx.exit") (state repair,
//   no pixels) -> CMain::Render continues -> 0x53EBE9 skygfx DrawFinalEffects
//   (SMAA/FXAA + skygfx's menu, postfx.cpp:1951-1956) -> RwCameraEndUpdate ->
//   RsCameraShowRaster.  I.e. our menu lands on top of the game's postfx and
//   skygfx's own menu still draws above ours — the ordering the task wanted,
//   achieved without touching the call site skygfx owns.
// ---------------------------------------------------------------------------
//
// Known-parity notes:
//   * RESET HANDLING: none — deliberately at skygfx parity. skygfx wires no
//     IDirect3DDevice9::Reset path (the DX9 backend survives in SA: no Reset()
//     is exercised in-windowed play), so we do not either. If a device reset
//     ever appears (fullscreen toggle), the backend's D3DPOOL_DEFAULT vertex/
//     index/font buffers need ImGui_ImplDX9_InvalidateDeviceObjects() +
//     ImGui_ImplDX9_CreateDeviceObjects() — add it then.
//   * INPUT: swallowing happens only in OUR WndProc subclass (the Win32
//     message path). SA also reads the keyboard through DirectInput, so game
//     input can still reach the game while the menu is open — same exposure
//     as skygfx's menu. Not a new leak, and the game's own WndProc hook
//     (src/hooks/EshHooks.cpp H_WNDPROC) stays below us in the chain.
// ---------------------------------------------------------------------------
#include "ui/EshUiMenu.h"

#include <Windows.h>        // GetAsyncKeyState, SetWindowLongPtr, CallWindowProc
#include <d3d9.h>           // IDirect3DDevice9, D3DDEVICE_CREATION_PARAMETERS

#include <algorithm>        // std::sort (stable panel ordering)
#include <string>           // std::string (stem join)
#include <vector>           // std::vector (weapon list)

#include "subhook.h"        // vendored: external/subhook (SUBHOOK_STATIC)

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include "audio/EarshotAudio.h"      // FROZEN façade — called, never edited
#include "engine/EshAudioEngine.h"   // IsReady / SetGlobalVolume / GetGlobalVolume
#include "engine/EshVoicePool.h"     // EshVoicePool_GetStats (const query)
#include "loaders/EshSoundBank.h"    // distant-distance statics + probe accessors
#include "loaders/EshFolderScanner.h"// probe-bank scan (read-only usage)

// imgui_impl_win32.h keeps this declaration inside `#if 0` on purpose (it
// would otherwise drag <windows.h> into the helper header) and tells callers
// to forward-declare it locally (backends/imgui_impl_win32.h:29-32); skygfx
// does the same at src/core/debugmenu_ui.cpp:357.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);

// Single version string, shown in the Status panel + window title bar.
#define ESH_UI_VERSION "v0.1.0-alpha1"

// ===========================================================================
// state
// ===========================================================================
static subhook_t g_eshUiHook        = nullptr;
static bool      g_eshUiOpen        = false;
static bool      g_eshUiCtxReady    = false;   // ImGui context + DX9 backend up
static bool      g_eshUiWin32Ready  = false;   // Win32 backend up
static bool      g_eshUiSubclassed  = false;   // WndProc subclassed exactly once
static bool      g_eshUiInited      = false;   // all of the above -> ready to draw
static HWND      g_eshUiHwnd        = nullptr;
static WNDPROC   g_eshUiOrigWndProc = nullptr;

static bool g_eshUiShowStatus   = true;
static bool g_eshUiShowSettings = true;
static bool g_eshUiShowSounds   = true;

// Read-only probe bank — see EshUi_BuildProbe().
static EshSoundBank g_eshUiProbe;
static std::string  g_eshUiProbeFolder;
static bool         g_eshUiProbeBuilt = false;

// RW's global IDirect3DDevice9* (skygfx src/rw/gta.cpp:8; the same address is
// plugin-sdk common.cpp:603 GetD3DDevice). Null-checked every frame.
static IDirect3DDevice9* EshUiDevice() {
    return *(IDirect3DDevice9**)0xC97C28;
}

// Defined further below (WndProc subclass); declared here because the lazy
// installer references it.
static LRESULT CALLBACK EshUiWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ===========================================================================
// probe bank (why the UI does not read the façade's live bank)
// ===========================================================================
// The façade's live EshSoundBank is file-local: src/audio/EarshotAudio.cpp:41
// sits inside an anonymous namespace (verified with dumpbin on the object
// file: `anonymous namespace'::s_bank, internal linkage), and src/audio/* is
// out of this lane's write scope. So the menu builds its OWN bank through the
// very same read-only EshFolderScanner entry points the façade calls
// (EarshotAudio.cpp:266-270 and :360) and iterates it via EshSoundBank's const
// accessors (RegisteredWeapons/GenericEntries). Built once on first menu open
// and rebuilt after "Reload audio folders"; the counts/keys it reports are
// derived from the same disk state the live bank is derived from.
static void EshUi_BuildProbe() {
    g_eshUiProbeFolder = EshFolderScanner::ResolveModFolder();
    g_eshUiProbe.Clear();
    EshFolderScanner::ScanWeapons(g_eshUiProbeFolder.c_str(), g_eshUiProbe);
    EshFolderScanner::LoadExplosions(g_eshUiProbeFolder.c_str(), g_eshUiProbe);
    EshFolderScanner::LoadRicochet(g_eshUiProbeFolder.c_str(), g_eshUiProbe);
    EshFolderScanner::LoadGunshells(g_eshUiProbeFolder.c_str(), g_eshUiProbe);
    EshFolderScanner::LoadBulletWhizz(g_eshUiProbeFolder.c_str(), g_eshUiProbe);
    EshFolderScanner::LoadTankCannon(g_eshUiProbeFolder.c_str(), g_eshUiProbe);
    g_eshUiProbeBuilt = true;
}

// ===========================================================================
// panels
// ===========================================================================
static void EshUi_StatusPanel() {
    if (!ImGui::CollapsingHeader("Status", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::Text("Version: %s", ESH_UI_VERSION);
    ImGui::Text("Engine ready: %s",
                EshAudioEngine::Instance().IsReady() ? "yes" : "no");

    unsigned active = 0, total = 0;
    EshVoicePool_GetStats(&active, &total);
    ImGui::Text("Voices: %u active / %u total", active, total);

    ImGui::Text("Mod root: %s",
                g_eshUiProbeFolder.empty() ? "(not resolved yet)"
                                           : g_eshUiProbeFolder.c_str());
    ImGui::Text("Weapon entries: %u", (unsigned)g_eshUiProbe.WeaponCount());
    ImGui::Text("Generic entries: %u",
                (unsigned)g_eshUiProbe.GenericEntries().size());
    ImGui::Text("Log file: %s", EshSoundBank::EshLogFilePath());
}

static void EshUi_SettingsPanel() {
    if (!ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    // Logging toggle — façade debug-menu contract (EarshotAudio.h:57-59).
    bool logging = EarshotAudio_IsLogging();
    if (ImGui::Checkbox("Logging", &logging))
        EarshotAudio_SetLogging(logging);

    float vol = EshAudioEngine::Instance().GetGlobalVolume();
    if (ImGui::SliderFloat("Global volume", &vol, 0.0f, 1.0f))
        EshAudioEngine::Instance().SetGlobalVolume(vol);

    // Live INI-owned thresholds — the hooks read these statics every shot
    // (src/hooks/EshHooks.cpp:584 and :830), so dragging is effective at once.
    ImGui::SliderFloat("Distant gunshot distance",
                       &EshSoundBank::s_distantGunshotDistance, 0.0f, 500.0f, "%.1f");
    ImGui::SliderFloat("Distant explosion distance",
                       &EshSoundBank::s_distantExplosionDistance, 0.0f, 1000.0f, "%.1f");

    // Façade debug-menu entry (EarshotAudio.h:28-29: "stop all sounds, free
    // everything, re-read INI + re-scan folders"). Safe pre-Init: StopAll and
    // the scans no-op on empty state.
    if (ImGui::Button("Reload audio folders")) {
        EarshotAudio_ReloadFolders();
        EshUi_BuildProbe();   // re-read the disk so the panels match the live bank
    }

    // Future extension point: "load new sound files from this UI, turn sounds
    // off" (M2+). Collapsed placeholder so the section is discoverable now.
    if (ImGui::CollapsingHeader("Sound file loading (coming soon)")) {
        ImGui::TextWrapped(
            "Placeholder for the M2 editor: pick a weapon/folder, load new "
            "sound files into the bank, and mute individual stem classes from "
            "this UI. No wiring yet.");
    }
}

// Stable ordering for the weapon table (unordered_map order is arbitrary).
static void EshUi_CollectWeapons(const EshSoundBank& bank,
                                 std::vector<const EshWeaponEntry*>& out) {
    out.clear();
    const std::unordered_map<uint64_t, EshWeaponEntry>& m = bank.RegisteredWeapons();
    out.reserve(m.size());
    for (std::unordered_map<uint64_t, EshWeaponEntry>::const_iterator it = m.begin();
         it != m.end(); ++it) {
        out.push_back(&it->second);
    }
    std::sort(out.begin(), out.end(),
              [](const EshWeaponEntry* a, const EshWeaponEntry* b) {
                  if (a->weaponType != b->weaponType) return a->weaponType < b->weaponType;
                  if (a->modelId != b->modelId)       return a->modelId < b->modelId;
                  return a->name < b->name;
              });
}

static std::string EshUi_JoinStems(const EshWeaponEntry& e) {
    std::string s;
    for (std::unordered_map<std::string, std::vector<std::string>>::const_iterator
             it = e.stems.begin(); it != e.stems.end(); ++it) {
        if (!s.empty()) s += ", ";
        s += it->first;
    }
    if (s.empty()) s = "(none)";
    return s;
}

static void EshUi_SoundsPanel() {
    if (!ImGui::CollapsingHeader("Sounds", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    const EshSoundBank& bank = g_eshUiProbe;
    ImGui::TextDisabled("%u registered weapon(s), %u generic entries",
                        (unsigned)bank.WeaponCount(),
                        (unsigned)bank.GenericEntries().size());

    // --- registered weapons: name + type + modelId + stem classes ---
    std::vector<const EshWeaponEntry*> weapons;
    EshUi_CollectWeapons(bank, weapons);

    if (ImGui::BeginTable("esh_weapons", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("Stem classes", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < weapons.size(); ++i) {
            const EshWeaponEntry& e = *weapons[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(e.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%u", e.weaponType);
            ImGui::TableSetColumnIndex(2);
            if (e.modelId >= 0) ImGui::Text("%d", e.modelId);
            else                ImGui::TextUnformatted("any");   // ped-generic
            ImGui::TableSetColumnIndex(3);
            const std::string stems = EshUi_JoinStems(e);
            ImGui::TextUnformatted(stems.c_str());
        }
        if (weapons.empty()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("(no weapon folders registered)");
        }
        ImGui::EndTable();
    }

    // --- generic categories (explosions / ricochet / gunshells / whizz / ...) ---
    ImGui::Separator();
    ImGui::Text("Generic categories:");

    const std::unordered_map<std::string, EshGenericEntry>& gens = bank.GenericEntries();
    std::vector<const std::string*> keys;
    keys.reserve(gens.size());
    for (std::unordered_map<std::string, EshGenericEntry>::const_iterator it = gens.begin();
         it != gens.end(); ++it) {
        keys.push_back(&it->first);
    }
    std::sort(keys.begin(), keys.end(),
              [](const std::string* a, const std::string* b) { return *a < *b; });

    if (keys.empty()) {
        ImGui::TextDisabled("(no generic categories loaded)");
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        const std::unordered_map<std::string, EshGenericEntry>::const_iterator entry =
            gens.find(*keys[i]);
        if (entry == gens.end()) continue;
        ImGui::BulletText("%s  (%u file%s)", keys[i]->c_str(),
                          (unsigned)entry->second.paths.size(),
                          entry->second.paths.size() == 1 ? "" : "s");
    }
}

// ===========================================================================
// frame
// ===========================================================================
static void EshUi_DrawMenu() {
    ImGui::SetNextWindowSize(ImVec2(620.0f, 460.0f), ImGuiCond_FirstUseEver);

    // Closing via the window X flips g_eshUiOpen -> no frames drawn next tick.
    if (ImGui::Begin("Earshot debug (" ESH_UI_VERSION ")", &g_eshUiOpen,
                     ImGuiWindowFlags_MenuBar)) {
        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("Panels")) {
                ImGui::MenuItem("Status", nullptr, &g_eshUiShowStatus);
                ImGui::MenuItem("Settings", nullptr, &g_eshUiShowSettings);
                ImGui::MenuItem("Sounds", nullptr, &g_eshUiShowSounds);
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        if (g_eshUiShowStatus)   EshUi_StatusPanel();
        if (g_eshUiShowSettings) EshUi_SettingsPanel();
        if (g_eshUiShowSounds)   EshUi_SoundsPanel();
    }
    ImGui::End();   // paired with Begin() regardless of its return value
}

// Lazy init — mirror of skygfx debugmenu_ui.cpp:377-395 (EnsureImGuiInit),
// split into stages so a device that cannot supply a focus window retries next
// frame instead of half-initialising, and so the WndProc is subclassed exactly
// once (a second SetWindowLongPtr would hand us OUR OWN proc and chain into
// ourselves).
static void EshUi_EnsureInit(IDirect3DDevice9* device) {
    if (g_eshUiInited) return;
    if (!device) return;

    if (!g_eshUiCtxReady) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;   // skygfx :383
        io.MouseDrawCursor = true;   // SA hides the OS cursor — ImGui draws its
                                     // own (skygfx debugmenu_ui.cpp:384)
        ImGui_ImplDX9_Init(device);
        g_eshUiCtxReady = true;
    }

    if (!g_eshUiWin32Ready) {
        D3DDEVICE_CREATION_PARAMETERS dcp = {};
        if (FAILED(device->GetCreationParameters(&dcp)) || !dcp.hFocusWindow)
            return;                                    // retry next frame
        g_eshUiHwnd = dcp.hFocusWindow;
        ImGui_ImplWin32_Init(g_eshUiHwnd);             // skygfx :390
        g_eshUiWin32Ready = true;
    }

    if (!g_eshUiSubclassed) {
        // Subclass OUTSIDE the game's WndProc hook (src/hooks/EshHooks.cpp
        // H_WNDPROC @0x747EB0): we wrap it, so CallWindowProc chains correctly.
        WNDPROC prev = (WNDPROC)SetWindowLongPtr(g_eshUiHwnd, GWL_WNDPROC,
                                                 (LONG_PTR)EshUiWndProc);
        if (prev == (WNDPROC)EshUiWndProc) {
            // Never record OUR OWN proc as the original — that would chain
            // straight back into ourselves. (Only reachable if this block ever
            // ran twice, which the flag prevents.)
            prev = nullptr;
        }
        if (prev) {
            g_eshUiOrigWndProc = prev;
            g_eshUiSubclassed   = true;     // success: subclass exactly once
        }
        // prev == NULL -> subclass failed (window keeps its own proc, we were
        // not installed) -> retry next frame; skygfx :392-394 marks it done
        // unconditionally, we do not so a transient failure cannot leave the
        // menu on screen with no input path.
    }

    g_eshUiInited = g_eshUiCtxReady && g_eshUiWin32Ready && g_eshUiSubclassed;
}

// Per-frame — structure mirrors skygfx debugmenu_ui.cpp:2205-2207 / :2431-2432.
static void EshUi_Frame() {
    // Ctrl+5 toggle, edge-detected (skygfx main.cpp:2568-2586, Ctrl+4 there).
    // Hardcoded for M1 — no INI key yet.
    static bool s_prevChord = false;
    const bool chord = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) &&
                       ((GetAsyncKeyState('5') & 0x8000) != 0);
    if (chord && !s_prevChord)
        EshUi_Toggle();
    s_prevChord = chord;

    IDirect3DDevice9* device = EshUiDevice();
    if (!device) return;                 // null-guard every frame
    EshUi_EnsureInit(device);
    if (!g_eshUiInited) return;

    ImGui_ImplWin32_NewFrame();          // skygfx debugmenu_ui.cpp:2205
    ImGui_ImplDX9_NewFrame();            // :2206
    ImGui::NewFrame();                   // :2207

    if (g_eshUiOpen) {
        if (!g_eshUiProbeBuilt)
            EshUi_BuildProbe();          // one-time disk scan, game thread
        EshUi_DrawMenu();                // "if(open) draw panels"
    }

    ImGui::Render();                     // skygfx debugmenu_ui.cpp:2431
    // DIVERGENCE from skygfx (:2432 is unconditional): only submit draw data
    // while the menu is open. io.MouseDrawCursor stays true, so ImGui builds a
    // software cursor quad every frame — drawing it while closed would paint a
    // stray arrow over gameplay. Skipping RenderDrawData also keeps the OS
    // cursor path untouched (the Win32 backend only reacts to cursor CHANGES).
    if (g_eshUiOpen)
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());   // :2432
}

// ===========================================================================
// WndProc subclass — mirror of skygfx debugmenu_ui.cpp:358-375
// ===========================================================================
static LRESULT CALLBACK EshUiWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui::GetCurrentContext()) {
        // Always let ImGui track mouse/keyboard state (skygfx :361-363), so
        // the event queue never goes stale while the menu is closed (the frame
        // above keeps draining it even when no panel is drawn).
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
            // Only consume input when the menu is open (skygfx :365-367).
            if (g_eshUiOpen)
                return 0;
        }
    }
    if (!g_eshUiOrigWndProc) {
        // Unreachable in practice (SetWindowLongPtr(GWL_WNDPROC) on a class
        // window always reports a non-NULL previous proc; a NULL return means
        // the subclass FAILED, in which case we are never installed and this
        // body never runs). If it ever did happen, chain to the game's own
        // CWndProc @0x747EB0 (AR Main.cpp:2903-2906; the address EshHooks.cpp
        // hooks) rather than DefWindowProc, so the game keeps its input.
        return CallWindowProc((WNDPROC)0x747EB0, hWnd, msg, wParam, lParam);
    }
    return CallWindowProc(g_eshUiOrigWndProc, hWnd, msg, wParam, lParam);
}

// ===========================================================================
// hook — CPostEffects::Render @0x7046E0 (address verification in the header
// comment). subhook trampoline pattern, same shape as the CallOriginal*
// helpers in src/hooks/EshHooks.cpp:340-485 (READ-ONLY reference).
// ===========================================================================
using PostEffectsRenderFn = void (__cdecl*)();

static void EshUi_CallOriginalPostEffectsRender() {
    const PostEffectsRenderFn orig = (PostEffectsRenderFn)0x7046E0;
    subhook_t h = g_eshUiHook;
    if (h) {
        if (void* tramp = subhook_get_trampoline(h)) {
            ((PostEffectsRenderFn)tramp)();
            return;
        }
        subhook_remove(h);
        orig();
        subhook_install(h);
        return;
    }
    orig();
}

static void __cdecl EshUi_HookedPostEffectsRender() {
    EshUi_CallOriginalPostEffectsRender();   // original body FIRST (postfx)
    EshUi_Frame();                           // then our menu, on top of postfx
}

// Self-installer: CRT static constructor runs at ASI load — no other file has
// to call us (precedent: src/debug/EshCrashHandler.cpp:652-668).
struct EshUiInstaller {
    EshUiInstaller();
};

EshUiInstaller::EshUiInstaller() {
    g_eshUiHook = subhook_new((void*)0x7046E0,
                              (void*)&EshUi_HookedPostEffectsRender,
                              subhook_flags_t(0));
    if (!g_eshUiHook || subhook_install(g_eshUiHook) != 0) {
        // Unconditional visibility; never MessageBox from init (crash-parity
        // rule C.2.2, src/hooks/EshHooks.cpp:14-16).
        OutputDebugStringA("[Earshot] FATAL: UI hook install failed: "
                           "CPostEffects::Render @0x7046E0\n");
        if (g_eshUiHook) {
            subhook_free(g_eshUiHook);
            g_eshUiHook = nullptr;
        }
        return;
    }
    OutputDebugStringA("[Earshot] UI hook installed: "
                       "CPostEffects::Render @0x7046E0\n");
}

static EshUiInstaller s_eshUiInstaller;

// ===========================================================================
// public surface
// ===========================================================================
void EshUi_Toggle() {
    g_eshUiOpen = !g_eshUiOpen;
}

bool EshUi_IsOpen() {
    return g_eshUiOpen;
}
