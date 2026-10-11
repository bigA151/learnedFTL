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
The full Fig. 14 comparison is still in progress. A full-size sequential-write
run exposed a reserve-exhaustion bug that remains under investigation in this
snapshot; this commit must not be described as a completed reproduction.

The workspace retains the raw VM results and immutable binary/source snapshots.
VM images, SSH keys, binaries and dependency caches are excluded from Git.

The original QEMU/FEMU license text is retained in [LICENSE](LICENSE); individual files may carry additional notices.
