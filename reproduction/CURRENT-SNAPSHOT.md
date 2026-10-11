# Current snapshot, 2026-10-12

The full Fig. 14 reproduction has not completed. The source passes prior
native and FEMU CRC/GC smoke checks, but a new full-size sequential-write run
fails during the second measurement window with insufficient paired-GC reserve.
A 64-job partial-pass native workload reproduces the failure. This evidence
is a correctness issue to repair, not a performance result to plot.

The user requested this intermediate code push, a second code push after the
reproduction if code changes, and Fig. 14-style plots of all measured cases.
