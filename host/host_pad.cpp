// host_pad.cpp — v1.0.37.0: HOST implementation of pad_thread, Stage 3.
// 1.0.37 changes: WGI table now carries its DIGITAL1/DIGITAL2 offset
// explicitly (derivation if-chain removed) + BOOT-PAD-MAP startup dump.
// Nothing else in the pad path changes (stateless polling, MTA record,
// PAD-DIAG format, m_pressed + m_value=255, CONNECTED status).
// 1.0.36 changes (stateless WGI + PAD-DIAG; mapping itself UNCHANGED —
// it already wrote m_pressed + m_value=255 to both button lists and
// CELL_PAD_STATUS_CONNECTED, verified against the task's three checks):
//  - wgi_first_pad() already re-enumerated fresh every poll in 1.0.35
//    (no cached Gamepad^ across ticks) — kept, plus per-poll count +
//    raw pointer recording for PAD-DIAG.
//  - CoInitializeEx HRESULT is now recorded (mta field): 0 = MTA join
//    OK; 0x80010106 (RPC_E_CHANGED_MODE) = thread already STA — the
//    diagnostic for silently dropped WGI re-enumeration.
//  - Activation-factory acquisition retries (every 10th poll) instead
//    of failing once forever (1.0.35 tried exactly once).
//  - PAD-DIAG line once per second (every 10th 100 ms poll) to
//    boot-watchdog.txt (watchdog_append is locked + thread-safe):
//    PAD-DIAG mta=%ld wgi_cnt=%u pad_ptr=%p raw_btn=0x%X raw_lx=%.2f
//      cell_stat=0x%X cell_d1=0x%X cell_d2=0x%X
//    Reading it: wgi_cnt=0 with the controller light on ->
//    RoInitialize/focus problem; pad_ptr changing every poll ->
//    enumeration churn (NOT a stale cache — enumeration is fresh);
//    raw_btn != 0 but cell_d1/d2 == 0 -> translation problem.
//    Competing theory kept on the record: the 1.0.35 freeze
//    (connected 1->0, buttons 0x0) is also consistent with the
//    controller auto-powering-off after ~15 min idle before the
//    owner's late-run presses and never re-enumerating — PAD-DIAG
//    discriminates this from a code bug, it does not assume it away.
// Why this file exists (measured, not assumed): Input/pad_thread.cpp is
// NOT part of emucore.vcxproj (stated in xbox-core-lib/linktest/
// host-stubs.cpp, and proven by LNK2019 for pad_thread::pad_thread,
// ~pad_thread, operator() and pad::g_started when boot_probe.cpp tried
// to create named_thread<pad_thread>). emucore's cellPad calls the
// STRICT pad::get_pad_thread() (ensure = fatal abort) at 21 sites, so
// a null g_pad_thread aborts the process on the game's first pad call.
// The core creates the pad_thread ONLY through the host's
// init_pad_handler callback, so the host must supply the object.
//
// v1.0.29 CHANGE (Stage 3, WGI delivery): player 0 is no longer a
// DISCONNECTED null pad. Init() builds a full button/stick set for
// player 0, and update_pad_states() polls Windows.Gaming.Input
// (WRL, no /ZW — same pattern as the Step 7 audio_pad_probe) and
// writes the reading into the Pad object that cellPad reads.
// cellPad's pad_get_data reads pad->m_buttons_external /
// m_sticks_external (NOT m_buttons/m_sticks directly), so this file
// fills BOTH: the Button/AnalogStick lists (for handler-side code)
// and the External lists (for the game). Connection: a present WGI
// gamepad sets m_port_status CONNECTED + m_pads_connected[0] + the
// pad_info now_connect count (via cellPad's own status propagation
// from m_port_status); no gamepad leaves player 0 DISCONNECTED.
// Players 1-6 stay DISCONNECTED null pads (BO3 local multiplayer on
// one pad is out of scope for this build).
//
// Mapping (WGI bit -> CELL_PAD): Menu->START, View->SELECT, A->CROSS,
// B->CIRCLE, X->SQUARE, Y->TRIANGLE, DPad->directions,
// LeftShoulder->L1, RightShoulder->R1, LeftThumbstick->L3,
// RightThumbstick->R3, triggers->L2/R2 analog. Sticks: WGI X -1..1
// maps to 0..255 (left=0, right=255); WGI Y +1=up maps to 0, -1=down
// maps to 255 (PS3 convention: 0=up). 0.05 deadzone on sticks,
// triggers pressed above 0.05. PRESS values: pressed ? 255 : 0
// (triggers use their analog value). Rumble is NOT wired (SetRumble
// stays in host-stubs.cpp as a no-op) — a game requesting rumble
// simply gets silence, not a crash.
// B button double duty, by design: WGI B -> CIRCLE here for the game,
// while the host's BackRequested handler (main_uwp) keeps
// Handled=true so the system Back action does not close the app.
// NO RPCS3 core file is modified; this TU is host-only.

