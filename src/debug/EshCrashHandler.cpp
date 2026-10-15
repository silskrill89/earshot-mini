// EshCrashHandler.cpp — crash forensics + diagnostics log (merged port).
//
// Source (READ-ONLY): E:\dev(dave)\skygfx_plus_expIV\src — every block cites
// the skygfx file:line it was ported from:
//   crashhandler.cpp  — unhandled-exception filter, minidump, WER backup,
//                       CRT-static-ctor self-install
//   diagnostics.cpp   — ring buffer, scope registry, dbglog, VEH, log init
//   main.cpp:4730-4768 — exe-version check + F8 console
//
// Divergences from skygfx are marked "DIVERGENCE:" inline and summarized in
// the port notes. Nothing here allocates or throws: all crash-path writes are
// stack buffers + raw CreateFile/WriteFile, every best-effort step runs in its
// own __try/__except, and the unhandled filter claims an Interlocked guard as
// its very first act (crashhandler.cpp:25-30).
//
// Link: dbghelp.lib (premake5.lua links { "dbghelp" }) for MiniDumpWriteDump.
#include "debug/EshCrashHandler.h"

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>   // abs
#include <string.h>

#include "DynAddress.h"   // plugin::GetGlobalAddress (exe-version check)

#ifndef ESH_DIAGNOSTICS_DISABLED

namespace {

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

// Re-entrancy guard: 0 = free, 1 = handler running (or ran)
// (crashhandler.cpp:46-47).
volatile LONG s_crashHandling = 0;

// Previous unhandled filter, saved at install time; chained after our work
// (crashhandler.cpp:49-50).
LPTOP_LEVEL_EXCEPTION_FILTER s_prevFilter = NULL;

// VEH handle returned by AddVectoredExceptionHandler (diagnostics.cpp:354).
LONG_PTR s_oldVEH = 0;

// Diagnostics log path (EarshotReminid_dbg.log). Filled by EshDiag_Init, or
// lazily by eshLogPath() before init (e.g. a crash during CRT static init).
char s_logPath[MAX_PATH] = { 0 };

// ---------------------------------------------------------------------------
// Path helpers (best-effort; return 0 on failure)
// ---------------------------------------------------------------------------

// Directory of the module containing this code (earshot_reminid.asi) WITH
// trailing backslash (crashhandler.cpp:59-77). Falls back to the exe dir.
DWORD eshSelfDir(char* buf, DWORD cap)
{
    HMODULE mod = NULL;
    DWORD n = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)&eshSelfDir, &mod) && mod)
        n = GetModuleFileNameA(mod, buf, cap);
    if (n == 0 || n >= cap)
        n = GetModuleFileNameA(NULL, buf, cap);
    if (n == 0 || n >= cap)
        return 0;
    char* slash = strrchr(buf, '\\');
    if (!slash)
        return 0;
    slash[1] = '\0';
    return 1;
}

// Directory of gta_sa.exe WITH trailing backslash (crashhandler.cpp:81-92).
DWORD eshExeDir(char* buf, DWORD cap)
{
    DWORD n = GetModuleFileNameA(NULL, buf, cap);
    if (n == 0 || n >= cap)
        return 0;
    char* slash = strrchr(buf, '\\');
    if (!slash)
        return 0;
    slash[1] = '\0';
    return 1;
}

// Default log path: <asi dir>\EarshotReminid_dbg.log (DIVERGENCE: skygfx uses
// skygfx_dbg.log beside skygfx.asi — crashhandler.cpp:111, main.cpp:4737-4741).
void eshBuildLogPath(char* out, DWORD cap)
{
    out[0] = '\0';
    if (!eshSelfDir(out, cap))
        return;
    strncat(out, "EarshotReminid_dbg.log", cap - strlen(out) - 1);
}

// Current log path, valid even before EshDiag_Init (lazy default build).
const char* eshLogPath(void)
{
    if (s_logPath[0] == '\0')
        eshBuildLogPath(s_logPath, MAX_PATH);
    return s_logPath;
}

} // namespace

