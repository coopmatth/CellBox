// stage_diag.h — diagnostic stage instrumentation + IAT sweep (1.0.19.0).
// Native TU, no /ZW. stage_write uses only kernelbase file APIs so it
// works before CRT user globals initialise and before WinRT activation.
#pragma once
void stage_write(int stage, const char* tag);
void iat_sweep(void (*logfn)(const char*));
