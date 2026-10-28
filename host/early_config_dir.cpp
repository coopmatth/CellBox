// early_config_dir.cpp — v1.0.23.0 host-side cache/config redirect attempt.
// Host-only, no core edit, no /ZW, kernel32 only.
//
// THE PROBLEM (measured 1.0.22): fs::get_cache_dir() on Windows IS
// fs::get_config_dir() (Utilities/File.cpp:2714-2745), and both are
// function-local magic statics computed ON FIRST CALL. The host's
// Run()-time SetEnvironmentVariable(RPCS3_CONFIG_DIR) arrives too late:
// by then the static already holds the exe dir (1.0.10/1.0.22 logs
// prove it: EMU-INIT-CONFIG-DIR resolved = exe dir). The first call
// happens before Run — from an emucore global constructor or the
// WinRT activation path; either way it precedes any host code that
// runs in Run(). The ONLY host code guaranteed to run before every
// normal (CRT$XCU) global constructor is a #pragma init_seg(compiler)
// constructor, which runs in the .CRT$XCC segment. This TU is that
// constructor. It sets RPCS3_CONFIG_DIR before any emucore static can
// read it — IF the derivation succeeds. The console run discriminates:
// BOOT-CACHE-DIR == LocalState means it won; == exe dir means it lost
// (derivation failed or an even-earlier caller exists), and the
// BOOT-EARLY-CONFIG-DIR line says which.
//
// DERIVATION (console): the packaged exe lives under
//   S:\Program Files\WindowsApps\<PFN>\XboxUwpHost.exe
// and %LOCALAPPDATA% on console is
//   Q:\Users\UserMgr0\AppData\Local\Packages\<family-lowercase>\AC
// (1.0.20 XCHECK, measured). LocalState is the same folder with \AC
// replaced by \LocalState (Windows paths are case-insensitive, so the
// lowercase family name in the env value is fine). No WinRT call is
// used here — WinRT is not initialised at compiler-segment time.
// DESKTOP TEST PATH: if RPCS3_XBOX_FORCE_LOCALSTATE is set in the
// process environment at startup, it is used verbatim (the desktop
// early-test proves the ordering this way; on console that variable
// is never set, so the LOCALAPPDATA derivation is the live path).
// If neither applies (plain desktop run), nothing is set and the
// default exe-dir behaviour is unchanged.
//
// SAFETY: stack buffers only, no heap, no CRT string code, no WinRT,
// no file I/O. The result is recorded in globals that boot_phase logs.

#include <windows.h>

#include "early_config_dir.h"

