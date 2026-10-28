// reassemble.cpp — pure C++, no WinRT, no core headers.
#include "reassemble.h"
#include <cstdio>
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

} // namespace reassemble
