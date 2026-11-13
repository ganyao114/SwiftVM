#include "base/error.h"
#include "code_image.h"
#include "runtime/backend/x64/recovery.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace swift::runtime::execution {
namespace {
constexpr std::array<std::uint8_t, 8> magic{'S', 'V', 'M', 'I', 'M', 'G', 0, 1};
constexpr std::size_t maximum_bytes = 256u << 20;
constexpr std::uint32_t maximum_units = 100000;

struct Invalid {
    const char* message;
};

void Require(bool condition, const char* message) {
    if (!condition) throw Invalid{message};
}

class Writer {
public:
    void U8(std::uint8_t value) {
        Require(bytes.size() < maximum_bytes, "code image exceeds its size limit");
        bytes.push_back(value);
    }

    void U16(std::uint16_t value) {
        for (unsigned i = 0; i < 2; ++i) U8(value >> (i * 8));
    }

    void U32(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) U8(value >> (i * 8));
    }

    void U64(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) U8(value >> (i * 8));
    }

    void Bytes(std::span<const std::uint8_t> data) {
        Require(data.size() <= maximum_bytes - bytes.size(), "code image exceeds its size limit");
        bytes.insert(bytes.end(), data.begin(), data.end());
    }

    std::vector<std::uint8_t> bytes;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data(data) {}

    std::span<const std::uint8_t> Bytes(std::size_t count) {
        Require(count <= data.size(), "truncated code image");
        const auto part = data.first(count);
        data = data.subspan(count);
        return part;
    }

    std::uint8_t U8() { return Bytes(1)[0]; }

    std::uint16_t U16() {
        std::uint16_t value{};
        for (unsigned i = 0; i < 2; ++i) value |= std::uint16_t{U8()} << (i * 8);
        return value;
    }

    std::uint32_t U32() {
        std::uint32_t value{};
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t{U8()} << (i * 8);
        return value;
    }

    std::uint64_t U64() {
        std::uint64_t value{};
        for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t{U8()} << (i * 8);
        return value;
    }

    std::uint32_t Count(std::size_t minimum) {
        const auto count = U32();
        Require(count <= data.size() / minimum, "code image count exceeds payload");
        return count;
    }

    Digest Hash() {
        Digest result;
        const auto part = Bytes(result.size());
        std::copy(part.begin(), part.end(), result.begin());
        return result;
    }

    bool Empty() const noexcept { return data.empty(); }

    std::size_t Remaining() const noexcept { return data.size(); }

private:
    std::span<const std::uint8_t> data;
};

bool ScopeValid(ir::Scope scope) {
    return scope == ir::Scope::Block || scope == ir::Scope::Function;
}

