#include "guest_space.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstring>
#include <fstream>

namespace swift::guest_call {

namespace {

constexpr std::uint64_t RoundUp(std::uint64_t v, std::uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}
constexpr std::uint64_t RoundDown(std::uint64_t v, std::uint64_t a) { return v & ~(a - 1); }

// --- minimal ELF64 little-endian structures --------------------------------
struct Ehdr {
    unsigned char ident[16];
    std::uint16_t type, machine;
    std::uint32_t version;
    std::uint64_t entry, phoff, shoff;
    std::uint32_t flags;
    std::uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr {
    std::uint32_t type, flags;
    std::uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct Shdr {
    std::uint32_t name, type;
    std::uint64_t flags, addr, offset, size;
    std::uint32_t link, info;
    std::uint64_t addralign, entsize;
};
struct Sym {
    std::uint32_t name;
    unsigned char info, other;
    std::uint16_t shndx;
    std::uint64_t value, size;
};
struct Rela {
    std::uint64_t offset, info;
    std::int64_t addend;
};

constexpr std::uint32_t PT_LOAD = 1;
constexpr std::uint32_t SHT_SYMTAB = 2;
constexpr std::uint32_t SHT_RELA = 4;
constexpr std::uint16_t EM_X86_64 = 62;
constexpr unsigned STT_FUNC = 2;
constexpr unsigned STT_OBJECT = 1;
constexpr unsigned STT_GNU_IFUNC = 10;
constexpr std::uint32_t R_X86_64_IRELATIVE = 37;

}  // namespace

GuestSpace::~GuestSpace() {
    if (bias_ != 0 && window_size_ != 0) {
        ::munmap(reinterpret_cast<void*>(bias_), window_size_);
    }
}

bool GuestSpace::ReserveWindow(std::uint32_t bits) {
    if (bits < 20 || bits > 47) {
        return false;
    }
    const std::uint64_t size = std::uint64_t{1} << bits;
    void* p = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        return false;
    }
    bias_ = reinterpret_cast<std::uint64_t>(p);
    if ((bias_ & (kHostPageSize - 1)) != 0) {
        ::munmap(p, size);
        return false;
    }
    bits_ = bits;
    mask_ = size - 1;
    window_size_ = size;
    const std::uint64_t pages = size >> kHostPageShift;
    page_bitmap_ = std::make_unique<std::uint64_t[]>((pages + 63) / 64);
    return true;
}

bool GuestSpace::MapFixed(std::uint64_t addr, std::uint64_t size) {
    if (bias_ == 0 || size == 0) {
        return false;
    }
    const std::uint64_t start = RoundDown(addr & mask_, kHostPageSize);
    const std::uint64_t end = RoundUp((addr & mask_) + size, kHostPageSize);
    if (end > window_size_) {
        return false;
    }
    // Guest pages are 4 KiB but host pages are 16 KiB, so two guest mappings
    // routinely share a host page: an ELF whose PT_LOADs are 0x400000 and
    // 0x401000, or a brk region that starts just past the image.  A blanket
    // MAP_FIXED over the rounded range would silently ZERO whatever was
    // already there -- which shows up much later as "the code is all nul
    // bytes".  Map only the host pages that are not present yet.
    std::uint64_t run_start = 0;
    bool in_run = false;
    for (std::uint64_t p = start; p <= end; p += kHostPageSize) {
        const bool need = p < end && !PageBit(p);
        if (need && !in_run) {
            run_start = p;
            in_run = true;
        } else if (!need && in_run) {
            void* r = ::mmap(reinterpret_cast<void*>(run_start + bias_), p - run_start,
                             PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
                             0);
            if (r == MAP_FAILED) {
                return false;
            }
            SetPageBits(run_start, p - run_start, true);
            in_run = false;
        }
    }
    return true;
}

void GuestSpace::SetPageBits(std::uint64_t addr, std::uint64_t size, bool present) {
    const std::uint64_t first = (addr & mask_) >> kHostPageShift;
    const std::uint64_t last = ((addr & mask_) + size - 1) >> kHostPageShift;
    for (std::uint64_t p = first; p <= last; ++p) {
        if (present) {
            page_bitmap_[p >> 6] |= std::uint64_t{1} << (p & 63);
        } else {
            page_bitmap_[p >> 6] &= ~(std::uint64_t{1} << (p & 63));
        }
    }
}

bool GuestSpace::PageBit(std::uint64_t addr) const {
    const std::uint64_t p = (addr & mask_) >> kHostPageShift;
    return (page_bitmap_[p >> 6] >> (p & 63)) & 1;
}

bool GuestSpace::RangeIsMapped(std::uint64_t addr, std::uint64_t size) const {
    if (page_bitmap_ == nullptr || size == 0) {
        return false;
    }
    const std::uint64_t base = addr & mask_;
    if (base + size > window_size_) {
        return false;
    }
    for (std::uint64_t p = base; p < base + size; p = RoundDown(p, kHostPageSize) + kHostPageSize) {
        if (!PageBit(p)) {
            return false;
        }
    }
    return true;
}

std::uint64_t GuestSpace::MappedBytesFrom(std::uint64_t addr, std::uint64_t length) const {
    std::uint64_t n = 0;
    while (n < length && RangeIsMapped(addr + n, 1)) {
        const std::uint64_t page_end =
                RoundDown((addr + n) & mask_, kHostPageSize) + kHostPageSize;
        const std::uint64_t chunk = page_end - ((addr + n) & mask_);
        n += chunk;
    }
    return n > length ? length : n;
}

bool GuestSpace::CreateArena(std::uint64_t base, std::uint64_t size) {
    if (!MapFixed(base, size)) {
        return false;
    }
    arena_base_ = base;
    arena_end_ = base + size;
    arena_next_ = base;
    return true;
}

std::uint64_t GuestSpace::ArenaAlloc(std::uint64_t size, std::uint64_t align) {
    if (align == 0) {
        align = 1;
    }
    const std::uint64_t addr = RoundUp(arena_next_, align);
    if (addr + size > arena_end_) {
        return 0;
    }
    arena_next_ = addr + size;
    return addr;
}

void GuestSpace::ArenaFree(std::uint64_t addr, std::uint64_t size) {
    if (addr + size == arena_next_) {
        arena_next_ = addr;  // LIFO fast path; otherwise leak until teardown
    }
}

bool GuestSpace::LoadElf(const std::string& path, Image& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < sizeof(Ehdr)) {
        error = "file too small";
        return false;
    }
    Ehdr eh{};
    std::memcpy(&eh, data.data(), sizeof(eh));
    if (std::memcmp(eh.ident, "\x7f"
                              "ELF",
                    4) != 0 ||
        eh.ident[4] != 2 || eh.machine != EM_X86_64) {
        error = "not a 64-bit x86-64 ELF";
        return false;
    }
    out.entry = eh.entry;
    out.min_vaddr = ~std::uint64_t{0};
    out.max_vaddr = 0;
    out.phentsize = eh.phentsize;
    out.phnum = eh.phnum;

