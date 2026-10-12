# LearnedFTL FEMU device subtree

This repository contains `hw/femu/` from [astlxmu/LearnedFTL](https://github.com/astlxmu/LearnedFTL), based on upstream commit `4702166cd`. It is a **QEMU/FEMU source subtree**, not a standalone build. To build it, place these files at `hw/femu/` within the authors' QEMU 7.0 tree and follow that project's build instructions. The root [FEMU](https://github.com/MoatLab/FEMU) 10.x tree has a different internal API.

## Paper mechanism completion and reproduction

The active implementation is in `bbssd/ld-tpftl.c` and `bbssd/ld-tpftl.h`.
It includes the fixed 128-byte model, quantized prediction bitmap checks,
sequential model replacement, group GC, cold-tail borrowing and paired GC,
cache consistency, and NAND dependency accounting.

`reproduction/` contains the mechanism audit, native invariant tests, FEMU
CRC/GC smoke runner, paper workload runner, and baseline source repair patch.
See `reproduction/reproduction-state.json` for the current verification status.
The full Fig. 14 comparison is still in progress. 45 formal measurement
windows have passed raw-result validation: all 30 fresh-write windows and
all 15 sequential-read windows. Fifteen separately labeled post-read
sequential-write windows are also complete. Random-read combinations remain
unfinished; an interrupted prewarm has been excluded and will be restarted
in a fresh VM. Each read case requires six full logical-size matching prewarm
passes before three formal measurements. Supplemental post-read writes do
not replace the fresh-write cohort.

The GC reserve exhaustion fix passed capacity-pressure regressions. A later
GC timing fix stores each page's source-read completion time, avoiding
artificial dependencies on subsequent destination writes. The full-geometry
64-request regression retained identical GC/program counts and reduced the
maximum virtual queued latency from 180.63 s to 9.55 s. Native invariant and
sanitizer regressions, a dense random preflight and FEMU CRC/GC smoke pass.
See `reproduction/README.zh-CN.md` for the current workflow and
`reproduction/verify_gc_read_dependencies.py` for the saved evidence check.
Measured figures and the final report remain pending the complete read
matrix; this is not yet a completed performance reproduction.

The workspace retains the raw VM results and immutable binary/source snapshots.
VM images, SSH keys, binaries and dependency caches are excluded from Git.

The original QEMU/FEMU license text is retained in [LICENSE](LICENSE); individual files may carry additional notices.
