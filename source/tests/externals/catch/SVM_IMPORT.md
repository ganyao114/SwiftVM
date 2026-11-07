# Catch2 source

Copied without source changes from the frozen SwiftVM tree at
`source/externals/catch`, whose version macros declare Catch2 3.2.1.
The upstream licence and source layout are retained. SwiftVM builds the
compiled Catch2 library only for `BUILD_TESTING`; upstream tests, examples and
installation extras are disabled.

The enclosing test CMake target selects C++20 for both the library and its
consumers. SwiftVM supplies a shared runner to distinguish compile contracts
from actual native execution. Retired IR/HIR fixtures are not imported.