void Validate(const CodeUnit& unit, bool byte_code, GuestISA isa) {
    const auto physical_pc = CodeAddress(isa, unit.pc);
    const auto instruction_mask = byte_code ? 0u : 3u;
    const auto& code = unit.code;
    const auto size = code.words.size() * sizeof(std::uint32_t);
    Require(ScopeValid(unit.scope) && unit.fp_mode < 128, "invalid code unit scope or FP domain");
    Require(bool(code) && code.relocatable && size && size <= maximum_bytes,
            "code unit has no relocatable native code");
    Require(!unit.sources.empty(), "code unit has no source dependencies");
    bool owns_entry{};
    for (const auto& source : unit.sources) {
        Require(source.span.size && source.span.size <= UINT64_MAX - source.span.pc &&
                        source.mode <= 1,
                "invalid code source dependency");
        owns_entry |=
                physical_pc >= source.span.pc && physical_pc - source.span.pc < source.span.size;
    }
    Require(owns_entry, "code unit entry is outside its source dependencies");
    for (const auto& site : code.faults) {
        if (byte_code) {
            Require(backend::x64::ValidRecovery(site, code.frame_bytes),
                    "invalid x64 flags recovery source");
        }
        Require(site.Kind() == backend::FaultSiteKind::GuestMemory ||
                        site.Kind() == backend::FaultSiteKind::DispatchInterrupt,
                "invalid native recovery site kind");
        Require(site.first_offset < site.last_offset && site.last_offset <= size &&
                        !(site.first_offset & instruction_mask) &&
                        !(site.last_offset & instruction_mask),
                "invalid native fault range");
        Require(site.unaligned_offset == backend::NoNativeOffset ||
                        (!(site.unaligned_offset & instruction_mask) &&
                         site.unaligned_offset >= site.first_offset &&
                         site.unaligned_offset < site.last_offset),
                "invalid unaligned fault offset");
        Require(site.fallback_offset == backend::NoNativeOffset ||
                        (!(site.fallback_offset & instruction_mask) && size >= 12 &&
                         site.fallback_offset <= size - 12),
                "invalid fault fallback offset");
    }
    for (const auto& link : code.links) {
        Require(link.kind == backend::LinkKind::Static, "invalid native link kind");
        const bool branch = !(link.branch_offset & instruction_mask) && link.branch_offset < size;
        Require(!(link.data_offset & 7) && size >= 8 && link.data_offset <= size - 8 && branch &&
                        !(link.fallback_offset & instruction_mask) && link.fallback_offset < size,
                "invalid native link offsets");
    }
    for (const auto& entry : code.entries)
        Require(!(entry.offset & instruction_mask) && entry.offset < size,
                "invalid native entry offset");
    std::size_t previous_end{};
    for (const auto& relocation : code.relocations) {
        const auto width = (relocation.kind == backend::RelocationKind::Absolute64 ||
                            relocation.kind == backend::RelocationKind::X64Immediate64)
                                   ? 8u
                           : relocation.kind == backend::RelocationKind::Arm64MoveWide64 ? 16u
                                                                                         : 0u;
        Require(width && relocation.symbol != backend::HostSymbol::None &&
                        !(relocation.offset &
                          (relocation.kind == backend::RelocationKind::X64Immediate64 ? 0u
                           : width == 8                                               ? 7u
                                                                                      : 3u)) &&
                        width <= size && relocation.offset <= size - width &&
                        relocation.offset >= previous_end,
                "invalid or overlapping native relocation");
        previous_end = relocation.offset + width;
    }
}

void Key(Writer& out, const CodeImageKey& key) {
    out.Bytes(key.build_id);
    out.Bytes(key.guest_id);
    out.Bytes(key.configuration);
    out.U64(key.load_bias);
    out.U32(key.host_isa);
    out.U16(static_cast<unsigned>(key.guest_isa));
    out.U16(key.location_version);
    out.U64(key.guest_profile);
    out.U8(static_cast<unsigned>(key.requested_scope));
}

CodeImageKey Key(Reader& in) {
    CodeImageKey key;
    key.build_id = in.Hash();
    key.guest_id = in.Hash();
    key.configuration = in.Hash();
    key.load_bias = in.U64();
    key.host_isa = in.U32();
    key.guest_isa = static_cast<GuestISA>(in.U16());
    key.location_version = in.U16();
    key.guest_profile = in.U64();
    Require((key.guest_isa == GuestISA::X86_64 || key.guest_isa == GuestISA::Arm64 ||
             key.guest_isa == GuestISA::Arm32) &&
                    key.location_version == 1,
            "invalid guest ISA/location format");
    key.requested_scope = static_cast<ir::Scope>(in.U8());
    Require(ScopeValid(key.requested_scope), "invalid image scope");
    Require(key.host_isa == 0xa64 || key.host_isa == 0x64 || key.host_isa == 0x8664 ||
                    key.host_isa == 0x1a64,
            "invalid image host ISA");
    return key;
}

void Unit(Writer& out,
          const CodeUnit& unit,
          const std::uint64_t* text_offset,
          bool byte_code,
          GuestISA isa) {
    Validate(unit, byte_code, isa);
    const auto& code = unit.code;
    out.U64(unit.pc);
    out.U8(unit.fp_mode);
    out.U8(static_cast<unsigned>(unit.scope));
    out.U16(0);
    out.U32(code.frame_bytes);
    out.U32(code.spill_slots);
    out.U32(static_cast<std::uint32_t>(code.words.size()));
    if (text_offset)
        out.U64(*text_offset);
    else
        for (const auto word : code.words) out.U32(word);
    out.U32(static_cast<std::uint32_t>(code.faults.size()));
    for (const auto& fault : code.faults) {
        out.U32(fault.first_offset);
        out.U32(fault.last_offset);
        out.U64(fault.guest_pc);
        out.U32(fault.unaligned_offset);
        out.U32(fault.fallback_offset);
        out.Bytes(fault.recovery);
    }
    out.U32(static_cast<std::uint32_t>(code.links.size()));
    for (const auto& link : code.links) {
        out.U8(static_cast<unsigned>(link.kind));
        out.U32(link.data_offset);
        out.U32(link.branch_offset);
        out.U32(link.fallback_offset);
        out.U64(link.target_pc);
    }
    out.U32(static_cast<std::uint32_t>(code.entries.size()));
    for (const auto& entry : code.entries) {
        out.U64(entry.pc);
        out.U32(entry.offset);
    }
    out.U32(static_cast<std::uint32_t>(code.relocations.size()));
    for (const auto& relocation : code.relocations) {
        out.U32(relocation.offset);
        out.U16(static_cast<unsigned>(relocation.kind));
        out.U16(static_cast<unsigned>(relocation.symbol));
        out.U64(std::bit_cast<std::uint64_t>(relocation.addend));
    }
    out.U32(static_cast<std::uint32_t>(unit.sources.size()));
    for (const auto& source : unit.sources) {
        out.U64(source.span.pc);
        out.U64(source.span.size);
        out.Bytes(source.digest);
        out.U8(source.mode);
    }
}

