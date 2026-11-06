Cephes binary128 subset from FEX's `External/cephes` directory at commit
[`a156752fae3a4f65fefb1ed95af980bcdfac43d7`](https://github.com/FEX-Emu/FEX/tree/a156752fae3a4f65fefb1ed95af980bcdfac43d7/External/cephes)
(Git tree `f8507f4489eca755d464904ce3e5aade006aee34`). The original author and
FEX conversion copyright notices remain in the sources; see LICENSE.

The nine C sources use SoftFloat binary128 values and local arithmetic state.
The FEX C++ namespace wrapper is replaced by a C declaration header. The unused
global merror declaration is removed; mtherr remains a no-output, stateless
compatibility entry. x87 architectural exceptions and the full input-range
reduction belong to arch/x86/x87_transcendental.cpp, not to Cephes error output.
