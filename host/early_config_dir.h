// early_config_dir.h — see early_config_dir.cpp for the rationale.
// Recorded by the #pragma init_seg(compiler) constructor; boot_phase
// logs these so the console run discriminates redirect-win vs -loss.
#pragma once

namespace emu_host
{

extern wchar_t g_early_config_dir_value[4096];
extern long g_early_config_dir_set;
extern long g_early_config_dir_reason;

// Pure derivation, unit-tested on desktop: LOCALAPPDATA value ->
// LocalState value (strip trailing \AC, append LocalState\); false
// when the input does not end in \AC.
bool early_derive_localstate(const wchar_t* localappdata, wchar_t* out, size_t out_chars);

} // namespace emu_host
