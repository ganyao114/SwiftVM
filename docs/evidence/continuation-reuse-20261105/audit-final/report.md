# A2 final cold coverage and entry-owner audit

This is the single existing CoreMark20 diagnostic, not a timing baseline. Exit0 and CRC4983 passed; the short-run validation warning is expected.

| Actual diagnostic work | Old baseline | A2 |
|---|---:|---:|
| Attempts / published native owners | 705 / 700 | 281 / 268 |
| Successful guest instruction occurrences | 31,990 | 18,983 |
| Unique guest instructions (separate FP modes) | 16,101 | 18,927 |
| Repeated instruction occurrences | 15,889 | 56 |
| Successful guest byte occurrences / union | 135,659 / 62,142 | 73,686 / 73,417 |
| Accepted front nodes / failed-prefix nodes | 87,595 / 251 | 59,022 / 1,987 |

281 attempts = 268 Function attempts + 13 Block retries. 255 Function units and 13 Block units publish. The 13 frontend failures add 752 emitted-prefix instructions and 1987 born nodes; all actual born IR is 61,009. Formation, optimizer and backend failures are zero. Runtime emitted 19,735 instructions includes rejected prefixes; successful-unit coverage is 18,983.

Actual registration: 268 native owners, 3,990 extra source-B entry offers, 4,258 unique ready PCs. Every offered extra was accepted here; no duplicate offer retained an older owner in this run. Root plus extras share the actual native owner range; every extra is a nonempty source-B start, and every such start except the root is offered. No empty B or exit stub is registered.

stop-ready recorded 74 positive callback events across 41 mode/PC keys and 45 attempts. 58 refer to extra entries and 16 to roots. Every address/owner matches an earlier ready registration. Callback counts are not a count of all pruned descendants or saved instructions.

Retained high water: IR 159,856 B, words 25,784 B, links 1,024 B, entries 4,096 B; maximum simultaneous 190,760 B. Entry scratch capacity is charged and included.

All coverage is mode-separated; this run's mode set and cross-mode copies are recorded in JSON. Counts and endpoints of every actual source range match the fixed ELF. The trace does not prove a runtime speedup, and ordinary A2 statistics are not spliced into this diagnostic. The separate func_tests audit records 30 LOCK-prefix overlap formation failures plus 18 unsupported emission failures; this rejected A2 design remains archived for the next narrower formation comparison.
