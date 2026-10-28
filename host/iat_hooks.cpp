// iat_hooks.cpp — 1.0.35 IAT hooks (see header). Design constraints:
// - Hook bodies never call VirtualAlloc/VirtualProtect (they ARE the hooks
//   for those); file writes use CreateFileW/WriteFile/CloseHandle only.
// - A thread_local reentrancy guard makes any nested hook call fall
//   through to the original function unlogged (no infinite recursion).
// - Stack frames are logged as RVAs vs the exe base (__ImageBase == module
//   handle of the exe), so they resolve against the shipped .map file.
// - RoFailFastWithErrorContext is variadic; the hook forwards the first
//   four register arguments (hr, count, and two slots). Since failfast
//   terminates the process, any imprecision there affects only the error
//   report's trailing arguments, not survival — and the killer.txt line
//   is already flushed before the original is called.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include "iat_hooks.h"

namespace
{
using NTSTATUS = LONG;
using RtlCaptureFn = USHORT(WINAPI*)(ULONG, ULONG, PVOID*, PULONG);

std::wstring g_killer_path;
std::wstring g_jit_path;
SRWLOCK g_killer_lock = SRWLOCK_INIT;
SRWLOCK g_jit_lock = SRWLOCK_INIT;
uintptr_t g_base = 0;
RtlCaptureFn g_capture = nullptr;
std::atomic<unsigned long long> g_jit_seq{0};
bool g_installed = false;
thread_local bool tl_in_hook = false;

// Original function pointers, filled by patch_all.
void* g_orig_nt_term = nullptr;
void* g_orig_term = nullptr;
void* g_orig_exit_proc = nullptr;
void* g_orig_exit = nullptr;
void* g_orig__exit = nullptr;
void* g_orig_failfast = nullptr;
void* g_orig_valloc = nullptr;
void* g_orig_vprot = nullptr;

void write_line(const std::wstring& path, SRWLOCK* lock, const char* line)
{
    if (path.empty())
        return;
    AcquireSRWLockExclusive(lock);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)strlen(line), &w, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(lock);
}

void write_fresh(const std::wstring& path, const char* header)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WriteFile(h, header, (DWORD)strlen(header), &w, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
    }
}

int capture_rvas(uintptr_t* out, int max)
{
    if (!g_capture || !g_base)
        return 0;
    void* frames[32];
    USHORT n = g_capture(1, 32, frames, nullptr);
    int count = 0;
    for (int i = 0; i < n && count < max; ++i)
        out[count++] = (uintptr_t)frames[i] - g_base;
    return count;
}

void killer_log(const char* fn, long code)
{
    char buf[2048];
    uintptr_t rvas[16];
    int n = capture_rvas(rvas, 16);
    int len = snprintf(buf, sizeof(buf),
        "BOOT-KILLER fn=%s tid=%lu code=%ld frames=%d rvas=",
        fn, GetCurrentThreadId(), code, n);
    for (int i = 0; i < n && len > 0 && len < (int)sizeof(buf) - 24; ++i)
        len += snprintf(buf + len, sizeof(buf) - len, "0x%llX ",
            (unsigned long long)rvas[i]);
    if (len > 0 && len < (int)sizeof(buf) - 3)
    {
        buf[len++] = '\r';
        buf[len++] = '\n';
        buf[len] = '\0';
    }
    write_line(g_killer_path, &g_killer_lock, buf);
}

// --- killer hooks ---
NTSTATUS NTAPI HookNtTerminateProcess(HANDLE h, NTSTATUS s)
{
    if (tl_in_hook || !g_orig_nt_term)
        return ((NTSTATUS(NTAPI*)(HANDLE, NTSTATUS))g_orig_nt_term)(h, s);
    tl_in_hook = true;
    killer_log("NtTerminateProcess", (long)s);
    tl_in_hook = false;
    return ((NTSTATUS(NTAPI*)(HANDLE, NTSTATUS))g_orig_nt_term)(h, s);
}

