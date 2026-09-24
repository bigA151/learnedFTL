# LearnedFTL FEMU device subtree

This repository contains `hw/femu/` from [astlxmu/LearnedFTL](https://github.com/astlxmu/LearnedFTL), based on upstream commit `4702166cd`. It is a **QEMU/FEMU source subtree**, not a standalone build. To build it, place these files at `hw/femu/` within the authors' QEMU 7.0 tree and follow that project's build instructions. The root [FEMU](https://github.com/MoatLab/FEMU) 10.x tree has a different internal API.

## Local experiment changes

Relative to upstream `4702166cd`, this copy contains three source changes:

- `backend/dram.c`: log an `mlock` failure instead of aborting, for hosts with a low locked-memory limit. Timing can then include host page faults.
- `bbssd/bb.c`: expose existing counters through FEMU log lines marked `METRICS`, reset on vendor admin opcode `0xef` / CDW10 `8` and printed on CDW10 `9`.
- `bbssd/ld-tpftl.c`: clarify the 256-block geometry comment.

The algorithm is implemented mainly in `bbssd/ld-tpftl.c` and declared in `bbssd/ld-tpftl.h`. Parameter-specific binaries and experiment scripts are outside this subtree. This repository does **not** contain raw traces, virtual-machine images, or results.

The original QEMU/FEMU license text is retained in [LICENSE](LICENSE); individual files may carry additional notices.
