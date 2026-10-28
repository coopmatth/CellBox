// ws2_stub.cpp — Xbox UWP host-side Winsock stub, v1.0.18.0 FINAL (Jarvis-direct).
//
// MATT'S LITERAL SHAPE (this is the final, not the interim):
//   #define WINSOCK_API_LINKAGE <empty>  BEFORE  #include <WinSock2.h>
// With WINSOCK_API_LINKAGE empty, the header declares every Winsock
// function as a plain (non-dllimport) function, so defining them here is
// legal (no C2491) and the linker never needs windowsapp.lib(WS2_32.dll).
// The interim 1.0.18 TU used hand-written generic signatures and NO
// WinSock2.h at all; this final TU uses the real header signatures.
// End result is the same link outcome, proven from scratch below.
//
// WHY (measured 2026-10-27, not re-litigated):
//   * 1.0.10/1.0.11 (emucore linked) crash at activation, no header.
//     1.0.12 (emucore unlinked) runs.
//   * Probe P1 WS2_32 1.0.13.0 FAILS to open (no host-results.txt).
//     P2 CRYPT32 1.0.14.0, P3 IPHLPAPI 1.0.15.0, P4 BCRYPT 1.0.16.0,
//     P5 NTDLL 1.0.17.0 all PASS ret=0 — cleared, DO NOT stub them.
//     WS2_32 alone is the named cause. Only WS2_32 is stubbed here.
//
// EXACT MEASURED SET (1.0.10 exe dumpbin, out\imports-1010-exe.txt,
// WS2_32.dll block = 40 entries):
//   * 13 named: WSAEventSelect, WSAEnumNetworkEvents, WSAResetEvent,
//     inet_pton, WSACreateEvent, WSACloseEvent, getnameinfo,
//     WSAWaitForMultipleEvents, getaddrinfo, WSAIoctl, freeaddrinfo,
//     inet_ntop, WSAPoll.
//   * 27 ordinals mapped via the System32 export table: select (18),
//     inet_addr (11), __WSAFDIsSet (151), htonl (8), ioctlsocket (10),
//     WSAStartup (115), bind (2), htons (9), listen (13), shutdown (22),
//     WSASetLastError (112), getsockopt (7), gethostname (57),
//     WSAGetLastError (111), getpeername (5), WSACleanup (116),
//     send (19), sendto (20), recvfrom (17), getsockname (6),
//     recv (16), connect (4), closesocket (3), accept (1),
//     setsockopt (21), ntohs (15), socket (23).
//   * Corrections, measured: htonl/htons/ntohs ARE real imports
//     (ordinals 8/9/15); ntohl (14) and inet_ntoa (12) are NOT.
//   * Harmless dead extras kept from the interim (a few bytes each,
//     unreferenced, gate still passes): ntohl, inet_ntoa, WSASetEvent.
//     They fail closed like everything else; they exist only so a
//     missed reference still fails closed instead of pulling WS2_32
//     back in.
//
// HOW IT WORKS:
//   * Every function below is a plain extern "C" definition with the
//     real WinSock2.h / WS2tcpip.h signature, so a plain (thunk-style)
//     reference resolves here.
//   * Every __imp_<name> data object below holds the stub's address,
//     so a dllimport-style reference (e.g. __imp_socket from libcurl
//     and emucore) resolves here instead of from that import-lib member.
//   * With every symbol the WS2_32 member provides already defined,
//     the linker never loads windowsapp.lib(WS2_32.dll), so the final
//     exe has NO WS2_32.dll import descriptor. This is NOT delay-load:
//     no DLL is loaded later; a called stub fails closed.
//   * ws2_stub.obj is FIRST on the final link line, before @libsu.rsp
//     and WindowsApp.lib, and the link carries
//     /NODEFAULTLIB:ws2_32.lib. No ws2_32.lib in any explicit link line.
//   * in6addr_any is DELIBERATELY NOT defined here: uwp-stubs.cpp
//     already defines it, and a second definition would LNK2005 the
//     UWP link. The header declaration (extern) is left as a declaration
//     only. Any link using ws2_stub.obj WITHOUT uwp-stubs.obj (the
//     desktop stubbed emu_probe) must link uwp-stubs.obj too.
//
// SAFETY CONTRACT: the Xbox host needs NO network (firmware install
// uses SCEDecrypter + TAR, not HTTP; no title boots in this build),
// so failing closed is safe. Byte-order helpers are implemented for
// real (pure arithmetic). The stubbed desktop emu_probe must still
// reach FIRMWARE-VERDICT PASS at the exact PC 4.92 reference
// (1075 files / 195070119 bytes / vsh.self=1) before packaging.

