HostPairResult EarlyPairProbe(u64 first, u64 second, u64 options) { return {first + 17, second ^ options}; }

void ProducerTimePseudoResults() {
    constexpr u64 options = 0x5ffedcba01234567ULL;
    u64 seed = 0x84be227df9990711ULL;
    // Layout 0 delays markers while old scalar homes stay live; layout 1 can
    // skip both markers; layout 2 revisits markers without rerunning producer.
    for (u32 kind = 0; kind < 5; ++kind) for (u32 layout = 0; layout < 3; ++layout)
    for (u32 pressure : {1u, 24u, 64u}) for (bool alias : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xda00})}; ir::Assembler as{block.get()};
        std::vector<ir::Value> keep;
        for (u32 index = 0; index < pressure; ++index)
            keep.push_back(as.LoadUniform(ir::Uniform{128 + index * 8, ValueType::U64}).SetType(ValueType::U64));
        ir::Value producer;
        if (kind < 3) {
            auto first = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
            auto second = alias ? first : as.LoadUniform(ir::Uniform{8, ValueType::U64}).SetType(ValueType::U64);
            auto third = as.LoadUniform(ir::Uniform{16, ValueType::U64}).SetType(ValueType::U64);
            producer = kind == 0 ? as.Cpuid(first, second, ir::Imm{options}, ir::Imm{reinterpret_cast<u64>(&EarlyPairProbe)}).SetType(ValueType::U64) :
                                  as.Div128(first, second, third, ir::Imm{kind == 2}).SetType(ValueType::U64);
        } else {
            auto input = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
            auto other = alias ? input : as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
            producer = as.VecSatMulHigh(input, other, input, ir::Imm{kind == 3 ? 16u : 32u}, ir::Imm{u32{5}}).SetType(ValueType::V128);
        }
        for (u32 index = 0; index < pressure; ++index) as.StoreUniform(ir::Uniform{768 + index * 8, ValueType::U64}, keep[index]);
        ir::Value skip;
        if (layout == 1) {
            auto condition = as.LoadUniform(ir::Uniform{64, ValueType::U8}).SetType(ValueType::U8);
            skip = as.Goto(ir::BOOL{condition});
        }
        const auto pseudo = [&] {
            if (kind == 0) return ir::Value{as.CpuidUpper(producer).SetType(ValueType::U64)};
            if (kind < 3) return ir::Value{as.Div128Remainder(producer).SetType(ValueType::U64)};
            return ir::Value{as.VecSatMulQC(producer).SetType(ValueType::U8)};
        };
        auto first = pseudo(), second = pseudo();
        if (layout == 1) as.BindLabel(skip);
        as.StoreUniform(ir::Uniform{2048, first.Type()}, first);
        auto sum = as.Add(first.SetCastType(ValueType::U64), ir::Operand{second.SetCastType(ValueType::U64)}).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{2056, ValueType::U64}, sum);
        auto bits = as.BitCast(first).SetType(ValueType::V128);
        as.StoreUniform(ir::Uniform{2064, ValueType::V128}, bits);
        if (layout == 2) {
            auto counter = as.LoadUniform(ir::Uniform{72, ValueType::U64}).SetType(ValueType::U64);
            auto next = as.Sub(counter, ir::Operand{ir::Imm{u64{1}}}).SetType(ValueType::U64);
            as.StoreUniform(ir::Uniform{72, ValueType::U64}, next);
            auto condition = as.TestNotZero(next).SetType(ValueType::U8);
            auto back = as.Goto(ir::BOOL{condition}); as.BindLabel(back);
            auto* label = &block->GetInstList().back(); block->RemoveInst(label); block->InsertBefore(label, first.Def());
        }
        // The first pseudo's only final use is in the terminal, after many
        // short-lived definitions have recycled their own homes.
        for (u32 index = 0; index < 128; ++index) {
            auto scratch = as.LoadImm(ir::Imm{u64{index + 19}}).SetType(ValueType::U64);
            as.StoreUniform(ir::Uniform{2080, ValueType::U64}, scratch);
        }
        block->SetTerminal(ir::terminal::If{ir::BOOL{first}, ir::terminal::LinkBlock{ir::Location{0xdb00}}, ir::terminal::LinkBlock{ir::Location{0xdc00}}});
        for (auto features : {x64::HostFeatures{}, x64::HostFeatures::Detect()}) {
            Compiled compiled{block.get(), features};
            for (u32 sample = 0; sample < 12; ++sample) {
                Storage actual;
                const u64 a = sample < 3 ? (sample == 0 ? 0 : sample == 1 ? 1 : UINT64_MAX) : Random(seed);
                const u64 b = sample < 3 ? sample : Random(seed), divisor = sample < 3 ? sample : Random(seed);
                actual.Put(0, a); actual.Put(8, b); actual.Put(16, divisor);
                u64 expected;
                if (kind == 0) expected = (alias ? a : b) ^ options;
                else if (kind < 3) expected = IndependentWideDivision(a, alias ? a : b, divisor, kind == 2)[1];
                else {
                    const u64 input = sample & 1 ? (kind == 3 ? 0x8000 : 0x80000000ULL) : (kind == 3 ? 0x4000 : 0x40000000ULL);
                    actual.Put(0, input); actual.Put(8, UINT64_MAX); actual.Put(32, input); actual.Put(40, UINT64_MAX);
                    expected = sample & 1;
                }
                actual.Put(64, sample & 1); actual.Put(72, 3);
                for (u32 index = 0; index < pressure; ++index) actual.Put(128 + index * 8, 0x1122334400000000ULL + index);
                actual.state->host_cpu_flags = 0xdabcdef123456789ULL;
                const auto mxcsr = _mm_getcsr();
                Check(compiled.fn(actual.state) == HaltReason::None, "producer-defined pseudo terminal halt");
                Check(actual.Get(2048) == expected && actual.Get(2056) == expected * 2, "producer-defined repeated pseudos survive skipped/revisited markers");
                Check(actual.Get(2064) == expected && actual.Get(2072) == 0, "producer-defined pseudo BitCast vector path has canonical upper zeros");
                // BOOL operands expose the low byte, even for a U64 source.
                Check(actual.state->current_loc == ir::Location{u8(expected) ? 0xdb00u : 0xdc00u},
                      "producer-defined pseudo terminal consumer survives home reuse kind=" + std::to_string(kind) +
                      "/layout=" + std::to_string(layout) + "/pressure=" + std::to_string(pressure) + "/sample=" + std::to_string(sample));
                Check(actual.state->host_cpu_flags == 0xdabcdef123456789ULL && _mm_getcsr() == mxcsr, "producer-defined pseudos preserve guest flags and host FP state");
                for (u32 index = 0; index < pressure; ++index)
                    Check(actual.Get(768 + index * 8) == 0x1122334400000000ULL + index, "early pseudo writes cannot overwrite an older live scalar home");
                Check(actual.Get(2080) == 146 && (layout != 2 || actual.Get(72) == 0), "pseudo pressure/loop completes without clobbering retained results");
            }
        }
    }
    std::cout << "PASS all pair/QC producer-time pseudos / skipped+revisited markers / home pressure / terminal+vector consumers\n";
}