namespace emu_host
{

// Recorded for boot_phase to log (read-only after CRT init).
wchar_t g_early_config_dir_value[4096] = {};
long g_early_config_dir_set = 0;    // 1 = RPCS3_CONFIG_DIR was set here
long g_early_config_dir_reason = 0; // 0=not packaged, no force env
                                    // 1=force env used
                                    // 2=LOCALAPPDATA \AC -> \LocalState
                                    // 3=LOCALAPPDATA missing
                                    // 4=LOCALAPPDATA does not end in \AC
                                    // 5=module path unreadable

namespace
{

bool wide_ends_with(const wchar_t* s, const wchar_t* suffix)
{
    size_t sl = 0, fl = 0;
    while (s[sl])
        ++sl;
    while (suffix[fl])
        ++fl;
    if (fl > sl)
        return false;
    for (size_t i = 0; i < fl; ++i)
    {
        wchar_t a = s[sl - fl + i], b = suffix[i];
        if (a >= L'A' && a <= L'Z')
            a = static_cast<wchar_t>(a + 32);
        if (b >= L'A' && b <= L'Z')
            b = static_cast<wchar_t>(b + 32);
        if (a != b)
            return false;
    }
    return true;
}

bool wide_contains(const wchar_t* s, const wchar_t* needle)
{
    size_t nl = 0;
    while (needle[nl])
        ++nl;
    if (nl == 0)
        return true;
    for (size_t i = 0; s[i]; ++i)
    {
        size_t j = 0;
        while (j < nl && s[i + j])
        {
            wchar_t a = s[i + j], b = needle[j];
            if (a >= L'A' && a <= L'Z')
                a = static_cast<wchar_t>(a + 32);
            if (b >= L'A' && b <= L'Z')
                b = static_cast<wchar_t>(b + 32);
            if (a != b)
                break;
            ++j;
        }
        if (j == nl)
            return true;
    }
    return false;
}

void wide_copy(wchar_t* dst, size_t dst_chars, const wchar_t* src)
{
    size_t i = 0;
    while (src[i] && i + 1 < dst_chars)
    {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

// Pure derivation, unit-testable on desktop: LOCALAPPDATA value ->
// LocalState value, or false if it does not end in \AC.
bool derive_localstate(const wchar_t* localappdata, wchar_t* out, size_t out_chars)
{
    if (!wide_ends_with(localappdata, L"\\AC"))
        return false;
    size_t len = 0;
    while (localappdata[len])
        ++len;
    // len >= 3 (the \AC suffix); strip 2 chars (AC), keep the backslash.
    size_t keep = len - 2;
    if (keep + 11 >= out_chars) // "\LocalState\" + NUL
        return false;
    for (size_t i = 0; i < keep; ++i)
        out[i] = localappdata[i];
    const wchar_t* tail = L"LocalState\\";
    size_t j = 0;
    while (tail[j])
        out[keep + j] = tail[j], ++j;
    out[keep + j] = 0;
    return true;
}

struct early_config_dir_init
{
    early_config_dir_init()
    {
        wchar_t forced[4096] = {};
        if (GetEnvironmentVariableW(L"RPCS3_XBOX_FORCE_LOCALSTATE", forced, 4096) > 0)
        {
            // get_config_dir() UNCONDITIONALLY strips the last path
            // component (dir.resize(dir.rfind('/') + 1), File.cpp) —
            // it treats the value as a file path, not a directory. A
            // trailing backslash makes rfind land on the final slash
            // so the full directory survives. Without it, a forced
            // ".../early-test-1023" degrades to ".../out/" (measured).
            size_t fl = 0;
            while (forced[fl])
                ++fl;
            if (fl > 0 && forced[fl - 1] != L'\\' && forced[fl - 1] != L'/' && fl + 1 < 4096)
            {
                forced[fl] = L'\\';
                forced[fl + 1] = 0;
            }
            SetEnvironmentVariableW(L"RPCS3_CONFIG_DIR", forced);
            wide_copy(g_early_config_dir_value, 4096, forced);
            g_early_config_dir_set = 1;
            g_early_config_dir_reason = 1;
            return;
        }
        wchar_t module[4096] = {};
        if (GetModuleFileNameW(nullptr, module, 4096) == 0)
        {
            g_early_config_dir_reason = 5;
            return;
        }
        if (!wide_contains(module, L"WindowsApps"))
        {
            g_early_config_dir_reason = 0;
            return;
        }
        wchar_t appdata[4096] = {};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, 4096) == 0)
        {
            g_early_config_dir_reason = 3;
            return;
        }
        wchar_t derived[4096] = {};
        if (!derive_localstate(appdata, derived, 4096))
        {
            g_early_config_dir_reason = 4;
            return;
        }
        SetEnvironmentVariableW(L"RPCS3_CONFIG_DIR", derived);
        wide_copy(g_early_config_dir_value, 4096, derived);
        g_early_config_dir_set = 1;
        g_early_config_dir_reason = 2;
    }
};

#pragma init_seg(compiler)
early_config_dir_init g_early_config_dir_init_instance;

} // namespace

bool early_derive_localstate(const wchar_t* localappdata, wchar_t* out, size_t out_chars)
{
    return derive_localstate(localappdata, out, out_chars);
}

} // namespace emu_host
