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
#include <string>

namespace reassemble {

// Returns true only if out exists at exactly expected_total bytes after
// this call (either pre-existing or freshly assembled from all parts).
// out_bytes = final output size. fail = human-readable reason on false.
bool reassemble(const std::string& dir, const std::string& base, int parts,
                const std::string& out_name, uint64_t expected_total,
                uint64_t& out_bytes, std::string& fail);

} // namespace reassemble