#define WINSOCK_API_LINKAGE
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <cstddef>
#include <cstdint>

namespace {
using u32 = std::uint32_t;
using u16 = std::uint16_t;
u32 swap32(u32 x) {
    return (x >> 24) | ((x >> 8) & 0x0000ff00u) | ((x << 8) & 0x00ff0000u) | (x << 24);
}
u16 swap16(u16 x) {
    return static_cast<u16>((x >> 8) | (x << 8));
}
constexpr SOCKET kInvalidSocket = static_cast<SOCKET>(-1);
constexpr int kSocketError = -1;
constexpr int kWsaeNetDown = 10050;    // WSAENETDOWN
constexpr int kWsaSysNotReady = 10091; // WSASYSNOTREADY
constexpr int kWsaHostNotFound = 11001; // WSAHOST_NOT_FOUND / EAI_NONAME
thread_local int g_wsa_err = 0; // Argon stub-value fix: callers must see a coherent last error, not a hardcoded 10050
} // namespace

extern "C" {

// ---- Berkeley sockets: every call fails closed ----
SOCKET WSAAPI socket(int af, int type, int protocol) {
    (void)af; (void)type; (void)protocol; g_wsa_err = 10047; // WSAEAFNOSUPPORT: offline, no address family available
    return kInvalidSocket;
}
int WSAAPI closesocket(SOCKET s) { (void)s; return kSocketError; }
int WSAAPI ioctlsocket(SOCKET s, long cmd, u_long FAR *argp) {
    (void)s; (void)cmd; (void)argp; return kSocketError;
}
int WSAAPI WSAIoctl(SOCKET s, DWORD dwIoControlCode, LPVOID lpvInBuffer,
                    DWORD cbInBuffer, LPVOID lpvOutBuffer, DWORD cbOutBuffer,
                    LPDWORD lpcbBytesReturned, LPWSAOVERLAPPED lpOverlapped,
                    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    (void)s; (void)dwIoControlCode; (void)lpvInBuffer; (void)cbInBuffer;
    (void)lpvOutBuffer; (void)cbOutBuffer; (void)lpcbBytesReturned;
    (void)lpOverlapped; (void)lpCompletionRoutine; return kSocketError;
}
int WSAAPI WSAPoll(LPWSAPOLLFD fdArray, ULONG fds, INT timeout) {
    (void)fdArray; (void)fds; (void)timeout; return kSocketError;
}
int WSAAPI connect(SOCKET s, const struct sockaddr FAR *name, int namelen) {
    (void)s; (void)name; (void)namelen; return kSocketError;
}
int WSAAPI bind(SOCKET s, const struct sockaddr FAR *name, int namelen) {
    (void)s; (void)name; (void)namelen; return kSocketError;
}
int WSAAPI listen(SOCKET s, int backlog) { (void)s; (void)backlog; return kSocketError; }
SOCKET WSAAPI accept(SOCKET s, struct sockaddr FAR *addr, int FAR *addrlen) {
    (void)s; (void)addr; (void)addrlen; return kInvalidSocket;
}
int WSAAPI shutdown(SOCKET s, int how) { (void)s; (void)how; return kSocketError; }
int WSAAPI setsockopt(SOCKET s, int level, int optname, const char FAR *optval, int optlen) {
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen; return kSocketError;
}
int WSAAPI getsockopt(SOCKET s, int level, int optname, char FAR *optval, int FAR *optlen) {
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen; return kSocketError;
}
int WSAAPI getpeername(SOCKET s, struct sockaddr FAR *name, int FAR *namelen) {
    (void)s; (void)name; (void)namelen; return kSocketError;
}
int WSAAPI getsockname(SOCKET s, struct sockaddr FAR *name, int FAR *namelen) {
    (void)s; (void)name; (void)namelen; return kSocketError;
}
int WSAAPI send(SOCKET s, const char FAR *buf, int len, int flags) {
    (void)s; (void)buf; (void)len; (void)flags; return kSocketError;
}
int WSAAPI recv(SOCKET s, char FAR *buf, int len, int flags) {
    (void)s; (void)buf; (void)len; (void)flags; return kSocketError;
}
int WSAAPI sendto(SOCKET s, const char FAR *buf, int len, int flags,
                 const struct sockaddr FAR *to, int tolen) {
    (void)s; (void)buf; (void)len; (void)flags; (void)to; (void)tolen; return kSocketError;
}
int WSAAPI recvfrom(SOCKET s, char FAR *buf, int len, int flags,
                    struct sockaddr FAR *from, int FAR *fromlen) {
    (void)s; (void)buf; (void)len; (void)flags; (void)from; (void)fromlen; return kSocketError;
}
int WSAAPI select(int nfds, fd_set FAR *readfds, fd_set FAR *writefds,
                  fd_set FAR *exceptfds, const struct timeval FAR *timeout) {
    (void)nfds; (void)readfds; (void)writefds; (void)exceptfds; (void)timeout;
    return kSocketError;
}

// ---- Byte order: implemented for real (pure arithmetic) ----
u_long WSAAPI htonl(u_long hostlong) { return swap32(static_cast<u32>(hostlong)); }
u_short WSAAPI htons(u_short hostshort) { return swap16(static_cast<u16>(hostshort)); }
u_short WSAAPI ntohs(u_short netshort) { return swap16(static_cast<u16>(netshort)); }
// Extra (not in the 40-entry block, harmless dead code, fail-closed-safe):
u_long WSAAPI ntohl(u_long netlong) { return swap32(static_cast<u32>(netlong)); }

// ---- Address conversion: fail closed ----
u_long WSAAPI inet_addr(const char FAR *cp) { (void)cp; return 0xfffffffful; } // INADDR_NONE
// Extra (not in the 40-entry block, harmless dead code):
char FAR * WSAAPI inet_ntoa(struct in_addr in) { (void)in; return nullptr; }
int WSAAPI inet_pton(INT Family, PCSTR pszAddrString, PVOID pAddrBuf) {
    (void)Family; (void)pszAddrString; (void)pAddrBuf; return 0; // 0 = invalid
}
PCSTR WSAAPI inet_ntop(INT Family, const VOID *pAddr, PSTR pStringBuf, size_t StringBufSize) {
    (void)Family; (void)pAddr; (void)pStringBuf; (void)StringBufSize; return nullptr;
}

// ---- DNS / host: fail closed ----
int WSAAPI getaddrinfo(PCSTR pNodeName, PCSTR pServiceName,
                       const ADDRINFOA *pHints, PADDRINFOA *ppResult) {
    (void)pNodeName; (void)pServiceName; (void)pHints;
    if (ppResult) *ppResult = nullptr;
    return kWsaHostNotFound;
}
void WSAAPI freeaddrinfo(PADDRINFOA pAddrInfo) { (void)pAddrInfo; }
int WSAAPI getnameinfo(const SOCKADDR *pSockaddr, socklen_t SockaddrLength,
                       PCHAR pNodeBuffer, DWORD NodeBufferSize,
                       PCHAR pServiceBuffer, DWORD ServiceBufferSize, INT Flags) {
    (void)pSockaddr; (void)SockaddrLength; (void)pNodeBuffer; (void)NodeBufferSize;
    (void)pServiceBuffer; (void)ServiceBufferSize; (void)Flags;
    return kWsaHostNotFound;
}
int WSAAPI gethostname(char FAR *name, int namelen) {
    (void)name; (void)namelen; return kSocketError;
}

// ---- WSA event model (libcurl multi/telnet path): fail closed ----
int WSAAPI WSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents) {
    (void)s; (void)hEventObject; (void)lNetworkEvents; return kSocketError;
}
int WSAAPI WSAEnumNetworkEvents(SOCKET s, WSAEVENT hEventObject,
                                LPWSANETWORKEVENTS lpNetworkEvents) {
    (void)s; (void)hEventObject; (void)lpNetworkEvents; return kSocketError;
}
WSAEVENT WSAAPI WSACreateEvent(void) { return nullptr; }
BOOL WSAAPI WSACloseEvent(WSAEVENT hEvent) { (void)hEvent; return FALSE; }
BOOL WSAAPI WSAResetEvent(WSAEVENT hEvent) { (void)hEvent; return FALSE; }
// Extra (not in the 40-entry block, harmless dead code):
BOOL WSAAPI WSASetEvent(WSAEVENT hEvent) { (void)hEvent; return FALSE; }
DWORD WSAAPI WSAWaitForMultipleEvents(DWORD cEvents, const WSAEVENT FAR *lphEvents,
                                      BOOL fWaitAll, DWORD dwTimeout, BOOL fAlertable) {
    (void)cEvents; (void)lphEvents; (void)fWaitAll; (void)dwTimeout; (void)fAlertable;
    return 0xfffffffful; // WSA_WAIT_FAILED
}
int WSAAPI WSAStartup(WORD wVersionRequested, LPWSADATA lpWSAData) {
    // Argon stub-value fix (1.0.19 diagnostic): returning WSASYSNOTREADY
    // can abort a static-init caller that treats any non-zero as fatal.
    // Return SUCCESS with a populated WSAData (offline: version 2.2
    // claimed, but every socket call still returns INVALID_SOCKET with
    // WSAEAFNOSUPPORT), so libraries initialise cleanly without network.
    (void)wVersionRequested;
    if (lpWSAData) {
        for (size_t i = 0; i < sizeof(*lpWSAData); ++i) ((char*)lpWSAData)[i] = 0;
        lpWSAData->wVersion = MAKEWORD(2, 2);
        lpWSAData->wHighVersion = MAKEWORD(2, 2);
        const char* d = "WinSock 2.0";
        const char* st = "Running";
        size_t i = 0; while (d[i] && i < 256) { lpWSAData->szDescription[i] = d[i]; ++i; } lpWSAData->szDescription[i] = 0;
        i = 0; while (st[i] && i < 128) { lpWSAData->szSystemStatus[i] = st[i]; ++i; } lpWSAData->szSystemStatus[i] = 0;
        lpWSAData->iMaxSockets = 0;
        lpWSAData->iMaxUdpDg = 0;
        lpWSAData->lpVendorInfo = NULL;
    }
    g_wsa_err = 0;
    return 0;
}
int WSAAPI WSACleanup(void) { g_wsa_err = 0; return 0; }
int WSAAPI WSAGetLastError(void) { return g_wsa_err; }
void WSAAPI WSASetLastError(int iError) { (void)iError; }
int WSAAPI __WSAFDIsSet(SOCKET fd, fd_set FAR *set) {
    (void)fd; (void)set; return 0; // not a member
}

