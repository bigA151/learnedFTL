# LearnedFTL（HPCA 2024）复现：实习生操作手册

> **2026-09-29 地址配置更正（覆盖本手册下方的旧 `filesize=30518m` 操作建议）：** fio 官方文档和客机只读地址日志证明，全局 `filesize=30518m` 会覆盖 `size` 的每任务区域边界；仅删除它又会让并发任务重复读单页。正式脚本现为 64 个任务分别写出 `offset` 与 `filesize=offset+476 MiB`，保存任务文件及哈希。旧 18 项成功退出的数据现在均标为**未完成**，需要从新虚拟机重做，不能入图。具体证据和验收见 [fio 地址范围核验](FIO_ADDRESS_LAYOUT_2026-09-29.zh-CN.md)。

> **2026-09-29 写入修复计划：** [详细实施与验收计划](WRITE_REPAIR_EXECUTION_PLAN_2026-09-29.zh-CN.md) 已制定，部分诊断机制和脚本已实施，但顺序写仍未解决。按阶段通过条件推进；旧 `complete` 状态不能代表新机制已通过。

> **本机只从 `/home/zheng/FEMU-ext4` 运行实验。旧 NTFS 目录保持只读。** 从本仓库根目录执行命令。先读「安全边界」和「正常／异常」。本手册以 **图 14 的五种 FTL、四种 FIO 负载**为可执行主线；其余图的现状和缺口列在末尾。**已有诊断图不是论文复现图；图 14 的写测试初始状态在论文中没有写明，当前六轮写预热属于额外实验选择。**

## 1. 安全边界与预计耗时

- 2026-09-25 内核崩溃报告显示 QEMU 向 NTFS 写入时触发 `iomap_write_end → ntfs_file_write_iter` 的 kernel BUG；现场 `write(fd=2)` 更像日志写入，不能断言当时写的是 qcow2。正式与诊断脚本现拒绝在 NTFS 上启动。崩溃中断的测量无效，原 NTFS 保持只读；所有新盘、日志和结果写入 ext4。
- 实验虚拟机中的 `/dev/nvme0n1` 是 FEMU 模拟盘；脚本对它做整盘写入，会覆盖其中任何数据。不要在这个盘上存文件。宿主机仓库数据不作为 FIO 目标。
- 需要 Linux x86-64、可用的 `/dev/kvm`、约 **45 GiB 内存加交换空间**，并预留数十 GiB 磁盘空间。当前机器具备约 45 GiB 内存和 8 GiB swap。FEMU 会把约 32 GiB NAND 放在宿主机内存里；若内存不足，系统可能严重交换或被 OOM 杀掉。
- 图 14 有 5×4＝20 项。每项独立启动虚拟机、预热约 192 GB 写入、测量 3×60 秒；完整运行可能需要**数小时至数天**，垃圾回收阶段可能特别慢。请看实际进度，不要按固定分钟数判定失败。
- 同时只能有一个使用本仓库 `images/guest-overlay.qcow2` 与本机 2222 端口的实验。当前如果已有实验在运行，**先等它结束**；不要启动下面的批量脚本。

## 2. 文件与一次性准备

| 文件 | 用途 |
| --- | --- |
| `reproduction/run_paper_fig14.sh` | 图 14 全流程：启动 FEMU、预热、测量、检查 |
| `reproduction/build_paper.py` | 按论文参数编译五种 FTL 的专用 FEMU |
| `reproduction/run_fio.py` | 单次 FIO、64 任务显式地址范围与 FEMU 计数器采集；由主脚本调用 |
| `reproduction/plot_fig14.py` | 从实际 CSV 画图，严格模式会拒绝不完整数据 |
| `reproduction/download_traces.sh`、`trace_to_fio.py`、`run_trace.py` | 公开 trace 下载、转换和单条回放 |
| `reproduction/results/` | 原始 FIO JSON、计数器、状态与图；**不要手改** |
| `reproduction/FIO_ADDRESS_LAYOUT_2026-09-29.zh-CN.md` | fio 地址范围修正的证据、旧数据作废原因与新任务文件验收 |
| `reproduction/README.md` | 技术背景和早期诊断记录，主操作以本手册为准 |

在仓库根目录核查：

```bash
pwd
ls -l /dev/kvm reproduction/images/guest-overlay.qcow2 reproduction/images/seed.iso reproduction/images/guest_ed25519
ls -l reproduction/bin/qemu-{learnedftl,dftl,tpftl,leaftl,ideal}-paper-full
bash reproduction/run_paper_fig14.sh --help
bash reproduction/run_paper_fig14.sh --status
```

`pwd` 应是这个 FEMU 仓库；五个二进制均应存在且可执行。缺二进制时从仓库根目录编译。**即使从 ext4 启动，复制来的 Meson 构建缓存也可能保留旧 NTFS 绝对路径；构建脚本现在会预检并拒绝这种缓存。**本机已在 ext4 重新配置 `reproduction/upstream/build-femu`，旧缓存保留为 `build-femu-ntfs-cache` 供追查：

```bash
python3 reproduction/build_paper.py --help
python3 reproduction/build_paper.py
```

编译日志在 `reproduction/bin/`。缺虚拟机镜像、种子 ISO 或 SSH 密钥时，先看仓库根目录的 `LOCAL_SETUP.txt` 和 `reproduction/README.md`；不要把另一个项目的系统盘直接覆盖到这里。当前宿主机依赖在 `.local-root/`，`femu-local-env.sh` 会提供本地运行环境。

## 3. 运行图 14

确认当前没有 FEMU 实验使用 2222 端口后，在仓库根目录执行：

```bash
set -o pipefail
bash reproduction/run_paper_fig14.sh 2>&1 | tee reproduction/results/paper_fig14-console.log
```

主脚本依次运行 `learnedftl dftl tpftl leaftl ideal`，每个算法运行 `randread read randwrite write`。每项都启动一台新的 FEMU 虚拟机；读测试先用相应的随机／顺序 512 KiB 写预热六盘，随后执行 4 KiB、`psync`、64 作业的 FIO 三次测量。当前脚本也对写测试独立六盘预热，确保算法之间的初始状态可比较；**论文只明确规定读测试约六轮预热，未写明写测试初始状态，因此这项写预热是额外选择，不能称为论文明确要求。**测试盘暴露约 32.00 **十进制 GB**。

开另一个终端查看进度（只读，不会启动虚拟机）：

```bash
bash reproduction/run_paper_fig14.sh --status
```

`完成` 表示预热 JSON、方法记录和三次测量摘要通过检查；`未完成` 表示已开始但尚未成功结束；`失败` 需要看对应错误文件。中断后继续：

```bash
set -o pipefail
bash reproduction/run_paper_fig14.sh --resume 2>&1 | tee -a reproduction/results/paper_fig14-console.log
```

续跑会**跳过已完成且原始记录完整的项**，未完成项从该项的全新虚拟机与六盘预热重新开始。按 `Ctrl+C` 时脚本会尝试关闭它自己启动的虚拟机。若宿主机突然断电或脚本被强杀，请确认旧 FEMU 进程已结束，再运行 `--resume`。

全部 20 项完成后可画当前方法的完整图；写测试必须注明六轮预热假设：

