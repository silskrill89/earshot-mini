// EshListener.cpp — drives miniaudio listener 0 from GTA's camera, once/frame.
//
// Citations
//   * TheCamera global: plugin-sdk CCamera.h:747 declares
//     `extern CCamera &TheCamera;` — its definition lives in plugin-sdk's
//     CCamera.cpp (compiled only into plugin.lib, which we do NOT link), so we
//     define it here exactly like skygfx_plus_expIV does (skygfx src/rw/gta.cpp:24):
//     `CCamera &TheCamera = *(CCamera*)0xB6F028;` — 1.0 US absolute
//     [PSDK CCamera.cpp:16 GLOBAL_ADDRESS_BY_VERSION; also re-audio-map.md row
//     "TheCamera global 0xB6F028"].
//   * Position: `TheCamera.GetPosition()` → CPlaceable::GetPosition
//     (plugin-sdk CPlaceable.h:53-54: `m_matrix ? m_matrix->GetPosition() :
//     m_placement.m_vPosn`) — CCamera derives CPlaceable (CCamera.h:29).
//     Used by the original mods too (HZ.cpp:357-364 listener position).
//   * Forward: `TheCamera.m_mCameraMatrix.GetForward()` (CCamera.h:208 member,
//     offset 0x974 validated at CCamera.h:644; getter CMatrix.h:73 — note GTA
//     stores forward in the matrix's `up` slot). This is the exact accessor the
//     EarShotOpenAL revamp uses for its listener (Main.cpp:3181) — copy MUST
//     (CMatrix::GetForward returns a non-const ref into the live matrix).
//   * World up is set once at engine init (EshAudioEngine.cpp), not here.
//
// Velocity / doppler: GTA has no native "listener velocity" — SA computes
// doppler itself per-sound from camera-distance deltas (re-audio-map.md §3,
// CAESound::GetRelativePlaybackFrequencyWithDoppler) and hands the listener
// none. The O3DE gem didn't set listener velocity either (re-o3de-salvage.md
// "Gotchas": gem sets no listener/source velocity -> no doppler there). miniaudio
// needs it for doppler (re-miniaudio.md §2: ma_engine_listener_set_velocity is
// the doppler input), so we derive it from position deltas with a clamped dt.
#include "engine/EshListener.h"

#include <Windows.h>
#include <mmsystem.h>   // timeGetTime (also pulls winmm via pragma below)
#pragma comment(lib, "winmm.lib")

#include "plugin.h"
#include "CCamera.h"
#include "CVector.h"

#include "engine/EshAudioEngine.h"

// TheCamera definition — see header comment (matches skygfx gta.cpp:24).
CCamera& TheCamera = *(CCamera*)0xB6F028;

namespace {

CVector s_prevPos;
DWORD   s_prevTickMs = 0;
bool    s_havePrev = false;

// dt clamp (task): reject both first-frame garbage and giant stalls so the
// derived velocity can't explode. [1 ms, 100 ms]
float ClampDt(DWORD nowMs) {
    float dt = (nowMs - s_prevTickMs) * 0.001f;   // unsigned math: wraps correctly
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.100f) dt = 0.100f;
    return dt;
}

} // namespace

void EshListener_Update() {
    EshAudioEngine& eng = EshAudioEngine::Instance();
    if (!eng.IsReady()) {
        return;
    }
    ma_engine* engine = eng.GetEngine();

    // --- position ------------------------------------------------------------
    CVector pos = TheCamera.GetPosition();          // CPlaceable.h:53-54
    ma_engine_listener_set_position(engine, 0, pos.x, pos.y, pos.z);   // :11372

    // --- forward (normalize; guard degenerate matrix) ------------------------
    CVector fwd = TheCamera.m_mCameraMatrix.GetForward();   // COPY — ref aliases the live matrix
    if (fwd.Magnitude() > 1e-4f) {                          // CVector::Magnitude (CVector.h)
        fwd.Normalize();                                    // CVector::Normalize (CVector.h:87)
        ma_engine_listener_set_direction(engine, 0, fwd.x, fwd.y, fwd.z);   // :11374
    }

    // --- velocity (doppler input) -------------------------------------------
    DWORD now = timeGetTime();
    if (s_havePrev) {
        float dt = ClampDt(now - s_prevTickMs);
        CVector vel;
        vel.x = (pos.x - s_prevPos.x) / dt;
        vel.y = (pos.y - s_prevPos.y) / dt;
        vel.z = (pos.z - s_prevPos.z) / dt;
        ma_engine_listener_set_velocity(engine, 0, vel.x, vel.y, vel.z);    // :11376
    }

    s_prevPos = pos;
    s_prevTickMs = now;
    s_havePrev = true;
}