BOOL WINAPI HookTerminateProcess(HANDLE h, UINT c)
{
    if (tl_in_hook || !g_orig_term)
        return ((BOOL(WINAPI*)(HANDLE, UINT))g_orig_term)(h, c);
    tl_in_hook = true;
    killer_log("TerminateProcess", (long)c);
    tl_in_hook = false;
    return ((BOOL(WINAPI*)(HANDLE, UINT))g_orig_term)(h, c);
}

void WINAPI HookExitProcess(UINT c)
{
    if (!tl_in_hook && g_orig_exit_proc)
    {
        tl_in_hook = true;
        killer_log("ExitProcess", (long)c);
        tl_in_hook = false;
    }
    ((void(WINAPI*)(UINT))g_orig_exit_proc)(c);
}

void __cdecl HookExit(int c)
{
    if (!tl_in_hook && g_orig_exit)
    {
        tl_in_hook = true;
        killer_log("exit", (long)c);
        tl_in_hook = false;
    }
    ((void(__cdecl*)(int))g_orig_exit)(c);
}

void __cdecl Hook_exit(int c)
{
    if (!tl_in_hook && g_orig__exit)
    {
        tl_in_hook = true;
        killer_log("_exit", (long)c);
        tl_in_hook = false;
    }
    ((void(__cdecl*)(int))g_orig__exit)(c);
}

void __stdcall HookFailFast(HRESULT hr, UINT32 cfo, void* a, void* b)
{
    if (!tl_in_hook && g_orig_failfast)
    {
        tl_in_hook = true;
        killer_log("RoFailFastWithErrorContext", (long)hr);
        tl_in_hook = false;
    }
    ((void(__stdcall*)(HRESULT, UINT32, void*, void*))g_orig_failfast)(hr, cfo, a, b);
}

// --- exec-allocation hooks (exec protections only) ---
bool is_exec_prot(DWORD p)
{
    return (p & 0xF0u) != 0; // 0x10/0x20/0x40/0x80
}

LPVOID WINAPI HookVirtualAlloc(LPVOID a, SIZE_T s, DWORD t, DWORD p)
{
    using Fn = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
    if (tl_in_hook || !g_orig_valloc || !is_exec_prot(p))
        return ((Fn)g_orig_valloc)(a, s, t, p);
    unsigned long long id = ++g_jit_seq;
    char buf[512];
    snprintf(buf, sizeof(buf),
        "BOOT-JIT-REQ id=%llu fn=VirtualAlloc size=%llu type=0x%X prot=0x%X tid=%lu\r\n",
        id, (unsigned long long)s, t, p, GetCurrentThreadId());
    tl_in_hook = true;
    write_line(g_jit_path, &g_jit_lock, buf);
    tl_in_hook = false;
    LPVOID r = ((Fn)g_orig_valloc)(a, s, t, p);
    DWORD err = r ? 0 : GetLastError();
    snprintf(buf, sizeof(buf),
        "BOOT-JIT-RES id=%llu fn=VirtualAlloc result=%p err=%lu\r\n",
        id, r, err);
    tl_in_hook = true;
    write_line(g_jit_path, &g_jit_lock, buf);
    tl_in_hook = false;
    return r;
}

BOOL WINAPI HookVirtualProtect(LPVOID a, SIZE_T s, DWORD np, PDWORD op)
{
    using Fn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
    if (tl_in_hook || !g_orig_vprot || !is_exec_prot(np))
        return ((Fn)g_orig_vprot)(a, s, np, op);
    unsigned long long id = ++g_jit_seq;
    char buf[512];
    snprintf(buf, sizeof(buf),
        "BOOT-JIT-REQ id=%llu fn=VirtualProtect size=%llu newprot=0x%X tid=%lu\r\n",
        id, (unsigned long long)s, np, GetCurrentThreadId());
    tl_in_hook = true;
    write_line(g_jit_path, &g_jit_lock, buf);
    tl_in_hook = false;
    BOOL r = ((Fn)g_orig_vprot)(a, s, np, op);
    DWORD err = r ? 0 : GetLastError();
    DWORD oldp = (r && op) ? *op : 0;
    snprintf(buf, sizeof(buf),
        "BOOT-JIT-RES id=%llu fn=VirtualProtect result=%d oldprot=0x%X err=%lu\r\n",
        id, (int)r, oldp, err);
    tl_in_hook = true;
    write_line(g_jit_path, &g_jit_lock, buf);
    tl_in_hook = false;
    return r;
}

