#include "catalog.h"

#include "base/error.h"

#include <algorithm>
#include <bit>

namespace swift::tests::correctness {
namespace {

constexpr std::uint64_t CodeEnd = 0x04010000, DataBase = 0x06000000;

// SplitMix64 is specified here rather than through implementation-dependent
// distributions. Each form/case has its own stream, independent of scheduling.
struct Random {
    std::uint64_t state;

    std::uint64_t Next() {
        auto value = (state += UINT64_C(0x9e3779b97f4a7c15));
        value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
        return value ^ (value >> 31);
    }
};

constexpr std::uint64_t integers[]{0,
                                   1,
                                   UINT64_MAX,
                                   UINT64_C(0x7fffffffffffffff),
                                   UINT64_C(0x8000000000000000),
                                   0x7fffffff,
                                   0x80000000,
                                   UINT64_C(0xaaaaaaaaaaaaaaaa),
                                   UINT64_C(0x5555555555555555),
                                   0xff,
                                   0x100,
                                   0xffff,
                                   0x10000,
                                   31,
                                   32,
                                   63,
                                   64,
                                   65,
                                   255};
constexpr std::uint64_t doubles[]{0,
                                  UINT64_C(0x8000000000000000),
                                  UINT64_C(0x3ff0000000000000),
                                  UINT64_C(0xbff0000000000000),
                                  1,
                                  UINT64_C(0x000fffffffffffff),
                                  UINT64_C(0x0010000000000000),
                                  UINT64_C(0x7fefffffffffffff),
                                  UINT64_C(0x7ff0000000000000),
                                  UINT64_C(0xfff0000000000000),
                                  UINT64_C(0x7ff8000000000042),
                                  UINT64_C(0x7ff0000000000042),
                                  UINT64_C(0x3ca0000000000000),
                                  UINT64_C(0x3fefffffffffffff),
                                  UINT64_C(0x0010000000000001),
                                  UINT64_C(0x8010000000000000)};
constexpr std::uint8_t counts[]{0, 1, 2, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 127, 128, 255};

std::uint64_t FormSeed(std::string_view id) {
    std::uint64_t value = UINT64_C(14695981039346656037);
    for (const auto c : id)
        value = (value ^ static_cast<unsigned char>(c)) * UINT64_C(1099511628211);
    return value;
}

std::uint64_t Integer(Random& random, std::uint64_t id, unsigned offset = 0) {
    return id < std::size(integers) ? integers[(id + offset) % std::size(integers)] : random.Next();
}

void Emit(Case& c, std::uint32_t word, unsigned bytes = 4) {
    for (unsigned i = 0; i < bytes; ++i)
        c.code.push_back(static_cast<std::uint8_t>(word >> (i * 8)));
}

void FP(State& state, unsigned vector, unsigned offset, std::uint64_t bits) {
    for (unsigned i = 0; i < 8; ++i)
        state.vectors[vector][offset + i] = static_cast<std::uint8_t>(bits >> (8 * i));
}

struct Registers {
    unsigned d{}, n{}, m{};
};

Registers Aliases(Random& random, std::uint64_t id, unsigned count) {
    Registers r{unsigned(random.Next() % count),
                unsigned(random.Next() % count),
                unsigned(random.Next() % count)};
    if (id % 5 == 0) r.d = r.n;
    if (id % 5 == 1) r.d = r.m;
    if (id % 5 == 2) r.n = r.m;
    if (id % 5 == 3) r.d = r.n = r.m;
    return r;
}

void Initialize(Case& c, const Form& form, Random& random) {
    auto& state = c.initial;
    const unsigned gprs = c.isa == ISA::X86_64 ? 16 : c.isa == ISA::Arm64 ? 32 : 15;
    for (unsigned i = 0; i < gprs; ++i)
        state.gpr[i] = c.isa == ISA::Arm32 ? std::uint32_t(random.Next()) : random.Next();
    const unsigned vectors = c.isa == ISA::Arm64 ? 32 : 16;
    const unsigned bytes = c.isa == ISA::X86_64 ? 32 : 16;
    for (unsigned i = 0; i < vectors; ++i)
        for (unsigned j = 0; j < bytes; ++j)
            state.vectors[i][j] = static_cast<std::uint8_t>(random.Next());
    if (c.isa == ISA::X86_64) {
        c.profile = "x86_64-user";
        state.flags = 0x202 | (random.Next() & 0xcd5);
        state.gpr[4] = DataBase + 0xff00;
        state.fp_control = 0x1f80 | ((c.id & 3) << 13) | ((c.id & 4) ? 0x8000 : 0) |
                           ((c.id & 8) ? 0x40 : 0) | (random.Next() & 0x3f);
    } else if (c.isa == ISA::Arm64) {
        c.profile = "a64-user";
        state.flags = (c.id & 15) << 28;
        state.gpr[31] = DataBase + 0xff00;
        state.fp_control =
                ((c.id & 3) << 22) | ((c.id & 4) ? 0x01000000 : 0) | ((c.id & 8) ? 0x02000000 : 0);
        state.fp_status = random.Next() & 0x0800009f;
    } else {
        c.profile = "armv7-user";
        state.flags = 0x10 | ((c.id & 15) << 28) | (random.Next() & 0x080f0000);
        const bool thumb = form.encoding == Encoding::Thumb16Register ||
                           form.encoding == Encoding::Thumb16Shift ||
                           form.encoding == Encoding::Thumb32Register;
        if (thumb) state.flags |= 32;
        state.gpr[13] = DataBase + 0xff00;
        state.fp_control = ((c.id & 3) << 22) | ((c.id & 4) ? 0x01000000 : 0) |
                           ((c.id & 8) ? 0x02000000 : 0) | (random.Next() & 0xf800009f);
    }
    MemoryRegion memory{DataBase, 65536, 3, {}};
    memory.bytes.resize(256);
    for (auto& byte : memory.bytes) byte = static_cast<std::uint8_t>(random.Next());
    c.memory.push_back(std::move(memory));
}

std::uint64_t Address(const Case& c) {
    constexpr unsigned offsets[]{0, 1, 7, 15, 31, 63, 127, 248, 65528};
    return DataBase + offsets[c.id % std::size(offsets)];
}

void X86Register(Case& c, const Form& form, Random& random) {
    auto r = Aliases(random, c.id, form.width == 8 ? 4 : 8);
    c.initial.gpr[r.d] = Integer(random, c.id);
    c.initial.gpr[r.m] = Integer(random, c.id, 3);
    if (form.width == 64) c.code.push_back(0x48);
    c.code.push_back(static_cast<std::uint8_t>(form.bits));
    c.code.push_back(static_cast<std::uint8_t>(0xc0 | (r.m << 3) | r.d));
}

void X86Shift(Case& c, const Form& form, Random& random) {
    const auto d = unsigned(random.Next() % 8);
    c.initial.gpr[d] = Integer(random, c.id);
    c.initial.gpr[1] = (c.initial.gpr[1] & ~UINT64_C(255)) | counts[c.id % std::size(counts)];
    if (form.width == 64) c.code.push_back(0x48);
    c.code.push_back(0xd3);
    c.code.push_back(static_cast<std::uint8_t>(0xe0 | d));
}

void X86Memory(Case& c, const Form& form, Random& random) {
    constexpr unsigned bases[]{0, 1, 2, 3, 6, 7};
    const auto base = bases[random.Next() % std::size(bases)];
    const auto reg = c.id % 3 ? unsigned(random.Next() % 8) : base;
    c.initial.gpr[reg] = Integer(random, c.id);
    c.initial.gpr[base] = Address(c);
    c.code = {0x48,
              static_cast<std::uint8_t>(form.bits),
              static_cast<std::uint8_t>((reg << 3) | base)};
}

void X86Vector(Case& c, const Form& form, Random& random) {
    const auto r = Aliases(random, c.id, 8);
    if (form.encoding == Encoding::X86ScalarFP) {
        FP(c.initial, r.d, 0, doubles[c.id % std::size(doubles)]);
        FP(c.initial,
           r.m,
           0,
           c.id < std::size(doubles) ? doubles[(c.id + 3) % std::size(doubles)] : random.Next());
    }
    c.code = {static_cast<std::uint8_t>(form.encoding == Encoding::X86ScalarFP ? 0xf2 : 0x66),
              0x0f,
              static_cast<std::uint8_t>(form.bits),
              static_cast<std::uint8_t>(0xc0 | (r.d << 3) | r.m)};
}

void A64Register(Case& c, const Form& form, Random& random) {
    auto r = Aliases(random, c.id, 31);
    c.initial.gpr[r.n] = Integer(random, c.id);
    c.initial.gpr[r.m] =
            form.category == "shift" ? counts[c.id % std::size(counts)] : Integer(random, c.id, 3);
    auto word = form.bits | (r.d) | (r.n << 5) | (r.m << 16);
    if (form.encoding == Encoding::A64Shifted) {
        constexpr unsigned shifts[]{0, 1, 7, 15, 31, 32, 63};
        const auto shift = shifts[c.id % std::size(shifts)] % form.width;
        word |= (shift << 10) | (unsigned(c.id % 3) << 22);
    }
    Emit(c, word);
}

void A64Vector(Case& c, const Form& form, Random& random) {
    const auto r = Aliases(random, c.id, 32);
    if (form.encoding == Encoding::A64ScalarFP) {
        FP(c.initial, r.n, 0, doubles[c.id % std::size(doubles)]);
        FP(c.initial,
           r.m,
           0,
           c.id < std::size(doubles) ? doubles[(c.id + 3) % std::size(doubles)] : random.Next());
    }
    Emit(c, form.bits | r.d | (r.n << 5) | (r.m << 16));
}

void A64Memory(Case& c, const Form& form, Random& random) {
    const auto r = Aliases(random, c.id, 31);
    c.initial.gpr[r.d] = Integer(random, c.id);
    c.initial.gpr[r.n] = Address(c);
    Emit(c, form.bits | r.d | (r.n << 5));
}

void A32Register(Case& c, const Form& form, Random& random) {
    const auto r = Aliases(random, c.id, 13);
    c.initial.gpr[r.n] = std::uint32_t(Integer(random, c.id));
    c.initial.gpr[r.m] = std::uint32_t(Integer(random, c.id, 3));
    // Half the deterministic cases execute unconditionally; the other half
    // exercise predicates against all sixteen NZCV inputs.
    const auto condition = c.id % 2 ? unsigned((c.id / 2) % 14) : 14;
    auto word = form.bits | (condition << 28) | (r.d << 12) | (r.n << 16) | r.m;
    if (form.encoding == Encoding::A32ShiftRegister) {
        const auto rs = r.n;
        c.initial.gpr[rs] = counts[c.id % std::size(counts)];
        word = form.bits | (condition << 28) | (r.d << 12) | (rs << 8) | r.m;
    } else {
        constexpr unsigned shifts[]{0, 1, 7, 15, 31};
        word |= (shifts[c.id % std::size(shifts)] << 7) | (unsigned(c.id % 4) << 5);
    }
    Emit(c, word);
}

std::uint32_t A32VectorRegisters(Registers r) {
    return ((r.d & 15) << 12) | ((r.d >> 4) << 22) | ((r.n & 15) << 16) | ((r.n >> 4) << 7) |
           (r.m & 15) | ((r.m >> 4) << 5);
}

void A32Vector(Case& c, const Form& form, Random& random) {
    auto r = Aliases(random, c.id, form.encoding == Encoding::A32ScalarFP ? 32 : 16);
    if (form.encoding == Encoding::A32ScalarFP) {
        FP(c.initial, r.n / 2, (r.n % 2) * 8, doubles[c.id % std::size(doubles)]);
        FP(c.initial,
           r.m / 2,
           (r.m % 2) * 8,
           c.id < std::size(doubles) ? doubles[(c.id + 3) % std::size(doubles)] : random.Next());
    } else {
        r.d *= 2;
        r.n *= 2;
        r.m *= 2;
    }
    Emit(c, form.bits | A32VectorRegisters(r));
}

void A32Memory(Case& c, const Form& form, Random& random) {
    const auto r = Aliases(random, c.id, 13);
    c.initial.gpr[r.d] = std::uint32_t(Integer(random, c.id));
    c.initial.gpr[r.n] = Address(c);
    const auto condition = c.id % 2 ? unsigned((c.id / 2) % 14) : 14;
    Emit(c, form.bits | (condition << 28) | (r.n << 16) | (r.d << 12));
}

void ThumbRegister(Case& c, const Form& form, Random& random) {
    const auto r = Aliases(random, c.id, 8);
    c.initial.gpr[r.n] = std::uint32_t(Integer(random, c.id));
    c.initial.gpr[r.m] = std::uint32_t(Integer(random, c.id, 3));
    if (form.encoding == Encoding::Thumb16Shift) {
        c.initial.gpr[r.d] = std::uint32_t(Integer(random, c.id));
        c.initial.gpr[r.m] = counts[c.id % std::size(counts)];
        Emit(c, form.bits | r.d | (r.m << 3), 2);
    } else if (form.encoding == Encoding::Thumb16Register) {
        Emit(c, form.bits | r.d | (r.n << 3) | (r.m << 6), 2);
    } else {
        const auto word = form.bits | (r.n << 16) | (r.d << 8) | r.m;
        Emit(c, word >> 16, 2);
        Emit(c, word & 65535, 2);
    }
}

using Generator = void (*)(Case&, const Form&, Random&);
constexpr Generator handlers[]{X86Register,
                               X86Shift,
                               X86Memory,
                               X86Vector,
                               X86Vector,
                               A64Register,
                               A64Register,
                               A64Memory,
                               A64Vector,
                               A64Vector,
                               A32Register,
                               A32Register,
                               A32Memory,
                               A32Vector,
                               A32Vector,
                               ThumbRegister,
                               ThumbRegister,
                               ThumbRegister};

}  // namespace

Case Generate(std::string_view form_id, std::uint64_t seed, std::uint64_t id) {
    const auto* form = FindForm(form_id);
    if (!form) swift::ThrowError("unknown correctness form", swift::ErrorKind::InvalidArgument);
    Random random{seed ^ FormSeed(form_id) ^ (id * UINT64_C(0xd1342543de82ef95))};
    Case c;
    c.isa = form->isa;
    c.form_id = form->id;
    c.seed = seed;
    c.id = id;
    c.max_steps = 16;
    Initialize(c, *form, random);
    handlers[static_cast<unsigned>(form->encoding)](c, *form, random);
    c.terminal_pc = CodeEnd;
    c.code_address = CodeEnd - c.code.size();
    c.initial.pc = c.code_address;
    std::string reason;
    if (!ValidateCase(c, &reason))
        swift::ThrowError("generator produced invalid form: " + reason,
                          swift::ErrorKind::InvalidState);
    return c;
}

}  // namespace swift::tests::correctness
