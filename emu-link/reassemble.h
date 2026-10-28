// reassemble — pure C++ chunk reassembly for the Portal ISO fallback.
// Why this exists: the Xbox Device Portal file upload fails partway on a
// single 4,599,644,160-byte ISO (measured: HTTP 500 after 3,678,715,904
// bytes, 2026-10-27). Firmware (206 MB) uploads and verifies byte-identical
// by the same route, so the route is sound below ~3.6 GB — the ISO is
// therefore shipped as 4 x 1,149,911,040-byte parts and reassembled in
// LocalState by the host on launch. Parts are NOT deleted by this function:
// if the size check fails, a later launch can retry from the parts.
// If the output already exists at exactly the expected size, reassembly
// is skipped (idempotent across launches).
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace reassemble {

// Returns true only if out exists at exactly expected_total bytes after
// this call (either pre-existing or freshly assembled from all parts).
// out_bytes = final output size. fail = human-readable reason on false.
bool reassemble(const std::string& dir, const std::string& base, int parts,
                const std::string& out_name, uint64_t expected_total,
                uint64_t& out_bytes, std::string& fail);

// 1.1.5: streamed SHA-256 of a file (self-contained, no bcrypt). hex is
// upper-case, 64 chars. Returns false if the file cannot be read.
bool sha256_file(const std::string& path, std::string& hex_upper, uint64_t& bytes);

struct PartsDeleteResult {
    bool attempted = false;   // ISO was hashed (>=1 part present)
    bool verified = false;    // ISO size AND sha256 matched
    int parts_present = 0;
    int deleted = 0;
    int failed = 0;
    uint64_t bytes_deleted = 0;
    std::string detail;
};

// Delete exactly <base>.part0..<parts-1> in dir, but ONLY if <out_name> in
// dir exists at expected_total bytes AND its SHA-256 equals
// expected_sha256_hex (case-insensitive). Anything else keeps every part.
// Skips (no hashing) when no part file exists. log receives one line per
// step (REASSEMBLE-PARTS ...).
PartsDeleteResult verify_and_delete_parts(const std::string& dir, const std::string& base, int parts,
                                          const std::string& out_name, uint64_t expected_total,
                                          const std::string& expected_sha256_hex,
                                          const std::function<void(const std::string&)>& log);

} // namespace reassemble
