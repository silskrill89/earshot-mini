// EshLog.h — throttle / first-run / lifecycle debug-log macros routed into the
// EXISTING EarshotReminid.log writer (EshSoundBank::EshLog_WriteLine) — no
// second log file, no duplicate path logic.
//
// Port of skygfx_plus_expIV src/skygfx.h:56-140:
//   perfNow / PerfTimer RAII   :56-81   -> EshPerfNow / EshPerfTimer + ESH_PERF_SCOPE
//   dbglog_throttle (2 s, 64 hashed slots) :83-96 -> EshLogThrottle + ESH_LOG_THROTTLE
//   dbglog_first (first-run-only)          :98-107 -> EshLogFirst + ESH_LOG_FIRST
//   DBGLOG_ENTER / EXIT_OK / BAIL          :126-140 -> ESH_LOG_ENTER / EXIT_OK / BAIL
//
// Routing (DIVERGENCE from skygfx): skygfx's dbglog writes its own
// skygfx_dbg.log; these macros write into the mod's normal log
// (PLUGIN_PATH/EarshotReminid.log) through the bank logger, so the INI
// Logging flag / "Max bytes in log" cap / verbose hotkey all govern them.
// Line format: "[HH:MM:SS.mmm] <msg>" (the AR writer's own date prefix is
// only added by EshSoundBank::EshLog, not by EshLog_WriteLine).
//
// Game-thread only — call from OnFrame / init paths, never from the audio
// thread (the writer does FILE* I/O).
#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "loaders/EshSoundBank.h"

// ---------------------------------------------------------------------------
// Perf clock (skygfx skygfx.h:59-74 — self-initializing, no init call needed)
// ---------------------------------------------------------------------------
inline LARGE_INTEGER& EshPerfFreq()
{
    static LARGE_INTEGER s_freq = { 0, 0 };
    if (s_freq.QuadPart == 0)
        QueryPerformanceFrequency(&s_freq);
    return s_freq;
}

inline double EshPerfNow()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * (1000.0 / (double)EshPerfFreq().QuadPart);
}

// ---------------------------------------------------------------------------
// Writer — one formatted line into EarshotReminid.log (gated by the Logging
// flag; no-op while verbose is off)
// ---------------------------------------------------------------------------
inline void EshLogf(const char* fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(msg))
        n = (int)sizeof(msg) - 1;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1100];
    int hlen = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %.*s",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        n, msg);
    if (hlen <= 0 || hlen >= (int)sizeof(line))
        return;
    EshSoundBank::EshLog_WriteLine(line);
}

#define ESH_LOG(...) EshLogf(__VA_ARGS__)

// ---------------------------------------------------------------------------
// Rate-limited logging: at most one line per tag per 2 s, 64 hashed slots
// (skygfx skygfx.h:83-96)
// ---------------------------------------------------------------------------
#define ESH_LOG_THROTTLE_MS 2000

inline bool EshLogThrottle(const char* tag)
{
    static double s_lastTime[64] = {};
    int idx = 0;
    for (const char* p = tag; *p; p++)
        idx = idx * 31 + (unsigned char)*p;
    idx = (idx & 0x7FFFFFFF) % 64;
    double now = EshPerfNow();
    if (now - s_lastTime[idx] < ESH_LOG_THROTTLE_MS)
        return false;
    s_lastTime[idx] = now;
    return true;
}

// First-run-only guard: logs exactly once per tag (skygfx skygfx.h:98-107)
inline bool EshLogFirst(const char* tag)
{
    static bool s_fired[64] = {};
    int idx = 0;
    for (const char* p = tag; *p; p++)
        idx = idx * 31 + (unsigned char)*p;
    idx = (idx & 0x7FFFFFFF) % 64;
    if (s_fired[idx])
        return false;
    s_fired[idx] = true;
    return true;
}

// tag keys the rate-limit bucket; the message is what gets written
// (skygfx uses the same split: dbglog_throttle("pf_pchain") + dbglog(...)).
#define ESH_LOG_THROTTLE(tag, ...) \
    do { if (EshLogThrottle(tag)) EshLogf(__VA_ARGS__); } while (0)

#define ESH_LOG_FIRST(tag, ...) \
    do { if (EshLogFirst(tag)) EshLogf(__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------
// Lifecycle macros — first call site only (skygfx skygfx.h:126-140)
// ---------------------------------------------------------------------------
#define ESH_LOG_ENTER(name) do { \
    static bool _esh_entered = false; \
    if (!_esh_entered) { _esh_entered = true; EshLogf("%s enter", name); } \
} while (0)

#define ESH_LOG_EXIT_OK(name) do { \
    static bool _esh_exited = false; \
    if (!_esh_exited) { _esh_exited = true; EshLogf("%s exit ok", name); } \
} while (0)

#define ESH_LOG_BAIL(name, reason) do { \
    static bool _esh_bailed = false; \
    if (!_esh_bailed) { _esh_bailed = true; EshLogf("%s exit BAIL: %s", name, reason); } \
} while (0)

// ---------------------------------------------------------------------------
// RAII perf scope (skygfx skygfx.h:75-81). DIVERGENCE: skygfx's
// PERF_SCOPE(name) pastes __LINE__ without an expansion step, so two scopes in
// one translation unit collide — the CAT indirection below fixes that.
// ---------------------------------------------------------------------------
struct EshPerfTimer {
    const char* name;
    double      startMs;
    explicit EshPerfTimer(const char* n) : name(n), startMs(EshPerfNow()) {}
    ~EshPerfTimer() { EshLogf("PERF [%s] %.2f ms", name, EshPerfNow() - startMs); }
};

#define ESH_PERF_SCOPE_CAT2(a, b) a##b
#define ESH_PERF_SCOPE_CAT(a, b)  ESH_PERF_SCOPE_CAT2(a, b)
#define ESH_PERF_SCOPE(name) EshPerfTimer ESH_PERF_SCOPE_CAT(_eshPerf_, __LINE__)(name)
