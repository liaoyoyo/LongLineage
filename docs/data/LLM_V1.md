# LLM V1 Binary Matrix Contract

`bernoulli_upper.llm.bgz` is a BGZF stream of independently checksummed site frames.

Frame layout, little-endian:

1. magic `LLM1` (4 bytes);
2. schema major/minor (`uint16`, `uint16`; exactly `1`, `0`; patch is implicitly
   zero for this physical format);
3. dataset order (`uint32`);
4. site order (`uint64`);
5. matrix dimension (`uint32`);
6. strict-upper value count (`uint64`);
7. invalid-mask byte count (`uint64`);
8. row-major strict-upper IEEE-754 binary64 values;
9. packed invalid mask, least-significant bit first;
10. raw 32-byte SHA-256 over the exact uncompressed bytes of fields 1-9.

The value count is exactly `dimension * (dimension - 1) / 2`, evaluated with checked
integer arithmetic. Values are ordered `(row=0,col=1..n-1)`, then
`(row=1,col=2..n-1)`, and so on. The invalid-mask byte count is exactly
`ceil(value_count / 8)`; bit zero of byte zero describes the first value and unused
high bits in the final byte are zero.

NaN/Inf are forbidden. An invalid entry has a mask bit and a canonical positive-zero
payload (`00 00 00 00 00 00 00 00`); zero without the mask is a valid distance.
Frames are strictly ordered by `(dataset_order,site_order)` and duplicate keys are
invalid. The companion site index virtual-offset range includes the complete frame,
including its 32 checksum bytes, and may not split a frame.

The artifact semantic SHA-256 starts with
`longlineage.bernoulli_upper<TAB>1.0.0<LF>` and then hashes fields 1-9 of every
uncompressed frame in canonical order; it excludes per-frame checksum bytes, BGZF
headers, blocks and EOF. Truncation, wrong endian, count mismatch, nonzero masked
payload or mask padding, checksum mismatch, duplicate/out-of-order key and missing
canonical 28-byte BGZF EOF are validation failures.
