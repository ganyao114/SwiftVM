# A3 final cold coverage and entry-owner audit

This is the single existing CoreMark20 diagnostic, not a timing baseline. Exit0 and CRC4983 passed; the short-run validation warning is expected.

| Actual diagnostic work | Old baseline | A3 |
|---|---:|---:|
| Attempts / published native owners | 705 / 700 | 686 / 681 |
| Successful guest instruction occurrences | 31,990 | 16,415 |
| Unique guest instructions (separate FP modes) | 16,101 | 16,224 |
| Repeated instruction occurrences | 15,889 | 191 |
| Successful guest byte occurrences / union | 135,659 / 62,142 | 63,295 / 62,642 |
| Accepted front nodes / failed-prefix nodes | 87,595 / 251 | 50,591 / 265 |
| All actual born nodes, including failures | 87,846 | 50,856 |

686 attempts = 681 Function attempts + 5 Block retries. 676 Function units and 5 Block units publish. The 5 frontend failures add 169 emitted-prefix instructions and 265 born nodes; all actual born IR is 50,856. Formation, optimizer and backend failures are zero. Runtime emitted 16,584 instructions includes rejected prefixes; successful-unit coverage is 16,415.

Actual registration: 681 native owners, 2,757 extra source-B entry offers, 3,438 unique ready PCs. Every offered extra was accepted here; no duplicate offer retained an older owner in this run. Root plus extras share the actual native owner range; every extra is a nonempty source-B start, and every such start except the root is offered. No empty B or exit stub is registered.

stop-ready recorded 186 positive callback events across 136 mode/PC keys and 124 attempts. 169 refer to extra entries and 17 to roots. Every address/owner matches an earlier ready registration. Callback counts are not a count of all pruned descendants or saved instructions.

Both actual coverage sets are mode0: no legitimate cross-FP-mode copies are charged as duplication. A3 removes 15,698 repeated occurrences and 36,990 actual born nodes. Unique coverage differs by 123; the full total-work difference is not claimed to come only from reuse. No A3 unit root is inside earlier source coverage and no A3 unit is entirely covered by earlier units; the remaining 191 repeated instructions are retained, not declared eliminated.

Retained high water: IR 153,712 B, words 22,368 B, links 1,024 B, entries 4,096 B; maximum simultaneous 181,200 B. Entry scratch capacity is charged and included.

All coverage is mode-separated; this run's mode set and cross-mode copies are recorded in JSON. Counts and endpoints of every actual source range match the fixed ELF. The trace does not prove a runtime speedup, and ordinary A3 statistics are not spliced into this diagnostic. A3 retains source-B entry registration and stop-ready while leaving CALL continuations for their first actual request. A2 speculative-continuation failures remain separately archived; no A2 run data is combined here.
