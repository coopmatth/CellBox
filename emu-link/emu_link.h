// emu_link — Jarvis-direct Emu-linking layer, 2026-10-26/27.
// HONEST SCOPE, stated in the file so it cannot be misread:
//  * This layer fills the REAL emu_callbacks surface from Emu/emu_callbacks.h
//    (verified 2026-10-26: call_from_main_thread, init_gs_render(utils::serial*),
//    get_gs_frame()->GSFrameBase, init_pad_handler(title_id), get_audio()->
//    AudioBackend, on_run/on_pause/on_resume/on_stop, update/save_emu_settings,
//    get_localized_string) with host callbacks: CoreWindow Dispatcher,
//    D3D12GSRender prototype, CoreWindow GSFrame shim, Windows.Gaming.Input
//    pad, XAudio2 audio.
//  * It does NOT include any RPCS3 core header and does NOT instantiate the
//    real Emulator object. Reason, measured in this run: the PC RPCS3 build
//    is a Qt GUI executable, not a linkable UWP library; System.h pulls
//    cfg/fxo/LLVM/Qt-adjacent machinery that cannot be linked into an
//    AppContainer host by dropping in one header. Building the core as a
//    UWP library is a separate, larger build — it is the next work after
//    this layer, and boot_gate() below refuses to claim otherwise.
//  * Therefore: callbacks_bound + probe counts + PSN Simulated + governor
//    ownership are PROVEN here; a real title boot is NOT proven here and
//    boot_gate() returns false with that reason, every time, until the
//    core library exists. A PASS verdict for this layer means the wiring
//    shape is correct, not that a game boots.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace emu_link {

using LogFn = std::function<void(const std::string& line)>;

// PSN status in host config. There is no "real PSN" value in this enum,
// by design: RPCS3 cannot do a real PSN login, and the host must never
// attempt one. Simulated first, RPCN second.
enum class PsnStatus : int { kSimulated = 0, kRpcn = 1 };

// Host callbacks, one per real emu_callbacks field used by the host.
// Signatures are host-native (std::function, plain types) so this file
// compiles without any core header; the mapping table in emu_link.cpp
// names the core field each one fills.
struct HostCallbacks {
    std::function<void(std::function<void()>)> call_from_main_thread;
    std::function<void()> init_gs_render;          // -> D3D12GSRender
    std::function<bool()> get_gs_frame;            // -> GSFrameBase shim ok?
    std::function<void(const std::string&)> init_pad_handler; // title_id
    std::function<bool()> get_audio;               // -> XAudio2 running?
    std::function<void()> on_run;
    std::function<void()> on_pause;
    std::function<void()> on_resume;
    std::function<void()> on_stop;
};

// B policy, shared with the host's proven BackRequested handler:
// 0 = ignore (no game), 1 = Circle (game running), 2 = overlay combo.
inline int back_action(bool game_running, bool overlay_combo) {
    if (overlay_combo) return 2;
    return game_running ? 1 : 0;
}

class EmuLink {
public:
    explicit EmuLink(LogFn log);
    // Binds all required callbacks; false if any required one is null.
    bool bind(const HostCallbacks& cb);
    // Probes each bound callback once and counts invocations. Returns
    // true only if every required probe fired.
    bool probe();
    void set_psn(PsnStatus s) { psn_ = s; }
    PsnStatus psn() const { return psn_; }
    void set_game_running(bool r) { game_running_ = r; }
    bool game_running() const { return game_running_; }
    int on_back_requested(bool overlay_combo) const {
        return back_action(game_running_, overlay_combo);
    }
    // The boot gate. Always false in this build, with the reason.
    // Callers must log boot_gate_reason() and must not claim a boot.
    bool boot_gate(std::string& reason) const;
    const char* boot_gate_reason() const {
        return "real Emulator object NOT linked: PC RPCS3 build is a Qt GUI "
               "executable, not a linkable UWP library; core-as-library "
               "build is the next work. Callback wiring only.";
    }
    bool verdict_ok() const {
        return bound_ && probed_ && psn_ == PsnStatus::kSimulated;
    }
    std::string summary() const;
    // Field mapping, core field -> host callback, for audit.
    static const char* field_map();
private:
    LogFn log_;
    HostCallbacks cb_;
    bool bound_ = false;
    bool probed_ = false;
    PsnStatus psn_ = PsnStatus::kSimulated;
    bool game_running_ = false;
    int probe_main_thread_ = 0;
    int probe_gs_render_ = 0;
    int probe_gs_frame_ = 0;
    int probe_pad_ = 0;
    int probe_audio_ = 0;
};

} // namespace emu_link