#include <chrono>
#include <thread>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <algorithm>

#include <roapi.h>
#include <windows.gaming.input.h>
#include <wrl/client.h>

#include "Input/pad_thread.h"
#include "Emu/System.h"
#include "Emu/Io/pad_types.h"
#include "pad_wgi.h"
#include "watchdog.h"

using Microsoft::WRL::ComPtr;
namespace GI = ABI::Windows::Gaming::Input;
namespace WF = ABI::Windows::Foundation;

namespace pad
{
    atomic_t<bool> g_started{false};
    atomic_t<bool> g_reset{false};
    atomic_t<bool> g_enabled{true};
    atomic_t<bool> g_home_menu_requested{false};
    std::string g_title_id;
}

// ---- WGI polling state (pad thread only) ----
namespace
{
    ComPtr<GI::IGamepadStatics> g_wgi_statics;
    bool g_wgi_com_init = false;
    long g_wgi_com_hr = -1; // CoInitializeEx HRESULT on the pad thread (PAD-DIAG mta)
    std::uint64_t g_wgi_factory_try = 0; // last poll index that retried the factory
    unsigned g_wgi_last_count = 0; // gamepads seen on the last enumeration
    void* g_wgi_last_ptr = nullptr; // raw IGamepad* from the last enumeration
    double g_wgi_last_lx = 0.0; // last successful LeftThumbstickX reading
    std::atomic<std::uint64_t> g_wgi_polls{0};
    std::atomic<std::uint64_t> g_wgi_connected{0};
    std::atomic<std::uint64_t> g_wgi_buttons{0};

    bool wgi_ensure()
    {
        if (!g_wgi_com_init)
        {
            // Runs on the pad thread (update_pad_states is called from
            // pad_thread::operator()). MTA join is what lets WGI
            // connect/re-enumerate notifications arrive; without it
            // they silently drop. S_FALSE = already MTA; changed-mode
            // (STA) is recorded, not hidden — it is the PAD-DIAG
            // discriminator for the RoInitialize theory.
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            g_wgi_com_hr = static_cast<long>(hr);
            if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
                g_wgi_com_init = true;
        }
        if (g_wgi_statics)
            return true;
        // Retry the factory at most every 10th poll (1.0.35 tried once
        // and never again — an early failure was permanent).
        const std::uint64_t p = g_wgi_polls.load();
        if (g_wgi_factory_try != 0 && p - g_wgi_factory_try < 10)
            return false;
        g_wgi_factory_try = p;
        HSTRING cls = nullptr;
        if (FAILED(WindowsCreateString(L"Windows.Gaming.Input.Gamepad", 28, &cls)))
            return false;
        HRESULT hr = RoGetActivationFactory(cls, __uuidof(GI::IGamepadStatics), (void**)&g_wgi_statics);
        WindowsDeleteString(cls);
        return SUCCEEDED(hr) && g_wgi_statics != nullptr;
    }

    // First connected WGI gamepad, or nullptr. Re-enumerated per poll
    // (100 ms cadence): enumeration diffing is how disconnect/reconnect
    // is observed here; the Added/Removed event path is unproven and
    // not used. NO Gamepad^ is cached across ticks — GetAt(0) runs
    // fresh on every poll (1.0.36 stateless requirement, already true
    // in 1.0.35 and kept).
    ComPtr<GI::IGamepad> wgi_first_pad()
    {
        g_wgi_last_count = 0;
        g_wgi_last_ptr = nullptr;
        if (!wgi_ensure()) return nullptr;
        ComPtr<WF::Collections::IVectorView<GI::Gamepad*>> view;
        if (FAILED(g_wgi_statics->get_Gamepads(&view)) || !view) return nullptr;
        unsigned count = 0;
        if (FAILED(view->get_Size(&count)) || count == 0) return nullptr;
        g_wgi_last_count = count;
        ComPtr<GI::IGamepad> gp;
        if (FAILED(view->GetAt(0, &gp))) return nullptr;
        g_wgi_last_ptr = gp.Get();
        return gp;
    }

