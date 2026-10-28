// Out-of-line GuestMemoryMap implementation (see header note: compiled
// without /ZW, like memory_governor.cpp, to avoid the UWP VirtualAlloc
// link conflict in the /ZW host object).
#include "d3d12_gsrender.h"

namespace d3d12_backend {

bool GuestMemoryMap::reserve() {
    base_ = VirtualAlloc(nullptr, (SIZE_T)kTotal, MEM_RESERVE, PAGE_NOACCESS);
    return base_ != nullptr;
}

bool GuestMemoryMap::commit_chunk(unsigned chunk_index, uint8_t pattern) {
    if (!base_ || chunk_index * kChunk >= kTotal) return false;
    void* p = (char*)base_ + chunk_index * kChunk;
    if (!VirtualAlloc(p, (SIZE_T)kChunk, MEM_COMMIT, PAGE_READWRITE)) return false;
    for (uint64_t off = 0; off < kChunk; off += 4096) ((volatile uint8_t*)p)[off] = pattern;
    for (uint64_t off = 0; off < kChunk; off += 4096)
        if (((volatile uint8_t*)p)[off] != pattern) return false;
    committed_ += kChunk;
    return true;
}

void* GuestMemoryMap::translate(uint32_t offset, int location) const {
    if (!base_) return nullptr;
    uint64_t off = location == 1 ? (256ull << 20) + offset : (uint64_t)offset;
    if (off >= kTotal) return nullptr;
    return (char*)base_ + off;
}

void GuestMemoryMap::release() {
    if (base_) { VirtualFree(base_, 0, MEM_RELEASE); base_ = nullptr; committed_ = 0; }
}

} // namespace d3d12_backend
