#include "audio_pad_probe.h"
#include <xaudio2.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <roapi.h>
#include <windows.gaming.input.h>
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>

using Microsoft::WRL::ComPtr;
namespace GI = ABI::Windows::Gaming::Input;
namespace WF = ABI::Windows::Foundation;

static void AppendLine(std::string& s, const char* fmt, ...) {
    char b[2048]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    s += b; s += "\r\n";
}
static std::string Narrow(HSTRING hs) {
    if (!hs) return "<null>";
    UINT32 len = 0; const wchar_t* p = WindowsGetStringRawBuffer(hs, &len);
    if (!p) return "<empty>";
    std::string out;
    int n = WideCharToMultiByte(CP_UTF8, 0, p, (int)len, nullptr, 0, nullptr, nullptr);
    if (n > 0) { out.resize((size_t)n); WideCharToMultiByte(CP_UTF8, 0, p, (int)len, out.data(), n, nullptr, nullptr); }
    return out;
}
// Generated tone only: 440 Hz sine, 16-bit PCM mono 48 kHz, 2 s, amp 8000.
static const unsigned kToneRate = 48000;
static const unsigned kToneSeconds = 2;
static std::vector<BYTE> MakeTonePcm16() {
    std::vector<BYTE> b((size_t)kToneRate * kToneSeconds * sizeof(short));
    short* s = reinterpret_cast<short*>(b.data());
    for (unsigned i = 0; i < kToneRate * kToneSeconds; ++i) {
        double t = (double)i / kToneRate;
        s[i] = (short)(8000.0 * sin(6.283185307179586 * 440.0 * t));
    }
    return b;
}
static HRESULT ComInitMta() { return CoInitializeEx(nullptr, COINIT_MULTITHREADED); }

// -------------------------- AudioProbe --------------------------
struct VoiceCb : IXAudio2VoiceCallback {
    std::atomic<unsigned> bufStart{0}, bufEnd{0}, streamEnd{0};
    std::atomic<long> voiceErr{0};
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override { streamEnd++; }
    void STDMETHODCALLTYPE OnBufferStart(void*) override { bufStart++; }
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { bufEnd++; }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT hr) override { voiceErr = hr; }
};
struct EngineCb : IXAudio2EngineCallback {
    std::atomic<long> critErr{0};
    void STDMETHODCALLTYPE OnProcessingPassStart() override {}
    void STDMETHODCALLTYPE OnProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnCriticalError(HRESULT hr) override { critErr = hr; }
};

struct AudioProbe::Impl {
    HMODULE dll = nullptr;
    ComPtr<IXAudio2> xa;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    VoiceCb vcb; EngineCb ecb;
    std::vector<BYTE> tone;
    ComPtr<IMMDeviceEnumerator> mmEnum;
    ComPtr<IMMDevice> mmDev;
    ComPtr<IAudioClient> aclient;
    ComPtr<IAudioRenderClient> render;
    bool comInited = false;
};

typedef HRESULT(__stdcall* XAudio2CreateFn)(IXAudio2** pp, UINT32 flags, XAUDIO2_PROCESSOR proc);

