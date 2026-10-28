#pragma once
#include <string>
#include <vector>
// Shared PSO probe: given a D3D12 device and VS/PS bytecode, create empty root
// signature (ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) + graphics PSO with input
// layout TEXCOORD0@0 / TEXCOORD1@16 R32G32B32A32_FLOAT (from DXIL sigs).
// Returns log text; sets *pso_hr to CreateGraphicsPipelineState HRESULT.
struct ID3D12Device;
std::string run_pso_probe(ID3D12Device* device,
    const std::vector<unsigned char>& vs, const std::vector<unsigned char>& ps,
    const char* label, long* pso_hr);
std::vector<unsigned char> load_file_bytes(const std::wstring& path, long* err);
std::string dxil_hash_summary(const std::vector<unsigned char>& dxbc);
