#pragma once

#include <memory>
#include <span>
#include "constraints.h"
#include "gpr_write_inputs.h"
#include "width_inputs.h"

namespace swift::runtime::ir {
class Inst;
}

namespace swift::runtime::compile {

// Optional semantic tables owned by the allocation request. Instruction and
// block pointers borrow sealed IR; no physical result or use graph is owned.
struct GPRInputs final {
    enum ConsumerFlag : u8 {
        AllowNoResult = 1,
        ForbidTransfer = 2,
        PinnedWProducer = 4,
        PublicationObserver = 8,
        WidthEmissionSite = 16
    };
    enum ViewFlag : u8 {
        Low32 = 1,
        WidthBridge = 2,
        KnownWWrite = 4,
        CaptureU32 = 8,
        RecolorSource = 16
    };
    struct ReadReuse {
        const ir::Inst* source;
        u16 position, publication, home;
        bool high_zero;
        u8 fixed_class_pools{};
    };
    struct ValueView {
        const ir::Inst* source;
        u32 wrapper{UINT32_MAX};
        u16 position;
        u8 flags, fixed_class_pools;
    };
    struct WPublication {
        u16 instruction, producer, home;
    };
    struct Clobber {
        u16 instruction;
        u32 mask;
    };
    struct ConsumerRule {
        u16 instruction;
        u8 flags;
    };
    struct BlockRange {
        const ir::Block* block;
        u32 first_read, read_count, first_view, view_count, first_clobber, clobber_count;
        u32 first_w_publication, w_publication_count, first_width, width_count, first_step,
                step_count;
        u32 first_write, write_count, first_capture, capture_count;
    };

    [[nodiscard]] const BlockRange* BlockFor(const ir::Block*) const;
    [[nodiscard]] std::span<const ReadReuse> Reads(const BlockRange&) const;
    [[nodiscard]] std::span<const ValueView> Views(const BlockRange&) const;
    [[nodiscard]] u8 ConsumerFlags(u32 instruction) const;
    [[nodiscard]] bool CrossesClobber(const BlockRange&, u32 begin, u32 end, u16 home) const;

    Vector<FixedGPRPublication> publications;
    Vector<ReadReuse> reads;
    Vector<ValueView> views;
    Vector<WPublication> w_publications;
    Vector<Clobber> clobbers;
    Vector<ConsumerRule> consumer_rules;
    Vector<BlockRange> blocks;
    std::unique_ptr<const WidthInputs> width;
    std::unique_ptr<const GPRWriteInputs> writes;
};

}  // namespace swift::runtime::compile