```bash
PYTHONPATH=reproduction/.python /home/zheng/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3 \
  reproduction/plot_fig14.py --variant-suffix=-paper-full --require-complete \
  --output reproduction/results/fig14-paper-full.png
```

同时生成同名 PDF。`--require-complete` 拒绝缺项或不足三次重复的数据；不要删掉它来声称完整复现。如果 Python 环境换了，先用 `python3 -c 'import matplotlib,numpy'` 检查，缺包时将包装到 `reproduction/.python`，再用设置 `PYTHONPATH` 的 Python 画图。当前机器上上述 bundled Python 已装好。

**只做流程冒烟检查**可用 `VARIANTS=ideal PATTERNS=read WARMUP_PASSES=1 REPEATS=1 RUNTIME_SECONDS=8`，但会覆盖该项原始路径，因此请在正式运行前做；做完删除该项的 `reproduction/results/paper_fig14/ideal/read/`，正式运行会重新生成。冒烟结果只能标为诊断，不能用于论文图。

## 4. 什么情况正常，什么情况异常

旧顺序读约 180 MiB/s 的地址生成问题、A/B/A 结果为何误导、修正后的对照与重跑检查见 [`SEQ_READ_ALIGNMENT_EXPLAINED.zh-CN.md`](SEQ_READ_ALIGNMENT_EXPLAINED.zh-CN.md)。

| 现象 | 判断与操作 |
| --- | --- |
| 日志出现 `starting clean FEMU`、`6-pass ... prewarm`、`3 FIO repetitions` | 正常的三个阶段；预热可能很久没有终端输出，检查虚拟机与客体 FIO 进程、设备写入扇区是否仍在增加；`warmup.json` 常在预热结束后才写入。 |
| FEMU 日志出现 `GC happens?`、`Model Training...`、`mlock failed` | 可能是正常运行信息。`mlock` 是当前主机锁内存限制引起；只要虚拟机、预热字节数继续前进，可继续观察。 |
| 状态为 `未完成`，虚拟机和 FIO 仍在运行 | 正常进行中。不要并行启动第二份脚本。 |
| `localhost:2222 已被占用` | 已有虚拟机或其他服务占用端口；查明并等待正在运行的实验完成。不要直接杀不明进程。 |
| `Boot failed`、SSH 连接失败 | 查 `reproduction/images/qemu-<算法>-<负载>-paper-full.log` 与对应 `FAILED.txt`；确认 `/dev/kvm`、镜像/密钥和 2222 端口，解决后 `--resume`。 |
| `Warmup failed`、字节数不等于六倍盘容量 | 此项**不合格**。查该项 `warmup.err`、`warmup.json`、FEMU 日志和宿主机可用内存/磁盘；解决后 `--resume` 从头跑该项。不要跳过预热。 |
| `FIO repetition ... failed` 或虚拟机崩溃 | 此项**不合格**。保存 `runN.err` 和 FEMU 日志，检查 `dmesg` 的 OOM 记录、空闲内存和磁盘；解决后 `--resume`。LeaFTL 发布代码在 4 KiB 随机写曾崩溃，重现时如实记为未复现，不要补造结果。 |
| 图 14 绘图提示 `Incomplete Figure 14` | 正确保护机制。先 `--status` 找缺项，修复并续跑；无法完成则只能输出明确标为“部分/诊断”的图。 |
| 长时间明显降速 | 垃圾回收可能很慢。看 FIO 进程、`/sys/block/nvme0n1/stat` 的写入扇区数是否持续增加；若持续增加就记录时间并继续。若进程退出或计数长时间不变，查错误日志和 OOM。 |
| 旧图出现约 180 MiB/s 的顺序读，或 CMT 某 LPN 在短时测试被读数万次 | 检查原始 fio JSON 是否有 `job options.filesize=30518m`。旧脚本漏掉 `filesize`；fio 3.16 在多作业 `size+offset_increment` 下反复读单一 4 KiB 地址。仅错开 4 KiB 会分散热点，却没有修正顺序扫盘。**所有旧图 14 的 fio 测量均需重做。** 见 `SEQ_READ_ALIGNMENT_EXPLAINED.zh-CN.md`。 |
| 下载 trace 失败 | 用 `reproduction/traces/download.log` 找失败 URL/状态；重新执行下载脚本以续传/重试，检查代理、DNS、证书及空闲磁盘。先前缺少宿主 `bzip2` 已通过 `.local-root` 的本地副本解决。 |

**2026-09-25 更正：先前只将顺序读旧结果判为过期是不够的。fio 自身的 I/O 地址日志证实旧命令缺少 `--filesize=30518m`，使多作业请求重复访问单一块；此前 16 组结果及 `fig14-16of20` 的吞吐、IOPS、命中率等均不能作为有效论文复现。修复版 `--status` 将旧组标为 `未完成`；不能复用旧图。错开 4 KiB 仍保留作为对照设置，但需要与显式 `filesize` 同时使用，并用地址日志验证。**

**不要把「命令退出码为 0」单独当成成功**；必须同时看到 `method.json` 的 `status: complete`、预热 `io_bytes` 正确、三次原始 FIO JSON 有效，正式图也通过严格绘图检查。

### 顺序读地址与 CMT／模型使用日志：短时诊断

当需要检查顺序读是否集中到少数 NAND 单元，以及哪些 LPN 的 CMT/模型被频繁使用时，从仓库根目录执行：

```bash
./femu-local-env.sh python3 reproduction/build_read_diag.py
bash reproduction/run_read_diag.sh
```

这是**独立诊断实验**，不会覆盖正式二进制。脚本先新建 COW 客体盘，执行与正式读测试相同的六盘顺序写预热，然后在同一台 VM 上测 1 轮旧间隔 `476m`、3 轮修正间隔 `487428k`；每轮默认 10 秒、4 KiB、`psync`、64 作业。默认运行可能因六盘预热耗时数分钟。可用 `ROUNDS=3 RUNTIME_SECONDS=10 bash reproduction/run_read_diag.sh` 显式指定短测轮数和时长；不要把改过预热轮数的结果称为论文参数实验。开始前确认 `bash reproduction/run_paper_fig14.sh --status` 对应没有正在运行的 VM，且端口 2222 空闲。预热会覆盖**新建的 FEMU 测试盘**，不要在盘上保存有价值的数据。

终端打印 `Output: .../read-log-时间戳/` 后，到该目录查看：