void ProducerTimePseudoValidation() {
    for (u32 kind = 0; kind < 3; ++kind) for (u32 variant = 0; variant < 5; ++variant) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xdd00})}; ir::Assembler as{block.get()};
        auto input = as.LoadImm(ir::Imm{u64{7}}).SetType(ValueType::U64);
        ir::Value producer = kind == 0 ? as.Cpuid(input, input, ir::Imm{u64{0}}, ir::Imm{reinterpret_cast<u64>(&EarlyPairProbe)}).SetType(ValueType::U64) :
                             kind == 1 ? as.Div128(input, input, input, ir::Imm{false}).SetType(ValueType::U64) :
                             as.VecSatMulHigh(input.SetCastType(ValueType::V128), input.SetCastType(ValueType::V128), input.SetCastType(ValueType::V128), ir::Imm{u32{16}}, ir::Imm{u32{5}}).SetType(ValueType::V128);
        if (kind == 2) input.Def()->SetReturn(ValueType::V128);
        if (variant == 0) producer = input;
        if (variant == 1) producer.Def()->SetReturn(ValueType::U32);
        auto pseudo = kind == 0 ? ir::Value{as.CpuidUpper(producer).SetType(ValueType::U64)} :
                      kind == 1 ? ir::Value{as.Div128Remainder(producer).SetType(ValueType::U64)} :
                      ir::Value{as.VecSatMulQC(producer).SetType(ValueType::U8)};
        if (variant == 2) pseudo.Def()->SetReturn(ValueType::U32);
        if (variant == 3) { block->RemoveInst(pseudo.Def()); block->InsertBefore(pseudo.Def(), producer.Def()); }
        IntrusivePtr<ir::Block> foreign{new ir::Block(ir::Location{0xde00})};
        if (variant == 4) { block->RemoveInst(producer.Def()); foreign->AppendInst(producer.Def()); }
        block->SetTerminal(ir::terminal::ReturnToHost{}); bool rejected{};
        try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "producer-time pseudo validates opcode/types/locality/order before publication");
    }
    std::cout << "PASS producer-time pseudo matching producer/type/locality/order validation\n";
}
