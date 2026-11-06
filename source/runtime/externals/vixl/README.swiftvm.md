# SVM exact VIXL subset

Fixed source: SwiftVM git 660b6ba14e3c8b6e6e21dc601fb66072b07b191f,
source/runtime/externals/vixl. This is the committed source, not its frozen WIP.
Per-file BSD notices and LICENSE are retained.

Production builds only the AArch64 Assembler exact encoding layer, operands,
instruction utilities and common support. No MacroAssembler, simulator,
UseScratchRegisterScope, literal/veneer pools, automatic helper calls or
scratch register allocator is included. SVM owns labels/fixups and explicit
DATA. CodeBuffer borrows the caller's single vector<uint32_t>; it has neither
an allocated byte buffer nor a second cursor. Ordinary vector growth emits
no instructions. Instruction legality and feature selection belong to SVM.

The controlled fork removes Label, Literal and pool APIs instead of retaining
an unused second fixup owner. It exposes the existing exact LogicalImmediate
emitter so the selector can reuse one successful immediate decode. The word
sink enforces SVM's 32-bit byte-offset limit; Writer reports the original
CodeTooLarge error at the current operation. The generic C++20 and system
register switch adjustments do not add automatic instruction selection.

The seven production translation units are compiler-intrinsics-vixl.cc,
cpu-features.cc, utils-vixl.cc, aarch64/assembler-aarch64.cc,
aarch64/operands-aarch64.cc, aarch64/instructions-aarch64.cc and
aarch64/cpu-aarch64.cc. Define VIXL_INCLUDE_TARGET_AARCH64 and include this
directory. Every compiled source retains its original BSD notice.

The hot GPR/control/load-store exact definitions live only in
`aarch64/assembler-aarch64-inl.h`; Operand/MemOperand construction and
predicates live only in `aarch64/operands-aarch64-inl.h`. Their former `.cc`
definitions are removed. The small exact path and borrowed-word write are
inlined so constant operand forms fold at the selector call site. Cold
interfaces retain the original translation units; this introduces neither
a parallel encoder nor a new instruction selection policy.
