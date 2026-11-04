# diStorm source provenance

These decoder sources were retained from SwiftVM's vendored diStorm tree.
Their public header identifies version 3.4.0; this integration does not claim
they are a pristine upstream release. The measured source files are unchanged.

The missing `COPYING` is restored byte-for-byte from upstream tag `v3.4.1`,
commit `39de02a118f95e254550a944843ee69f721c79d9`:

https://github.com/gdabah/distorm/blob/39de02a118f95e254550a944843ee69f721c79d9/COPYING

SHA256: `951d8009149f9f7ec2166f731c8b0f7e09859a7b8818697b7d717e140ac1eceb`.

The BSD notice and the existing GPLv3-or-later notice in `decoder.h` are both
retained. The GPL notice is also present in upstream v3.4.1; this integration does not
rewrite or resolve that upstream notice discrepancy.
`COPYING.GPL-3.0` supplies the GPLv3 text from
https://www.gnu.org/licenses/gpl-3.0.txt. No source was relicensed.

Only the byte decoder is built; no legacy SwiftVM frontend or middle pipeline
is linked through this directory.
