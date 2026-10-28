// reassemble.cpp — pure C++, no WinRT, no core headers.
#include "reassemble.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace reassemble {

static std::string join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '\\' || last == '/') return dir + name;
    return dir + "\\" + name;
}

static bool file_size(const std::string& path, uint64_t& size) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    size = (uint64_t)f.tellg();
    return true;
}

bool reassemble(const std::string& dir, const std::string& base, int parts,
                const std::string& out_name, uint64_t expected_total,
                uint64_t& out_bytes, std::string& fail) {
    out_bytes = 0;
    std::string out_path = join(dir, out_name);
    uint64_t existing = 0;
    if (file_size(out_path, existing)) {
        out_bytes = existing;
        if (existing == expected_total) return true; // idempotent
        fail = "existing output size mismatch, will rebuild";
    }
    // 1.1.5: prove every part exists BEFORE creating/truncating the output,
    // so a missing-parts launch never leaves a 0-byte ISO behind.
    for (int i = 0; i < parts; ++i) {
        uint64_t psz = 0;
        if (!file_size(join(dir, base + ".part" + std::to_string(i)), psz)) {
            fail = "missing part " + std::to_string(i);
            return false;
        }
    }
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) { fail = "cannot open output"; return false; }
    std::vector<char> buf(1 << 20);
    uint64_t total = 0;
    for (int i = 0; i < parts; ++i) {
        std::string part_path = join(dir, base + ".part" + std::to_string(i));
        std::ifstream in(part_path, std::ios::binary);
        if (!in) { fail = "missing part " + std::to_string(i); return false; }
        while (in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            std::streamsize n = in.gcount();
            if (n > 0) { out.write(buf.data(), n); total += (uint64_t)n; }
        }
    }
    out.close();
    out_bytes = total;
    if (total != expected_total) {
        fail = "assembled size mismatch";
        return false;
    }
    return true;
}

// ---- 1.1.5: SHA-256 (FIPS 180-4), self-contained ----
namespace {
struct Sha256 {
    uint32_t h[8];
    uint64_t total;
    unsigned char buf[64];
    size_t blen;
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    Sha256() { reset(); }
    void reset() {
        static const uint32_t iv[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                                       0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
        std::memcpy(h, iv, sizeof(h)); total = 0; blen = 0;
    }
    void block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void update(const unsigned char* p, size_t n) {
        total += n;
        while (n > 0) {
            if (blen == 0 && n >= 64) { block(p); p += 64; n -= 64; continue; }
            size_t take = std::min(n, (size_t)64 - blen);
            std::memcpy(buf + blen, p, take); blen += take; p += take; n -= take;
            if (blen == 64) { block(buf); blen = 0; }
        }
    }
    std::string final_hex() {
        uint64_t bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char z = 0;
        while (blen != 56) update(&z, 1);
        unsigned char lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = (unsigned char)(bits >> (56 - 8*i));
        update(lenb, 8);
        static const char* hx = "0123456789ABCDEF";
        std::string r;
        for (int i = 0; i < 8; ++i)
            for (int s = 28; s >= 0; s -= 4) r.push_back(hx[(h[i] >> s) & 0xF]);
        return r;
    }
};
} // namespace

bool sha256_file(const std::string& path, std::string& hex_upper, uint64_t& bytes) {
    hex_upper.clear(); bytes = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    Sha256 sh;
    std::vector<char> buf(4 << 20);
    uint64_t total = 0;
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = in.gcount();
        if (n > 0) { sh.update((const unsigned char*)buf.data(), (size_t)n); total += (uint64_t)n; }
    }
    if (in.bad()) return false;
    bytes = total;
    hex_upper = sh.final_hex();
    return true;
}

PartsDeleteResult verify_and_delete_parts(const std::string& dir, const std::string& base, int parts,
                                          const std::string& out_name, uint64_t expected_total,
                                          const std::string& expected_sha256_hex,
                                          const std::function<void(const std::string&)>& log) {
    PartsDeleteResult r;
    auto say = [&](const std::string& m) { if (log) log("REASSEMBLE-PARTS " + m); };
    for (int i = 0; i < parts; ++i) {
        uint64_t psz = 0;
        if (file_size(join(dir, base + ".part" + std::to_string(i)), psz)) ++r.parts_present;
    }
    if (r.parts_present == 0) { r.detail = "no part files present; nothing to delete"; say("skip: " + r.detail); return r; }
    const std::string iso = join(dir, out_name);
    uint64_t isz = 0;
    if (!file_size(iso, isz)) { r.detail = "assembled ISO missing; parts kept"; say("keep: " + r.detail); return r; }
    if (isz != expected_total) {
        r.detail = "ISO size " + std::to_string(isz) + " != expected " + std::to_string(expected_total) + "; parts kept";
        say("keep: " + r.detail); return r;
    }
    r.attempted = true;
    say("verifying sha256 of " + iso + " (" + std::to_string(isz) + " bytes)");
    std::string hex; uint64_t hb = 0;
    if (!sha256_file(iso, hex, hb)) { r.detail = "cannot read ISO for hashing; parts kept"; say("keep: " + r.detail); return r; }
    std::string want = expected_sha256_hex;
    std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return (char)std::toupper(c); });
    if (hb != expected_total || hex != want) {
        r.detail = "sha256 mismatch got=" + hex + " want=" + want + "; parts kept";
        say("keep: " + r.detail); return r;
    }
    r.verified = true;
    say("sha256 OK " + hex + " size OK " + std::to_string(hb) + "; deleting " + base + ".part0.." + std::to_string(parts - 1));
    for (int i = 0; i < parts; ++i) {
        const std::string pp = join(dir, base + ".part" + std::to_string(i));
        uint64_t psz = 0;
        if (!file_size(pp, psz)) continue; // already absent
        if (std::remove(pp.c_str()) == 0) { ++r.deleted; r.bytes_deleted += psz; say("deleted " + pp + " bytes=" + std::to_string(psz)); }
        else { ++r.failed; say("FAILED to delete " + pp); }
    }
    r.detail = "verified; deleted=" + std::to_string(r.deleted) + " failed=" + std::to_string(r.failed);
    return r;
}

} // namespace reassemble
