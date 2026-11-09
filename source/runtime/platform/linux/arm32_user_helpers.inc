// Generated from arm32_user_helpers.S by tools/arm32/generate_user_helpers.py.
static constexpr std::uint32_t cmpxchg[]{0xf57ff05bu, 0xe1923f9fu, 0xe0533000u, 0x1a000002u, 0xe182cf91u, 0xe35c0000u, 0x1afffff9u, 0xf57ff01fu, 0xf57ff05bu, 0xe2730000u, 0xe12fff1eu};
static constexpr std::uint32_t cmpxchg64[]{0xe92d40f0u, 0xe1c040d0u, 0xe1c160d0u, 0xf57ff05bu, 0xe1b20f9fu, 0xe1500004u, 0x01510005u, 0x1a000004u, 0xe1a23f96u, 0xe3530000u, 0x1afffff8u, 0xe3a00000u, 0xea000001u, 0xf57ff01fu, 0xe3a00001u, 0xf57ff05bu, 0xe2700000u, 0xe8bd80f0u};
static constexpr std::uint32_t entry_cmpxchg64[]{0xeafffc86u};
static constexpr std::uint32_t entry_barrier[]{0xf57ff05bu, 0xe12fff1eu};
static constexpr std::uint32_t entry_cmpxchg[]{0xeafffc4eu};
static constexpr std::uint32_t entry_tls[]{0xee1d0f70u, 0xe12fff1eu};
static constexpr std::uint32_t helper_version[]{0x00000005u};
static constexpr Code code[]{
    {0x100, cmpxchg},
    {0x180, cmpxchg64},
    {0xf60, entry_cmpxchg64},
    {0xfa0, entry_barrier},
    {0xfc0, entry_cmpxchg},
    {0xfe0, entry_tls},
    {0xffc, helper_version},
};