| 文件 | 内容与正常判断 |
| --- | --- |
| `warmup.json`、`warmup.err` | `jobs[0].error=0`，默认写入 `192002654208` 字节才算预热成功。预热过程中 JSON 要等命令结束才写完；进程仍在且扇区计数增加属于正常。 |
| `baseline/fio.json`、`shifted-1/fio.json` 等 | 各轮 `error=0`、`runtime≈10000 ms`；两组都必须有 `filesize=30518m`，并检查前 100 条地址是否随顺序读推进；旧间隔是否仍明显较慢要看修正后的实测，不预设结果。 |
| 每轮 `first100.csv` | 复位后**全局最先完成地址记录的 100 次有效 4 KiB 数据页读**，含 LPN/LBA、通道、LUN、块、页。它不保证每个 fio 作业各占一定数量，因此不能单靠这 100 行判断总体是否集中。 |
| 每轮 `lun_usage.csv` | 64 个 `(ch,lun)` 的**全部有效数据读**次数和累计模拟等待时间；这是判断请求是否集中到少数单元的主证据。等待时间是模拟读延迟超过基础 40 µs 的部分。 |
| 每轮 `summary.csv`、`histogram.csv`、`top20.csv`、`bottom20.csv` | 按 LPN 统计 CMT 命中、模型尝试、模型命中、bitmap 过滤后实际读取翻译页、模型预测失败后实际读取翻译页，以及模型不可用后读取翻译页。这些计数先保存在内存中，每轮结束一次性输出；只有前 100 条地址也在结束时统一写出。bitmap 过滤意味着跳过模型预测，**仍会读取翻译页**，不能解释成省掉了一次 NAND 读。直方图的 `uses=N,lpns=M` 表示有 M 个 LPN 各被使用 N 次；`zero_lpns` 包括未访问的 LPN；`min_positive` 是**至少用过一次的最少次数**。Top/Bottom 列出对应的 LPN 和次数，不是缓存物理槽位编号。 |
| 每轮 `diagnostic.log`、`report.err`、`reset.err` | 原始诊断日志与管理命令错误。需有 `READ_DIAG_BEGIN`、100 行地址、64 行 LUN、六个统计摘要、`READ_DIAG_END`，且日志能解析成 CSV。 |
| `comparison.csv`、`method.json` | 前者并列展示各轮速度、最热 LUN 占比及 CMT/模型分类；后者只在所有轮次和日志成功时写 `status: complete`，并记录任务文件地址布局版本。 |

**异常处理：**构建失败先看 `reproduction/bin/build-learnedftl-read-diag.log`；启动失败看输出目录的 `qemu.log`、`guest-serial.log`；预热失败看 `warmup.err`、`warmup.json`；缺少 100 行地址、64 行 LUN 或 `READ_DIAG_END` 时该轮不合格，保留完整日志后重跑脚本。脚本为每次运行创建不同的客体盘和目录，不会把前次日志覆盖。上述计数仅覆盖每轮 fio 测量，预热和管理命令不会混入；不同轮次会清零。详细机制见 [`SEQ_READ_ALIGNMENT_EXPLAINED.zh-CN.md`](SEQ_READ_ALIGNMENT_EXPLAINED.zh-CN.md)。

### 原始 fio 地址核验（1 秒短测）

在 ext4 根目录、确认 2222 端口没有正在运行的实验后执行：

```bash
bash reproduction/verify_fio_address_range.sh
```

该脚本独立新建 COW 客体盘，用 fio 自身的 `write_iolog` 保存每次读的字节偏移和原始 JSON：两作业分别测试缺少/增加 `filesize`，再以 64 作业检查修正版。末尾输出每组总读数、不同地址数、最大重复次数；修正版应看到不同地址数接近总读数、最大重复次数为 1。缺 `filesize` 的复现组预期有单一地址反复出现。它只检验负载地址生成，**没有六盘预热，也不测论文吞吐量**。输出目录为 `reproduction/results/diagnostics/fio-address-check-时间戳/`；本机一次完整核验记录在 `reproduction/results/diagnostics/fio-address-check-20260925T131508Z-1706471/`：无 `filesize` 时 1,999 次读只访问 1,001 个地址，最大重复 999 次；加上后 1,999 次读有 1,999 个地址，64 作业抽样 6,464 次读有 6,464 个地址。若启动失败看 `qemu.log` 和 `serial.log`，fio 失败看每组 `.err`；不要用失败组作结论。

### 本机已踩过的坑：症状、证据、处理

本次五组失败的原始证据、GDB 栈、修复边界与仍未解决的 LearnedFTL 写性能问题见 [`FAILURES_2026-09-26.zh-CN.md`](FAILURES_2026-09-26.zh-CN.md)。**`--status` 的“完成”只表示记录完整，不保证数值合理。**

| 坑 | 实际证据与容易误判的地方 | 检查和补救 |
| --- | --- | --- |
| 从 ext4 启动，但 Meson 仍指向 NTFS | 迁移时把 `reproduction/upstream/build-femu` 原样复制，`config-host.mak` 的 `SRC_PATH`、`build.ninja` 的 Meson 命令和依赖路径仍写死旧 NTFS。实际构建报 `Read-only file system`；这时诊断和正式实验都**尚未启动**，不能把旧二进制或旧结果当成本次输出。 | 检查 `rg "^SRC_PATH=" reproduction/upstream/build-femu/config-host.mak` 是否为 `/home/zheng/FEMU-ext4/reproduction/upstream`；旧缓存移开保存后，用 `femu-local-env.sh` 从 ext4 重新运行 QEMU `configure`，再重新编译。共享构建预检会在改动源码前拒绝仍带旧路径的缓存。核对 `reproduction/bin/configure-ext4.log` 与新构建日志。 |
| 在 NTFS 上运行 QEMU | 2026-09-25 主机内核报告：`iomap_write_end → ntfs_file_write_iter → vfs_write` 触发 `kernel BUG`。现场是 QEMU 的 `write(fd=2, 71 字节)`，更像重定向后的标准错误日志；**不能声称那一笔写的必然是 qcow2**。以前同分区上也出现 qcow2 `fallocate(0x10) is not supported` 告警。 | 只在 `/home/zheng/FEMU-ext4` 运行；`findmnt -T "$PWD"` 必须显示 ext4；原 NTFS 保持只读。正式与诊断脚本已拒绝 `ntfs3/ntfs/fuseblk`。详见 [`NTFS_CRASH_2026-09-25.zh-CN.md`](NTFS_CRASH_2026-09-25.zh-CN.md)。 |
| 把崩溃中断的结果或可解析镜像当作成功 | `qemu-img check` 只能证明镜像容器结构可解析，不能证明最后一次写入数据完整；fio JSON 可能在中断时只写了一部分。 | 检查 `method.json: status=complete`、预热字节、每轮 `error=0`、运行时长和脚本版本。中断项从新 VM 与预热重跑。 |
| 只看吞吐量、IOPS 或 `fio error=0` | 旧 fio 请求可以成功、数值也稳定，但实际地址反复命中单页；吞吐、IOPS 的异常不能直接归咎于 FTL，更不能据此判断论文有造假。 | 先保存原始 fio JSON 与 `write_iolog`，核对请求地址分布、预热、二进制和计数器，再做性能归因。修正后的短测旧间隔约 1321 MiB/s、错开间隔约 1332–1368 MiB/s；完整正式结果仍待重跑。 |
| fio 未指定整体设备 `filesize` | 旧命令同时用了 `size=476m`、`offset_increment`、64 作业和 `time_based`，却没有 `filesize`。fio 3.16 自身的地址日志中，2 作业 1 秒共 1,999 次读只有 1,001 个不同地址，一个 4 KiB 地址重复 999 次；增加 `filesize=30518m` 后，同样 1,999 次读对应 1,999 个地址。64 作业的限速抽样中，修复后的 6,464 次读也全部不同。**旧图的“顺序读”并非预期的大范围顺序扫盘。** | **此句是 2026-09-25 的旧建议，已被 2026-09-29 更正取代。** 新脚本必须按任务单独给出 `offset` 与 `filesize=offset+范围`。核对原始 fio JSON 的 `job options.filesize`，并用 fio `write_iolog` 或诊断的 `first100.csv` 检查地址推进。任何旧的 16 组图 14 结果都要重跑。 |
| 仅把相邻作业起点错开 4 KiB | 旧命令下，`476m → 487428k` 曾把 LearnedFTL 从约 184 MiB/s 提高到约 1,578 MiB/s；但当时大多数作业仍反复读各自的一个块。提高的数值证明热点被分散，**不证明论文的顺序读已复现**。 | 保留旧间隔与错开间隔作为历史诊断对照；正式重跑必须使用各任务独立终点，再看真实的 LUN 分布、地址序列和吞吐量。旧的“只需错开”解释已撤回。 |
| 只看最先 100 条物理地址 | 作业启动时间不同，最早 100 条可偏向少数线程；旧实验中其中大量落在 `(ch=0,lun=0)`，但只有全量计数才能定量。 | 同时查看 `first100.csv` 和 `lun_usage.csv`；后者覆盖整轮全部有效数据读，并记录每个 LUN 的模拟排队等待时间。 |
| 把 bitmap 过滤当作省掉翻译页读取 | 发布代码中 `bitmap=0` 会跳过模型预测，之后仍走 `process_translation_page_read()`；模型预测失败也会走这条路径。模型训练标记、模型尝试、模型正确命中不是一个计数。 | 每轮分开看 `model_hit`、`bitmap_filtered_translation`、`model_miss_translation`，另看 `model_unavailable_translation` 和 `model_attempt`。日志仅在读测量期间计数，结束时统一导出 CSV。 |
| LeaFTL 四组预热在首次 GC 崩溃 | GDB 在 GC 擦除分支抓到 `fprintf(gc_fp, ...)` 的 `gc_fp=NULL`；作者把日志写到本机不存在的 `/home/lzh/femu/log/gc_frequency.txt`。四次主机日志均为 QEMU 段错误，不是 SSH 故障。 | 专用构建已给可选日志加空指针保护，并修正两处独立的 `quick_sort` 闭区间越界；两种修改都要在方法记录中披露。修订版单独通过了六盘顺序预热，四组正式重跑仍须逐项验收。 |
| LearnedFTL 顺序写报 free line 耗尽 | 六轮预热后曾在 4 KiB 写中 QEMU 段错误；诊断复现时 free 队列为空但计数为 1，victim 从 17 增至 240。源码低空闲回退漏设 `vl`，会跳过回收；另一个分支可把已在 victim 队列的当前 line 重复入队。原版成功轮次每 550 次用户 4 KiB 写触发 550 次 GC、约 1756 万次 GC 页写，说明性能低另有整 line 搬迁原因。 | 用独立 `build_gc_fixed.py` 构建修复候选版，并按下文的全新镜像流程验证。强制边界分支和两次无插桩复测通过，但写速仍约 8 IOPS；不要将候选版数据混入原版图 14。详见 `GC_ROOT_CAUSE_2026-09-26.zh-CN.md`。 |
| 把旧图或旧状态误当为正式复现 | 旧图画的是实际测得的数据，但负载生成方式有缺陷；图形本身能生成、`fio error=0`、速度稳定都不足以证明方法正确。 | `bash reproduction/run_paper_fig14.sh --status` 会因旧任务文件版本与哈希缺失而标为未完成。保留旧图只作诊断；四种算法需同一正确命令、各自六盘预热、三轮 60 秒后重新绘图。 |

