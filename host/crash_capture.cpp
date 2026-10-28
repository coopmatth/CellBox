// crash_capture.cpp â€” see crash_capture.h for the rationale.
// Host-only TU. Raw Win32 file I/O only in the fault paths (no CRT
// allocation, no std::string formatting inside VEH/terminate handlers).
// The listener path uses the same raw writer under an SRW lock.

#include "crash_capture.h"

#include <windows.h>
#include <exception>
#include <csignal>
#include <cstdlib>

#include "util/logs.hpp"

#include "Emu/Memory/vm.h"

namespace emu_host
{

namespace
{

// ---- raw append writer (no allocation, safe in fault handlers) ----
void raw_append(const wchar_t* path, const char* line, unsigned long len)
{
    HANDLE h = CreateFile2(path, FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(h, line, len, &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

void append_u32_hex(char* buf, unsigned long& pos, unsigned long v)
{
    static const char* hex = "0123456789ABCDEF";
    buf[pos++] = '0';
    buf[pos++] = 'x';
    bool started = false;
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        unsigned long d = (v >> shift) & 0xF;
        if (d || started || shift == 0)
        {
            buf[pos++] = hex[d];
            started = true;
        }
    }
}

void append_u64_hex(char* buf, unsigned long& pos, unsigned long long v)
{
    static const char* hex = "0123456789ABCDEF";
    buf[pos++] = '0';
    buf[pos++] = 'x';
    bool started = false;
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        unsigned long d = static_cast<unsigned long>((v >> shift) & 0xF);
        if (d || started || shift == 0)
        {
            buf[pos++] = hex[d];
            started = true;
        }
    }
}

void append_cstr(char* buf, unsigned long& pos, const char* s)
{
    while (*s && pos < 900)
        buf[pos++] = *s++;
}

void append_u64_dec(char* buf, unsigned long& pos, unsigned long long v)
{
    char tmp[24];
    int n = 0;
    if (v == 0)
    {
        buf[pos++] = '0';
        return;
    }
    while (v > 0 && n < 24)
    {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && pos < 900)
        buf[pos++] = tmp[--n];
}

// Wide path buffers, filled once by crash_capture_install.
wchar_t g_crash_log_path[MAX_PATH * 2] = {};
wchar_t g_veh_log_path[MAX_PATH * 2] = {};
SRWLOCK g_crash_lock = SRWLOCK_INIT;
bool g_installed = false;

bool wide_from_utf8(const std::string& s, wchar_t* out, int out_chars)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out, out_chars);
    return n > 0;
}

// ---- VEH: process-wide, every thread ----
// v1.0.27.0: on an access violation the VEH ALSO tries commit-on-
// touch (Grok verdict on the 1.0.26 AV 0xC0000005 after on_ready).
// If the fault address lies inside a known SEC_RESERVE view, commit
// exactly that one 4 KiB page with VirtualAllocFromApp MEM_COMMIT
// and return EXCEPTION_CONTINUE_EXECUTION so the faulting
// instruction retries. Out-of-range faults, DEP faults, commit
// failures, and repeat faults on the same page (4+ times) fall
// through to EXCEPTION_CONTINUE_SEARCH (the old behaviour). The
// full 32 GiB region is NEVER committed: that would re-break the
// 5 GiB Game-partition commit ceiling that killed 1.0.23/1.0.25.
// Range bases are the core's own extern const pointers (Emu/Memory/
// vm.h, vm.cpp:43-55): g_base 8 GiB, g_exec 12 GiB, g_stat 4 GiB
// (default size). They are set by global constructors before Run(),
// so they are valid by boot time. g_hook_addr itself is static in
// vm.cpp (internal linkage, not exported), so it is DERIVED, not
// referenced: the stat reservation scan starts at g_hook_addr and
// its first free 4 GiB-aligned window past the 32 GiB hook region
// is hook + 0x800000000, hence hook = g_stat_addr - 0x800000000
// (same 4 GiB-step scan logic as vm.cpp:29-38). m_common (the
// preallocated vm::main block, guest 0x10000, 0xFFF0000 bytes) lives
// inside g_base at host offset 0x10000, so it is range-checked first
// and reported as m_common, not base.
const char* range_label(unsigned long long addr)
{
    const unsigned long long stat = reinterpret_cast<unsigned long long>(vm::g_stat_addr);
    const unsigned long long hook = stat - 0x800000000ULL;
    const unsigned long long base = reinterpret_cast<unsigned long long>(vm::g_base_addr);
    const unsigned long long exec = reinterpret_cast<unsigned long long>(vm::g_exec_addr);
    if (addr >= base + 0x10000ULL && addr < base + 0x10000ULL + 0xFFF0000ULL)
        return "m_common";
    if (addr >= hook && addr < hook + 0x800000000ULL)
        return "g_hook";
    if (addr >= base && addr < base + 0x200000000ULL)
        return "g_base";
    if (addr >= exec && addr < exec + 0x300000000ULL)
        return "g_exec";
    if (addr >= stat && addr < stat + 0x100000000ULL)
        return "g_stat";
    return "none";
}

// Anti-loop table: fixed-size, no allocation in the handler. A page
// that faults 4+ times is not retried â€” its commit is not making
// progress (e.g. guard page), so fall through instead of spinning.
struct fault_count_entry { unsigned long long page; unsigned int count; };
fault_count_entry g_fault_counts[16] = {};
unsigned int g_fault_count_used = 0;

unsigned int fault_count_for(unsigned long long page)
{
    for (unsigned int i = 0; i < g_fault_count_used; ++i)
    {
        if (g_fault_counts[i].page == page)
            return ++g_fault_counts[i].count;
    }
    if (g_fault_count_used < 16)
    {
        g_fault_counts[g_fault_count_used].page = page;
        g_fault_counts[g_fault_count_used].count = 1;
        ++g_fault_count_used;
        return 1;
    }
    return 1; // table full: do not block a first-seen page on bookkeeping
}

LONG WINAPI veh_handler(EXCEPTION_POINTERS* ep)
{
    const unsigned long code = static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode);
    const unsigned long long instr = reinterpret_cast<unsigned long long>(ep->ExceptionRecord->ExceptionAddress);
    const unsigned long long fault = (ep->ExceptionRecord->NumberParameters > 1)
        ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[1])
        : 0ULL;
    const unsigned long long info0 = (ep->ExceptionRecord->NumberParameters > 0)
        ? static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[0])
        : 0ULL;
    const unsigned long long module_base = reinterpret_cast<unsigned long long>(GetModuleHandleW(nullptr));
    const char* range = (code == 0xC0000005 && fault != 0) ? range_label(fault) : "none";
    char buf[1024];
    unsigned long pos = 0;
    append_cstr(buf, pos, "BOOT-VEH code=");
    append_u32_hex(buf, pos, code);
    append_cstr(buf, pos, " addr=");
    append_u64_hex(buf, pos, instr);
    append_cstr(buf, pos, " thread=");
    append_u64_dec(buf, pos, static_cast<unsigned long long>(GetCurrentThreadId()));
    append_cstr(buf, pos, " info0=");
    if (ep->ExceptionRecord->NumberParameters > 0)
        append_u64_hex(buf, pos, info0);
    else
        append_cstr(buf, pos, "none");
    append_cstr(buf, pos, " fault=");
    if (fault != 0)
        append_u64_hex(buf, pos, fault);
    else
        append_cstr(buf, pos, "none");
    append_cstr(buf, pos, " base=");
    if (module_base != 0)
        append_u64_hex(buf, pos, module_base);
    else
        append_cstr(buf, pos, "none");
    append_cstr(buf, pos, " rva=");
    if (fault != 0 && module_base != 0 && fault >= module_base)
        append_u64_hex(buf, pos, fault - module_base);
    else
        append_cstr(buf, pos, "none");
    append_cstr(buf, pos, " range=");
    append_cstr(buf, pos, range);
    append_cstr(buf, pos, "\r\n");
    raw_append(g_veh_log_path, buf, pos);
    // v1.0.28.0 (Grok cross-check additions): key registers from
    // CONTEXT (the null base is typically RCX/RDX for a this-pointer
    // deref) and the first 16 bytes of the faulting instruction.
    // Registers are read from ep->ContextRecord; the instruction
    // bytes are copied only after VirtualQuery proves the page is
    // committed and executable/readable, so the handler itself
    // cannot fault reading them.
    {
        const CONTEXT* ctx = ep->ContextRecord;
        if (ctx)
        {
            char rbuf[512];
            unsigned long rpos = 0;
            append_cstr(rbuf, rpos, "BOOT-VEH-REGS rip=");
            append_u64_hex(rbuf, rpos, static_cast<unsigned long long>(ctx->Rip));
            append_cstr(rbuf, rpos, " rcx=");
            append_u64_hex(rbuf, rpos, static_cast<unsigned long long>(ctx->Rcx));
            append_cstr(rbuf, rpos, " rdx=");
            append_u64_hex(rbuf, rpos, static_cast<unsigned long long>(ctx->Rdx));
            append_cstr(rbuf, rpos, " r8=");
            append_u64_hex(rbuf, rpos, static_cast<unsigned long long>(ctx->R8));
            append_cstr(rbuf, rpos, " r9=");
            append_u64_hex(rbuf, rpos, static_cast<unsigned long long>(ctx->R9));
            append_cstr(rbuf, rpos, " rsp=");
            append_u64_hex(rbuf, rpos, static_cast<unsigned long long>(ctx->Rsp));
            append_cstr(rbuf, rpos, " rbp=");
            append_u64_hex(rbuf, rpos, static_cast<unsigned long long>(ctx->Rbp));
            append_cstr(rbuf, rpos, "\r\n");
            raw_append(g_veh_log_path, rbuf, rpos);
        }
        MEMORY_BASIC_INFORMATION mbi = {};
        char ibuf[256];
        unsigned long ipos = 0;
        append_cstr(ibuf, ipos, "BOOT-VEH-INSN bytes=");
        if (VirtualQuery(reinterpret_cast<void*>(instr), &mbi, sizeof(mbi)) != 0
            && mbi.State == MEM_COMMIT
            && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_READONLY | PAGE_READWRITE)) != 0)
        {
            const unsigned char* ip = reinterpret_cast<const unsigned char*>(instr);
            static const char* hexd = "0123456789ABCDEF";
            for (int k = 0; k < 16; ++k)
            {
                ibuf[ipos++] = hexd[(ip[k] >> 4) & 0xF];
                ibuf[ipos++] = hexd[ip[k] & 0xF];
            }
        }
        else
        {
            append_cstr(ibuf, ipos, "unreadable");
        }
        append_cstr(ibuf, ipos, "\r\n");
        raw_append(g_veh_log_path, ibuf, ipos);
    }
    // v1.0.28.0: stack trace on ANY AV (Matt requirement) â€” up to 16
    // return addresses from the faulting thread, each with RVA and
    // range check, flushed before the handler returns. RtlCapture-
    // StackBackTrace is resolved dynamically (no static import the
    // console OS might not provide; same pattern as the VEH resolve).
    // FramesToSkip=1 skips this handler's own frame. If resolution
    // fails, one unavailable line is logged instead of silence.
    {
        typedef unsigned short (WINAPI* RtlCaptureStackBackTraceFn)(unsigned long, unsigned long, void**, unsigned long*);
        void* addrs[16] = {};
        unsigned long captured = 0;
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        RtlCaptureStackBackTraceFn capture = nullptr;
        if (ntdll)
            capture = reinterpret_cast<RtlCaptureStackBackTraceFn>(GetProcAddress(ntdll, "RtlCaptureStackBackTrace"));
        if (capture)
            captured = capture(1, 16, addrs, nullptr);
        if (captured == 0)
        {
            const char* noline = "BOOT-VEH-STACK unavailable (RtlCaptureStackBackTrace not resolved or 0 frames)\r\n";
            unsigned long nlen = 0;
            while (noline[nlen]) ++nlen;
            raw_append(g_veh_log_path, noline, nlen);
        }
        else
        {
            for (unsigned long i = 0; i < captured; ++i)
            {
                const unsigned long long a = reinterpret_cast<unsigned long long>(addrs[i]);
                const char* arange = range_label(a);
                // Module + section for this address (Grok addition):
                // AllocationBase via VirtualQuery, module basename via
                // GetModuleFileNameW, section via the module's PE
                // section table (only when the base parses as MZ/PE).
                char modname[64] = {};
                char secname[16] = {};
                {
                    MEMORY_BASIC_INFORMATION mbi2 = {};
                    if (VirtualQuery(reinterpret_cast<void*>(a), &mbi2, sizeof(mbi2)) != 0 && mbi2.AllocationBase != nullptr)
                    {
                        wchar_t wpath[260] = {};
                        if (GetModuleFileNameW(reinterpret_cast<HMODULE>(mbi2.AllocationBase), wpath, 260) != 0)
                        {
                            int wlen = 0;
                            while (wpath[wlen] != 0) ++wlen;
                            int start = wlen;
                            while (start > 0 && wpath[start - 1] != L'\\' && wpath[start - 1] != L'/') --start;
                            int o = 0;
                            for (int q = start; q < wlen && o < 60; ++q)
                                modname[o++] = (wpath[q] < 128) ? static_cast<char>(wpath[q]) : '?';
                            modname[o] = 0;
                        }
                        const unsigned char* base8 = reinterpret_cast<const unsigned char*>(mbi2.AllocationBase);
                        if (base8[0] == 'M' && base8[1] == 'Z')
                        {
                            const unsigned int peoff = *reinterpret_cast<const unsigned int*>(base8 + 0x3C);
                            if (peoff < 0x1000 && base8[peoff] == 'P' && base8[peoff + 1] == 'E')
                            {
                                const unsigned short nsec = *reinterpret_cast<const unsigned short*>(base8 + peoff + 6);
                                const unsigned int optsize = *reinterpret_cast<const unsigned short*>(base8 + peoff + 20);
                                const unsigned char* sec = base8 + peoff + 24 + optsize;
                                const unsigned long long rva2 = a - reinterpret_cast<unsigned long long>(mbi2.AllocationBase);
                                for (unsigned short si = 0; si < nsec && si < 96; ++si)
                                {
                                    const unsigned char* ent = sec + si * 40;
                                    const unsigned int vsz = *reinterpret_cast<const unsigned int*>(ent + 8);
                                    const unsigned int va = *reinterpret_cast<const unsigned int*>(ent + 12);
                                    if (rva2 >= va && rva2 < va + vsz)
                                    {
                                        int o = 0;
                                        for (int q = 0; q < 8 && ent[q] != 0 && o < 14; ++q) secname[o++] = static_cast<char>(ent[q]);
                                        secname[o] = 0;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                char sbuf[512];
                unsigned long spos = 0;
                append_cstr(sbuf, spos, "BOOT-VEH-STACK idx=");
                append_u64_dec(sbuf, spos, static_cast<unsigned long long>(i));
                append_cstr(sbuf, spos, " addr=");
                append_u64_hex(sbuf, spos, a);
                append_cstr(sbuf, spos, " rva=");
                if (module_base != 0 && a >= module_base)
                    append_u64_hex(sbuf, spos, a - module_base);
                else
                    append_cstr(sbuf, spos, "none");
                append_cstr(sbuf, spos, " range=");
                append_cstr(sbuf, spos, arange);
                append_cstr(sbuf, spos, " module=");
                append_cstr(sbuf, spos, (modname[0] != 0) ? modname : "none");
                append_cstr(sbuf, spos, " section=");
                append_cstr(sbuf, spos, (secname[0] != 0) ? secname : "none");
                append_cstr(sbuf, spos, "\r\n");
                raw_append(g_veh_log_path, sbuf, spos);
            }
        }
    }
    // Commit-on-touch (1.0.27): access violation, read(0)/write(1)
    // only, fault inside a known reserved view, page not already
    // retried 4+ times. Commit exactly one 4 KiB page and retry the
    // faulting instruction. Everything else keeps the old behaviour.
    if (code == 0xC0000005 && fault != 0 && (info0 == 0 || info0 == 1) && range[0] != 'n')
    {
        const unsigned long long page = fault & ~0xFFFULL;
        if (fault_count_for(page) < 4)
        {
            void* committed = VirtualAllocFromApp(reinterpret_cast<void*>(page), 4096, MEM_COMMIT, PAGE_READWRITE);
            char cbuf[512];
            unsigned long cpos = 0;
            append_cstr(cbuf, cpos, "BOOT-VEH-COMMIT page=");
            append_u64_hex(cbuf, cpos, page);
            append_cstr(cbuf, cpos, " range=");
            append_cstr(cbuf, cpos, range);
            append_cstr(cbuf, cpos, " result=");
            if (committed != nullptr)
            {
                append_cstr(cbuf, cpos, "ok");
                append_cstr(cbuf, cpos, "\r\n");
                raw_append(g_veh_log_path, cbuf, cpos);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            append_cstr(cbuf, cpos, "fail err=");
            append_u32_hex(cbuf, cpos, static_cast<unsigned long>(GetLastError()));
            append_cstr(cbuf, cpos, "\r\n");
            raw_append(g_veh_log_path, cbuf, cpos);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        char lbuf[512];
        unsigned long lpos = 0;
        append_cstr(lbuf, lpos, "BOOT-VEH-COMMIT-LOOP page=");
        append_u64_hex(lbuf, lpos, page);
        append_cstr(lbuf, lpos, " range=");
        append_cstr(lbuf, lpos, range);
        append_cstr(lbuf, lpos, " (4+ faults, not retried)\r\n");
        raw_append(g_veh_log_path, lbuf, lpos);
    }
    // The log is on disk now; let the OS terminate the app normally
    // (or the SEH filter swallow it, as before).
    return EXCEPTION_CONTINUE_SEARCH;
}

void atexit_handler()
{
    const char* line = "BOOT-ATEXIT: normal exit path (exit()/main return; NOT _exit/TerminateProcess/failfast/PLM kill)\r\n";
    unsigned long len = 0;
    while (line[len])
        ++len;
    raw_append(g_veh_log_path, line, len);
}

void abort_handler(int /*sig*/)
{
    const char* line = "BOOT-ABORT: SIGABRT (direct abort() on some thread; no std::terminate, no VEH)\r\n";
    unsigned long len = 0;
    while (line[len])
        ++len;
    raw_append(g_veh_log_path, line, len);
    _exit(3);
}

void invalid_param_handler(const wchar_t* /*expr*/, const wchar_t* /*func*/, const wchar_t* /*file*/, unsigned int /*line*/, uintptr_t /*reserved*/)
{
    const char* line = "BOOT-INVPARAM: CRT invalid-parameter (no std::terminate, no VEH)\r\n";
    unsigned long len = 0;
    while (line[len])
        ++len;
    raw_append(g_veh_log_path, line, len);
}

void purecall_handler()
{
    const char* line = "BOOT-PURECALL: pure virtual call (no std::terminate, no VEH)\r\n";
    unsigned long len = 0;
    while (line[len])
        ++len;
    raw_append(g_veh_log_path, line, len);
    _exit(3);
}

LONG WINAPI uhf_handler(EXCEPTION_POINTERS* ep)
{
    char buf[256];
    unsigned long pos = 0;
    append_cstr(buf, pos, "BOOT-UHF: unhandled SEH code=");
    append_u32_hex(buf, pos, ep ? static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode) : 0);
    append_cstr(buf, pos, " addr=");
    append_u64_hex(buf, pos, ep ? static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)) : 0);
    append_cstr(buf, pos, "\r\n");
    raw_append(g_veh_log_path, buf, pos);
    return EXCEPTION_CONTINUE_SEARCH;
}

void terminate_handler()
{
    const char* line = "BOOT-TERMINATE: std::terminate called during boot\r\n";
    unsigned long len = 0;
    while (line[len])
        ++len;
    raw_append(g_veh_log_path, line, len);
    std::abort();
}

// ---- core log listener: fatal/error only, mirrored to LocalState ----
struct crash_listener : logs::listener
{
    void log(u64 /*stamp*/, const logs::message& msg, std::string_view prefix,
             std::string_view text) override
    {
        const logs::level sev = static_cast<logs::level>(msg);
        if (sev > logs::level::error)
            return; // always/fatal/error only; notice/trace would flood
        std::string line = "BOOT-CORE-LOG sev=";
        line += (sev == logs::level::always ? "always" : sev == logs::level::fatal ? "fatal" : "error");
        line += " ch=";
        line += prefix;
        line += " text=";
        line += text;
        line += "\r\n";
        AcquireSRWLockExclusive(&g_crash_lock);
        raw_append(g_crash_log_path, line.c_str(), static_cast<unsigned long>(line.size()));
        ReleaseSRWLockExclusive(&g_crash_lock);
    }
};

} // namespace

void crash_capture_install(const std::string& localStateUtf8,
                           std::function<void(const std::string&)> log)
{
    if (g_installed)
    {
        log("BOOT-CAPTURE: already installed (listener+VEH+terminate)");
        return;
    }
    std::string crash_log = localStateUtf8 + "/boot-crash-log.txt";
    std::string veh_log = localStateUtf8 + "/boot-veh-log.txt";
    if (!wide_from_utf8(crash_log, g_crash_log_path, MAX_PATH * 2) ||
        !wide_from_utf8(veh_log, g_veh_log_path, MAX_PATH * 2))
    {
        log("BOOT-CAPTURE: FAILED to build log paths (wide conversion)");
        return;
    }
    // Fresh files per run: truncate stale ones (CreateFile2 +
    // CREATE_ALWAYS) so a repeat run's log is unambiguous (an old
    // BOOT-VEH line must never be mistaken for new). No DeleteFileW:
    // it is not proven in the app partition.
    {
        HANDLE t = CreateFile2(g_crash_log_path, GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (t != INVALID_HANDLE_VALUE)
            CloseHandle(t);
        t = CreateFile2(g_veh_log_path, GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
        if (t != INVALID_HANDLE_VALUE)
            CloseHandle(t);
    }
    static crash_listener* listener = new crash_listener();
    logs::listener::add(listener);
    // Resolve the VEH dynamically: a static import of
    // AddVectoredExceptionHandler that the console OS does not provide
    // would kill the exe at load (the 1.0.10 pattern). If it is
    // unavailable, the listener + SEH + terminate handler still stand.
    {
        using veh_fn = PVOID(WINAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        veh_fn add_veh = k32 ? reinterpret_cast<veh_fn>(GetProcAddress(k32, "AddVectoredExceptionHandler")) : nullptr;
        if (add_veh)
        {
            add_veh(1, veh_handler);
            log("BOOT-CAPTURE: VEH installed (dynamic resolve OK)");
        }
        else
        {
            log("BOOT-CAPTURE: VEH UNAVAILABLE (dynamic resolve failed; listener+SEH+terminate still active)");
        }
    }
    std::set_terminate(terminate_handler);
    std::signal(SIGABRT, abort_handler);
    _set_invalid_parameter_handler(invalid_param_handler);
    _set_purecall_handler(purecall_handler);
    SetUnhandledExceptionFilter(uhf_handler);
    std::atexit(atexit_handler);
    g_installed = true;
    log("BOOT-CAPTURE: installed (core fatal/error listener -> boot-crash-log.txt; VEH -> boot-veh-log.txt; terminate+abort+invparam+purecall+UHF+atexit handlers set; stale logs cleared)");
    log("BOOT-CAPTURE: NOTE WinRT failfast has NO handler (bypasses VEH/terminate/UHF); BOOT-TICK-PRE/POST + BOOT-DISPATCH-EXCEPTION are its evidence");
}

int seh_filter(unsigned long code)
{
    // C++ exceptions must propagate to the caller's catch: swallowing
    // them here would lose what(). Hardware faults are logged here
    // (raw write only) and then swallowed by the handler.
    if (code == 0xE06D7363)
        return EXCEPTION_CONTINUE_SEARCH;
    char buf[256];
    unsigned long pos = 0;
    append_cstr(buf, pos, "BOOT-SEH: main-thread SEH code=");
    append_u32_hex(buf, pos, code);
    append_cstr(buf, pos, "\r\n");
    raw_append(g_veh_log_path, buf, pos);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool call_boot_game_seh(const std::string& iso_path,
                        const std::string& title_id,
                        bool direct,
                        cfg_mode mode,
                        const std::string& config_path,
                        const std::optional<std::string>& db_config,
                        game_boot_result& result_out,
                        bool& running_out)
{
    // No C++ locals with destructors in this function: only the
    // primitive result holder. All string arguments live in the caller.
    game_boot_result result = game_boot_result::generic_error;
    __try
    {
        result = Emu.BootGame(iso_path, title_id, direct, mode, config_path, db_config);
    }
    __except (seh_filter(GetExceptionCode()))
    {
        return false;
    }
    result_out = result;
    running_out = Emu.IsRunning();
    return true;
}

} // namespace emu_host
