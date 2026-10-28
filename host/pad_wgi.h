// pad_wgi.h — host-visible WGI pad polling counters (v1.0.29.0, Stage 3).
// Defined in host_pad.cpp; read by the main_uwp watch loop for
// BOOT-PAD-WGI log lines. No core file modified.
#pragma once

#include <cstdint>

// Total update_pad_states() polls performed by the pad thread.
std::uint64_t host_pad_wgi_polls();
// 1 while a WGI gamepad was present on the last poll, else 0.
std::uint64_t host_pad_wgi_connected();
// WGI button bitmask from the last successful reading (0 if none).
std::uint64_t host_pad_wgi_buttons();
