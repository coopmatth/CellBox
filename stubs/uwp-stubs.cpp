// UWP link stubs for the Xbox host: desktop-only Win32/CRT/SDL APIs that
// emucore and its dependency libs reference but AppContainer forbids.
// Every stub is a fail-closed no-op: it returns an error/empty result so the
// calling code takes its failure path, never a fabricated success. The Xbox
// host never calls these paths on purpose:
//   - SDL joystick/haptic: replaced by Windows.Gaming.Input (WGI) pad path.
//   - WinMM audio/MIDI: replaced by the XAudio2 backend already in the host.
//   - dbghelp backtraces / PDH cpu stats / MessageBox dialogs: diagnostics
//     only; the host logs instead of showing UI or walking stacks.
//   - SHGetKnownFolderPath / shell item APIs: LLVM/OpenAL config-dir
//     lookups; the host sets its own LocalState paths.
//   - CRT low-level fd IO (close/read/write/lseek) + strdup/tempnam: the
//     OneCore CRT import lib does not provide the __imp_ forms these
//     objects expect. NOTE: LLVM fd-based file IO therefore fails in this
//     host build; RPCS3's own cache IO uses its fs layer, not LLVM fds,
//     but any LLVM path that opens a real fd will hit these stubs and fail
//     instead of writing. That is a known functional gap, not a silent pass.
// GUID stubs are zero GUIDs: their only consumers are the stubbed-out
// shell/known-folder calls above, so the values are never compared.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <SDL3/SDL.h>

using HWND_ = void*;
using HANDLE_ = void*;
using BOOL_ = int;
using DWORD_ = unsigned long;
using UINT_ = unsigned int;
using LPCSTR_ = const char*;
using LPCWSTR_ = const wchar_t*;
using LPWSTR_ = wchar_t*;
using LPDWORD_ = unsigned long*;
using PVOID_ = void*;
using SIZE_T_ = unsigned __int64;
using HRESULT_ = long;

struct GUID_ { unsigned long Data1; unsigned short Data2; unsigned short Data3; unsigned char Data4[8]; };

