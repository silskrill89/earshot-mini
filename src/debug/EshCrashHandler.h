// EshCrashHandler.h — crash forensics + diagnostics log for earshot_reminid.
//
// Merged port of the skygfx_plus_expIV diagnostics stack (READ-ONLY source,
// every function cites its skygfx file:line in EshCrashHandler.cpp):
//   * src/core/crashhandler.cpp — CRT-static-ctor self-installing
//     SetUnhandledExceptionFilter: one-line crash record + MiniDumpWriteDump
//     minidump, previous-filter chaining, Interlocked re-entrancy guard,
//     per-step SEH, WER LocalDumps registry backup.
//   * src/core/diagnostics.{h,cpp} — VEH crash-context log (code/EIP, zone
//     heuristic, READ/WRITE/DEP + null-deref offset, REGS, stack[0-7], nearest
//     scope tag, ring dump, crash-count cap, guard-phase pass-through), dbglog
//     line format, boot log rotation, 32x160 lock-free ring buffer,
//     scope-tag registry.
//   * src/core/main.cpp:4730-4768 — exe-version check + F8-held console.
//
// Files (all beside the .asi — same PLUGIN_PATH derivation the bank logger
// uses, EshSoundBank.cpp:300-324):
//   EarshotReminid_dbg.log                  diagnostics log (rotated to
//                                           EarshotReminid_dbg.prev.log at boot)
//   EarshotCrash_YYYY-MM-DD_HHMMSS.dmp      minidump on a hard crash
//
// The normal (non-crash) application log stays EarshotReminid.log — see
// src/debug/EshLog.h, which routes into the existing bank logger.
//
// To disable everything: #define ESH_DIAGNOSTICS_DISABLED before including
// this header (or add it to the project defines) — the whole API stubs to
// no-ops (skygfx diagnostics.h:12-24) and EshCrashHandler.cpp compiles to an
// empty TU.
#pragma once

#include <windows.h>

// Scope-tag registry geometry (skygfx skygfx.h:110-117)
#define ESH_SCOPE_TAG_MAX 64

struct EshScopeTag {
    const char* name;
    void*       addr;
};

// Ring buffer geometry (skygfx diagnostics.cpp:31-33)
#define ESH_RING_SIZE    32
#define ESH_RING_MSG_LEN 160

// Log levels (skygfx skygfx.h:50)
enum EshDiagLevel {
    ESH_DIAG_TRACE = -1,
    ESH_DIAG_INFO  = 0,
    ESH_DIAG_WARN  = 1,
    ESH_DIAG_ERROR = 2,
    ESH_DIAG_FATAL = 3
};

#ifdef ESH_DIAGNOSTICS_DISABLED

// Stubs — all diagnostic calls become no-ops (skygfx diagnostics.h:12-24)
#define EshDiag_Init(logPath)       ((void)0)
#define EshDiag_InstallVEH()        ((void)0)
#define EshDiag_RemoveVEH()         ((void)0)
#define EshDiag_CheckExeVersion()   ((void)0)
#define EshDiag_ConsoleFromF8()     ((void)0)
#define EshDiag_RegisterScope(n, a) ((void)0)
#define EshDiag_RingPut(m)          ((void)0)
#define EshDiag_RingDump()          ((void)0)
#define EshDiag_LogLoc(...)         ((void)0)
#define ESH_DBGLOG(...)             ((void)0)
#define ESH_DBGLOG_WARN(...)        ((void)0)
#define ESH_DBGLOG_ERR(...)         ((void)0)

#else

// Initialize the diagnostics log: sets the path (NULL = <asi dir>\
// EarshotReminid_dbg.log), rotates the previous boot's log to
// <name>.prev.log, truncates a fresh one (skygfx diagnostics.cpp:359-396).
void EshDiag_Init(const char* logPath);

// Install/remove the VEH crash-context logger (AddVectoredExceptionHandler(1)
// — forensics only, ALWAYS returns EXCEPTION_CONTINUE_SEARCH so it never
// suppresses an exception; skygfx diagnostics.cpp:399-411, :267).
void EshDiag_InstallVEH(void);
void EshDiag_RemoveVEH(void);

// Host-exe sanity check (skygfx main.cpp:4753-4761): reads the two version
// signature DWORDs, warns "host exe is not SA 1.0 US" on mismatch and
// CONTINUES — never aborts.
void EshDiag_CheckExeVersion(void);

// F8-held-at-load console (skygfx main.cpp:4763-4768): AllocConsole +
// freopen stdin/stdout/stderr so dbg output is visible.
void EshDiag_ConsoleFromF8(void);

// Guard-phase pass-through flag (skygfx g_inGuardedIm2DPass,
// diagnostics.cpp:186-195): while nonzero the VEH only pushes a bounded ring
// entry and returns EXCEPTION_CONTINUE_SEARCH — no file I/O, so frame-level
// __try/__except regions are never derailed. No site sets it yet (we have no
// guarded-Im2D equivalent); declared so future SEH-guarded regions can opt in.
extern volatile LONG EshDiag_inGuardedRegion;

// Scope-tag registry: address -> function name, matched against the crash EIP
// within +/-0x400 at crash time (skygfx skygfx.h:119-120, diagnostics.cpp:72-97).
void EshDiag_RegisterScope(const char* name, void* addr);
void EshDiag_LookupScope(void* eip, char* outName, int outSize, int* outOffset);

// Ring buffer — lock-free, single writer (game thread); raw WriteFile dump at
// crash time, no allocation (skygfx diagnostics.cpp:36-63).
void EshDiag_RingPut(const char* msg);
void EshDiag_RingDump(void);

// dbglog_loc: "[HH:MM:SS.mmm] [LEVEL] file:line func() - msg" into
// EarshotReminid_dbg.log + ring push (skygfx diagnostics.cpp:104-160).
void EshDiag_LogLoc(EshDiagLevel level, const char* file, int line,
                    const char* func, const char* fmt, ...);

#define ESH_DBGLOG(...)      EshDiag_LogLoc(ESH_DIAG_INFO,  __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define ESH_DBGLOG_WARN(...) EshDiag_LogLoc(ESH_DIAG_WARN,  __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define ESH_DBGLOG_ERR(...)  EshDiag_LogLoc(ESH_DIAG_ERROR, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)

#endif // ESH_DIAGNOSTICS_DISABLED
