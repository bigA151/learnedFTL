# LearnedFTL completion and Fig. 14 reproduction

The current implementation and workflow are documented in [README.zh-CN.md](README.zh-CN.md). See [MECHANISM_AUDIT.zh-CN.md](MECHANISM_AUDIT.zh-CN.md) for the paper mechanism audit and [reproduction-state.json](reproduction-state.json) for the live verification state.

The formal matrix contains five FTLs, four FIO workloads and three measurements each. Reads receive six full logical-size matching write prewarm passes; fresh-write repetitions use independent VMs. Post-read writes form a separate supplemental cohort. Scripts validate raw FIO data, source/binary snapshots, job regions and QEMU counter ordering before plotting.

This is a QEMU 7/FEMU device source subtree, requiring the prepared full SDK and guest resources described in the Chinese guide. VM images, keys, binaries and dependency caches are excluded. Historical instructions are preserved in `README.legacy*`; use the current matrix runner, `run_fig14_matrix.py`.

Full plots and the report require all 60 formal windows. Completing the matrix does not itself establish the paper's performance conclusions.
