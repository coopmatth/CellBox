// stage_diag.cpp — STAGE instrumentation + full IAT sweep (diagnostic, 1.0.19.0).
// Stage file: LocalState\stage-diag.txt (SEPARATE from host-results.txt, so a
// crash before Run() cannot overwrite it and Run()'s CREATE_ALWAYS log cannot
// erase stage 1/2 evidence). Path is derived without WinRT: module folder name
// (PackageFullName) -> family name, plus %LOCALAPPDATA%\Packages\<family>\LocalState.
// Stage 1 fires from #pragma init_seg(compiler): before user-segment CRT
// constructors (LLVM/protobuf/curl/wolfssl globals) run.
// Stage 2 fires at the first line of main(), before CoreApplication::Run.
// Stage 3 is the existing Run() header (logged by the host itself).
// Stage 4 fires immediately before emu_host::init_phase().
// Proof matrix: stage-diag.txt absent after a clean launch = loader/IAT/TLS
// failure (or stage-path failure — see below); stage 1 only = constructor
// crash; stages 1-2 = activation crash; stages 1-3 (header) = Run() reached;
// stage 4 = Emu.Init() entered. If stage 1 is absent AND host-results.txt is
// also absent/old, first check the stage path derivation (desktop pre-test
// proves it on Win32; on console the family parse is the only new step).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include "stage_diag.h"

static bool stage_path(WCHAR* out, DWORD outChars) {
    WCHAR mod[MAX_PATH];
    if (!GetModuleFileNameW(NULL, mod, MAX_PATH)) return false;
    // folder name = last path component's parent
    WCHAR* lastSlash = NULL;
    for (WCHAR* p = mod; *p; ++p) if (*p == L'\\' || *p == L'/') lastSlash = p;
    if (!lastSlash) return false;
    *lastSlash = 0;
    WCHAR* folder = NULL;
    for (WCHAR* p = mod; *p; ++p) if (*p == L'\\' || *p == L'/') folder = p;
    if (!folder) return false;
    folder++;
    // family = first token before '_' + '_' + token after last "__"
    WCHAR name[128]; WCHAR hash[64];
    WCHAR* firstUs = NULL;
    for (WCHAR* p = folder; *p; ++p) if (*p == L'_') { firstUs = p; break; }
    if (!firstUs) return false;
    size_t nlen = (size_t)(firstUs - folder);
    if (nlen >= 128) return false;
    for (size_t i = 0; i < nlen; ++i) name[i] = folder[i];
    name[nlen] = 0;
    WCHAR* dbl = NULL;
    for (WCHAR* p = folder; *p; ++p) if (p[0] == L'_' && p[1] == L'_') dbl = p;
    if (!dbl) return false;
    dbl += 2;
    size_t hlen = 0;
    while (dbl[hlen] && hlen < 63) { hash[hlen] = dbl[hlen]; ++hlen; }
    hash[hlen] = 0;
    WCHAR base[MAX_PATH];
    DWORD bl = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (bl == 0 || bl >= MAX_PATH) {
        // fallback: %USERPROFILE%\AppData\Local
        WCHAR up[MAX_PATH];
        if (!GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH)) return false;
        if (swprintf_s(base, MAX_PATH, L"%s\\AppData\\Local", up) < 0) return false;
    }
    if (swprintf_s(out, outChars, L"%s\\Packages\\%s_%s\\LocalState\\stage-diag.txt",
                   base, name, hash) < 0) return false;
    return true;
}

void stage_write(int stage, const char* tag) {
    WCHAR path[MAX_PATH];
    if (!stage_path(path, MAX_PATH)) return;
    HANDLE h = CreateFile2(path, GENERIC_WRITE, FILE_SHARE_READ, OPEN_ALWAYS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER zero = {};
    SetFilePointerEx(h, zero, NULL, FILE_END);
    char line[256];
    int len = 0;
    // manual format: STAGE:<n>:<tag>\r\n (no CRT stdio in this path)
    const char* pre = "STAGE:";
    while (pre[len]) { line[len] = pre[len]; ++len; }
    line[len++] = (char)('0' + stage);
    line[len++] = ':';
    for (const char* p = tag; *p && len < 250; ++p) line[len++] = *p;
    line[len++] = '\r'; line[len++] = '\n';
    DWORD w = 0;
    WriteFile(h, line, (DWORD)len, &w, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

#pragma init_seg(compiler)
struct Stage1Marker {
    Stage1Marker() { stage_write(1, "LOADER_OK_PRE_CTORS"); }
};
static Stage1Marker g_stage1_marker;

struct IatEntry { const wchar_t* dll; const char* sym; };
static const IatEntry kIat[] = {
#include "iat_table.inc"
};

static bool is_api_ms(const wchar_t* dll) {
    // case-insensitive "api-ms-" prefix
    if (!dll) return false;
    wchar_t a = dll[0], b = dll[1], c = dll[2], d = dll[3];
    if (a >= L'A' && a <= L'Z') a += 32;
    if (b >= L'A' && b <= L'Z') b += 32;
    if (c >= L'A' && c <= L'Z') c += 32;
    return (a == L'a' && b == L'p' && c == L'i' && d == L'-');
}

void iat_sweep(void (*logfn)(const char*)) {
    int total = 0, pass = 0, fail = 0, fwd = 0;
    char buf[512];
    const int n = (int)(sizeof(kIat) / sizeof(kIat[0]));
    for (int i = 0; i < n; ++i) {
        total++;
        HMODULE h = GetModuleHandleW(kIat[i].dll);
        if (!h) h = LoadLibraryExW(kIat[i].dll, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!h) {
            if (is_api_ms(kIat[i].dll)) {
                // Forwarder-resolved: the api-ms DLL is not itself a loaded
                // module; its symbols resolve through the umbrella. Count
                // separately, NOT as a failure (reaching Run() already
                // proves the loader resolved the IAT).
                fwd++;
            } else {
                fail++;
                if (logfn) {
                    int o = 0;
                    const char* p = "IAT-NOLOAD ";
                    while (*p) buf[o++] = *p++;
                    for (const wchar_t* q = kIat[i].dll; *q && o < 400; ++q) buf[o++] = (char)*q;
                    buf[o++] = ' ';
                    for (const char* q = kIat[i].sym; *q && o < 500; ++q) buf[o++] = *q;
                    buf[o] = 0;
                    logfn(buf);
                }
            }
            continue;
        }
        FARPROC proc = GetProcAddress(h, kIat[i].sym);
        if (!proc) {
            fail++;
            if (logfn) {
                int o = 0;
                const char* p = "IAT-NULL ";
                while (*p) buf[o++] = *p++;
                for (const wchar_t* q = kIat[i].dll; *q && o < 400; ++q) buf[o++] = (char)*q;
                buf[o++] = ' ';
                for (const char* q = kIat[i].sym; *q && o < 500; ++q) buf[o++] = *q;
                buf[o] = 0;
                logfn(buf);
            }
        } else {
            pass++;
        }
    }
    if (logfn) {
        char sum[256];
        // manual int formatting, no snprintf dependency issue (snprintf is fine here)
        _snprintf_s(sum, sizeof(sum), _TRUNCATE,
                    "IAT-SWEEP total=%d pass=%d fail=%d fwd-unresolved=%d (fwd = api-ms forwarder not a loaded module, not a failure; reaching Run already proves IAT load)",
                    total, pass, fail, fwd);
        logfn(sum);
    }
}
