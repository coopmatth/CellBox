// CANDIDATE — UNVERIFIED, do not deploy as-is.
// Supplied by Matt 2026-10-26 ~21:46 EDT (outside analysis) as a pure-C++
// DXBC/DXIL retail container hash, intended to replace dxil.dll signing.
// Verification required before any use (see notes at end of file):
//  1. Hash bytes [20..end] of the SIGNED control PSO blob and assert the
//     result matches bytes [4..19] of that control blob byte-for-byte.
//  2. Only if (1) passes, hash the unsigned Mesa DXIL blob in-memory and
//     run the PC desktop pre-test CreateGraphicsPipelineState.
//  3. Only if (2) passes on PC, test on Series S (expect GREEN if the
//     hash alone is sufficient; if it still fails, Xbox enforces more
//     than the container hash, e.g. validator version / PSV0 / STAT).
// Caveats recorded by Jarvis 2026-10-26:
//  - The pasted description is internally inconsistent on padding
//    (bit-length in M[0] vs M[1] in different paragraphs; the code uses
//    M[0]=num_bits and M[15]=(msg_len<<1)|1, with an unused
//    num_dword_len variable). Do not trust the code until (1) passes.
//  - dxil.dll Error 126 in the Step 3 probe means "module not found"
//    (file absent from the Xbox OS image / package), not proven to be
//    a dependency-rejection of desktop KERNEL32/ADVAPI32 imports.
//  - Mesa blobs lack the STAT and HASH chunks the signed control blobs
//    carry; hashing alone may not be sufficient on Xbox even if it
//    passes on PC. The console probe is the deciding test.
//  - Storage notes from the same analysis (broadFileSystemAccess
//    ignored on Xbox; removableStorage USB path; LocalState upload
//    via Device Portal) match the Step 3 probe (DENIED 0x80070005,
//    RemovableDevices count=0) and are queued for Step 5/6, not
//    implemented here. No firmware/game file is supplied by this file.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
inline bool sign_dxil_container(void* data, size_t size)
{
    if (!data || size < 32)
        return false;
    auto* bytes = static_cast<uint8_t*>(data);
    if (bytes[0] != 'D' || bytes[1] != 'X' || bytes[2] != 'B' || bytes[3] != 'C')
        return false;
    const uint8_t* msg = bytes + 20;
    const uint32_t msg_len = static_cast<uint32_t>(size - 20);
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static const uint32_t r[64] = {
        7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
        5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
        4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
        6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
    };
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xefcdab89;
    uint32_t h2 = 0x98badcfe;
    uint32_t h3 = 0x10325476;
    auto process_block = [&](const uint32_t M[16]) {
        uint32_t a = h0, b = h1, c = h2, d = h3;
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t f, g;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) & 15;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) & 15;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) & 15;
            }
            uint32_t temp = d;
            d = c;
            c = b;
            uint32_t x = a + f + k[i] + M[g];
            b = b + ((x << r[i]) | (x >> (32 - r[i])));
            a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    };
    uint32_t offset = 0;
    while (msg_len - offset >= 64) {
        uint32_t M[16];
        std::memcpy(M, msg + offset, 64);
        process_block(M);
        offset += 64;
    }
    uint32_t rem = msg_len - offset;
    uint32_t num_bits = msg_len * 8;
    if (rem >= 56) {
        uint8_t block1[64] = {};
        std::memcpy(block1, msg + offset, rem);
        block1[rem] = 0x80;
        uint32_t M1[16];
        std::memcpy(M1, block1, 64);
        process_block(M1);
        uint32_t M2[16] = {};
        M2[0]  = num_bits;
        M2[15] = (msg_len << 1) | 1;
        process_block(M2);
    } else {
        uint8_t tail[64] = {};
        std::memcpy(tail, &num_bits, 4);
        if (rem > 0)
            std::memcpy(tail + 4, msg + offset, rem);
        tail[4 + rem] = 0x80;
        uint32_t final_word = (msg_len << 1) | 1;
        std::memcpy(tail + 60, &final_word, 4);
        uint32_t M[16];
        std::memcpy(M, tail, 64);
        process_block(M);
    }
    std::memcpy(bytes + 4,  &h0, 4);
    std::memcpy(bytes + 8,  &h1, 4);
    std::memcpy(bytes + 12, &h2, 4);
    std::memcpy(bytes + 16, &h3, 4);
    return true;
}
