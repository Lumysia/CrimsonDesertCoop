#include <cdcoop/core/game_structures.h>

#include <cmath>
#include <Windows.h>

namespace cdcoop {

RuntimeOffsets& get_runtime_offsets() {
    static RuntimeOffsets offsets;
    return offsets;
}

namespace {

bool has_access(uintptr_t addr, std::size_t size, DWORD allowed) {
    if (!is_valid_ptr(addr) || size == 0) return false;
    if (size > UINTPTR_MAX - addr) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (!(mbi.State & MEM_COMMIT)) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    if (!(mbi.Protect & allowed)) return false;

    // The whole [addr, addr+size) span must live inside this single committed
    // region; a read that spills into the next (possibly unmapped) region
    // would still fault.
    const uintptr_t end = addr + size;
    const uintptr_t region_end =
        reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end <= region_end;
}

} // namespace

bool is_readable(uintptr_t addr, std::size_t size) {
    constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                               PAGE_EXECUTE_WRITECOPY;
    return has_access(addr, size, readable);
}

bool is_writable(uintptr_t addr, std::size_t size) {
    constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return has_access(addr, size, writable);
}

uintptr_t resolve_ptr_chain(uintptr_t base, std::initializer_list<uint32_t> offsets) {
    uintptr_t addr = base;
    for (auto offset : offsets) {
        // Fail closed if the slot we're about to read isn't backed by
        // committed memory. After a game patch an offset can point past the
        // end of the struct, or the struct may have been freed — a bare
        // dereference here would fault and crash the game. is_valid_ptr alone
        // can't catch that (the address is numerically in range); only a live
        // page-state check can. See is_readable.
        auto next = read_mem<uintptr_t>(addr, offset);
        if (!is_valid_ptr(next)) return 0;
        addr = next;
    }
    return addr;
}

uint32_t dynamic_scan_float(uintptr_t base, uint32_t min_off, uint32_t max_off,
                            uint32_t stride, float plausible_min, float plausible_max) {
    if (!is_valid_ptr(base) || stride == 0 || max_off <= min_off) return 0;

    // Verify the entire scan region lives in committed, readable memory
    // before we start dereferencing. dynamic_scan_float runs on a fresh
    // marker pointer (e.g. dragon mount HP resolution) whose exact
    // allocation size is unknown — scanning past the end of the struct
    // into an unmapped page would AV the game process. Scan ranges are
    // small (a few hundred bytes), so a single VirtualQuery covers the
    // typical case; if the range spans region boundaries we conservatively
    // bail instead of iterating (dragon HP resolution is non-critical).
    if (max_off > UINTPTR_MAX - base ||
        !is_readable(base + min_off, max_off - min_off)) return 0;

    for (uint32_t off = min_off; off < max_off; off += stride) {
        float v = *reinterpret_cast<float*>(base + off);
        if (!std::isfinite(v)) continue;
        if (v < plausible_min || v > plausible_max) continue;
        return off;
    }
    return 0;
}

} // namespace cdcoop
