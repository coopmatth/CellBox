// emu_link.cpp — pure C++, no WinRT, no core headers. Compiles without
// /ZW in both the desktop and UWP routes, like memory_governor.cpp.
#include "emu_link.h"
#include <cstdio>

namespace emu_link {

EmuLink::EmuLink(LogFn log) : log_(log) {}

const char* EmuLink::field_map() {
    return "call_from_main_thread->CoreWindow Dispatcher; "
           "init_gs_render(serial*)->D3D12GSRender prototype; "
           "get_gs_frame()->CoreWindow GSFrameBase shim (bounds/flip/present); "
           "init_pad_handler(title_id)->Windows.Gaming.Input (B->Circle when "
           "game_running, BackRequested Handled=true, overlay View+Menu/L3+R3); "
           "get_audio()->XAudio2 backend; on_run/on_pause/on_resume/on_stop->"
           "host lifecycle log; update/save_emu_settings->host config log; "
           "get_localized_string->identity (host has no Qt locale table); "
           "psn_status=Simulated first, RPCN second, never real PSN; "
           "llvm_threads=1; governor (Step 8 caps, abort 3.0 GiB) owns memory; "
           "Y-flip viewport-negative-height ONLY, flip_yz OFF";
}

bool EmuLink::bind(const HostCallbacks& cb) {
    cb_ = cb;
    bound_ = (bool)cb_.call_from_main_thread && (bool)cb_.init_gs_render &&
             (bool)cb_.get_gs_frame && (bool)cb_.init_pad_handler &&
             (bool)cb_.get_audio && (bool)cb_.on_run && (bool)cb_.on_pause &&
             (bool)cb_.on_resume && (bool)cb_.on_stop;
    if (log_) {
        log_(std::string("EMU-LINK-BIND: ") + (bound_ ? "PASS" : "FAIL") +
             " (all required emu_callbacks fields non-null?)");
        log_(std::string("EMU-LINK-FIELDMAP: ") + field_map());
    }
    return bound_;
}

bool EmuLink::probe() {
    if (!bound_) { probed_ = false; return false; }
    // Each probe is a real invocation of the host callback, counted.
    cb_.call_from_main_thread([this]() { ++probe_main_thread_; });
    cb_.init_gs_render(); ++probe_gs_render_;
    if (cb_.get_gs_frame()) ++probe_gs_frame_;
    cb_.init_pad_handler("BLUS31527"); ++probe_pad_;
    if (cb_.get_audio()) ++probe_audio_;
    cb_.on_run();
    probed_ = probe_main_thread_ > 0 && probe_gs_render_ > 0 &&
              probe_gs_frame_ > 0 && probe_pad_ > 0 && probe_audio_ > 0;
    if (log_) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "EMU-LINK-PROBE: %s main_thread=%d gs_render=%d gs_frame=%d "
            "pad=%d audio=%d psn=%d (0=Simulated,1=RPCN)",
            probed_ ? "PASS" : "FAIL", probe_main_thread_, probe_gs_render_,
            probe_gs_frame_, probe_pad_, probe_audio_, (int)psn_);
        log_(buf);
    }
    return probed_;
}

bool EmuLink::boot_gate(std::string& reason) const {
    reason = boot_gate_reason();
    return false;
}

std::string EmuLink::summary() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "EMU-LINK-SUMMARY bound=%d probed=%d game_running=%d psn=%d "
        "back_no_game=%d back_in_game=%d back_overlay=%d",
        (int)bound_, (int)probed_, (int)game_running_, (int)psn_,
        back_action(false, false), back_action(true, false),
        back_action(false, true));
    return buf;
}

} // namespace emu_link