    // 1. place PT_LOADs at their linked guest addresses.
    for (std::uint16_t i = 0; i < eh.phnum; ++i) {
        Phdr ph{};
        std::memcpy(&ph, data.data() + eh.phoff + std::size_t{i} * eh.phentsize, sizeof(ph));
        if (ph.type != PT_LOAD || ph.memsz == 0) {
            continue;
        }
        if (!MapFixed(ph.vaddr, ph.memsz)) {
            error = "cannot map PT_LOAD at " + std::to_string(ph.vaddr);
            return false;
        }
        if (ph.filesz > 0) {
            if (ph.offset + ph.filesz > data.size()) {
                error = "PT_LOAD extends past end of file";
                return false;
            }
            std::memcpy(ToHost(ph.vaddr), data.data() + ph.offset, ph.filesz);
        }
        out.min_vaddr = std::min(out.min_vaddr, ph.vaddr);
        out.max_vaddr = std::max(out.max_vaddr, ph.vaddr + ph.memsz);
        // The program header table is in memory whenever a PT_LOAD covers its
        // file offset; that is what AT_PHDR must point at.
        if (eh.phoff >= ph.offset && eh.phoff + std::uint64_t{eh.phnum} * eh.phentsize <=
                                             ph.offset + ph.filesz) {
            out.phdr = ph.vaddr + (eh.phoff - ph.offset);
        }
    }

    // 2. symbols (.symtab); missing on the freestanding guests, which is fine.
    for (std::uint16_t i = 0; i < eh.shnum; ++i) {
        Shdr sh{};
        std::memcpy(&sh, data.data() + eh.shoff + std::size_t{i} * eh.shentsize, sizeof(sh));
        if (sh.type == SHT_SYMTAB && sh.link < eh.shnum) {
            Shdr str{};
            std::memcpy(&str, data.data() + eh.shoff + std::size_t{sh.link} * eh.shentsize,
                        sizeof(str));
            const std::size_t count = sh.size / sizeof(Sym);
            for (std::size_t s = 0; s < count; ++s) {
                Sym sym{};
                std::memcpy(&sym, data.data() + sh.offset + s * sizeof(Sym), sizeof(sym));
                if (sym.name == 0 || sym.value == 0) {
                    continue;
                }
                const char* nm = data.data() + str.offset + sym.name;
                const unsigned type = sym.info & 0xf;
                if (type == STT_FUNC || type == STT_GNU_IFUNC) {
                    out.functions.emplace(nm, sym.value);
                    if (type == STT_GNU_IFUNC) {
                        out.ifuncs.emplace(nm);
                    }
                } else if (type == STT_OBJECT) {
                    out.objects.emplace(nm, sym.value);
                }
            }
        } else if (sh.type == SHT_RELA) {
            const std::size_t count = sh.size / sizeof(Rela);
            for (std::size_t r = 0; r < count; ++r) {
                Rela rela{};
                std::memcpy(&rela, data.data() + sh.offset + r * sizeof(Rela), sizeof(rela));
                if (static_cast<std::uint32_t>(rela.info & 0xffffffff) == R_X86_64_IRELATIVE) {
                    out.irelative_targets.push_back(rela.offset);
                    out.irelative_resolvers.push_back(static_cast<std::uint64_t>(rela.addend));
                }
            }
        }
    }
    return true;
}

}  // namespace swift::guest_call
