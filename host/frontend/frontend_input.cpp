// frontend_input.cpp - WGI menu pad (WRL pattern from host_pad.cpp:
// RoGetActivationFactory IGamepadStatics, fresh enumeration per poll,
// no cached Gamepad across ticks).
#include "frontend_input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <roapi.h>
#include <windows.gaming.input.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
namespace GI = ABI::Windows::Gaming::Input;
namespace WF = ABI::Windows::Foundation;

namespace frontend
{
namespace
{
// WGI GamepadButtons bits (windows.gaming.input.h; cross-checked
// against audio_pad_probe.cpp's kButtons table).
constexpr unsigned kBtnMenu = 0x1;
constexpr unsigned kBtnView = 0x2;
constexpr unsigned kBtnA = 0x4;
constexpr unsigned kBtnB = 0x8;
constexpr unsigned kBtnDUp = 0x40;
constexpr unsigned kBtnDDown = 0x80;
constexpr unsigned kBtnDLeft = 0x100;
constexpr unsigned kBtnDRight = 0x200;

constexpr std::uint64_t kRepeatDelayMs = 350;
constexpr std::uint64_t kRepeatRateMs = 120;
constexpr std::uint64_t kComboHoldMs = 800;
constexpr double kStickThreshold = 0.6;
} // namespace

struct MenuPad::Impl
{
    ComPtr<GI::IGamepadStatics> statics;
    long statics_hr = 0;
    unsigned retry_in = 0;
};

bool MenuPad::init(const LogFn& log)
{
    m_log = log;
    if (!m_impl) m_impl = new Impl();
    HSTRING cls = nullptr;
    static const wchar_t kCls[] = L"Windows.Gaming.Input.Gamepad";
    HRESULT hr = WindowsCreateString(kCls,
                                     static_cast<UINT32>(wcslen(kCls)), &cls);
    if (SUCCEEDED(hr))
    {
        GI::IGamepadStatics* raw_statics = nullptr;
        hr = RoGetActivationFactory(cls, __uuidof(GI::IGamepadStatics),
                                    reinterpret_cast<void**>(&raw_statics));
        if (SUCCEEDED(hr) && raw_statics) m_impl->statics.Attach(raw_statics);
        WindowsDeleteString(cls);
    }
    m_impl->statics_hr = static_cast<long>(hr);
    if (m_log)
    {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "FRONTEND-INPUT-INIT WGI IGamepadStatics: %s hr=0x%08lX (menu pad state separate from game pad state; retry in poll on failure)",
                 SUCCEEDED(hr) ? "PASS" : "FAIL", static_cast<unsigned long>(hr));
        m_log(buf);
    }
    return SUCCEEDED(hr);
}

MenuInput MenuPad::poll()
{
    ++m_polls;
    if (m_impl && !m_impl->statics)
    {
        if (m_impl->retry_in > 0) --m_impl->retry_in;
        else
        {
            m_impl->retry_in = 60; // ~1 Hz retry, not once-forever
            LogFn quiet;
            init(m_log ? m_log : quiet);
        }
    }
    if (!m_impl || !m_impl->statics)
        return update_from_reading(0, 0.0, 0.0, GetTickCount64(), false);

    ComPtr<WF::Collections::IVectorView<GI::Gamepad*>> pads;
    HRESULT hr = m_impl->statics->get_Gamepads(&pads);
    unsigned count = 0;
    if (SUCCEEDED(hr) && pads) pads->get_Size(&count);
    if (!pads || count == 0)
        return update_from_reading(0, 0.0, 0.0, GetTickCount64(), false);

    ComPtr<GI::IGamepad> gp;
    hr = pads->GetAt(0, &gp);
    if (FAILED(hr) || !gp)
        return update_from_reading(0, 0.0, 0.0, GetTickCount64(), false);
    GI::GamepadReading r{};
    hr = gp->GetCurrentReading(&r);
    if (FAILED(hr))
        return update_from_reading(0, 0.0, 0.0, GetTickCount64(), false);
    return update_from_reading(static_cast<unsigned>(r.Buttons),
                               r.LeftThumbstickX, r.LeftThumbstickY,
                               GetTickCount64(), true);
}

