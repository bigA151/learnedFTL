# LearnedFTL cross-group GC experiment

This branch contains an experimental implementation of the cross-group allocation and paired group GC described in LearnedFTL (HPCA 2024). Read [the status report](CROSS_GROUP_IMPLEMENTATION_STATUS_2026-09-28.zh-CN.md) before using results. The 64-job sequential-write workload still collapses after filling the device, so this branch is not a validated Figure 14 reproduction.

`bbssd/ld-tpftl.c`, `bbssd/ld-tpftl.h`, and `bbssd/bb.c` are the source changes tested in the full FEMU checkout at `/home/zheng/FEMU-ext4`. The source change can be reviewed directly in this branch. The physical LPN/generation arrays are diagnostic instrumentation and their memory cost is not part of the paper's stated budget.

The four small evidence directories contain the original fio JSON, FEMU log, method JSON, and available readback or per-second bandwidth files. Run the parser with:

```bash
python3 reproduction/validate_cross_group.py reproduction/evidence/one-hot --require-borrow --require-pair
python3 reproduction/validate_cross_group.py reproduction/evidence/two-hot --require-borrow --require-pair
python3 reproduction/validate_cross_group.py reproduction/evidence/seq64
python3 reproduction/validate_cross_group.py reproduction/evidence/rand64
```

The experiment runner, base guest image, and full FEMU build environment live in the separate local FEMU reproduction workspace. This repository alone is a source and evidence snapshot, not a bootable VM image.

## Detailed repair plan (2026-09-29)

See the [implementation and acceptance plan](WRITE_REPAIR_EXECUTION_PLAN_2026-09-29.zh-CN.md) and the [dated execution record](WRITE_REPAIR_PROGRESS_2026-09-29.zh-CN.md). M1's controlled donor tests and parts of M2–M4 have been executed; the full repair and Figure 14 have **not** passed their acceptance gates.

The current diagnostic builder is in `build_gc_diag.py` and `build_paper.py`; `cross_group_patch.py` supplies the borrow transform. `learnedftl-cross-borrow.patch` is the generated C diff for the latest tested diagnostic build. The runner `run_gc_diag.sh` expects the separate full FEMU checkout, ext4 storage, guest image, SSH key and local environment wrapper. Do not run it from an NTFS checkout or treat the 64-page forced-GC tests as paper-parameter results.