CodeUnit Unit(Reader& in,
              std::span<const std::uint8_t> text,
              bool external,
              std::uint64_t& previous_text_end,
              bool byte_code,
              GuestISA isa) {
    CodeUnit unit;
    unit.pc = in.U64();
    unit.fp_mode = in.U8();
    unit.scope = static_cast<ir::Scope>(in.U8());
    Require(in.U16() == 0, "invalid code unit reserved bits");
    auto& code = unit.code;
    code.relocatable = true;
    code.frame_bytes = in.U32();
    code.spill_slots = in.U32();
    const auto count = in.U32();
    Require(count && count <= maximum_bytes / 4, "invalid native word count");
    if (external) {
        const auto offset = in.U64(), bytes = std::uint64_t{count} * 4;
        Require(!(offset & 15) && offset >= previous_text_end && offset <= text.size() &&
                        bytes <= text.size() - offset,
                "invalid or overlapping external native text");
        previous_text_end = offset + bytes;
        Reader words{
                text.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(bytes))};
        code.words.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) code.words.push_back(words.U32());
    } else {
        Require(count <= in.Remaining() / 4, "native words exceed payload");
        code.words.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) code.words.push_back(in.U32());
    }
    code.faults.resize(in.Count(32));
    for (auto& fault : code.faults) {
        fault.first_offset = in.U32();
        fault.last_offset = in.U32();
        fault.guest_pc = in.U64();
        fault.unaligned_offset = in.U32();
        fault.fallback_offset = in.U32();
        const auto recipe = in.Bytes(fault.recovery.size());
        std::copy(recipe.begin(), recipe.end(), fault.recovery.begin());
    }
    code.links.resize(in.Count(21));
    for (auto& link : code.links) {
        link.kind = static_cast<backend::LinkKind>(in.U8());
        link.data_offset = in.U32();
        link.branch_offset = in.U32();
        link.fallback_offset = in.U32();
        link.target_pc = in.U64();
    }
    code.entries.resize(in.Count(12));
    for (auto& entry : code.entries) {
        entry.pc = in.U64();
        entry.offset = in.U32();
    }
    code.relocations.resize(in.Count(16));
    for (auto& relocation : code.relocations) {
        relocation.offset = in.U32();
        relocation.kind = static_cast<backend::RelocationKind>(in.U16());
        relocation.symbol = static_cast<backend::HostSymbol>(in.U16());
        relocation.addend = std::bit_cast<std::int64_t>(in.U64());
    }
    unit.sources.resize(in.Count(49));
    for (auto& source : unit.sources) {
        source.span.pc = in.U64();
        source.span.size = in.U64();
        source.digest = in.Hash();
        source.mode = in.U8();
    }
    Validate(unit, byte_code, isa);
    return unit;
}

