# fmt source

Copied without source changes from the frozen SwiftVM tree at
`source/externals/fmt` (the snapshot declares `FMT_VERSION=90101`). Headers, library sources, CMake support and
license are retained; upstream tests and documentation are not built here.

SwiftVM uses the compiled formatting library through `base/format.h`.
IR storage and allocation do not depend on its headers.