MenuInput MenuPad::update_from_reading(unsigned buttons, double lx, double ly,
                                       std::uint64_t now_ms, bool connected)
{
    MenuInput out;
    out.connected = connected;
    out.raw_buttons = connected ? buttons : 0;
    m_last_buttons = out.raw_buttons;
    m_last_lx = lx;
    m_last_ly = ly;
    m_last_connected = connected;
    if (!connected)
    {
        m_prev_buttons = 0;
        m_prev_dirs = 0;
        m_combo_since_ms = 0;
        m_combo_fired = false;
        return out;
    }
    const unsigned pressed = buttons & ~m_prev_buttons;
    out.accept = (pressed & kBtnA) != 0;
    out.back = (pressed & kBtnB) != 0;
    out.menu = (pressed & kBtnMenu) != 0;
    out.view = (pressed & kBtnView) != 0;

    // Directions: dpad + left stick merged (WGI Y: +1 = up).
    unsigned dirs = 0;
    if ((buttons & kBtnDUp) || ly > kStickThreshold) dirs |= 1;
    if ((buttons & kBtnDDown) || ly < -kStickThreshold) dirs |= 2;
    if ((buttons & kBtnDLeft) || lx < -kStickThreshold) dirs |= 4;
    if ((buttons & kBtnDRight) || lx > kStickThreshold) dirs |= 8;
    bool* dirOut[4] = {&out.up, &out.down, &out.left, &out.right};
    for (int i = 0; i < 4; ++i)
    {
        const unsigned bit = 1u << i;
        if (dirs & bit)
        {
            if (!(m_prev_dirs & bit))
            {
                *dirOut[i] = true;
                m_dir_since_ms[i] = now_ms;
                m_dir_next_ms[i] = now_ms + kRepeatDelayMs;
            }
            else if (now_ms >= m_dir_next_ms[i])
            {
                *dirOut[i] = true;
                m_dir_next_ms[i] = now_ms + kRepeatRateMs;
            }
        }
    }
    m_prev_dirs = dirs;
    m_prev_buttons = buttons;

    // Exit combo: View+Menu held together (in-game stop request).
    if ((buttons & (kBtnView | kBtnMenu)) == (kBtnView | kBtnMenu))
    {
        if (m_combo_since_ms == 0) m_combo_since_ms = now_ms ? now_ms : 1;
        if (!m_combo_fired && now_ms - m_combo_since_ms >= kComboHoldMs)
        {
            out.exit_combo = true;
            m_combo_fired = true;
        }
    }
    else
    {
        m_combo_since_ms = 0;
        m_combo_fired = false;
    }
    return out;
}

void MenuPad::sync_to_current()
{
    // Recompute baselines from the last seen reading: whatever is held
    // right now becomes "already down", so it cannot edge-fire after a
    // handoff. Direction repeat timers reset too.
    m_prev_buttons = m_last_connected ? m_last_buttons : 0;
    unsigned dirs = 0;
    if (m_last_connected)
    {
        if ((m_last_buttons & kBtnDUp) || m_last_ly > kStickThreshold) dirs |= 1;
        if ((m_last_buttons & kBtnDDown) || m_last_ly < -kStickThreshold) dirs |= 2;
        if ((m_last_buttons & kBtnDLeft) || m_last_lx < -kStickThreshold) dirs |= 4;
        if ((m_last_buttons & kBtnDRight) || m_last_lx > kStickThreshold) dirs |= 8;
    }
    m_prev_dirs = dirs;
    m_combo_since_ms = 0;
    m_combo_fired = false;
    if (m_log)
        m_log("FRONTEND-INPUT-SYNC menu pad baseline adopted buttons=0x" +
              std::to_string(m_prev_buttons) +
              " (held buttons will not fire until re-pressed)");
}

} // namespace frontend