旧的 4 KiB 随机读绝对速度、写入异常和 LeaFTL 崩溃还有各自的问题；这里的顺序读负载修复**不能自动解释或修复其他负载**。如果看到新的异常，先保存原始 fio JSON、I/O 地址日志、QEMU 日志和方法记录，再改参数；一次只改一个变量，以便判断因果。

## 5. 论文参数、补充设置与差异

| 项目 | 论文或源码依据 | 本脚本具体值／差异 |
| --- | --- | --- |
| NAND 几何 | 8 通道×8 way、每芯片 256 块、每块 512 页、4 KiB/页 | 五个专用二进制对齐该几何；物理量 32 GiB（约 34.36 十进制 GB） |
| 逻辑容量与预留 | 论文称逻辑 32 GB、预留 2 GB；与上述几何单位不完全自洽 | `devsz_mb=30518`，逻辑 32,000,442,368 字节，预留约 2.36 十进制 GB；这是明确记录的折中 |
| NAND 时延 | 读 40 µs、写 200 µs、擦 2 ms | 使用作者 FEMU 源码对应默认值；构建时不额外改时延 |
| 映射缓存 | LearnedFTL 1.5%；DFTL/TPFTL 3%；LeaFTL 与传统缓存等内存 | 专用二进制设置相应比例；LeaFTL 按结构体字节数近似等额，条目个数并不相同 |
| LeaFTL 翻译页 | 论文描述 512 项 | 作者发布代码使用 256 项；改为 512 需重做算法相关结构，目前**未严格对齐** |
| 模型分段 | 论文 8 段 | 保持作者发布算法设定 |
| FIO 测量 | 4 KiB、`psync`、64 线程、随机/顺序读写、至少三轮 | 64 `numjobs`、每轮 60 秒、三轮；**60 秒是脚本选择，论文未给单轮时长** |
| 预热 | 512 KiB 随机/顺序写约六盘；读测试各用相应模式 | 每项六盘；随机预热用 `norandommap=1,randrepeat=0`。**论文未说明随机映射策略**，这里允许重复命中，以免发布代码在满盘后垃圾回收近乎停滞 |
| 测试范围 | 论文未公开具体 FIO 分区方式、随机种子和运行顺序 | 为每个作业分别指定 `offset` 与 `filesize=offset+476 MiB`，64 作业各用 `floor(30518/64)=476` MiB 独立区域；顺序读的相邻起点间隔为 476 MiB + 4 KiB，使起点依次错开一页，64 个区域仍不重叠且都在盘内。其他负载间隔为 476 MiB。**旧全局 filesize 短测不能证明独立区域性能；**论文未说明任务起点，这只是记录在案的脚本设置，不能说错开本身带来性能提升；`randrepeat=0`；固定脚本所列算法/负载顺序。每项新虚拟机，写盘内容不跨项保留 |
| 宿主内存 | 论文未说明页锁定失败处理 | 本机 `mlock` 上限约 8 MiB；作者代码做了仅日志告警的宿主适配，可能带来页缺失干扰 |
| 吞吐、命中、写放大 | 图 14 三面板 | 原始 FIO 吞吐和 FEMU 计数器计算；图是我们的重新测量，不能先验保证数值等于论文 |

作者仓库版本为 `astlxmu/LearnedFTL` 提交 `4702166cd`，与本仓库根目录 FEMU 10.1 的接口不同；专用二进制用作者的 FEMU/QEMU 7.0 源码编译。源代码和论文中未公开的设定已经在表中标出。改变预热次数、测量时长、二进制或容量后，必须把结果标成新实验，不得混入正式图。

## 6. 收集的数据及如何核验

每项目录 `reproduction/results/paper_fig14/<算法>/<负载>/` 包含：

- `method.json`：容量、预热模式/次数、FIO 设置、完成状态；这是该项的方法记录。
- `warmup.json` / `warmup.err`：FIO 预热原始 JSON 与标准错误；`write.io_bytes` 应为 `30518 × 6 × 1048576 = 192002654208` 字节，且 `jobs[0].error=0`。
- `run1-console.json` 至 `run3-console.json`、`runN.err`：每轮吞吐、IOPS、读写字节、P99 及 FEMU 统计摘要/错误。
- `FAILED.txt`：最近失败原因；成功续跑会移除它。