    std::uint8_t axis_u8(double v) // -1..1 -> 0..255
    {
        if (v < -1.0) v = -1.0;
        if (v > 1.0) v = 1.0;
        return static_cast<std::uint8_t>((v + 1.0) * 127.5);
    }
    std::uint8_t axis_y_u8(double v) // WGI +1=up -> PS3 0=up
    {
        if (v < -1.0) v = -1.0;
        if (v > 1.0) v = 1.0;
        return static_cast<std::uint8_t>((1.0 - v) * 127.5);
    }
    std::uint8_t trig_u8(double v) // 0..1 -> 0..255
    {
        if (v < 0.0) v = 0.0;
        if (v > 1.0) v = 1.0;
        return static_cast<std::uint8_t>(v * 255.0);
    }

    // 1.0.37: the register (DIGITAL1/DIGITAL2) is now CARRIED in the
    // table, never derived by an if-chain. Console-measured 1.0.36
    // PAD-DIAG proved the symptom (A -> d1=0x40 Down, Start -> nothing,
    // LB -> d1=0x4 R3) even though the 1.0.36 table values and the
    // derivation read correctly against the tree constants
    // (pad_types.h Digital1Flags/Digital2Flags) — so the derivation
    // is removed entirely rather than re-debated: if 1.0.37 PAD-DIAG
    // still shows a swap, the BOOT-PAD-MAP dump below proves which
    // table the binary actually holds, and the cause lies outside
    // this table (second writer / stale binary), not in it.
    // press_offset 0 = no pressure entry (digital-only: SELECT/START/L3/R3/directions/PS).
    struct WgiBtn { unsigned bit; u32 offset; u32 out; u32 press_offset; };
    static const WgiBtn kWgiBtns[] = {
        {0x2, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_SELECT, 0},
        {0x1, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_START, 0},
        {0x1000, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_L3, 0},
        {0x2000, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_R3, 0},
        {0x40, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_UP, CELL_PAD_BTN_OFFSET_PRESS_UP},
        {0x80, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_DOWN, CELL_PAD_BTN_OFFSET_PRESS_DOWN},
        {0x100, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_LEFT, CELL_PAD_BTN_OFFSET_PRESS_LEFT},
        {0x200, CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_RIGHT, CELL_PAD_BTN_OFFSET_PRESS_RIGHT},
        {0x400, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L1, CELL_PAD_BTN_OFFSET_PRESS_L1},
        {0x800, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R1, CELL_PAD_BTN_OFFSET_PRESS_R1},
        {0x20, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_TRIANGLE, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE},
        {0x8, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CIRCLE, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE},
        {0x4, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CROSS, CELL_PAD_BTN_OFFSET_PRESS_CROSS},
        {0x10, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_SQUARE, CELL_PAD_BTN_OFFSET_PRESS_SQUARE},
    };

    void set_button(std::shared_ptr<Pad>& pad, u32 offset, u32 out, bool pressed, u16 value)
    {
        for (auto& b : pad->m_buttons)
        {
            if (b.m_offset == offset && b.m_outKeyCode == out)
            {
                b.m_pressed = pressed;
                b.m_value = value;
                break;
            }
        }
        for (auto& b : pad->m_buttons_external)
        {
            if (b.m_offset == offset && b.m_outKeyCode == out)
            {
                b.m_pressed = pressed;
                b.m_value = value;
                break;
            }
        }
    }
    void set_stick(std::shared_ptr<Pad>& pad, u32 offset, u16 value)
    {
        for (auto& s : pad->m_sticks)
        {
            if (s.m_offset == offset) { s.m_value = value; break; }
        }
        for (auto& s : pad->m_sticks_external)
        {
            if (s.m_offset == offset) { s.m_value = value; break; }
        }
    }
}

