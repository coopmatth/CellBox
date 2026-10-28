#pragma once
// Step 7 shared audio + pad probes (same code on desktop pre-test, the PC
// 60 s harness in xbox-audio-pad, and the UWP console host). Audio: XAudio2
// first (XAudio2_9.dll loaded dynamically so a missing/blocked DLL cannot
// stop the host before it logs), WASAPI tried only if the XAudio2 path
// fails; both attempts are recorded, never silently substituted. The tone
// is a generated 440 Hz sine (16-bit PCM, 48 kHz) - no audio file exists.
// HRESULTs prove API acceptance only; audible output is user-reported.
// Pad: Windows.Gaming.Input Gamepad enumeration + polled readings via the
// WinRT ABI. No input is ever faked: with no controller, count=0 is the
// result and transitions stay "not yet measured".
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>

struct AudioProbeMetrics {
    bool xaudio_attempted = false;
    bool xaudio_ok = false;          // engine+mastering+source+submit+start all S_OK
    long xaudio_create_hr = 0;
    long mastering_hr = 0;
    long source_hr = 0;
    long submit_hr = 0;
    long start_hr = 0;
    unsigned mastering_channels = 0; // negotiated (GetVoiceDetails), not requested
    unsigned mastering_rate = 0;
    unsigned source_channels = 0;
    unsigned source_rate = 0;
    bool wasapi_attempted = false;
    bool wasapi_ok = false;
    long wasapi_last_hr = 0;
    unsigned wasapi_channels = 0;    // negotiated (GetMixFormat), not requested
    unsigned wasapi_rate = 0;
    unsigned wasapi_bits = 0;
    unsigned wasapi_buffer_frames = 0;
};

class AudioProbe {
public:
    // Full bring-up + looped tone start. Every step's HRESULT is in the log.
    std::string init_and_start();
    // One-line current voice/engine state (buffers queued, samples played,
    // callback counters) or WASAPI padding if the fallback is running.
    std::string state_line(const char* tag);
    void stop();
    const AudioProbeMetrics& metrics() const { return m_m; }
    bool running() const { return m_m.xaudio_ok || m_m.wasapi_ok; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    AudioProbeMetrics m_m;
    std::string wasapi_fallback();   // appended to the init log on XAudio2 failure
};

struct PadTransition {
    double t_ms = 0.0;               // probe-relative (QPC)
    std::string text;                // one human-readable line (no CRLF)
};

struct PadProbeMetrics {
    bool statics_ok = false;
    long statics_hr = 0;
    long enum_hr = 0;
    unsigned count = 0;              // last enumerated count, as the API reported it
    unsigned long long polls = 0;
    unsigned long long transitions = 0;
    unsigned count_changes = 0;      // re-enumeration count changes (plug/unplug observed by polling)
    long last_reading_hr = 0;
};

class PadProbe {
public:
    // Activation + first enumeration (identities as the API reports them).
    std::string init_and_enumerate();
    // One poll pass: re-reads every pad, diffs buttons/axes against the
    // previous reading, re-enumerates if `reenumerate` (count-change log).
    // New transition lines are appended to `out`.
    void poll_once(bool reenumerate, std::vector<PadTransition>& out);
    std::string summary(const char* tag) const;
    const PadProbeMetrics& metrics() const { return m_m; }

private:
    struct Impl;
    Impl* m_impl = nullptr;
    PadProbeMetrics m_m;
};