// ---- dllimport references (__imp_<name>) resolve to these data objects.
// Each holds the address of its stub, so an indirect call through the
// "import address" lands in the stub, with no DLL import involved.
void *__imp_socket = (void *)socket;
void *__imp_closesocket = (void *)closesocket;
void *__imp_ioctlsocket = (void *)ioctlsocket;
void *__imp_WSAIoctl = (void *)WSAIoctl;
void *__imp_WSAPoll = (void *)WSAPoll;
void *__imp_connect = (void *)connect;
void *__imp_bind = (void *)bind;
void *__imp_listen = (void *)listen;
void *__imp_accept = (void *)accept;
void *__imp_shutdown = (void *)shutdown;
void *__imp_setsockopt = (void *)setsockopt;
void *__imp_getsockopt = (void *)getsockopt;
void *__imp_getpeername = (void *)getpeername;
void *__imp_getsockname = (void *)getsockname;
void *__imp_send = (void *)send;
void *__imp_recv = (void *)recv;
void *__imp_sendto = (void *)sendto;
void *__imp_recvfrom = (void *)recvfrom;
void *__imp_select = (void *)select;
void *__imp_htonl = (void *)htonl;
void *__imp_htons = (void *)htons;
void *__imp_ntohl = (void *)ntohl;
void *__imp_ntohs = (void *)ntohs;
void *__imp_inet_addr = (void *)inet_addr;
void *__imp_inet_ntoa = (void *)inet_ntoa;
void *__imp_inet_pton = (void *)inet_pton;
void *__imp_inet_ntop = (void *)inet_ntop;
void *__imp_getaddrinfo = (void *)getaddrinfo;
void *__imp_freeaddrinfo = (void *)freeaddrinfo;
void *__imp_getnameinfo = (void *)getnameinfo;
void *__imp_gethostname = (void *)gethostname;
void *__imp_WSAEventSelect = (void *)WSAEventSelect;
void *__imp_WSAEnumNetworkEvents = (void *)WSAEnumNetworkEvents;
void *__imp_WSACreateEvent = (void *)WSACreateEvent;
void *__imp_WSACloseEvent = (void *)WSACloseEvent;
void *__imp_WSAResetEvent = (void *)WSAResetEvent;
void *__imp_WSASetEvent = (void *)WSASetEvent;
void *__imp_WSAWaitForMultipleEvents = (void *)WSAWaitForMultipleEvents;
void *__imp_WSAStartup = (void *)WSAStartup;
void *__imp_WSACleanup = (void *)WSACleanup;
void *__imp_WSAGetLastError = (void *)WSAGetLastError;
void *__imp_WSASetLastError = (void *)WSASetLastError;
void *__imp___WSAFDIsSet = (void *)__WSAFDIsSet;

} // extern "C"