std::uint64_t host_pad_wgi_polls() { return g_wgi_polls.load(); }
std::uint64_t host_pad_wgi_connected() { return g_wgi_connected.load(); }
std::uint64_t host_pad_wgi_buttons() { return g_wgi_buttons.load(); }

pad_thread::pad_thread(void* curthread, void* curwindow, std::string_view title_id)
    : m_curthread(curthread)
    , m_curwindow(curwindow)
{
    pad::g_title_id = title_id;
    pad::g_pad_thread = this;
    pad::g_started = false;
}

pad_thread::~pad_thread()
{
    pad::g_pad_thread = nullptr;
}

void pad_thread::Init()
{
    // Player 0: full button/stick set, driven by WGI in
    // update_pad_states(). Players 1-6: DISCONNECTED null pads.
    m_handlers.clear();
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++)
    {
        m_pads[i] = std::make_shared<Pad>(
            pad_handler::null, i,
            CELL_PAD_STATUS_DISCONNECTED,
            CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE | CELL_PAD_CAPABILITY_ACTUATOR,
            CELL_PAD_DEV_TYPE_STANDARD);
        m_pads[i]->m_class_type = CELL_PAD_PCLASS_TYPE_STANDARD;
        m_pads_connected[i] = false;
    }
    {
        auto& pad = m_pads[0];
        // DIGITAL1 buttons (offset 2)
        const std::vector<std::set<u32>> empty;
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, empty, CELL_PAD_CTRL_SELECT);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, empty, CELL_PAD_CTRL_L3);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, empty, CELL_PAD_CTRL_R3);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, empty, CELL_PAD_CTRL_START);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, empty, CELL_PAD_CTRL_UP);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, empty, CELL_PAD_CTRL_RIGHT);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, empty, CELL_PAD_CTRL_DOWN);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, empty, CELL_PAD_CTRL_LEFT);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL1, empty, CELL_PAD_CTRL_PS);
        // DIGITAL2 buttons (offset 3)
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, empty, CELL_PAD_CTRL_L2);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, empty, CELL_PAD_CTRL_R2);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, empty, CELL_PAD_CTRL_L1);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, empty, CELL_PAD_CTRL_R1);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, empty, CELL_PAD_CTRL_TRIANGLE);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, empty, CELL_PAD_CTRL_CIRCLE);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, empty, CELL_PAD_CTRL_CROSS);
        pad->m_buttons.emplace_back(CELL_PAD_BTN_OFFSET_DIGITAL2, empty, CELL_PAD_CTRL_SQUARE);
        // PRESS buttons (offsets 8-19), outKeyCode == offset value
        const u32 press_offsets[] = {
            CELL_PAD_BTN_OFFSET_PRESS_RIGHT, CELL_PAD_BTN_OFFSET_PRESS_LEFT,
            CELL_PAD_BTN_OFFSET_PRESS_UP, CELL_PAD_BTN_OFFSET_PRESS_DOWN,
            CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE,
            CELL_PAD_BTN_OFFSET_PRESS_CROSS, CELL_PAD_BTN_OFFSET_PRESS_SQUARE,
            CELL_PAD_BTN_OFFSET_PRESS_L1, CELL_PAD_BTN_OFFSET_PRESS_R1,
            CELL_PAD_BTN_OFFSET_PRESS_L2, CELL_PAD_BTN_OFFSET_PRESS_R2,
        };
        for (u32 off : press_offsets)
            pad->m_buttons.emplace_back(off, empty, off);
        // Sticks: [0]=LX [1]=LY [2]=RX [3]=RY (offsets are what
        // cellPad reads via the external arrays, so game data is
        // correct regardless of internal index order).
        pad->m_sticks[0] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, empty, empty);
        pad->m_sticks[1] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, empty, empty);
        pad->m_sticks[2] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, empty, empty);
        pad->m_sticks[3] = AnalogStick(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, empty, empty);
        // External mirrors (cellPad's pad_get_data reads these).
        // m_buttons_external is a vector; m_sticks_external is a
        // fixed array<AnalogStickExternal,4>, assigned by index.
        for (const auto& b : pad->m_buttons)
            pad->m_buttons_external.push_back(ButtonExternal{b.m_offset, b.m_outKeyCode, 0, false});
        for (u32 si = 0; si < 4; ++si)
            pad->m_sticks_external[si] = AnalogStickExternal{pad->m_sticks[si].m_offset, 128};
        // 1.0.37: dump the mapping table the binary actually holds, so
        // the 1.0.37 console run can prove (or refute) that the table
        // in this file is the table in the binary. One line per entry.
        for (const auto& m : kWgiBtns)
        {
            char mbuf[160];
            std::snprintf(mbuf, sizeof(mbuf),
                "BOOT-PAD-MAP wgi=0x%X off=%u out=0x%X press=%u\r\n",
                m.bit, (unsigned)m.offset, (unsigned)m.out, (unsigned)m.press_offset);
            watchdog_append(mbuf);
        }
        // Default stick centre (external already 128; internal too).
        for (auto& s : pad->m_sticks) s.m_value = 128;
    }
    m_info.now_connect = 0;
    m_info.system_info = 0;
    m_info.ignore_input = false;
    num_ldd_pad = 0;
}