每轮更完整的原始 FIO JSON 与计数器 JSON 在 `reproduction/results/<算法>-paper-full/<负载>/runN.json` 和 `runN-metrics.json`。汇总表是 `reproduction/results/fio_runs.csv`，按算法、负载、轮次去重。FEMU 统计中包含映射访问/缓存/模型命中、NAND 读写擦次数和估算能耗；图 14 使用 FIO 吞吐、读命中比例与 NAND 页写次数/主机 4 KiB 写次数。**估算能耗不是论文 NANDFlashSim 图 22 的等价结果。**正式图片在 `reproduction/results/fig14-paper-full.png` 和 `.pdf`；保存原始 JSON、方法记录、脚本版本与日志，才能审计图片。


## 旧 16/20 部分结果图：仅供诊断，禁止作为论文复现

旧脚本生成过 DFTL、TPFTL、LearnedFTL、Ideal 四种 FTL 的 16 项数据，但 **fio 命令缺少 `filesize`，这些数据在当前验收规则下全部未完成**。以下命令只用于查看历史错误如何影响结果，不能将图片或 CSV 用作论文复现。运行 `PYTHONPATH=reproduction/.python /home/zheng/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3 reproduction/plot_partial_fig14.py` 可重建 `reproduction/results/fig14-16of20-overview.png/.pdf` 和七张单项指标图；三次测量的均值/样本标准差在 `fig14-16of20-aggregates.csv`。LeaFTL 四项预热失败；即便不考虑 LeaFTL，其他 16 项也必须重跑。完整统计口径与限制见 `reproduction/results/fig14-16of20-NOTES.md`。

## 7. 公开 trace 与其他论文图的界限

无需 VPN 的下载方式：

```bash
bash reproduction/download_traces.sh
bash reproduction/download_traces.sh --systor
```

脚本通过 HTTPS 下载 UMass 的三条 WebSearch 原始档案，并校验 BZip2 与 SHA-256；`--systor` 另取 CMU 可独立读取的一段 Systor 2017 档案。下载记录在 `reproduction/traces/download.log`。三条 WebSearch 已按论文表格的请求数选择最繁忙连续窗口；运行窗口长度约 52.5、58.5、58.5 分钟，处在论文要求的 20 分钟到 2 小时内。转换时 WebSearch 的 LBA×512、Systor 原值按字节，超出盘容量的地址分组取模，必要时按 512 B 对齐；旁边的 JSON 保存转换说明。单条回放示例先读帮助：

```bash
python3 reproduction/trace_to_fio.py --help
python3 reproduction/run_trace.py --help
```

**图 21/22 目前不能标为严格复现**：论文没有公开 WebSearch 放大因子及确切窗口起点；现有 Systor 下载只是全集的一段，读比例与论文选段不同；论文能耗采用 NANDFlashSim，当前只有 FEMU 的计数/估值。图 19/20 的 RocksDB/Filebench 工作负载也尚未形成与图 14 同等完整的自动化与测量；不要用图 14 的结果代替。后续若补齐，必须分别记录数据库/文件系统版本、数据量、线程数、预处理、统计口径和完整原始日志。

## 8. 交付前检查清单

### LearnedFTL 写入异常的 GC 诊断

正式实验占用端口 2222 时，不要同时启动诊断虚拟机。等正式脚本退出，在 **ext4** 目录运行：

```bash
cd /home/zheng/FEMU-ext4
./femu-local-env.sh python3 reproduction/build_gc_diag.py
RUNTIME_SECONDS=60 WARMUP_PASSES=6 bash reproduction/run_gc_diag.sh
```

诊断会创建独立的 qcow2 镜像，先执行六轮顺序写预热，再执行 64 线程、4 KiB、错开起点的顺序写。输出位于 `reproduction/results/diagnostics/gc-<时间>-<进程号>/`；`method.json` 记录参数和退出码，`warmup.json`/`fio.json` 是原始 fio 结果，`qemu.log` 是完整日志，`gc-summary.log` 只提取计数和空间耗尽行。GC 计数器在测量前清零，于测量结束的 FEMU 管理命令中一次性输出：GC 总调用数、三条分支各自次数、最终返回 false 的次数（返回 false 前也可能完成回收）、line 分配与失败次数、成功回收数、用户写与 GC 写次数、最低和结束时的空闲/victim line 数。

只有 `warmup.json` 达到目标字节数、`method.json` 的两个退出码为 0，且 `gc-summary.log` 包含 `GC_DIAG`，才可当作有效诊断。 修复候选版通过 `./femu-local-env.sh python3 reproduction/build_gc_fixed.py` 构建，再以 `GC_DIAG_BINARY=qemu-learnedftl-paper-gcfix RUNTIME_SECONDS=180 bash reproduction/run_gc_diag.sh` 测试；该版**没有** `GC_DIAG` 行，以预热字节数、fio JSON 错误码、两个退出码和 QEMU 是否出现 `GC_EXHAUST`/`buduijin` 判定。若出现 `GC_EXHAUST` 或虚拟机崩溃，保留整个目录；该轮为故障证据，不能作为图 14 的性能结果。若没有出现耗尽，说明本次没有复现该故障，不能据此宣布分配问题已修复。诊断版二进制与正式论文参数版分开，诊断计数不会改变正式图的数据口径。


1. `--status` 的 20 项均为 `完成`。
2. 严格绘图命令成功，图片和 PDF 均存在；图中没有人为补的论文数字或缺失柱。
3. 保留 `paper_fig14-console.log`、每项 `method.json`/`warmup.json`/`run*.json`、`fio_runs.csv`、五个二进制的编译日志。
4. 报告论文与代码差异：容量单位、LeaFTL 翻译页、随机预热策略、60 秒测量和宿主 `mlock`。若 LeaFTL 仍崩溃，明确记为无法完成，不能称 20 项齐全。
5. 报告机器配置、源码提交、脚本版本、异常与补救记录。不要把早先的 `fig14-partial`、30 GiB 诊断数据混进正式图。

