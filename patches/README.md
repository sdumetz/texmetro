# Patches

Small fixes applied to submoduled dependencies (`src/thirdparty/*`) that
aren't worth forking the dependency for. Applied automatically by
`src/texmetro.pro` at `qmake` time (idempotent: re-running `qmake` won't
try to re-apply a patch that's already in place).

## vcglib-histogram-tolerance.patch

`vcg/math/histogram.h`'s `Histogram::Percentile()` has a sanity-check assert
comparing two sums of the same values accumulated in a different order (once
incrementally in `Add()`, once by summing the bins). Floating point addition
isn't associative, so on a histogram fed enough samples (e.g. texmetro's
seam-color-discrepancy histogram on a model with many texture-seam edges)
the two sums can differ by a tiny amount and the exact-equality assert fires,
aborting the whole program. The patch relaxes it to a relative-tolerance
comparison.

Not yet reported upstream (cnr-isti-vclab/vcglib). If it gets fixed there and
the vcglib submodule is bumped past that fix, this patch — and the
`system()` call applying it in `texmetro.pro` — can be dropped.

To regenerate after changing the fix: from `src/thirdparty/vcglib`,
`git diff > ../../../patches/vcglib-histogram-tolerance.patch`.