extern "C" {

// ---- GUID data (consumers are stubbed to fail, values never compared) ----
// NOTE: plain `const` at namespace scope has INTERNAL linkage in C++ even
// inside extern "C", which is why attempt 2 still missed these: each needs
// an explicit `extern` to be visible to the linker.
extern const GUID_ CLSID_FileOperation = {};
extern const GUID_ CLSID_TaskbarList = {};
extern const GUID_ FOLDERID_LocalAppData = {};
extern const GUID_ FOLDERID_Profile = {};
extern const GUID_ FOLDERID_ProgramData = {};
extern const GUID_ FOLDERID_RoamingAppData = {};
extern const GUID_ GUID_NULL = {};
extern const GUID_ IID_IAgileObject = {};
extern const GUID_ IID_IMarshal = {};
extern const GUID_ IID_IDropTarget = {};
extern const GUID_ IID_IShellItem = {};
extern const GUID_ IID_ITaskbarList3 = {};
extern const GUID_ IID_IUnknown = {};
// in6addr_any (miniupnpc SSDP): 16-byte IPv6 any-address, same extern rule.
extern const unsigned char in6addr_any[16] = {};

// ---- Win32 dialog/window/shell (x64: undecorated __stdcall names) ----
int __stdcall MessageBoxA(HWND_, LPCSTR_, LPCSTR_, UINT_) { return 0; }
int __stdcall MessageBoxW(HWND_, LPCWSTR_, LPCWSTR_, UINT_) { return 0; }
void* __stdcall GetDesktopWindow() { return nullptr; }
void* __stdcall GetForegroundWindow() { return nullptr; }
int __stdcall FindFirstFileNameW(LPCWSTR_, DWORD_, LPDWORD_, LPWSTR_) { return 0; }
int __stdcall FindNextFileNameW(HANDLE_, LPDWORD_, LPWSTR_) { return 0; }
int __stdcall GetProcessGroupAffinity(HANDLE_, unsigned short*, unsigned short*) { return 0; }
void* __stdcall OpenSemaphoreA(DWORD_, BOOL_, LPCSTR_) { return nullptr; }
PVOID_ __stdcall RtlSetVolatileMemory(PVOID_ dst, SIZE_T_ size, int value) { return std::memset(dst, value, (std::size_t)size); }
HRESULT_ __stdcall SHGetKnownFolderPath(const GUID_*, DWORD_, HANDLE_, LPWSTR_*) { return (HRESULT_)0x80004005; }
void* __stdcall ILCreateFromPathW(LPCWSTR_) { return nullptr; }
void __stdcall ILFree(void*) {}
HRESULT_ __stdcall SHCreateItemFromIDList(const void*, const GUID_*, void**) { return (HRESULT_)0x80004005; }

// ---- psapi ----
int __stdcall K32EnumProcessModules(HANDLE_, void**, DWORD_, LPDWORD_) { return 0; }
int __stdcall K32EnumProcessModulesEx(HANDLE_, void**, DWORD_, LPDWORD_, DWORD_) { return 0; }

// ---- pdh (performance counters) ----
unsigned long __stdcall PdhOpenQueryW(LPCWSTR_, unsigned __int64, void**) { return 0xC0000BC6UL; }
unsigned long __stdcall PdhAddEnglishCounterW(void*, LPCWSTR_, unsigned __int64, void**) { return 0xC0000BC6UL; }
unsigned long __stdcall PdhCollectQueryData(void*) { return 0xC0000BC6UL; }
unsigned long __stdcall PdhCloseQuery(void*) { return 0; }
unsigned long __stdcall PdhGetFormattedCounterArrayW(void*, DWORD_, LPDWORD_, LPDWORD_, void*) { return 0xC0000BC6UL; }

// ---- dbghelp (stack walks / symbolization) ----
int __stdcall StackWalk64(DWORD_, HANDLE_, HANDLE_, void*, void*, void*, void*, void*, void*) { return 0; }
int __stdcall SymInitializeW(HANDLE_, LPCWSTR_, BOOL_) { return 0; }
int __stdcall SymInitialize(HANDLE_, LPCSTR_, BOOL_) { return 0; }
DWORD_ __stdcall SymSetOptions(DWORD_) { return 0; }
int __stdcall SymCleanup(HANDLE_) { return 0; }
void* __stdcall SymFunctionTableAccess64(HANDLE_, unsigned __int64) { return nullptr; }
unsigned __int64 __stdcall SymGetModuleBase64(HANDLE_, unsigned __int64) { return 0; }
int __stdcall SymGetLineFromAddrW64(HANDLE_, unsigned __int64, LPDWORD_, void*) { return 0; }
int __stdcall SymFromAddrW(HANDLE_, unsigned __int64, unsigned __int64*, void*) { return 0; }
int __stdcall SymFromAddr(HANDLE_, unsigned __int64, unsigned __int64*, void*) { return 0; }

// ---- WinMM audio / MIDI (host uses XAudio2; these paths are dead) ----
unsigned int __stdcall waveOutGetNumDevs() { return 0; }
unsigned int __stdcall waveOutGetDevCapsW(unsigned int, void*, unsigned int) { return 1; }
unsigned int __stdcall waveOutOpen(void**, unsigned int, void*, unsigned __int64, unsigned __int64, DWORD_) { return 1; }
unsigned int __stdcall waveOutClose(void*) { return 0; }
unsigned int __stdcall waveOutPrepareHeader(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall waveOutUnprepareHeader(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall waveOutWrite(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall waveInGetNumDevs() { return 0; }
unsigned int __stdcall waveInGetDevCapsW(unsigned int, void*, unsigned int) { return 1; }
unsigned int __stdcall waveInOpen(void**, unsigned int, void*, unsigned __int64, unsigned __int64, DWORD_) { return 1; }
unsigned int __stdcall waveInClose(void*) { return 0; }
unsigned int __stdcall waveInPrepareHeader(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall waveInUnprepareHeader(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall waveInAddBuffer(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall waveInStart(void*) { return 1; }
unsigned int __stdcall waveInStop(void*) { return 1; }
unsigned int __stdcall waveInReset(void*) { return 1; }
unsigned int __stdcall midiOutGetNumDevs() { return 0; }
unsigned int __stdcall midiOutGetDevCapsA(unsigned int, void*, unsigned int) { return 1; }
unsigned int __stdcall midiOutOpen(void**, unsigned int, unsigned __int64, unsigned __int64, DWORD_) { return 1; }
unsigned int __stdcall midiOutClose(void*) { return 0; }
unsigned int __stdcall midiOutPrepareHeader(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall midiOutUnprepareHeader(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall midiOutShortMsg(void*, DWORD_) { return 1; }
unsigned int __stdcall midiOutLongMsg(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall midiInGetNumDevs() { return 0; }
unsigned int __stdcall midiInGetDevCapsA(unsigned int, void*, unsigned int) { return 1; }
unsigned int __stdcall midiInOpen(void**, unsigned int, unsigned __int64, unsigned __int64, DWORD_) { return 1; }
unsigned int __stdcall midiInClose(void*) { return 0; }
unsigned int __stdcall midiInPrepareHeader(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall midiInUnprepareHeader(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall midiInAddBuffer(void*, void*, unsigned int) { return 1; }
unsigned int __stdcall midiInStart(void*) { return 1; }
unsigned int __stdcall midiInStop(void*) { return 1; }
unsigned int __stdcall midiInReset(void*) { return 1; }

// ---- avrt (OpenAL WASAPI thread characteristics; host uses XAudio2) ----
void* __stdcall AvSetMmThreadCharacteristicsW(LPCWSTR_, unsigned long*) { return nullptr; }
int __stdcall AvRevertMmThreadCharacteristics(void*) { return 0; }
void* __imp_AvSetMmThreadCharacteristicsW = (void*)AvSetMmThreadCharacteristicsW;
void* __imp_AvRevertMmThreadCharacteristics = (void*)AvRevertMmThreadCharacteristics;

// ---- iphlpapi (miniupnpc SSDP interface lookup) ----
unsigned long __stdcall GetBestInterface(unsigned long, unsigned long*) { return 1; }
void* __imp_GetBestInterface = (void*)GetBestInterface;

// ---- wldap32 (curl LDAP; RPCS3 never uses LDAP URLs) ----
void* __stdcall ldap_initW(LPCWSTR_, unsigned long) { return nullptr; }
void* __stdcall ldap_sslinitW(void*, LPCWSTR_, unsigned long) { return nullptr; }
unsigned long __stdcall ldap_unbind_s(void*) { return 1; }
unsigned long __stdcall ldap_set_optionW(void*, int, const void*) { return 1; }
unsigned long __stdcall ldap_simple_bind_sW(void*, LPCWSTR_, LPCWSTR_) { return 1; }
unsigned long __stdcall ldap_search_sW(void*, LPCWSTR_, unsigned long, LPCWSTR_, LPCWSTR_, int, void**, void**, void*, unsigned long) { return 1; }
unsigned long __stdcall ldap_msgfree(void*) { return 1; }
char* __stdcall ldap_err2stringA(unsigned long) { return nullptr; }
void* __stdcall ldap_first_entry(void*, void*) { return nullptr; }
void* __stdcall ldap_next_entry(void*, void*) { return nullptr; }
wchar_t* __stdcall ldap_first_attributeW(void*, void*, unsigned long*) { return nullptr; }
wchar_t* __stdcall ldap_next_attributeW(void*, void*, void*) { return nullptr; }
void** __stdcall ldap_get_values_lenW(void*, void*, LPCWSTR_) { return nullptr; }
unsigned long __stdcall ldap_value_free_len(void**) { return 1; }
wchar_t* __stdcall ldap_get_dnW(void*, void*) { return nullptr; }
void __stdcall ldap_memfreeW(void*) {}
void __stdcall ber_free(void*, int) {}
void* __imp_ldap_initW = (void*)ldap_initW;
void* __imp_ldap_sslinitW = (void*)ldap_sslinitW;
void* __imp_ldap_unbind_s = (void*)ldap_unbind_s;
void* __imp_ldap_set_optionW = (void*)ldap_set_optionW;
void* __imp_ldap_simple_bind_sW = (void*)ldap_simple_bind_sW;
void* __imp_ldap_search_sW = (void*)ldap_search_sW;
void* __imp_ldap_msgfree = (void*)ldap_msgfree;
void* __imp_ldap_err2stringA = (void*)ldap_err2stringA;
void* __imp_ldap_first_entry = (void*)ldap_first_entry;
void* __imp_ldap_next_entry = (void*)ldap_next_entry;
void* __imp_ldap_first_attributeW = (void*)ldap_first_attributeW;
void* __imp_ldap_next_attributeW = (void*)ldap_next_attributeW;
void* __imp_ldap_get_values_lenW = (void*)ldap_get_values_lenW;
void* __imp_ldap_value_free_len = (void*)ldap_value_free_len;
void* __imp_ldap_get_dnW = (void*)ldap_get_dnW;
void* __imp_ldap_memfreeW = (void*)ldap_memfreeW;
void* __imp_ber_free = (void*)ber_free;

// ---- CRT low-level IO / posix helpers (OneCore import lib lacks __imp_ forms) ----
char* strdup(const char* s) {
    if (!s) return nullptr;
    std::size_t n = std::strlen(s) + 1;
    char* d = (char*)std::malloc(n);
    if (d) std::memcpy(d, s, n);
    return d;
}
int close(int) { return -1; }
int read(int, void*, unsigned int) { return -1; }
int write(int, const void*, unsigned int) { return -1; }
long lseek(int, long, int) { return -1; }
std::FILE* fdopen(int, const char*) { return nullptr; }
int isatty(int) { return 0; }
char* tempnam(const char*, const char*) { return nullptr; }

// ---- __imp_ pointers for dllimport references to the functions above ----
void* __imp_MessageBoxA = (void*)MessageBoxA;
void* __imp_MessageBoxW = (void*)MessageBoxW;
void* __imp_GetDesktopWindow = (void*)GetDesktopWindow;
void* __imp_GetForegroundWindow = (void*)GetForegroundWindow;
void* __imp_FindFirstFileNameW = (void*)FindFirstFileNameW;
void* __imp_FindNextFileNameW = (void*)FindNextFileNameW;
void* __imp_GetProcessGroupAffinity = (void*)GetProcessGroupAffinity;
void* __imp_OpenSemaphoreA = (void*)OpenSemaphoreA;
void* __imp_StackWalk64 = (void*)StackWalk64;
void* __imp_SymInitializeW = (void*)SymInitializeW;
void* __imp_SymInitialize = (void*)SymInitialize;
void* __imp_SymSetOptions = (void*)SymSetOptions;
void* __imp_SymCleanup = (void*)SymCleanup;
void* __imp_SymFunctionTableAccess64 = (void*)SymFunctionTableAccess64;
void* __imp_SymGetModuleBase64 = (void*)SymGetModuleBase64;
void* __imp_SymGetLineFromAddrW64 = (void*)SymGetLineFromAddrW64;
void* __imp_SymFromAddrW = (void*)SymFromAddrW;
void* __imp_SymFromAddr = (void*)SymFromAddr;
void* __imp_waveOutGetNumDevs = (void*)waveOutGetNumDevs;
void* __imp_waveOutGetDevCapsW = (void*)waveOutGetDevCapsW;
void* __imp_waveOutOpen = (void*)waveOutOpen;
void* __imp_waveOutClose = (void*)waveOutClose;
void* __imp_waveOutPrepareHeader = (void*)waveOutPrepareHeader;
void* __imp_waveOutUnprepareHeader = (void*)waveOutUnprepareHeader;
void* __imp_waveOutWrite = (void*)waveOutWrite;
void* __imp_waveInGetNumDevs = (void*)waveInGetNumDevs;
void* __imp_waveInGetDevCapsW = (void*)waveInGetDevCapsW;
void* __imp_waveInOpen = (void*)waveInOpen;
void* __imp_waveInClose = (void*)waveInClose;
void* __imp_waveInPrepareHeader = (void*)waveInPrepareHeader;
void* __imp_waveInUnprepareHeader = (void*)waveInUnprepareHeader;
void* __imp_waveInAddBuffer = (void*)waveInAddBuffer;
void* __imp_waveInStart = (void*)waveInStart;
void* __imp_waveInStop = (void*)waveInStop;
void* __imp_waveInReset = (void*)waveInReset;
void* __imp_midiOutGetNumDevs = (void*)midiOutGetNumDevs;
void* __imp_midiOutGetDevCapsA = (void*)midiOutGetDevCapsA;
void* __imp_midiOutOpen = (void*)midiOutOpen;
void* __imp_midiOutClose = (void*)midiOutClose;
void* __imp_midiOutPrepareHeader = (void*)midiOutPrepareHeader;
void* __imp_midiOutUnprepareHeader = (void*)midiOutUnprepareHeader;
void* __imp_midiOutShortMsg = (void*)midiOutShortMsg;
void* __imp_midiOutLongMsg = (void*)midiOutLongMsg;
void* __imp_midiInGetNumDevs = (void*)midiInGetNumDevs;
void* __imp_midiInGetDevCapsA = (void*)midiInGetDevCapsA;
void* __imp_midiInOpen = (void*)midiInOpen;
void* __imp_midiInClose = (void*)midiInClose;
void* __imp_midiInPrepareHeader = (void*)midiInPrepareHeader;
void* __imp_midiInUnprepareHeader = (void*)midiInUnprepareHeader;
void* __imp_midiInAddBuffer = (void*)midiInAddBuffer;
void* __imp_midiInStart = (void*)midiInStart;
void* __imp_midiInStop = (void*)midiInStop;
void* __imp_midiInReset = (void*)midiInReset;
void* __imp_strdup = (void*)strdup;
void* __imp_close = (void*)close;
void* __imp_read = (void*)read;
void* __imp_write = (void*)write;
void* __imp_lseek = (void*)lseek;
void* __imp_SHGetKnownFolderPath = (void*)SHGetKnownFolderPath;
void* __imp_ILCreateFromPathW = (void*)ILCreateFromPathW;
void* __imp_ILFree = (void*)ILFree;
void* __imp_SHCreateItemFromIDList = (void*)SHCreateItemFromIDList;

// ---- SDL joystick/haptic (replaced by WGI; report zero devices) ----
// Still inside extern "C": SDL declares these with C linkage.

void SDL_free(void* p) { std::free(p); }
const char* SDL_GetError() { return ""; }
SDL_JoystickID* SDL_GetJoysticks(int* count) {
    if (count) *count = 0;
    return nullptr;
}
SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID) { return nullptr; }
Uint16 SDL_GetJoystickVendor(SDL_Joystick*) { return 0; }
Uint16 SDL_GetJoystickProduct(SDL_Joystick*) { return 0; }
int SDL_GetNumJoystickAxes(SDL_Joystick*) { return 0; }
int SDL_GetNumJoystickHats(SDL_Joystick*) { return 0; }
int SDL_GetNumJoystickButtons(SDL_Joystick*) { return 0; }
Sint16 SDL_GetJoystickAxis(SDL_Joystick*, int) { return 0; }
Uint8 SDL_GetJoystickHat(SDL_Joystick*, int) { return 0; }
bool SDL_GetJoystickButton(SDL_Joystick*, int) { return false; }
bool SDL_SetJoystickLED(SDL_Joystick*, Uint8, Uint8, Uint8) { return false; }
void SDL_CloseJoystick(SDL_Joystick*) {}
SDL_Haptic* SDL_OpenHapticFromJoystick(SDL_Joystick*) { return nullptr; }
void SDL_CloseHaptic(SDL_Haptic*) {}
int SDL_CreateHapticEffect(SDL_Haptic*, const SDL_HapticEffect*) { return -1; }
bool SDL_UpdateHapticEffect(SDL_Haptic*, SDL_HapticEffectID, const SDL_HapticEffect*) { return false; }
bool SDL_RunHapticEffect(SDL_Haptic*, SDL_HapticEffectID, Uint32) { return false; }
bool SDL_StopHapticEffect(SDL_Haptic*, SDL_HapticEffectID) { return false; }
void SDL_DestroyHapticEffect(SDL_Haptic*, int) {}
Uint64 SDL_GetTicks() { return 0; }

} // extern "C"