论文：[HPCA 2024 LearnedFTL](https://ranger.uta.edu/~jiang/publication/Conferences/2024/HPCA24(LearnedFTL).pdf)；作者代码：[astlxmu/LearnedFTL](https://github.com/astlxmu/LearnedFTL)；数据源：[UMass Storage Traces](https://traces.cs.umass.edu/docs/traces/storage/)、[CMU Systor](https://ftp.pdl.cmu.edu/pub/datasets/cacheDatasets/original/systor/)。


## 近期写入问题与新盘对照（2026-09-28）

顺序写不是只在六轮预热后慢：一轮写满 30518 MiB 逻辑盘后，LearnedFTL 64 任务仅约 8.4 IOPS。新盘连续写 60 秒时，正式参数二进制前 27 秒约 1.1 GiB/s；第 28 秒写入累计接近整盘容量，第 29 秒跌至约 0.96 MiB/s，后半段远低于其他算法。**60 秒平均值 482 MiB/s 会掩盖后段断崖。**诊断原始结果和解释在 `reproduction/WRITE_INITIAL_STATE_FINDINGS_2026-09-28.zh-CN.md`，每秒带宽在相应 `fio-bw-per-second.csv`。

论文写测试明确为 4 KiB、`psync`、64 线程，没有给出写前填充量和计时长度。若要核对“新盘直接写”的同条件表现，运行下面的**诊断对照**；每个算法、每次重复使用全新虚拟机，不覆盖六轮预热结果。默认五种算法各三次、每次 60 秒；这不是替论文补齐缺失方法细节，也不自动生成“正式图 14”。

```bash
cd /home/zheng/FEMU-ext4
FRESH_REPEATS=3 RUNTIME_SECONDS=60 bash reproduction/run_fresh_write_compare.sh
```

测试流程可先用 `FRESH_VARIANTS=ideal FRESH_REPEATS=1 RUNTIME_SECONDS=5` 冒烟，结果只能标为诊断。脚本输出 manifest.csv 的路径；每轮目录内 `method.json` 写明零预热、任务起点和退出码，`fio.json` 是原始测量，`fio-bw.log` 与 `fio-bw-per-second.csv` 是逐秒带宽。`complete` 且 `fio.json` 的 `error=0` 才有效；`failed` 要保留日志，不得补柱形。后半段持续速度、GC 写页/主机写页和空闲 line，比整段平均更能说明是否真正解决写入问题。

本轮还发现数据 GC 路径重复清空与计入擦除。独立修复版将擦除能耗计数大约减半，但顺序写仍约 8.5 IOPS；简单的按组无效页排序曾触发重复释放，经防止递归回收后能正常完成但仍约 8.4 IOPS。这些都是隔离诊断候选，不要把它们的数字写成论文原版。旧冷组计数器误用 `lr_nodes[].u`，该位初始化即为 1；以 v2 报告为准。六轮预热后，打开 line 合计只余 5120 个可追加页（20 MiB），唯一有空页的组部分模型已有有效记录；能否按论文条件借用尚须界定。

新盘连续 180 秒随机写的正式参数 LearnedFTL 单轮平均为 207.84 MiB/s，第 1/60/120/180 秒约为 707/203/50/62 MiB/s；数据见 `WRITE_INITIAL_STATE_FINDINGS_2026-09-28.zh-CN.md`。这仍未达到六轮随机写预热的 192 GB，不能用来取代近满盘随机写测试。若实习生观察到平均值较高，应同时查逐秒曲线与累计写入量。

### 物理页身份校验版

需要复核 LearnedFTL 内部映射时，在 ext4 目录运行：

```bash
cd /home/zheng/FEMU-ext4
GC_DIAG_FIX=1 GC_DIAG_PHYS_AUDIT=1 ./femu-local-env.sh python3 reproduction/build_gc_diag.py
GC_DIAG_BINARY=qemu-learnedftl-gc-phys-audit WARMUP_PASSES=0 RUNTIME_SECONDS=60 GC_FIO_JOBS=64 GC_FIO_PATTERN=write bash reproduction/run_gc_diag.sh
```

`PHYS_ID_SUMMARY` 的 `writes/copies/reads` 分别是主机写入、GC 搬迁、实际选中 PPA 的读取校验次数；`copies>0` 表示这轮确实经过数据页 GC。`PHYS_ID_MISMATCH`/`PHYS_ID_NO_SOURCE`、fio 非零退出、QEMU 崩溃都表示失败：不要继续绘图，保留完整诊断目录供定位。只有退出码为零且日志有 `PHYS_ID_SUMMARY` 和 `GC_DIAG` 才算诊断完成；即使通过，也只证明本轮被检查的路径，没有证明跨组借用已实现。

### 跨组借用诊断版（2026-09-28）

最新隔离实现、阈值选择、受控验证、64 任务结果与尚未满足的论文条件见 [`CROSS_GROUP_IMPLEMENTATION_STATUS_2026-09-28.zh-CN.md`](CROSS_GROUP_IMPLEMENTATION_STATUS_2026-09-28.zh-CN.md)。受控单热组和双热组测试确实触发借用、热冷组联合 GC，读回及物理页身份检查通过。**64 任务顺序写与随机写本次借用均为零；顺序写第 60 秒仍只有约 0.335 MiB/s。不要把诊断版标为已修复或将其数据绘入正式图 14。**

构建诊断版并复查受控双热组路径：

```bash
cd /home/zheng/FEMU-ext4
GC_DIAG_FIX=1 GC_DIAG_PHYS_AUDIT=1 GC_DIAG_FULL_GTD_GC=1 GC_DIAG_CROSS_BORROW=1 GC_DIAG_GREEDY_GROUP=1 GC_DIAG_GUARD_REENTRY=1 ./femu-local-env.sh python3 reproduction/build_gc_diag.py
GC_DIAG_BINARY=qemu-learnedftl-cross-borrow-greedy WARMUP_PASSES=0 GC_WARMUP_IO_MIB=16416 RUNTIME_SECONDS=10 GC_FIO_JOBS=2 GC_FIO_PATTERN=write GC_POST_READ=1 bash reproduction/run_gc_diag.sh
python3 reproduction/validate_cross_group.py reproduction/results/diagnostics/gc-<本轮目录名> --require-borrow --require-pair
```

正常结果应有 `fio=0, report=0`、`CG_BORROW success>0`、`paired_gc>0`、`PHYS_ID_SUMMARY copies>0`、三个 `post-*.json` 读回成功，校验脚本打印 `PASS`。出现 `GC_EXHAUST`、`PHYS_ID_MISMATCH`、`GC_GROUP_NO_TARGET` 或校验失败，保留本轮全部日志并停止使用该候选结果。该工作负载刻意构造冷热差异，用于正确性测试，不代表论文图 14 的 64 任务负载。

## 2026-09-29 写入修复原型：实习生核验入口

**状态：这是正确性诊断，不能生成论文图。** 最新源码、参数选择和每轮原始目录见 [`WRITE_REPAIR_PROGRESS_2026-09-29.zh-CN.md`](WRITE_REPAIR_PROGRESS_2026-09-29.zh-CN.md)。公开仓库分支为 `codex/write-repair-audit-20260929`。图 14 脚本当前的“完成”标记属于较早的二进制与方法，**不得**当成本原型的正式 64 任务写入结果。

只从 `/home/zheng/FEMU-ext4` 构建和运行；确认旧 NTFS 保持只读，且 2222 端口无 FEMU 占用。一次只跑一轮；每轮脚本创建新的 COW 客机盘。以下命令会向该**临时测试盘**写数据，不会写宿主机其他块设备：

```bash
cd /home/zheng/FEMU-ext4
GC_DIAG_CROSS_BORROW=1 GC_DIAG_GREEDY_GROUP=1 GC_DIAG_FULL_GTD_GC=1 GC_DIAG_PHYS_AUDIT=1 GC_DIAG_FIX=1 ./femu-local-env.sh python3 reproduction/build_gc_diag.py
CG_ASSERT_MODEL_BITMAP=1 CG_BORROW_ALLOW_MULTI=1 CG_BORROW_DONOR_LIMIT=2 CG_BORROW_PAIR_TRIGGER=2048 GC_DIAG_BINARY=qemu-learnedftl-cross-borrow-greedy GC_PREPARE_SPARSE_GROUPS=130 GC_FIO_JOBS=2 GC_MEASURE_SIZE_MIB=128 GC_FIO_PATTERN=write WARMUP_PASSES=0 RUNTIME_SECONDS=55 GC_POST_READ=1 bash reproduction/run_gc_diag.sh
```

记下终端的 `Output:` 路径，再执行 `python3 reproduction/validate_cross_group.py <Output目录> --require-borrow --require-pair --min-component-groups 7 --require-shared-line`。**正常的诊断结果**是 fio/report 退出码都为 0，验证器输出 `PASS`，`qemu.log` 有 `CG_SHARED_LINE members>=3`、`CG_COPY_DONE`、随后 `CG_ERASE_DONE`、`PHYS_ID_SUMMARY` 和 `MODEL_BITMAP_AUDIT wrong=0`。结果目录的 `method.json` 保存实际 fio、借用和诊断参数及退出码，`manifest.json` 保存源码、补丁、二进制等 SHA-256；`fio.json`、读回 JSON 和 `qemu.log` 是原始证据。`CG_STAGE_BUDGET` 报告本轮暂存 LPN 的数组内容与索引字节数，当前诊断上限 16 MiB，**不是论文给出的参数或完整进程 RSS**。

**异常处理：**出现 `PHYS_ID_MISMATCH`、`CG_PENDING_COPY`、`CG_SOURCE_STILL_VALID`、`CG_STAGE_LIMIT`、`GC_INVARIANT`、模型 `wrong>0`，或验证器未输出 `PASS`，保留整个目录并停止使用该版本作性能图；不要删失败日志或用旧轮结果填补。若只有 `fio=0` 但没有借用、复制或实际模型尝试，说明目标路径没被覆盖，也不能判正确性通过。遇到进程退出，先看 `method.json` 的退出码和日志最后一条事件；进程不存在本身不能说明成功。

空间拒绝故障用例另用 `CG_TEST_BUDGET_FREE=0` 注入**筛查值**；它会故意让 QEMU/fio 失败。此时只能用 `python3 reproduction/validate_cross_group.py <Output目录> --expect-budget-reject` 判断是否按预期在回收前拒绝。`actual_free` 仍是真实数值，不得把注入值描述成 SSD 实际空闲量。这个用例及上述 2,048 页触发值、候选组限制、单/双任务、运行时长均为诊断选择；论文写测试的 64 任务条件尚待正确性门槛通过后另行执行。

### 64 任务短筛查的带宽口径与当前结论（2026-09-29）

要分析 `GC_BW_LOG=1` 产生的 `fio-bw.log`，运行 `python3 reproduction/summarize_gc_bw.py <Output目录>`。脚本把同一秒所有作业的 KiB/s **相加**，输出 `fio-bw-per-second.csv` 与 `fio-bw-summary.json`，并核对日志整轮均值和 `fio.json` 相差小于 5%。**不要把日志最后一行当成设备总带宽**：64 作业通常每秒约有 64 行。随机写有些秒样本行数不足 64，查看摘要中的 `min_job_samples_in_second`，末段数值作为诊断近似值，整轮平均以 `fio.json` 为准。

当前一轮 40 秒的新盘短筛查表明：默认门槛下顺序写末 10 秒约 6.94 MiB/s；随机写末 10 秒约 251.18 MiB/s。随机写即使仍有合格冷组，借用也被 `free<=32` 门槛挡住。`CG_ALLOW_LOW_FREE=1` 放行后确有借用，但分散借用导致没有联合 GC；再加 `CG_BORROW_STICKY=1` 才在同轮触发 62 次联合 GC，末 10 秒约 343.17 MiB/s。**这些开关和结果只用于诊断，非论文公开参数，不是正式论文图。**每种配置只有一次独立启动，不能当作稳定性能结论。若实习生复跑，应保存 `method.json`、`manifest.json`、`fio.json`、`fio-bw.log`、两个派生带宽文件以及 `qemu.log`；比较时必须确认开关、fio 地址布局、初始盘面和实际二进制哈希一致。

目标分配故障用例可用 `CG_TEST_FAIL_TARGET_AT=1`，预期 fio/QEMU **失败**且 `qemu.log` 出现 `CG_TARGET_ALLOC_INJECT`，但没有 `CG_COPY_DONE`、`CG_ERASE_DONE`；用 `python3 reproduction/validate_cross_group.py <Output目录> --expect-target-fail` 验证。它是故障注入，不得作为性能结果。实际低空闲（235 稀疏组）短测的 `free=20` 与注入的预算值是两回事；看 `CG_DONOR_POLICY` 的 `eligible` 和 `gate_open` 才能判断“有冷组却被门槛挡住”。


### 随机写三次独立短测（诊断专用）

在 ext4 目录运行 `bash reproduction/run_write_short_matrix.sh`。脚本自动使用六个全新 FEMU 客机盘，分别对默认策略与诊断性的低空闲借用加固定 donor 策略各运行三次。每轮是 64 作业、4 KiB psync 随机写、40 秒、空盘起始，不含论文要求的读预热；结束后输出 `matrix.csv`、二进制 SHA-256、每轮方法记录、fio 原始 JSON、逐秒日志、QEMU 日志与验证结果。若任一轮非零退出、验证失败、二进制哈希改变或启用借用的轮次没有联合 GC，应判为**异常或未覆盖机制**，不能挑其余成功轮次直接作论文结果。

2026-09-29 的六轮实际记录见 `WRITE_REPAIR_PROGRESS_2026-09-29.zh-CN.md` 第五轮。该对照的低空闲放行与固定 donor 都是论文未公开的实现选择，**仅用于定位代码瓶颈**。这些轮次没有主机读回与六轮预热，物理身份审计只覆盖被访问/搬迁的路径；通过不能替代正式正确性或图 14 验收。


### 120 秒长测的正常与异常判断

2026-09-29 已做一对空盘随机写 120 秒对照，条件、原始目录和数值见进度记录第六轮。**程序正常退出不等于性能恢复**：借用版虽完成 195 次联合 GC，末段仍降至约 133 MiB/s。看 `fio.json` 的整轮平均，同时用 `fio-bw-per-second.csv` 看阶段趋势；逐秒记录只有 `job_samples=64` 才能按完整秒作比较。若出现样本不足，保留原始日志、明确排除该秒，不要静默填补。另查 `qemu.log` 的 `CG_BORROW`、`CG_COMPONENT_GC`、`PHYS_ID_SUMMARY`、`MODEL_BITMAP_AUDIT`，以及 `method.json` 的所有诊断开关。GC 搬迁页数/主机写页只能称为数据页搬迁比，**不能称为完整 WAF**。长测对照仍是诊断用途，不能替代六轮预热及正式论文图。


### 顺序写仍需解决

同一诊断借用策略在 40 秒顺序写中没有借用成功，末十秒约 4 MiB/s，详情见进度记录第七轮。日志中有未训练组，但单组剩余页数不足当前 8,192 页借用资格；不能把“有组”误读为“能借”。若 `MODEL_BITMAP_AUDIT attempts=0`，严格模型验证会失败，这表示该轮**未覆盖模型预测路径**，即使 fio 和读回均无错误，也不能宣称模型检查通过。不要把这轮结果写进论文图。


### 2026-09-29 顺序写受控对照与下一步

旧全局 `filesize=30518m` 不能保证每任务 476 MiB 区域。更正后，64×476 MiB、40 秒的顺序写末十秒仍约 7.64 MiB/s，借用 0；同一诊断二进制和多组借用开关下，64×128 MiB 一轮触发 431,416 页借用、47 次联合 GC，末段日志约 313.87 MiB/s，但有缺失秒与不完整样本。受限一对一版本在活跃 donor 同时写入时主动终止，不能算性能结果。原始目录、正确性检查及方法差异见进度记录第八轮和 [fio 地址范围核验](FIO_ADDRESS_LAYOUT_2026-09-29.zh-CN.md)。**这些是新盘短时诊断，不是论文图 14。**

正式续跑先用 `bash reproduction/run_paper_fig14.sh --status` 核对旧结果当前为 18 项未完成、2 项失败。执行 `--resume` 时脚本会先将未完成项的旧原始目录移动到 `results/paper_fig14-archive/时间戳/`，再从头预热和测量。每轮必须保存 `runN-jobfile.fio`、`runN-console.json`、原始 fio JSON 和 `method.json`；缺任务文件、哈希不符或版本不是 `explicit-per-job-end-v1` 均判未完成。脚本现在能生成正确地址，不意味着 LearnedFTL 和 LeaFTL 的写入问题已经修复。

### 共享 line 长测防错结论（2026-09-29）

旧诊断二进制的一轮 120 秒顺序写出现 `buduijin`（line 剩余页已小于 0）及 victim 队列计数失配，fio=255，必须判为失败并保留原始日志。修正共享 line 封闭状态与写入前空间检查后，同条件三次独立 120 秒短工作集复测通过，详情见进度记录第九轮。若新实验再出现 `CG_NO_REST`、`GC_INVARIANT`、`PHYS_ID_MISMATCH`、fio 非零退出或读回失败，应立即判失败，保存 `qemu.log`、`fio.json`、`manifest.json` 和任务文件，不得用其他成功轮次补位。三轮成功的 `MODEL_BITMAP_AUDIT attempts=0` 代表模型路径未覆盖，不能填作模型正确性证据；128 MiB 地址范围也不能冒充论文参数。

新构建另有一轮 40 秒、测后读回的模型路径补测：22,719 次 bitmap 预测全部匹配，错误 0；原始目录见进度记录“新构建的模型路径补测”。它不能替代三轮 120 秒测试末态（那三轮模型尝试为 0），也不能替代六盘预热后的验证。

### 顺序写 GC 空间为何塌陷（2026-09-30）

按 [GC 空间证据与计算方法](GC_SPACE_CAUSAL_2026-09-30.zh-CN.md) 核对最新显式地址范围诊断：64 个任务各 476 MiB 时，每归还一条 32,768 页 line，约 32,762 页需要搬迁，结束时 238 条组 line 打开、只有 17 条全局 free line、0 个合格 donor。64×128 MiB 三轮的每条 line 搬迁约 7,547–8,394 页，但使用了论文未公开的固定 donor 等策略。**这是诊断差异，不是正式图 14 的性能对比。**

实习生在一轮结束后运行 `python3 reproduction/summarize_gc_space.py <本轮结果目录>`，保存 `gc-space-summary.json`，并与 `fio.json`、`method.json`、`qemu.log` 一起归档。正常：fio/report 为 0、脚本成功、`data_pages_copied` 与 `gc_writes` 一致。异常：脚本报计数缺失/不一致、fio 失败、`PHYS_ID_MISMATCH` 或 `GC_INVARIANT`；保留原目录，不得将该轮作性能结果。`uncopied_slots` 含未写位置，并非净空闲页；`data_pages_copied` 也不是完整 WAF。

同构建、同 476 MiB 范围的 40 秒补测显示随机写的数据页 GC 搬迁为 0、末十秒约 662.98 MiB/s；顺序写搬迁 2,587,885 页、末十秒约 1.72 MiB/s。**不能只比较整轮平均**，顺序写前十秒约 1,022 MiB/s 会掩盖后段。随机写还未经历数据页搬迁 GC，不能判为长期通过。原始目录见上述 GC 空间证据文档。

### 随机写 120 秒为何失败（2026-09-30）

见[长测原始证据与复测门槛](RANDOM_WRITE_LONG_GC_2026-09-30.zh-CN.md)。首次长测因诊断缓冲 16 MiB 上限提前中止；提高到 128 MiB 后，两次新盘实验分别形成 120 组或 144 组联合回收组件，目标 line 预算 239/287，实际 free=16，均由 `CG_BUDGET_REJECT` 在搬迁前停止。**fio/report=255 是异常结果，不能用失败前速度作论文数据。**实习生遇到此标记时保存完整结果目录，核对 `method.json`、`manifest.json` 和 `qemu.log`，不要删除预算保护或把它当作内核/宿主盘崩溃。128 MiB 是诊断程序的宿主暂存安全上限，不是论文 SSD 的缓存参数。

`CG_BORROW_MAX_ACTIVE=1` 是新增的**诊断开关**，限制每个 donor 同时服务一个热组；默认 256。它在两轮 120 秒随机写中都未使实验完成，一轮默认 8,192 页、一轮 2,048 页触发，最终均为 `GC_EXHAUST free=0` 与 `CG_NO_REST`。不要把下调触发值写成修复成功，也不要以失败轮的前半段吞吐量填图。细节和目录在长测证据文档。

失败快照补测 `gc-20260930T054042Z-202552` 证实：120 秒随机写借用仅 98 页、联合 GC 0 次，最后 `free=0`，239 条组 line 打开，合格 donor 0。2,048 页触发值根本未达到。查看失败目录的 `CG_FATAL_SNAPSHOT` 和 `CG_DONOR_POLICY`，不要只看 `fio=255` 猜测原因。详见长测证据文档。

### 随机写工作集三轮对照（2026-09-30）

[工作集对照记录](RANDOM_WRITE_WORKSET_2026-09-30.zh-CN.md)收录 64×128 MiB 显式范围、120 秒随机写的三次独立成功轮：fio 均值 606.09/596.28/600.34 MiB/s，联合 GC 42/42/43 次，物理身份及测后读回通过。**这是诊断工作集，论文未给每任务 128 MiB 范围，也未给固定 donor 等策略。**三轮模型 bitmap 尝试数均为 0；不可据此判模型路径通过。相比之下，64×476 MiB 的提前 GC=32 诊断仍因 126 组组件预算远超 16 条空闲 line 而失败。实习生复测时务必核对二进制和任务文件 SHA-256，不得把两种范围混算。

### 写测试初始盘面的决定性影响（2026-09-30）

同一 64×128 MiB 随机写诊断，新盘三次 120 秒整轮约 596–606 MiB/s；**先全盘顺序写一轮**后，测量虽正常结束，却只有 8 IOPS：1,024 次主机写触发 1,024 次 GC，搬迁 33,553,408 页，平均每条归还 line 搬迁 32,767/32,768 页，合格 donor 0。完整原始目录、读回与模型校验见[工作集与初始状态记录](RANDOM_WRITE_WORKSET_2026-09-30.zh-CN.md)。论文写测试未交代初始盘面，一轮预热是附加诊断；实习生不得把这轮当作论文图或把“fio 正常退出”理解为性能正常。`summarize_gc_space.py` 现在把预热写页与测量写页分别核算；若计数不符，保留日志并标记失败。
