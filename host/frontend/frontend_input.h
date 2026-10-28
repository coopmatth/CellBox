// frontend_input.h - CellBox F1: gamepad menu navigation on the WGI
// layer (same ABI pattern as host_pad.cpp / audio_pad_probe.cpp, WRL,
// no /ZW). The MENU pad state here is wholly separate from the game
// pad state in host_pad.cpp: this class owns its own previous-reading
// and edge detection, is only consumed by the frontend, and is synced
// (sync_to_current) on every handoff so a button held across the
// library->game or game->library boundary can never fire on the
// other side. During a game it is polled ONLY for the exit combo.
#pragma once

#include <cstdint>
#include "frontend_library.h" // LogFn

namespace frontend
{

struct MenuInput
{
    bool up = false, down = false, left = false, right = false; // edges (+ auto-repeat)
    bool accept = false; // A edge
    bool back = false;   // B edge
    bool menu = false;   // Menu edge
    bool view = false;   // View edge
    bool exit_combo = false; // View+Menu held >= 800 ms (fires once per hold)
    bool connected = false;
    unsigned raw_buttons = 0;
};

class MenuPad
{
public:
    // Acquires IGamepadStatics (retried inside poll() if it fails, the
    // host_pad 1.0.36 lesson). Logs FRONTEND-INPUT-INIT with the hr.
    bool init(const LogFn& log);
    // One WGI poll -> menu events. No pad = connected=false, no events.
    MenuInput poll();
    // Edge/repeat core over one reading (WGI button bits + left stick).
    // Split out so the desktop pre-test drives it with synthetic data.
    MenuInput update_from_reading(unsigned buttons, double lx, double ly,
                                  std::uint64_t now_ms, bool connected);
    // Adopt the last reading as the baseline: held buttons produce no
    // edges until released and pressed again (handoff leak guard).
    void sync_to_current();
    unsigned long long polls() const { return m_polls; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    LogFn m_log;
    unsigned long long m_polls = 0;
    unsigned m_prev_buttons = 0;
    unsigned m_prev_dirs = 0; // merged dpad+stick dirs: 1U 2D 4L 8R
    std::uint64_t m_dir_since_ms[4] = {};
    std::uint64_t m_dir_next_ms[4] = {};
    std::uint64_t m_combo_since_ms = 0;
    bool m_combo_fired = false;
    unsigned m_last_buttons = 0;
    double m_last_lx = 0.0, m_last_ly = 0.0;
    bool m_last_connected = false;
};

} // namespace frontend
