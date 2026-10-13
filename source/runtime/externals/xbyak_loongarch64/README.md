# Xbyak LoongArch64

The headers, `src/` and `LICENSE` are vendored from
https://github.com/loongson/xbyak_loongarch64 at commit
`21af45e2ae09151804c71c6931d50fb212f35f93`.

Local portability fixes add `<cstdint>` and `<cstring>` to the main header and
remove duplicate template default arguments from two definitions. The instruction
encodings are unchanged.

SwiftVM uses the assembler on Linux hosts and for cross-host code generation.
Its local CMake target is `swift_xbyak_loongarch64`. The adapter uses a plain
read/write allocator; executable mappings and instruction-cache publication are
owned by SwiftVM's code cache.
