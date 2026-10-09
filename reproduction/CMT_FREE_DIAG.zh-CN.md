# 密集写入时观察 free_cmt_entry_list

## 目的与含义

`ld-tpftl.c` 是本项目 LearnedFTL 的实现文件。`free_cmt_entry_list` 是空闲 **CMT 映射缓存槽位**队列；它不是 NAND 空闲页队列，也不是 `free_line_list`。

缓存尚未装满时，插入映射会从队首取一个槽位，空闲数量减少。缓存满后，淘汰策略把一个槽位清空并放到队尾，然后新的映射通常立即取走它。因此定时采样可能总看到 `free=0`，实际却在持续淘汰、复用。`free=0` 本身不是故障，也不能据此判断 GC 失败。

这里为每个槽位使用稳定编号：`slot_id = entry - cm->cmt_entries`。编号不是 LPN、PPN 或物理内存地址。队列顺序由 `queue_position` 表示。被归还的槽位已重置，所以其 LPN 和 PPN 通常都是 `18446744073709551615`（uint64 全 1，表示无效/未映射），`dirty=0`、`prefetch=0`、`next_avail_time=0`。这个值不是一个实际页号。淘汰前的旧 LPN 不在本次空闲链表日志中。

## 运行方法

本机只在 ext4 目录运行，先确认没有其他 FEMU 实验占用 2222 端口：

```bash
cd /home/zheng/FEMU-ext4
bash reproduction/run_cmt_free_diag.sh
```

脚本编译独立诊断二进制，然后依次启动两台全新 FEMU，分别做顺序写 `write` 和随机写 `randwrite`。默认每轮测量 20 秒，虚拟机启动、日志输出和验证额外耗时。需要更长观察时：

```bash
CMT_SECONDS=60 bash reproduction/run_cmt_free_diag.sh
```

每轮固定条件：

- 64 作业，`4k`、`psync`、`direct=1`，每作业一个在途请求。
- 每作业 128 MiB 区域，总活动区域 8 GiB；相邻起点相隔 476 MiB + 4 KiB，各任务区域互不重叠。每任务显式 `offset`、`filesize=offset+size`，保存实际 fio 任务文件。
- `time_based=1`、`randrepeat=0`。随机写使用 fio 默认随机地址生成/随机映射行为，确切 fio 版本与参数见原始 JSON。顺序写写到区域末尾后从区域起点继续。
- 不做预热，分别从全新 FEMU 状态开始。COW 系统镜像继承客机操作系统，FEMU 模拟盘状态在每次启动重新创建。
- 暴露盘大小 `30518 MiB`；CMT 使用当前复现构建的物理页数 × 1.5%（整数取整），实际容量写进快照。NAND 时延与其他论文构建参数沿用 `build_paper.py`。
- 使用 `GC_DIAG_FIX=1` 的安全修正和原有 GC 诊断；不启用跨组借用原型。本次只添加观察，不改变 CMT 的分配、淘汰、预取策略。

这些短时长、新盘、128 MiB 区域、起点布局与采样方式是**诊断选择**，不是论文全部公开规定。存在日志采样开销，本次吞吐量不可直接填进论文图，也不能据此声称长时间稳态或六轮预热后验证通过。

## 日志采集方式

FTL 线程在以下边界把链表内容复制到有上限的内存缓冲区，不逐 IO 打印：

1. CMT 初始化，以及 fio 开始前复位计数器。
2. 分配次数首次达到 1、64、1024、8192、65536，以及第一次变空。
3. 最初 8 次淘汰归还，以及对应的新映射分配；捕获容易漏掉的 `0→1→0`。
4. 每完成 1024 个主机写页检查时间，距上次定时采样约 1 秒时捕获一次；这是近似间隔，不是严格的每秒定时器。
5. fio 停止后，由现有 FEMU 管理命令触发最后快照，一次性输出 CSV。

最多 256 份快照、96 MiB 条目缓冲，另有少量元数据与校验临时内存。超过上限会记录 `dropped>0`，脚本判为不完整；不会静默当作完整结果。限制较长实验的持续时间或分批运行后重试。这里只保存抽样时刻的完整链表，**不是保存每一次变化**。

`allocations` 统计所有映射插入（包括预取），不等于主机写请求数或单纯的 CMT miss；`returns` 统计归还的槽位数。`allocations`、`returns`、`host_write_pages` 从每轮测量前清零；`phase=0` 是初始化，`phase=1` 通常是测量。`elapsed_ns` 从 FEMU 的 CMT 初始化时刻算起，Markdown 摘要另换算成相对测量前快照的秒数。初始化阶段与测量阶段不能把计数直接相减。

## 在哪里查看

```bash
cat reproduction/results/diagnostics/cmt-free-latest.txt
```

它给出本批次目录，其中：

| 文件 | 内容 |
|---|---|
| `STATUS.txt` | `RUNNING` / `COMPLETE` / `FAILED exit=...` |
| `write-console.log`、`randwrite-console.log` | 两轮实际结果目录、启动/测量/退出信息 |
| `write-summary.json`、`randwrite-summary.json` | 通过验证后的关键计数、吞吐量、IOPS |

每轮实际结果目录保存：