// ---------------------------------------------------------------------------
// Ring buffer — lock-free, single writer (game thread); raw WriteFile dump at
// crash time, no allocation (diagnostics.cpp:31-63)
// ---------------------------------------------------------------------------
namespace {

char s_ring[ESH_RING_SIZE][ESH_RING_MSG_LEN];
volatile int s_ringPos = 0;

} // namespace

void EshDiag_RingPut(const char* msg)
{
    int pos = s_ringPos;
    char* slot = s_ring[pos % ESH_RING_SIZE];
    slot[0] = 0;
    strncpy(slot, msg, ESH_RING_MSG_LEN - 1);
    slot[ESH_RING_MSG_LEN - 1] = 0;
    s_ringPos = pos + 1;   // volatile store, single thread
}

void EshDiag_RingDump(void)
{
    int count = s_ringPos;
    if (count > ESH_RING_SIZE)
        count = ESH_RING_SIZE;
    int start = (s_ringPos - count) % ESH_RING_SIZE;
    // Always log to a file handle directly (recursive logging would be bad
    // during a crash) — diagnostics.cpp:48-49. DIVERGENCE: adds
    // FILE_SHARE_WRITE so a concurrent EshDiag_LogLoc handle can't block us.
    HANDLE h = CreateFileA(eshLogPath(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD w = 0;
    const char* hdr = "--- LAST LOG ENTRIES ---\n";
    WriteFile(h, hdr, (DWORD)strlen(hdr), &w, NULL);
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % ESH_RING_SIZE;
        WriteFile(h, s_ring[idx], (DWORD)strnlen(s_ring[idx], ESH_RING_MSG_LEN), &w, NULL);
        WriteFile(h, "\n", 1, &w, NULL);
    }
    const char* ftr = "--- END LOG ENTRIES ---\n";
    WriteFile(h, ftr, (DWORD)strlen(ftr), &w, NULL);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------
// Scope-tag registry — address -> name, nearest-EIP +/-0x400 match at crash
// (skygfx skygfx.h:110-120, diagnostics.cpp:65-97)
// ---------------------------------------------------------------------------
namespace {

EshScopeTag s_scopeTags[ESH_SCOPE_TAG_MAX];
int s_scopeTagCount = 0;

} // namespace

volatile LONG EshDiag_inGuardedRegion = 0;

void EshDiag_RegisterScope(const char* name, void* addr)
{
    if (s_scopeTagCount >= ESH_SCOPE_TAG_MAX)
        return;
    s_scopeTags[s_scopeTagCount].name = name;
    s_scopeTags[s_scopeTagCount].addr = addr;
    s_scopeTagCount++;
}

void EshDiag_LookupScope(void* eip, char* outName, int outSize, int* outOffset)
{
    outName[0] = 0;
    *outOffset = 0;
    uintptr_t addr = (uintptr_t)eip;
    int best = -1;
    int bestDist = 0x7FFFFFFF;
    for (int i = 0; i < s_scopeTagCount; i++) {
        int dist = (int)(addr - (uintptr_t)s_scopeTags[i].addr);
        if (dist >= -0x400 && dist < 0x400 && abs(dist) < bestDist) {
            bestDist = abs(dist);
            best = i;
            *outOffset = dist;
        }
    }
    if (best >= 0) {
        strncpy(outName, s_scopeTags[best].name, outSize - 1);
        outName[outSize - 1] = 0;
    }
}

// ---------------------------------------------------------------------------
// dbglog — "[HH:MM:SS.mmm] [LEVEL] file:line func() - msg" into
// EarshotReminid_dbg.log + ring push (skygfx diagnostics.cpp:100-160)
// ---------------------------------------------------------------------------
namespace {

const char* s_diagLevelStr[] = { "INFO ", "WARN ", "ERROR", "FATAL" };

void eshDiagLogInternal(EshDiagLevel level, const char* file, int line,
                        const char* func, const char* fmt, va_list ap)
{
    char msg[2048];
    int len = vsnprintf(msg, sizeof(msg), fmt, ap);
    if (len < 0)
        return;
    if (len >= (int)sizeof(msg))
        len = (int)sizeof(msg) - 1;

    const char* shortFile = file;
    for (const char* p = file; *p; p++)
        if (*p == '\\' || *p == '/') shortFile = p + 1;

    char buf[2304];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int hlen;
    if (level >= ESH_DIAG_INFO)
        hlen = snprintf(buf, sizeof(buf),
                        "[%02d:%02d:%02d.%03d] [%s] %s:%d %s() - %s\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        s_diagLevelStr[level], shortFile, line, func, msg);
    else
        hlen = snprintf(buf, sizeof(buf),
                        "[%02d:%02d:%02d.%03d] [TRACE] %s:%d %s() - %s\n",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                        shortFile, line, func, msg);
    if (hlen < 0 || hlen >= (int)sizeof(buf))
        return;

    // Ring push (truncated to fit). DIVERGENCE: skygfx pushes msg+14 with a
    // "skip timestamp prefix" comment (diagnostics.cpp:132) although msg has
    // no timestamp — it silently drops the first 14 chars of every entry.
    // We push the message as-is.
    char ringMsg[ESH_RING_MSG_LEN];
    snprintf(ringMsg, sizeof(ringMsg), "%s", msg);
    EshDiag_RingPut(ringMsg);

    // Raw CreateFile/WriteFile per line — no long-lived lock, no allocation
    // (diagnostics.cpp:135-141).
    HANDLE h = CreateFileA(eshLogPath(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(h, buf, (DWORD)hlen, &written, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

} // namespace

void EshDiag_LogLoc(EshDiagLevel level, const char* file, int line,
                    const char* func, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    eshDiagLogInternal(level, file, line, func, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------
// VEH crash-context log — forensics only, never suppresses
// (skygfx diagnostics.cpp:162-268)
// ---------------------------------------------------------------------------
namespace {

// Stack[0-7] dump in its own SEH scope: a bogus ESP must not fault inside the
// VEH (DIVERGENCE: skygfx reads it unguarded, diagnostics.cpp:237-241).
void eshLogStack(DWORD esp)
{
    if (esp <= 0x10000 || esp >= 0x80000000)
        return;
    __try {
        DWORD* sp = (DWORD*)esp;
        ESH_DBGLOG("  STACK[0-7]: %08X %08X %08X %08X %08X %08X %08X %08X",
                   sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

LONG WINAPI eshVehHandler(EXCEPTION_POINTERS* ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Let C++ exceptions pass through to try/catch (diagnostics.cpp:175-178)
    if (code == 0xE06D7363)
        return EXCEPTION_CONTINUE_SEARCH;

    // Guard-phase pass-through: bounded ring entry only, no file I/O
    // (diagnostics.cpp:186-195; skygfx also logs render phase + guard depth —
    // we have neither, so only the EIP survives).
    if (EshDiag_inGuardedRegion) {
        char gmsg[ESH_RING_MSG_LEN];
        snprintf(gmsg, sizeof(gmsg), "GUARD-FAULT eip=%08X",
                 ep->ContextRecord->Eip);
        gmsg[sizeof(gmsg) - 1] = 0;
        EshDiag_RingPut(gmsg);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Crash-count cap (diagnostics.cpp:197-198)
    static int s_crashCount = 0;
    if (++s_crashCount > 20)
        return EXCEPTION_CONTINUE_SEARCH;

    CONTEXT* ctx = ep->ContextRecord;
    DWORD eip = ctx->Eip;
    DWORD esp = ctx->Esp;

    // Zone heuristic from the EIP range (diagnostics.cpp:205-209)
    const char* crashZone = "UNKNOWN";
    if (eip >= 0x400000 && eip < 0x800000)  crashZone = "GAME_TEXT";
    else if (eip >= 0x10000000)             crashZone = "HEAP/DLL";
    else if (eip < 0x10000)                 crashZone = "NULL_DEREF";

    // Crash type + null-deref offset (diagnostics.cpp:211-230)
    const char* crashType = "ACCESS_VIOLATION";
    if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
        DWORD accessType = ep->ExceptionRecord->ExceptionInformation[0];
        DWORD accessAddr = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
        if (accessType == 0)      crashType = "READ";
        else if (accessType == 1) crashType = "WRITE";
        else if (accessType == 8) crashType = "DEP";

        if (accessAddr < 0x1000)
            ESH_DBGLOG("CRASH[%d]: %s at 0x%08X (zone=%s) accessing 0x%08X — NULL-pointer deref, offset +0x%X",
                       s_crashCount, crashType, eip, crashZone, accessAddr, accessAddr);
        else
            ESH_DBGLOG("CRASH[%d]: %s at 0x%08X (zone=%s) accessing 0x%08X",
                       s_crashCount, crashType, eip, crashZone, accessAddr);
    } else {
        ESH_DBGLOG("CRASH[%d]: code=0x%08X at 0x%08X (zone=%s)",
                   s_crashCount, code, eip, crashZone);
    }

    // REGS + EBP/ESP (diagnostics.cpp:232-234)
    ESH_DBGLOG("  REGS: EAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X",
               ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx, ctx->Esi, ctx->Edi);
    ESH_DBGLOG("  STACK: EBP=%08X ESP=%08X", ctx->Ebp, esp);

    eshLogStack(esp);   // diagnostics.cpp:237-241 (SEH-wrapped, see above)

    // Nearest scope tag (diagnostics.cpp:244-251)
    char scopeName[128];
    int scopeOff = 0;
    EshDiag_LookupScope((void*)(uintptr_t)eip, scopeName, sizeof(scopeName), &scopeOff);
    if (scopeName[0])
        ESH_DBGLOG("  EARSHOT SCOPE: %s (+0x%X)", scopeName, scopeOff);
    else
        ESH_DBGLOG("  EARSHOT SCOPE: (none matched within 0x400)");

    // Ring dump — raw WriteFile, no allocation (diagnostics.cpp:254)
    EshDiag_RingDump();

    // Always let the crash propagate: no auto-fixes, no swallowing
    // (diagnostics.cpp:256-267). The unhandled filter downstream writes the
    // minidump.
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void EshDiag_InstallVEH(void)
{
    if (s_oldVEH)
        return;
    s_oldVEH = (LONG_PTR)AddVectoredExceptionHandler(1, eshVehHandler);
}

void EshDiag_RemoveVEH(void)
{
    if (s_oldVEH) {
        RemoveVectoredExceptionHandler((HANDLE)s_oldVEH);
        s_oldVEH = 0;
    }
}

// ---------------------------------------------------------------------------
// Log init — rotate the previous boot's log to <name>.prev.log, then truncate
// a fresh one (skygfx diagnostics.cpp:359-396; the rotation is FIX-5
// "log-evidence destruction" — never wipe the last boot's crash evidence)
// ---------------------------------------------------------------------------
void EshDiag_Init(const char* logPath)
{
    if (logPath && logPath[0]) {
        strncpy(s_logPath, logPath, MAX_PATH - 1);
        s_logPath[MAX_PATH - 1] = '\0';
    } else {
        eshBuildLogPath(s_logPath, MAX_PATH);
    }
    if (s_logPath[0] == '\0')
        return;

    // "<dir>\EarshotReminid_dbg.log" -> "<dir>\EarshotReminid_dbg.prev.log";
    // strip only the FILENAME's extension (dots in the directory survive).
    char prevPath[MAX_PATH];
    strncpy(prevPath, s_logPath, MAX_PATH - 1);
    prevPath[MAX_PATH - 1] = '\0';
    char* slash = strrchr(prevPath, '\\');
    if (!slash) slash = strrchr(prevPath, '/');
    char* dot = strrchr(prevPath, '.');
    if (dot && (!slash || dot > slash))
        *dot = '\0';
    strncat(prevPath, ".prev.log", sizeof(prevPath) - strlen(prevPath) - 1);

    if (!MoveFileExA(s_logPath, prevPath, MOVEFILE_REPLACE_EXISTING)) {
        // Rename failed (locked reader?) — copy so the old evidence survives
        // the truncate below (diagnostics.cpp:385-389).
        CopyFileA(s_logPath, prevPath, FALSE);
    }

    // Create (now empty) log for this boot (diagnostics.cpp:392-395)
    HANDLE hLog = CreateFileA(s_logPath, GENERIC_WRITE, FILE_SHARE_READ,
                              NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hLog != INVALID_HANDLE_VALUE)
        CloseHandle(hLog);
}

// ---------------------------------------------------------------------------
// Host-exe version check (skygfx main.cpp:4753-4761)
// ---------------------------------------------------------------------------
void EshDiag_CheckExeVersion(void)
{
    // DIVERGENCE: skygfx reads the two DWORDs unguarded; we wrap the reads in
    // SEH so a foreign exe whose image doesn't cover those RVAs warns instead
    // of faulting at load (crash-parity rule: never AV during init).
    DWORD v1 = 0, v2 = 0;
    __try {
        v1 = *(const DWORD*)plugin::GetGlobalAddress(0x82457C);
        v2 = *(const DWORD*)plugin::GetGlobalAddress(0x8245BC);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ESH_DBGLOG_WARN("version check: signature read faulted — host exe is not SA 1.0 US");
        return;
    }

    ESH_DBGLOG("ver check: 0x82457C=%08X, 0x8245BC=%08X (expect 0x94BF)", v1, v2);
    // DIVERGENCE: skygfx warns only on "v1 mismatch && v2 match"; the task
    // semantics are "either mismatch -> warn and continue", which is what a
    // version gate should say.
    if (v1 != 0x94BF || v2 != 0x94BF)
        ESH_DBGLOG_WARN("host exe is not SA 1.0 US (0x%08X/0x%08X, expect 0x94BF) — continuing", v1, v2);
    else
        ESH_DBGLOG("version check OK");
}

// ---------------------------------------------------------------------------
// F8-held-at-load console (skygfx main.cpp:4763-4768)
// ---------------------------------------------------------------------------
void EshDiag_ConsoleFromF8(void)
{
    if (GetAsyncKeyState(VK_F8) & 0x8000) {
        AllocConsole();
        freopen("CONIN$", "r", stdin);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        ESH_DBGLOG("F8 held at load — debug console attached");
    }
}

// ===========================================================================
// Unhandled-exception filter (crashhandler.cpp)
//
// Why SetUnhandledExceptionFilter and NOT a VEH for the dump: expected/caught
// first-chance faults must not produce dumps; the filter only runs when
// nothing else claimed the exception (crashhandler.cpp:17-23). The VEH above
// stays forensics-only.
// ===========================================================================
namespace {

// (a) One-line crash record appended to EarshotReminid_dbg.log
// (crashhandler.cpp:94-165; DIVERGENCE: skygfx writes skygfx_dbg.log).
void eshCrashAppendLog(EXCEPTION_POINTERS* ep)
{
    char path[MAX_PATH];
    eshBuildLogPath(path, sizeof(path));
    if (path[0] == '\0')
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    DWORD code = 0;
    DWORD addr = 0;
    if (ep && ep->ExceptionRecord)
        code = ep->ExceptionRecord->ExceptionCode;
    if (ep && ep->ContextRecord)
        addr = ep->ContextRecord->Eip;   // x86 ExceptionAddress
    else if (ep && ep->ExceptionRecord)
        addr = (DWORD)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;

    char line[320];
    int n;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->NumberParameters >= 2)
    {
        DWORD op = (DWORD)ep->ExceptionRecord->ExceptionInformation[0];
        DWORD fault = (DWORD)ep->ExceptionRecord->ExceptionInformation[1];
        const char* opStr =
            op == 0 ? "READ" :
            op == 1 ? "WRITE" :
            op == 8 ? "DEP" : "OTHER";
        n = snprintf(line, sizeof(line),
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [CRASH] code=0x%08X addr=0x%08X op=%u(%s) fault=0x%08X\n",
            (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
            (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
            (unsigned)st.wMilliseconds,
            code, addr, op, opStr, fault);
    } else {
        n = snprintf(line, sizeof(line),
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [CRASH] code=0x%08X addr=0x%08X\n",
            (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
            (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
            (unsigned)st.wMilliseconds,
            code, addr);
    }
    if (n <= 0)
        return;
    if (n >= (int)sizeof(line))
        n = (int)sizeof(line) - 1;   // never read past the buffer

    // "a" semantics: append regardless of EOF; independent handle
    // (crashhandler.cpp:155-164).
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(h, line, (DWORD)n, &written, NULL);
    CloseHandle(h);
}

// (b) Minidump: <asi dir>\EarshotCrash_YYYY-MM-DD_HHMMSS.dmp
// (crashhandler.cpp:167-219; DIVERGENCE: skygfx writes beside gta_sa.exe and
// names it skygfx_crash_* — the task puts dumps beside the .asi).
void eshCrashWriteDump(EXCEPTION_POINTERS* ep)
{
    char dir[MAX_PATH];
    if (!eshSelfDir(dir, sizeof(dir)))
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
    snprintf(path, sizeof(path),
             "%sEarshotCrash_%04u-%02u-%02u_%02u%02u%02u.dmp",
             dir,
             (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);

    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return;   // best-effort: fall through, never raise

    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_EXCEPTION_INFORMATION* pMei = NULL;
    if (ep) {
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        pMei = &mei;
    }

    MINIDUMP_TYPE type = (MINIDUMP_TYPE)(
        MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);

    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                hFile, type, pMei, NULL, NULL);
    DWORD size = GetFileSize(hFile, NULL);

    if (!ok || size == 0 || size == INVALID_FILE_SIZE) {
        // Retry once with MiniDumpNormal (crashhandler.cpp:208-217)
        CloseHandle(hFile);
        hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          hFile, MiniDumpNormal, pMei, NULL, NULL);
    }
    CloseHandle(hFile);
}

// The unhandled filter (crashhandler.cpp:225-249)
LONG WINAPI eshCrashUnhandledFilter(EXCEPTION_POINTERS* ep)
{
    // 1. Claim the re-entrancy flag BEFORE anything else (no alloc, no I/O).
    if (InterlockedCompareExchange(&s_crashHandling, 1, 0) != 0)
        return EXCEPTION_EXECUTE_HANDLER;   // recursive fault: don't redo work

    // 2. (a) log line — own SEH scope so a fault here still lets the dump run
    __try {
        eshCrashAppendLog(ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    // 3. (b) minidump — own SEH scope; failure/0-byte retry handled internally
    __try {
        eshCrashWriteDump(ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    // 4. Chain: previous filter runs AFTER our work; prefer its behavior
    if (s_prevFilter)
        return s_prevFilter(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

// (4) Best-effort WER LocalDumps backup — silent failure (likely no admin)
// (crashhandler.cpp:251-280). KEPT: it is three Reg calls and covers the
// case where our own dump write fails (disk full, etc.).
void eshCrashInstallWerBackup(void)
{
    char dir[MAX_PATH];
    if (!eshExeDir(dir, sizeof(dir)))
        return;
    size_t len = strlen(dir);
    if (len > 0 && dir[len - 1] == '\\') {
        dir[len - 1] = '\0';   // DumpFolder without trailing slash
        len--;
    }

    HKEY hKey = NULL;
    LONG rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                              "SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\gta_sa.exe",
                              0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL);
    if (rc != ERROR_SUCCESS)
        return;   // silent: no admin rights, etc. — never abort

    RegSetValueExA(hKey, "DumpFolder", 0, REG_EXPAND_SZ,
                   (const BYTE*)dir, (DWORD)(len + 1));   // +1 = NUL
    DWORD one = 1;
    RegSetValueExA(hKey, "DumpType", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExA(hKey, "DumpMonitored", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegCloseKey(hKey);
}

// Self-installer: CRT static init runs this at DLL load — no hook required
// (crashhandler.cpp:282-302).
struct EshCrashHandlerInstaller {
    EshCrashHandlerInstaller();
};

EshCrashHandlerInstaller::EshCrashHandlerInstaller()
{
    // Save + install; we run first, the previous filter runs after us and its
    // return value wins (crashhandler.cpp:12-15, :290-298).
    s_prevFilter = SetUnhandledExceptionFilter(eshCrashUnhandledFilter);

    // WER backup for the case our own dump write fails.
    eshCrashInstallWerBackup();
}

// Namespace-scope static: constructor executes during CRT initialization of
// this ASI — no entry-point hook required.
EshCrashHandlerInstaller s_eshCrashInstaller;

} // anonymous namespace

#endif // ESH_DIAGNOSTICS_DISABLED