struct Target
{
    const char* name;
    void* hook;
    void** orig;
};

// Patch EVERY IAT slot matching name (an exe can import the same function
// through several contract DLLs); returns the number of slots patched.
int patch_all(const char* name, void* hook, void** orig_out)
{
    auto* dos = (PIMAGE_DOS_HEADER)g_base;
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    auto* nt = (PIMAGE_NT_HEADERS64)(g_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    int patched = 0;
    auto* desc = (PIMAGE_IMPORT_DESCRIPTOR)(g_base + dir.VirtualAddress);
    for (; desc->Name; ++desc)
    {
        auto* ft = (PIMAGE_THUNK_DATA64)(g_base + desc->FirstThunk);
        auto* it = desc->OriginalFirstThunk
            ? (PIMAGE_THUNK_DATA64)(g_base + desc->OriginalFirstThunk)
            : ft;
        for (; it->u1.AddressOfData; ++it, ++ft)
        {
            if (it->u1.Ordinal & IMAGE_ORDINAL_FLAG64)
                continue;
            auto* byname = (PIMAGE_IMPORT_BY_NAME)(g_base + it->u1.AddressOfData);
            if (strcmp(byname->Name, name) != 0)
                continue;
            if (patched == 0)
                *orig_out = (void*)ft->u1.Function;
            DWORD oldp = 0;
            if (!VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function),
                    PAGE_READWRITE, &oldp))
                continue;
            ft->u1.Function = (ULONGLONG)hook;
            DWORD tmp = 0;
            VirtualProtect(&ft->u1.Function, sizeof(ft->u1.Function), oldp, &tmp);
            FlushInstructionCache(GetCurrentProcess(), &ft->u1.Function,
                sizeof(ft->u1.Function));
            ++patched;
        }
    }
    return patched;
}

void summary_line(const char* name, int patched)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "BOOT-HOOK-INSTALL %s slots=%d%s\r\n",
        name, patched, patched ? "" : " (NOT IMPORTED - no coverage)");
    write_line(g_killer_path, &g_killer_lock, buf);
}

} // namespace

void xbox_hooks_install(const std::wstring& localStateDir)
{
    if (g_installed)
        return;
    g_installed = true;
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    g_killer_path = localStateDir + L"\\killer.txt";
    g_jit_path = localStateDir + L"\\jit-alloc.txt";
    write_fresh(g_killer_path, "BOOT-KILLER-LOG-START (1.0.35 IAT hooks)\r\n");
    write_fresh(g_jit_path, "BOOT-JIT-LOG-START (exec-only VirtualAlloc/VirtualProtect)\r\n");

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
        g_capture = (RtlCaptureFn)GetProcAddress(ntdll, "RtlCaptureStackBackTrace");

    summary_line("NtTerminateProcess",
        patch_all("NtTerminateProcess", (void*)HookNtTerminateProcess, &g_orig_nt_term));
    summary_line("TerminateProcess",
        patch_all("TerminateProcess", (void*)HookTerminateProcess, &g_orig_term));
    summary_line("ExitProcess",
        patch_all("ExitProcess", (void*)HookExitProcess, &g_orig_exit_proc));
    summary_line("exit",
        patch_all("exit", (void*)HookExit, &g_orig_exit));
    summary_line("_exit",
        patch_all("_exit", (void*)Hook_exit, &g_orig__exit));
    summary_line("RoFailFastWithErrorContext",
        patch_all("RoFailFastWithErrorContext", (void*)HookFailFast, &g_orig_failfast));
    summary_line("VirtualAlloc",
        patch_all("VirtualAlloc", (void*)HookVirtualAlloc, &g_orig_valloc));
    summary_line("VirtualProtect",
        patch_all("VirtualProtect", (void*)HookVirtualProtect, &g_orig_vprot));
}