| 文件 | 内容 |
|---|---|
| `cmt-free-snapshots.csv` | 每次采样的阶段、原因、时间、完成写页、分配/归还次数、容量、free/used、实际遍历数量 |
| `cmt-free-entries.csv` | 每次采样的**全部空闲槽位**：队列位置、槽位编号、LPN/PPN、dirty、prefetch、next_avail_time |
| `CMT_FREE_CONTENTS.zh-CN.md` | 人可读的全部快照摘要，展示队首最多 12 个槽位；完整内容仍在 entries CSV |
| `cmt-free-summary.json` | 计数与数据完整性验证结果 |
| `fio.json`、`fio.err`、`measure.fio` | 原始 fio 数据、错误、确切作业配置 |
| `fio-bw.log` | fio 约每秒写带宽日志；原始 fio 日志格式，单位见 fio 版本规范 |
| `qemu.log`、`gc-summary.log` | 原始 FEMU 日志、GC 计数及 CMT dump 完成信息 |
| `method.json`、`manifest.json` | 方法、返回码、源码/脚本/二进制哈希与文件系统 |

空链表的快照在 entries CSV 中没有行，需要结合 snapshots CSV 查看。不要把“没有条目行”误判为没有采样。

要打印特定快照的完整队列（把路径及编号替换为实际值）：

```bash
python3 - /完整结果目录/cmt-free-entries.csv 9 <<'PY'
import csv, sys
with open(sys.argv[1]) as f:
    for row in csv.DictReader(f):
        if row['snapshot_id'] == sys.argv[2]:
            print(row)
PY
```

## 完成、正常、异常及补救

**完成标准：批次 `STATUS.txt` 为 `COMPLETE`，两份 summary 的 `validation=PASS`。**仅出现 `Done`、fio 退出或链表变空都不够。校验要求：每份快照 free+used=capacity、实际条目数量等于 free、槽位合法且不重复、空闲字段已清空；最终计数满足 `end_free = start_free - allocations + returns`，完成主机写页 × 4096 与 fio 写入字节数一致；捕获到第一次变空及 8 对归还/取走。

| 现象 | 判断与补救 |
|---|---|
| 运行中尚无 CSV | 正常，缓冲到 fio 结束才 dump。观察控制台阶段与本轮 QEMU/fio 进程；不要仅凭文件没增长判断卡死。 |
| free 从容量降到 0，随后 0→1→0；定时快照持续 0 | 预期的满缓存槽位循环。看 allocations/returns 是否增加及 fio 是否继续完成 IO。 |
| entries 中 invalid LPN/PPN | 正常，槽位归还前已清空。 |
| `RUNNING` 但原脚本、QEMU/fio 均已消失 | 中断，不能算完成。保留日志，确认端口空闲后重跑整个短诊断；强杀/掉电可能来不及写 `FAILED`。 |
| 端口占用 | 等现有实验结束；不要杀不明进程。 |
| `CMT_FREE_INVARIANT` | 真正的队列/计数/字段异常，诊断立即停止。保留本轮 qemu.log 和 manifest，定位源码；不要绕过校验。 |
| `CMT_FREE_DUMP_ERROR` | 检查 ext4 写权限、剩余空间与路径；修复后重跑。 |
| `dropped>0` 或缺少完成标志/CSV | 不完整。缩短/拆分观察，或审查缓冲上限后重跑；不得手改完成标志。 |
| 未捕获第一次变空或 8 对复用 | 负载未覆盖目标路径，summary 拒绝通过。延长短实验后重试，仍需检查实际 fio 字节数。 |
| GC/分配故障、fio 错误、身份校验异常 | 整轮失败，保留全部记录；空闲 CMT 为零不能解释这些故障。 |

若只需重新验证已保存 CSV，可执行：

```bash
python3 reproduction/summarize_cmt_free.py /完整结果目录
```

这项验证覆盖 CMT 空闲队列和计数一致性，不等于 FTL 物理映射、模型预测、数据代次的完整正确性验证。

## 2026-10-09 实际运行结果

两轮均为默认 20 秒，64 作业，缓存容量 125,829。每轮 45 份快照，无丢弃，完整内容验证通过。

|负载|主机写页|CMT 分配|淘汰归还|最终 free|最终 used|
|---|---:|---:|---:|---:|---:|
|顺序写|5,558,823|5,565,170|5,439,341|0|125,829|
|随机写|3,791,578|4,024,360|3,898,531|0|125,829|

顺序写最初归还的槽位为 `36, 289, 511, 812, 853, 896, 938, 980`；随机写为 `11027, 22401, 26937, 30592, 31440, 38468, 46246, 14138`。每次归还后队列中暂时只有该槽位，随后的插入又把队列取空。这些是缓存槽位编号，不是连续或集中的数据地址。

在带采样开销的本轮记录中，从第一次 CMT 分配到第一次取空，顺序写约 0.458 秒、随机写约 0.657 秒；不可把这个间隔当成无仪器开销的硬件指标。测量前管理命令与 fio 真正提交之间另有启动间隔，不能把 before_fio 的时间直接当第一笔 IO 时刻。

本轮顺序/随机写在测后仍有 16/67 条空闲 NAND line，数据 GC 搬迁计数均为 0。因此已经充分触发了 CMT 淘汰，但没有覆盖数据 GC 的稳态搬迁。这进一步说明“CMT 空闲队列为零”不等于“无 NAND 空闲 line”，也不能由此宣布先前的写入 GC 问题已解决。

本机记录：

- 批次：`results/diagnostics/cmt-free-20261009T091419Z-43003/`。
- 顺序写：`results/diagnostics/gc-20261009T091419Z-43071/`。
- 随机写：`results/diagnostics/gc-20261009T091510Z-43303/`。
- 变化图：批次下 `cmt-free-changes.png`、`.pdf`。图只连结实际采样点，不表示记录了每一次变化。

本机批次目录的 `evidence-archive/` 还保存紧凑原始记录与 gzip 压缩的完整 entries CSV；解压后内容与原文件一致，校验见归档内 `SHA256SUMS.json`。原始日志包含本机路径等运行元数据，自动审批未允许公开上传，故原始数据保存在本机，GitHub 同步代码、脚本和本说明。