void pad_thread::operator()()
{
    Init();
    pad::g_reset = false;
    pad::g_started = true;

    // Stay alive while emulation runs; return when Emu stops so the
    // named_thread join on teardown completes. Re-init on reset flag,
    // mirroring the real thread's reset handling in miniature.
    // v1.0.29: poll WGI into player 0 on every 100 ms tick.
    while (!Emu.IsStopped())
    {
        if (pad::g_reset.load())
        {
            Init();
            pad::g_reset = false;
        }
        update_pad_states();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void pad_thread::open_home_menu()
{
    // No host home-menu overlay in this probe; request flag only.
    pad::g_home_menu_requested = true;
}

void pad_thread::InitLddPad(u32, const u32*)
{
    // No LDD pads on this host.
}

void pad_thread::apply_copilots()
{
    // No copilots on null pads (copilot lists stay empty).
}

void pad_thread::update_pad_states()
{
    g_wgi_polls.fetch_add(1);
    // 1.0.43 (iii) edge state, shared by the disconnect + success paths below.
    static unsigned s_edge_last_raw = 0;
    static bool s_edge_first = true;
    auto edge_check = [&](unsigned raw, u32 d1, u32 d2)
    {
        if (s_edge_first) { s_edge_last_raw = raw; s_edge_first = false; return; }
        if (raw == s_edge_last_raw) return;
        char ebuf[256];
        std::snprintf(ebuf, sizeof(ebuf),
            "[PAD-EDGE] wgi_cnt=%u raw_btn=0x%X cell_d1=0x%X cell_d2=0x%X\r\n",
            g_wgi_last_count, raw, d1, d2);
        watchdog_append(ebuf);
        s_edge_last_raw = raw;
    };
    auto& pad = m_pads[0];
    if (!pad) return;
    // PAD-DIAG once per second (every 10th 100 ms poll), to
    // boot-watchdog.txt. cell_d1/d2 are computed from the EXTERNAL
    // button list exactly as cellPad reports them (pressed DIGITAL1 /
    // DIGITAL2 outKeyCode ORs), so raw_btn != 0 with cell_d1/d2 == 0
    // proves a translation gap, and wgi_cnt / pad_ptr show whether
    // enumeration itself sees the controller.
    auto diag = [&]()
    {
        if ((g_wgi_polls.load() % 10) != 1)
            return;
        u32 d1 = 0, d2 = 0;
        for (const auto& b : pad->m_buttons_external)
        {
            if (!b.m_pressed)
                continue;
            if (b.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1)
                d1 |= b.m_outKeyCode;
            else if (b.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2)
                d2 |= b.m_outKeyCode;
        }
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "PAD-DIAG mta=%ld wgi_cnt=%u pad_ptr=%p raw_btn=0x%X raw_lx=%.2f cell_stat=0x%X cell_d1=0x%X cell_d2=0x%X\r\n",
            g_wgi_com_hr, g_wgi_last_count, g_wgi_last_ptr,
            (unsigned)g_wgi_buttons.load(), g_wgi_last_lx,
            (unsigned)pad->m_port_status, d1, d2);
        watchdog_append(buf);
    };
    ComPtr<GI::IGamepad> gp = wgi_first_pad();
    // 1.0.43 (iii) wgi_cnt transitions (first poll sets the baseline silently).
    {
        static unsigned s_cnt_last = 0;
        static bool s_cnt_first = true;
        if (s_cnt_first) { s_cnt_last = g_wgi_last_count; s_cnt_first = false; }
        else if (g_wgi_last_count != s_cnt_last)
        {
            char cbuf[160];
            std::snprintf(cbuf, sizeof(cbuf),
                "[PAD-CNT] wgi_cnt_prev=%u wgi_cnt=%u pad_ptr=%p\r\n",
                s_cnt_last, g_wgi_last_count, g_wgi_last_ptr);
            watchdog_append(cbuf);
            s_cnt_last = g_wgi_last_count;
        }
    }
    if (!gp)
    {
        g_wgi_connected.store(0);
        g_wgi_buttons.store(0);
        pad->m_port_status = CELL_PAD_STATUS_DISCONNECTED;
        m_pads_connected[0] = false;
        m_info.now_connect = 0;
        diag();
        edge_check(0, 0, 0);
        return;
    }
    GI::GamepadReading r{};
    if (FAILED(gp->GetCurrentReading(&r)))
    {
        g_wgi_connected.store(0);
        diag();
        return;
    }
    g_wgi_connected.store(1);
    g_wgi_buttons.store(static_cast<std::uint64_t>(r.Buttons));
    g_wgi_last_lx = r.LeftThumbstickX;
    pad->m_port_status = CELL_PAD_STATUS_CONNECTED;
    m_pads_connected[0] = true;
    m_info.now_connect = 1;

    const unsigned btns = static_cast<unsigned>(r.Buttons);
    for (const auto& m : kWgiBtns)
    {
        const bool pressed = (btns & m.bit) != 0;
        // 1.0.37: offset comes from the table itself (no derivation).
        set_button(pad, m.offset, m.out, pressed, pressed ? 255 : 0);
        if (m.press_offset != 0)
            set_button(pad, m.press_offset, m.press_offset, pressed, pressed ? 255 : 0);
    }
    // Triggers (analog + pressed flag).
    const bool l2p = r.LeftTrigger > 0.05;
    const bool r2p = r.RightTrigger > 0.05;
    set_button(pad, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L2, l2p, trig_u8(r.LeftTrigger));
    set_button(pad, CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R2, r2p, trig_u8(r.RightTrigger));
    set_button(pad, CELL_PAD_BTN_OFFSET_PRESS_L2, CELL_PAD_BTN_OFFSET_PRESS_L2, l2p, trig_u8(r.LeftTrigger));
    set_button(pad, CELL_PAD_BTN_OFFSET_PRESS_R2, CELL_PAD_BTN_OFFSET_PRESS_R2, r2p, trig_u8(r.RightTrigger));
    // Sticks with 0.05 deadzone.
    const double dz = 0.05;
    double lx = std::abs(r.LeftThumbstickX) < dz ? 0.0 : r.LeftThumbstickX;
    double ly = std::abs(r.LeftThumbstickY) < dz ? 0.0 : r.LeftThumbstickY;
    double rx = std::abs(r.RightThumbstickX) < dz ? 0.0 : r.RightThumbstickX;
    double ry = std::abs(r.RightThumbstickY) < dz ? 0.0 : r.RightThumbstickY;
    set_stick(pad, CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X, axis_u8(lx));
    set_stick(pad, CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y, axis_y_u8(ly));
    set_stick(pad, CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X, axis_u8(rx));
    set_stick(pad, CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y, axis_y_u8(ry));
    {
        u32 d1 = 0, d2 = 0;
        for (const auto& b : pad->m_buttons_external)
        {
            if (!b.m_pressed)
                continue;
            if (b.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL1)
                d1 |= b.m_outKeyCode;
            else if (b.m_offset == CELL_PAD_BTN_OFFSET_DIGITAL2)
                d2 |= b.m_outKeyCode;
        }
        edge_check(static_cast<unsigned>(r.Buttons), d1, d2);
    }
    diag();
}

std::shared_ptr<PadHandlerBase> pad_thread::GetHandler(pad_handler)
{
    // Host pad_thread never constructs real handlers; cellPad paths in
    // this probe use GetPads() only. Returning null (not a handler) is
    // safe here because host Init() never calls GetHandler.
    return nullptr;
}

void pad_thread::InitPadConfig(cfg_pad&, pad_handler, std::shared_ptr<PadHandlerBase>& handler)
{
    handler = nullptr;
}