std::string AudioProbe::init_and_start() {
    std::string log;
    m_impl = new Impl();
    Impl& im = *m_impl;
    AppendLine(log, "--- Step 7 AUDIO probe: XAudio2 first, WASAPI only as recorded fallback ---");
    AppendLine(log, "Tone: GENERATED 440Hz sine, PCM16 mono %uHz, %us, LoopCount=XAUDIO2_LOOP_INFINITE (no audio file)", kToneRate, kToneSeconds);
    HRESULT chr = ComInitMta();
    im.comInited = SUCCEEDED(chr);
    AppendLine(log, "CoInitializeEx(MTA) hr=0x%08lX", (unsigned long)chr);
    m_m.xaudio_attempted = true;
    // Dynamic load so a missing/blocked XAudio2_9.dll is a logged result,
    // not a loader failure that kills the host before it can report.
    SetLastError(0);
    im.dll = LoadLibraryExW(L"XAudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    DWORD le = GetLastError();
    AppendLine(log, "LoadLibraryExW(XAudio2_9.dll, SEARCH_SYSTEM32): handle=%p GetLastError=%lu", (void*)im.dll, le);
    if (!im.dll) {
        SetLastError(0);
        im.dll = LoadPackagedLibrary(L"XAudio2_9.dll", 0);
        le = GetLastError();
        AppendLine(log, "LoadPackagedLibrary(XAudio2_9.dll): handle=%p GetLastError=%lu", (void*)im.dll, le);
    }
    XAudio2CreateFn createFn = nullptr;
    if (im.dll) createFn = (XAudio2CreateFn)(void*)GetProcAddress(im.dll, "XAudio2Create");
    AppendLine(log, "GetProcAddress(XAudio2Create)=%p", (void*)createFn);
    HRESULT hr = createFn ? createFn(&im.xa, 0, XAUDIO2_USE_DEFAULT_PROCESSOR) : E_FAIL;
    m_m.xaudio_create_hr = hr;
    AppendLine(log, "XAudio2Create: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        hr = im.xa->RegisterForCallbacks(&im.ecb);
        AppendLine(log, "IXAudio2::RegisterForCallbacks hr=0x%08lX", (unsigned long)hr);
        hr = im.xa->CreateMasteringVoice(&im.master); // defaults = system-chosen format
        m_m.mastering_hr = hr;
        AppendLine(log, "CreateMasteringVoice(defaults): %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    }
    if (SUCCEEDED(hr) && im.master) {
        XAUDIO2_VOICE_DETAILS d{};
        im.master->GetVoiceDetails(&d);
        m_m.mastering_channels = d.InputChannels; m_m.mastering_rate = d.InputSampleRate;
        AppendLine(log, "MasteringVoice negotiated (GetVoiceDetails): channels=%u rate=%u creationFlags=0x%X", d.InputChannels, d.InputSampleRate, d.CreationFlags);
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM; wfx.nChannels = 1; wfx.nSamplesPerSec = kToneRate;
        wfx.wBitsPerSample = 16; wfx.nBlockAlign = 2; wfx.nAvgBytesPerSec = kToneRate * 2;
        AppendLine(log, "SourceVoice requested: PCM channels=1 rate=%u bits=16", kToneRate);
        hr = im.xa->CreateSourceVoice(&im.source, &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &im.vcb);
        m_m.source_hr = hr;
        AppendLine(log, "CreateSourceVoice: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    }
    if (SUCCEEDED(hr) && im.source) {
        XAUDIO2_VOICE_DETAILS d{};
        im.source->GetVoiceDetails(&d);
        m_m.source_channels = d.InputChannels; m_m.source_rate = d.InputSampleRate;
        AppendLine(log, "SourceVoice negotiated (GetVoiceDetails): channels=%u rate=%u", d.InputChannels, d.InputSampleRate);
        im.tone = MakeTonePcm16();
        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = (UINT32)im.tone.size(); buf.pAudioData = im.tone.data();
        buf.LoopCount = XAUDIO2_LOOP_INFINITE;
        hr = im.source->SubmitSourceBuffer(&buf);
        m_m.submit_hr = hr;
        AppendLine(log, "SubmitSourceBuffer(%u bytes, loop-infinite): %s hr=0x%08lX", (unsigned)im.tone.size(), SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    }
    if (SUCCEEDED(hr) && im.source) {
        hr = im.source->Start();
        m_m.start_hr = hr;
        AppendLine(log, "SourceVoice.Start: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
        m_m.xaudio_ok = SUCCEEDED(hr);
        XAUDIO2_VOICE_STATE st{}; im.source->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        im.source->GetState(&st, 0);
        AppendLine(log, "SourceVoice initial GetState: buffersQueued=%u samplesPlayed=%llu callbacks(bufStart=%u bufEnd=%u voiceErr=0x%08lX engineCrit=0x%08lX)",
            st.BuffersQueued, st.SamplesPlayed, im.vcb.bufStart.load(), im.vcb.bufEnd.load(),
            (unsigned long)im.vcb.voiceErr.load(), (unsigned long)im.ecb.critErr.load());
    }
    if (!m_m.xaudio_ok) {
        AppendLine(log, "AUDIO-XAUDIO2: FAIL create-hr=0x%08lX mastering-hr=0x%08lX source-hr=0x%08lX submit-hr=0x%08lX start-hr=0x%08lX -> trying WASAPI fallback (recorded, not silent)",
            (unsigned long)m_m.xaudio_create_hr, (unsigned long)m_m.mastering_hr, (unsigned long)m_m.source_hr,
            (unsigned long)m_m.submit_hr, (unsigned long)m_m.start_hr);
        log += wasapi_fallback();
    } else {
        AppendLine(log, "AUDIO-XAUDIO2: PASS (engine+mastering+source+submit+start S_OK; audible output NOT proven by HRESULTs - user report required)");
    }
    return log;
}

std::string AudioProbe::wasapi_fallback() {
    std::string log;
    Impl& im = *m_impl;
    m_m.wasapi_attempted = true;
    AppendLine(log, "--- WASAPI fallback attempt (recorded separately from XAudio2) ---");
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&im.mmEnum));
    AppendLine(log, "CoCreateInstance(MMDeviceEnumerator) hr=0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        hr = im.mmEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &im.mmDev);
        AppendLine(log, "GetDefaultAudioEndpoint(eRender, eMultimedia) hr=0x%08lX", (unsigned long)hr);
    }
    WAVEFORMATEX* mix = nullptr;
    if (SUCCEEDED(hr)) {
        hr = im.mmDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&im.aclient);
        AppendLine(log, "IMMDevice::Activate(IAudioClient) hr=0x%08lX", (unsigned long)hr);
    }
    if (SUCCEEDED(hr)) {
        hr = im.aclient->GetMixFormat(&mix);
        AppendLine(log, "IAudioClient::GetMixFormat hr=0x%08lX", (unsigned long)hr);
        if (SUCCEEDED(hr) && mix) {
            m_m.wasapi_channels = mix->nChannels; m_m.wasapi_rate = mix->nSamplesPerSec; m_m.wasapi_bits = mix->wBitsPerSample;
            AppendLine(log, "WASAPI negotiated mix format: tag=0x%X channels=%u rate=%u bits=%u blockAlign=%u",
                mix->wFormatTag, mix->nChannels, mix->nSamplesPerSec, mix->wBitsPerSample, mix->nBlockAlign);
        }
    }
    UINT32 bufFrames = 0;
    if (SUCCEEDED(hr) && mix) {
        const REFERENCE_TIME dur = 10000000; // 1 s request; actual in GetBufferSize
        hr = im.aclient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, dur, 0, mix, nullptr);
        AppendLine(log, "IAudioClient::Initialize(SHARED, 1s) hr=0x%08lX", (unsigned long)hr);
        if (SUCCEEDED(hr)) {
            hr = im.aclient->GetBufferSize(&bufFrames);
            m_m.wasapi_buffer_frames = bufFrames;
            AppendLine(log, "IAudioClient::GetBufferSize hr=0x%08lX bufferFrames=%u", (unsigned long)hr, bufFrames);
            if (SUCCEEDED(hr)) {
                hr = im.aclient->GetService(IID_PPV_ARGS(&im.render));
                AppendLine(log, "GetService(IAudioRenderClient) hr=0x%08lX", (unsigned long)hr);
            }
        }
    }
    if (SUCCEEDED(hr) && im.render && mix && bufFrames) {
        BYTE* data = nullptr;
        hr = im.render->GetBuffer(bufFrames, &data);
        AppendLine(log, "IAudioRenderClient::GetBuffer(%u) hr=0x%08lX", bufFrames, (unsigned long)hr);
        if (SUCCEEDED(hr) && data) {
            const bool isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->wBitsPerSample == 32);
            const bool isPcm16 = (mix->wFormatTag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 16) ||
                (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->wBitsPerSample == 16);
            if (!isFloat && !isPcm16) {
                AppendLine(log, "WASAPI tone fill: UNSUPPORTED mix sample type (tag=0x%X bits=%u) - buffer left silent, recorded not faked", mix->wFormatTag, mix->wBitsPerSample);
            }
            for (UINT32 i = 0; i < bufFrames; ++i) {
                double v = sin(6.283185307179586 * 440.0 * (double)i / mix->nSamplesPerSec);
                for (unsigned ch = 0; ch < mix->nChannels; ++ch) {
                    if (isFloat) ((float*)data)[(size_t)i * mix->nChannels + ch] = (float)(0.25 * v);
                    else if (isPcm16) ((short*)data)[(size_t)i * mix->nChannels + ch] = (short)(8000.0 * v);
                    else ((BYTE*)data)[((size_t)i * mix->nChannels + ch) * (mix->wBitsPerSample / 8)] = 0;
                }
            }
            hr = im.render->ReleaseBuffer(bufFrames, 0);
            AppendLine(log, "IAudioRenderClient::ReleaseBuffer hr=0x%08lX", (unsigned long)hr);
            if (SUCCEEDED(hr)) {
                hr = im.aclient->Start();
                AppendLine(log, "IAudioClient::Start hr=0x%08lX (one-shot buffer; no event loop in fallback probe)", (unsigned long)hr);
            }
        }
    }
    m_m.wasapi_last_hr = hr;
    m_m.wasapi_ok = SUCCEEDED(hr) && im.aclient != nullptr;
    AppendLine(log, "AUDIO-WASAPI-FALLBACK: %s last-hr=0x%08lX (audible output NOT proven by HRESULTs - user report required)",
        m_m.wasapi_ok ? "PASS" : "FAIL", (unsigned long)hr);
    if (mix) CoTaskMemFree(mix);
    return log;
}

std::string AudioProbe::state_line(const char* tag) {
    char b[1024];
    if (!m_impl) { snprintf(b, sizeof(b), "%s audio=NOT-INITIALISED", tag); return b; }
    Impl& im = *m_impl;
    if (im.source) {
        XAUDIO2_VOICE_STATE st{}; im.source->GetState(&st, 0);
        snprintf(b, sizeof(b), "%s xaudio2 buffersQueued=%u samplesPlayed=%llu callbacks(bufStart=%u bufEnd=%u streamEnd=%u voiceErr=0x%08lX engineCrit=0x%08lX) mastering=%uch/%uHz",
            tag, st.BuffersQueued, st.SamplesPlayed, im.vcb.bufStart.load(), im.vcb.bufEnd.load(), im.vcb.streamEnd.load(),
            (unsigned long)im.vcb.voiceErr.load(), (unsigned long)im.ecb.critErr.load(), m_m.mastering_channels, m_m.mastering_rate);
    } else if (im.aclient) {
        UINT32 pad = 0; HRESULT hr = im.aclient->GetCurrentPadding(&pad);
        snprintf(b, sizeof(b), "%s wasapi padding=%u bufferFrames=%u padding-hr=0x%08lX mix=%uch/%uHz/%ubit", tag, pad, m_m.wasapi_buffer_frames,
            (unsigned long)hr, m_m.wasapi_channels, m_m.wasapi_rate, m_m.wasapi_bits);
    } else {
        snprintf(b, sizeof(b), "%s audio=FAILED xaudio-create=0x%08lX wasapi-attempted=%d wasapi-last=0x%08lX",
            tag, (unsigned long)m_m.xaudio_create_hr, m_m.wasapi_attempted ? 1 : 0, (unsigned long)m_m.wasapi_last_hr);
    }
    return b;
}

void AudioProbe::stop() {
    if (!m_impl) return;
    Impl& im = *m_impl;
    if (im.source) { im.source->Stop(); im.source->DestroyVoice(); im.source = nullptr; }
    if (im.master) { im.master->DestroyVoice(); im.master = nullptr; }
    if (im.aclient) im.aclient->Stop();
    im.render.Reset(); im.aclient.Reset(); im.mmDev.Reset(); im.mmEnum.Reset(); im.xa.Reset();
    if (im.dll) { FreeLibrary(im.dll); im.dll = nullptr; }
    if (im.comInited) CoUninitialize();
    delete m_impl; m_impl = nullptr;
}

// -------------------------- PadProbe --------------------------
struct PadState {
    ComPtr<GI::IGamepad> pad;
    bool havePrev = false;
    GI::GamepadReading prev{};
};
struct PadProbe::Impl {
    ComPtr<GI::IGamepadStatics> statics;
    std::vector<PadState> pads;
    long long qpf = 0, startQpc = 0;
    double now_ms() const {
        LARGE_INTEGER q; QueryPerformanceCounter(&q);
        return qpf ? 1000.0 * (double)(q.QuadPart - startQpc) / (double)qpf : 0.0;
    }
    bool comInited = false;
};

static const struct { unsigned bit; const char* name; } kButtons[] = {
    {0x1, "Menu"}, {0x2, "View"}, {0x4, "A"}, {0x8, "B"}, {0x10, "X"}, {0x20, "Y"},
    {0x40, "DPadUp"}, {0x80, "DPadDown"}, {0x100, "DPadLeft"}, {0x200, "DPadRight"},
    {0x400, "LeftShoulder"}, {0x800, "RightShoulder"}, {0x1000, "LeftThumbstick"},
    {0x2000, "RightThumbstick"}, {0x4000, "Paddle1"}, {0x8000, "Paddle2"},
    {0x10000, "Paddle3"}, {0x20000, "Paddle4"},
};

static std::string PadIdentity(unsigned idx, GI::IGamepad* pad) {
    std::string s;
    AppendLine(s, "  pad[%u] IGamepad=%p", idx, (void*)pad);
    ComPtr<IInspectable> insp;
    if (pad) pad->QueryInterface(IID_PPV_ARGS(&insp));
    if (insp) {
        HSTRING hs = nullptr;
        HRESULT hr = insp->GetRuntimeClassName(&hs);
        AppendLine(s, "  pad[%u] runtimeClass hr=0x%08lX value=%s", idx, (unsigned long)hr, Narrow(hs).c_str());
        if (hs) WindowsDeleteString(hs);
    }
    ComPtr<GI::IGameController> ctrl;
    HRESULT qhr = pad ? pad->QueryInterface(IID_PPV_ARGS(&ctrl)) : E_FAIL;
    AppendLine(s, "  pad[%u] QI(IGameController) hr=0x%08lX", idx, (unsigned long)qhr);
    if (ctrl) {
        boolean w = 0; HRESULT hr = ctrl->get_IsWireless(&w);
        AppendLine(s, "  pad[%u] IsWireless hr=0x%08lX value=%d", idx, (unsigned long)hr, (int)w);
        ComPtr<GI::IHeadset> hs2; hr = ctrl->get_Headset(&hs2);
        AppendLine(s, "  pad[%u] Headset hr=0x%08lX present=%d", idx, (unsigned long)hr, hs2 ? 1 : 0);
    }
    ComPtr<GI::IRawGameController> raw;
    qhr = pad ? pad->QueryInterface(IID_PPV_ARGS(&raw)) : E_FAIL;
    AppendLine(s, "  pad[%u] QI(IRawGameController) hr=0x%08lX", idx, (unsigned long)qhr);
    if (raw) {
        HSTRING dn = nullptr, id = nullptr;
        UINT16 vid = 0, pid = 0;
        // DisplayName/NonRoamableId live on IRawGameController2 (contract 5).
        ComPtr<GI::IRawGameController2> raw2;
        HRESULT q2 = raw->QueryInterface(IID_PPV_ARGS(&raw2));
        AppendLine(s, "  pad[%u] QI(IRawGameController2) hr=0x%08lX", idx, (unsigned long)q2);
        HRESULT h1 = E_NOINTERFACE, h2 = E_NOINTERFACE;
        if (raw2) { h1 = raw2->get_DisplayName(&dn); h2 = raw2->get_NonRoamableId(&id); }
        HRESULT h3 = raw->get_HardwareVendorId(&vid); HRESULT h4 = raw->get_HardwareProductId(&pid);
        AppendLine(s, "  pad[%u] DisplayName hr=0x%08lX value=%s", idx, (unsigned long)h1, Narrow(dn).c_str());
        AppendLine(s, "  pad[%u] NonRoamableId hr=0x%08lX value=%s", idx, (unsigned long)h2, Narrow(id).c_str());
        AppendLine(s, "  pad[%u] HardwareVendorId hr=0x%08lX value=0x%04X HardwareProductId hr=0x%08lX value=0x%04X",
            idx, (unsigned long)h3, vid, (unsigned long)h4, pid);
        if (dn) WindowsDeleteString(dn);
        if (id) WindowsDeleteString(id);
    }
    return s;
}

std::string PadProbe::init_and_enumerate() {
    std::string log;
    m_impl = new Impl();
    Impl& im = *m_impl;
    LARGE_INTEGER f, q; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q);
    im.qpf = f.QuadPart; im.startQpc = q.QuadPart;
    AppendLine(log, "--- Step 7 PAD probe: Windows.Gaming.Input Gamepad (no input faked) ---");
    HRESULT chr = ComInitMta();
    im.comInited = SUCCEEDED(chr);
    AppendLine(log, "CoInitializeEx(MTA) hr=0x%08lX", (unsigned long)chr);
    HRESULT rhr = RoInitialize(RO_INIT_MULTITHREADED);
    AppendLine(log, "RoInitialize(MULTITHREADED) hr=0x%08lX", (unsigned long)rhr);
    HSTRING cls = nullptr;
    HRESULT hr = WindowsCreateString(L"Windows.Gaming.Input.Gamepad", 28, &cls);
    AppendLine(log, "WindowsCreateString(Gamepad) hr=0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        hr = RoGetActivationFactory(cls, __uuidof(GI::IGamepadStatics), (void**)&im.statics);
        WindowsDeleteString(cls);
    }
    m_m.statics_hr = hr; m_m.statics_ok = SUCCEEDED(hr) && im.statics != nullptr;
    AppendLine(log, "RoGetActivationFactory(IGamepadStatics): %s hr=0x%08lX", m_m.statics_ok ? "PASS" : "FAIL", (unsigned long)hr);
    if (!m_m.statics_ok) {
        AppendLine(log, "PAD-ENUM: BLOCKED (no IGamepadStatics - enumeration not possible, count NOT claimed)");
        return log;
    }
    // Reuse poll machinery for enumeration so identities/count share one path.
    std::vector<PadTransition> ignored;
    poll_once(true, ignored);
    AppendLine(log, "PAD-ENUM: count=%u enum-hr=0x%08lX (count as API reported; if 0, transitions are NOT yet measured - no input faked)",
        m_m.count, (unsigned long)m_m.enum_hr);
    for (unsigned i = 0; i < (unsigned)im.pads.size(); ++i) log += PadIdentity(i, im.pads[i].pad.Get());
    AppendLine(log, "Disconnect/reconnect observation method: re-enumeration diffing during poll loop (GamepadAdded/Removed events exist in API but event path is UNPROVEN by this probe)");
    return log;
}

void PadProbe::poll_once(bool reenumerate, std::vector<PadTransition>& out) {
    if (!m_impl || !m_m.statics_ok) return;
    Impl& im = *m_impl;
    if (reenumerate) {
        ComPtr<WF::Collections::IVectorView<GI::Gamepad*>> view;
        HRESULT hr = im.statics->get_Gamepads(&view);
        m_m.enum_hr = hr;
        unsigned n = 0;
        if (SUCCEEDED(hr) && view) view->get_Size(&n);
        if (n != m_m.count || im.pads.size() != n) {
            if (m_m.polls > 0 || !im.pads.empty() || n != 0) {
                PadTransition t; t.t_ms = im.now_ms();
                char b[256]; snprintf(b, sizeof(b), "PAD-COUNT-CHANGE t=%.1fms count %u -> %u enum-hr=0x%08lX", t.t_ms, m_m.count, n, (unsigned long)hr);
                t.text = b; out.push_back(t); m_m.count_changes++;
            }
            m_m.count = n;
            im.pads.assign(n, PadState());
            for (unsigned i = 0; i < n; ++i) {
                GI::IGamepad* gp = nullptr;
                if (view && SUCCEEDED(view->GetAt(i, &gp)) && gp) { im.pads[i].pad.Attach(gp); }
            }
        }
    }
    m_m.polls++;
    for (unsigned i = 0; i < (unsigned)im.pads.size(); ++i) {
        PadState& ps = im.pads[i];
        if (!ps.pad) continue;
        GI::GamepadReading r{};
        HRESULT hr = ps.pad->GetCurrentReading(&r);
        m_m.last_reading_hr = hr;
        if (FAILED(hr)) continue;
        if (!ps.havePrev) { ps.prev = r; ps.havePrev = true; continue; }
        const double t = im.now_ms();
        const unsigned oldB = (unsigned)ps.prev.Buttons, newB = (unsigned)r.Buttons;
        if (oldB != newB) {
            for (auto& bt : kButtons) {
                if ((oldB & bt.bit) == (newB & bt.bit)) continue;
                PadTransition tr; tr.t_ms = t;
                char b[512];
                snprintf(b, sizeof(b), "PAD-TRANSITION t=%.1fms pad=%u button=%s %s mask=0x%X readingTimestamp=%llu",
                    t, i, bt.name, (newB & bt.bit) ? "DOWN" : "UP", newB, r.Timestamp);
                tr.text = b; out.push_back(tr); m_m.transitions++;
            }
        }
        auto axis = [&](const char* nm, double o, double v) {
            if (fabs(v - o) <= 0.05) return;
            PadTransition tr; tr.t_ms = t;
            char b[512];
            snprintf(b, sizeof(b), "PAD-TRANSITION t=%.1fms pad=%u axis=%s value=%.3f readingTimestamp=%llu", t, i, nm, v, r.Timestamp);
            tr.text = b; out.push_back(tr); m_m.transitions++;
        };
        axis("LeftTrigger", ps.prev.LeftTrigger, r.LeftTrigger);
        axis("RightTrigger", ps.prev.RightTrigger, r.RightTrigger);
        axis("LeftThumbstickX", ps.prev.LeftThumbstickX, r.LeftThumbstickX);
        axis("LeftThumbstickY", ps.prev.LeftThumbstickY, r.LeftThumbstickY);
        axis("RightThumbstickX", ps.prev.RightThumbstickX, r.RightThumbstickX);
        axis("RightThumbstickY", ps.prev.RightThumbstickY, r.RightThumbstickY);
        ps.prev = r;
    }
}

std::string PadProbe::summary(const char* tag) const {
    char b[640];
    snprintf(b, sizeof(b), "%s statics=%s statics-hr=0x%08lX count=%u polls=%llu transitions=%llu count-changes=%u last-reading-hr=0x%08lX enum-hr=0x%08lX",
        tag, m_m.statics_ok ? "PASS" : "FAIL", (unsigned long)m_m.statics_hr, m_m.count, m_m.polls,
        m_m.transitions, m_m.count_changes, (unsigned long)m_m.last_reading_hr, (unsigned long)m_m.enum_hr);
    return b;
}

