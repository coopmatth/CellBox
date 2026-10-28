// Host stubs for symbols emucore references but which live in the Qt GUI app
// (rpcs3qt) or in Input peripherals the Xbox host does not use. These are
// LINK-PROBE stubs only: the real Xbox host replaces pad handling with
// Windows.Gaming.Input, configs with its own cfg objects, and the GL device
// name with the D3D12 device. Peripheral trackers (PS Move camera, Wiimote,
// SDL) are unused on Xbox and return empty/no-op results. Do NOT ship these
// stubs as the pad implementation; they only let the link probe prove that
// emucore itself links.
#include "stdafx.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include "fatal_log.h"
#include "Utilities/Thread.h"
#include "Input/pad_thread.h"
#include "Emu/Io/pad_config.h"
#include "Input/ps_move_config.h"
#include "Input/product_info.h"
#include "Input/ps_move_tracker.h"
#include "Input/sdl_instance.h"
#include "Input/wiimote_handler.h"
#include <functional>
#include <string>
#include <string_view>

namespace pad {
    atomic_t<pad_thread*> g_pad_thread{nullptr};
    shared_mutex g_pad_mutex;
}

// pad_thread methods referenced by cellPad/cellGem but implemented in
// Input/pad_thread.cpp (not part of emucore.vcxproj). Stubbed here for the
// probe; the host provides real pad handling.
s32 pad_thread::AddLddPad() { return -1; }
void pad_thread::UnregisterLddPad(u32) {}
void pad_thread::SetRumble(u32, u8, u8) {}
void pad_thread::SetIntercepted(bool) {}

// Config globals defined by the Qt app in the desktop build.
cfg_input_configurations g_cfg_input_configs;
cfg_ps_moves g_cfg_move;
std::string g_input_config_override;
// cfg_ps_moves ctor/load live in Input/ps_move_config.cpp (not in emucore).
cfg_ps_moves::cfg_ps_moves() {}
bool cfg_ps_moves::load() { return false; }

// Qt event-loop helper: desktop pumps Qt events while waiting; host runs
// the op directly (dispatcher pump is the host's own job).
void qt_events_aware_op(int, std::function<bool()>) {}

// Fatal-error reporter (1.0.35): replaces the probe no-op. Appends to
// LocalState\rpcs3-fatal.txt (never host-results.txt) with thread id and
// ms since boot-return, flushed + closed before returning, so the line
// survives whatever the caller does next. NOTE on the [[noreturn]]
// contract: emergency_exit (Utilities/Thread.cpp:3466) only reaches
// report_fatal_error when g_tls_this_thread is null; for a normal named
// worker thread it _endthreadex's first and never calls this. A direct
// caller that expects noreturn and gets a return is the pre-existing
// stub contract situation, not something this file can fix -- the
// evidence is already on disk either way.
namespace {
std::wstring g_fatal_dir;
SRWLOCK g_fatal_lock = SRWLOCK_INIT;
std::atomic<unsigned long long> g_fatal_boot_ms{0};
std::atomic<bool> g_fatal_boot_set{false};
} // namespace

void xbox_fatal_set_dir(const wchar_t* dir)
{
    if (!dir) return;
    g_fatal_dir = dir;
    std::wstring path = g_fatal_dir + L"\\rpcs3-fatal.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
void xbox_fatal_set_boot_ms(unsigned long long ms)
{
    g_fatal_boot_ms.store(ms);
    g_fatal_boot_set.store(true);
}
void xbox_fatal_write(const char* text, size_t text_len, bool /*is_html*/, bool /*include_help_text*/)
{
    if (g_fatal_dir.empty() || !text) return;
    char textbuf[1001];
    size_t n = text_len < 1000 ? text_len : 1000;
    for (size_t i = 0; i < n; ++i) textbuf[i] = text[i];
    textbuf[n] = '\0';
    unsigned long long tick = GetTickCount64();
    long long rel = g_fatal_boot_set.load() ? (long long)(tick - g_fatal_boot_ms.load()) : -1;
    char line[1280];
    snprintf(line, sizeof(line), "BOOT-FATAL tid=%lu tick=%llu boot_ms=%lld text=%s\r\n",
        GetCurrentThreadId(), tick, rel, textbuf);
    std::wstring path = g_fatal_dir + L"\\rpcs3-fatal.txt";
    AcquireSRWLockExclusive(&g_fatal_lock);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)strlen(line), &w, nullptr);
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    ReleaseSRWLockExclusive(&g_fatal_lock);
}
void report_fatal_error(std::string_view text, bool is_html, bool include_help_text)
{
    xbox_fatal_write(text.data(), text.size(), is_html, include_help_text);
}

// GL device name: desktop GL backend only; Xbox uses D3D12.
// Note the const return type: the mangled symbol is const std::string.
namespace gl {
    const std::string get_device_name() { return {}; }
}

// Input product enumeration: desktop SDL/USB enumeration; Xbox uses WGI.
namespace input {
    std::vector<product_info> get_products_by_class(int) { return {}; }
}

// PS Move camera tracker (template, DiagnosticsEnabled=false): unused on Xbox.
template <>
ps_move_tracker<false>::ps_move_tracker() {}
template <>
ps_move_tracker<false>::~ps_move_tracker() {}
template <>
void ps_move_tracker<false>::set_image_data(const void*, u64, u32, u32, s32) {}
template <>
void ps_move_tracker<false>::process_image() {}
template <>
void ps_move_tracker<false>::set_active(u32, bool) {}
template <>
void ps_move_tracker<false>::set_hue(u32, u16) {}
template <>
void ps_move_tracker<false>::set_hue_threshold(u32, u8) {}
template <>
void ps_move_tracker<false>::set_saturation_threshold(u32, u8) {}
template <>
void ps_move_tracker<false>::set_value_threshold(u32, u8) {}
template <>
std::tuple<u8, u8, u8> ps_move_tracker<false>::hsv_to_rgb(u16, f32, f32) { return {}; }
template <>
std::tuple<s16, f32, f32> ps_move_tracker<false>::rgb_to_hsv(f32, f32, f32) { return {}; }

// SDL instance: desktop SDL3 init; Xbox uses WGI, so never initialised.
#ifdef HAVE_SDL3
sdl_instance::~sdl_instance() {}
sdl_instance& sdl_instance::get_instance() { static sdl_instance inst; return inst; }
bool sdl_instance::initialize() { return false; }
void sdl_instance::pump_events() {}
#endif

// Wiimote handler: no Wiimotes on Xbox.
wiimote_handler* wiimote_handler::get_instance() { return nullptr; }
std::vector<wiimote_state> wiimote_handler::get_states() { return {}; }