std::vector<std::uint8_t> Encode(const CodeImage& image,
                                 std::vector<std::uint8_t>* text,
                                 std::span<const std::uint8_t> tail) {
    Require(ScopeValid(image.key.requested_scope) && image.units.size() <= maximum_units,
            "invalid code image key or unit count");
    Writer payload;
    Key(payload, image.key);
    std::vector<std::uint64_t> offsets;
    if (text) {
        offsets = CodeUnitTextOffsets(image);
        Writer body;
        for (std::size_t i = 0; i < image.units.size(); ++i) {
            body.bytes.resize(static_cast<std::size_t>(offsets[i]), 0);
            for (const auto word : image.units[i].code.words) body.U32(word);
        }
        body.bytes.resize((body.bytes.size() + 15) & ~std::size_t{15}, 0);
        body.Bytes(tail);
        *text = std::move(body.bytes);
        payload.U64(text->size());
        payload.Bytes(HashBytes(*text));
    }
    payload.U32(static_cast<std::uint32_t>(image.units.size()));
    for (std::size_t i = 0; i < image.units.size(); ++i)
        Unit(payload,
             image.units[i],
             text ? &offsets[i] : nullptr,
             image.key.host_isa == 0x8664,
             image.key.guest_isa);
    Require(payload.bytes.size() <= maximum_bytes - 56, "code image exceeds its size limit");
    Writer result;
    result.Bytes(magic);
    result.U32(CodeImageFormatVersion);
    result.U32(text ? 1 : 0);
    result.U64(payload.bytes.size());
    result.Bytes(HashBytes(payload.bytes));
    result.Bytes(payload.bytes);
    return std::move(result.bytes);
}

bool Decode(std::span<const std::uint8_t> bytes,
            std::span<const std::uint8_t> text,
            bool external,
            CodeImage& output,
            std::string& error) {
    try {
        Require(bytes.size() <= maximum_bytes && text.size() <= maximum_bytes,
                "code image exceeds its size limit");
        Reader header{bytes};
        const auto signature = header.Bytes(magic.size());
        Require(std::equal(magic.begin(), magic.end(), signature.begin()),
                "invalid code image magic");
        Require(header.U32() == CodeImageFormatVersion, "incompatible code image format");
        Require(header.U32() == (external ? 1u : 0u), "unexpected code image storage layout");
        const auto size = header.U64();
        const auto digest = header.Hash();
        Require(size == header.Remaining(), "code image payload size mismatch");
        const auto payload = header.Bytes(static_cast<std::size_t>(size));
        Require(HashBytes(payload) == digest, "code image checksum mismatch");
        Reader in{payload};
        CodeImage image;
        image.key = Key(in);
        if (external) {
            Require(in.U64() == text.size(), "external native text size mismatch");
            Require(in.Hash() == HashBytes(text), "external native text checksum mismatch");
        }
        const auto count = in.Count(44);
        Require(count <= maximum_units, "too many native units");
        image.units.reserve(count);
        std::uint64_t end{};
        for (std::uint32_t i = 0; i < count; ++i)
            image.units.push_back(Unit(
                    in, text, external, end, image.key.host_isa == 0x8664, image.key.guest_isa));
        Require(in.Empty(), "trailing code image metadata");
        output = std::move(image);
        error.clear();
        return true;
    } catch (const Invalid& failure) {
        error = failure.message;
        return false;
    }
}
}  // namespace

std::vector<std::uint64_t> CodeUnitTextOffsets(const CodeImage& image) {
    std::vector<std::uint64_t> offsets;
    offsets.reserve(image.units.size());
    std::uint64_t size{};
    for (const auto& unit : image.units) {
        size = (size + 15) & ~std::uint64_t{15};
        const auto bytes = unit.code.words.size() * std::uint64_t{4};
        if (size > maximum_bytes || bytes > maximum_bytes - size)
            ThrowError("AOT native text exceeds its size limit", ErrorKind::ResourceLimit);
        offsets.push_back(size);
        size += bytes;
    }
    return offsets;
}

std::vector<std::uint8_t> EncodeCodeImage(const CodeImage& image) {
    try {
        return Encode(image, nullptr, {});
    } catch (const Invalid& failure) {
        ThrowError(failure.message, ErrorKind::InvalidArgument);
    }
}

bool DecodeCodeImage(std::span<const std::uint8_t> bytes, CodeImage& image, std::string& error) {
    return Decode(bytes, {}, false, image, error);
}

std::vector<std::uint8_t> EncodeCodeMetadata(const CodeImage& image,
                                             std::vector<std::uint8_t>& text,
                                             std::span<const std::uint8_t> tail) {
    try {
        return Encode(image, &text, tail);
    } catch (const Invalid& failure) {
        ThrowError(failure.message, ErrorKind::InvalidArgument);
    }
}

bool DecodeCodeMetadata(std::span<const std::uint8_t> metadata,
                        std::span<const std::uint8_t> text,
                        CodeImage& image,
                        std::string& error) {
    return Decode(metadata, text, true, image, error);
}
}  // namespace swift::runtime::execution
