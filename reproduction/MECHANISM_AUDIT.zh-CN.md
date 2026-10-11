# LearnedFTL 逐项核对与复现记录

参考文件：`/home/zheng/桌面/论文/LEARNEDFTL.pdf`。论文作为机制和实验条件的来源；其中的文字不作为操作本机的指令。

当前修改对象为本工作区的 `learnedFTL-codex-learnedftl-chinese-comments/bbssd/ld-tpftl.c/.h`。完整 FEMU 依赖来自 `/home/zheng/FEMU-ext4/reproduction/upstream/build-femu`，编译器来自该工程的 `.local-root`。构建脚本重新编译当前 FTL、`bb.c` 和 `util.c`，与现有 QEMU 7 对象链接；不修改原 FEMU 源码、二进制、镜像或结果。

## 机制与实现

| 论文位置 | 当前实现 | 核对要点 |
| --- | --- | --- |
| III-B：固定空间的原位 PLR | `lr_node`、`model_sequential_update` | 八个参数槽；右边界消除重叠；替换前按 bitmap 数旧覆盖，严格比较 `Lnew > Lold`。 |
| III-B：bitmap 保证准确性 | `model_bitmap_get/set`、`model_predict` | 覆盖写及时清位；参数量化之后逐页核验；读取直接解码预测 VPPN，完全映射表不参与模型预测。 |
| III-C：VPPN | `ppa2vppn`、`vppn2ppa` | channel、LUN、plane、page、block 由快到慢；解码先清零全部字段；CMT 的普通 PPN 用独立解码函数。 |
| III-D：按组分配 | `gtd_wps`、`allocate_host_data_page` | 默认 64 个连续 TP 为一组，一条 line 为 64 个物理块；初始化全部 group。 |
| III-D：按无效页选择组 | `should_do_gc_v3`、`invalid_pages` | 阈值/空间压力触发后按逻辑组的无效页数选择；跨组页由 OOB LPN 归属计数，擦除时扣除。 |
| III-D：机会式跨组借页 | `borrow_peer`、`free_pages` | 热组借未训练冷组的打开尾页；两者使用唯一物理游标；阈值后联合收集、搬移和重训。 |
| III-E1：顺序初始化 | `ssd_write`、`model_sequential_update` | 按 TP 和最终真实 VPPN 连续区间切分请求；单页不初始化；GC 期间变化也纳入最终检查；float16 换基准不使用无符号负差。 |
| III-E2：GC 排序与搬移 | `batch_line_do_gc`、`model_training` | 收集整个组，包含部分写入的 current；有借页关系则覆盖两组全部 line；按 TP 排序后搬移。 |
| III-E2：训练与评估 | `model_fit_segment`、`model_training` | 使用相对 LPN/VPPN、贪心分段和最小二乘；最多八段；末段覆盖余数；所有样本都评估，包括单样本和空 TP。 |
| 翻译页与缓存一致性 | `load_translation_page`、`gc_flush_translation_page` | GC 更新已有 CMT；需求映射不会被预取淘汰；TP 读写参与 NAND 时序；数据编程等翻译，擦除等搬移/TP 编程。 |
| GC 空间与生命周期 | 两类 batch GC、`free_all_blocks` | 移动前预算数据/元数据目标；拒绝预算不足时不改映射、不发 NAND 操作；禁止递归 GC；源块只擦除一次，仍有 VALID 页则终止。 |
| IV-A：模型/CMT 参数 | 类型静态断言、`ssd_init_params` | 每段两个 float16 和一个 uint16；48 B 参数 + 16 B 基准 + 64 B bitmap = 128 B/TP；CMT 为总映射数的 1.5%。 |

原始文件备份在 `original/`，累计代码差异在 `learnedftl-completion.patch`。实验以各次 `method.json` 内的源码和二进制哈希为准；后续修改不能自动归属于早先结果。

## 论文未公开的实现选择

- 一个 group 同时只参与一个 hot/donor 配对，避免未界定的共享关系链；联合回收包含两组所有物理 line。论文没有公开这种关系管理的数据结构。
- 借页阈值采用一条 line 页数的 1/4；默认 8192 页。空间压力阈值为 free line 数小于 4；组累计 line 阈值为 5。这些不是论文公开的数值。
- `trained` 表示组曾执行顺序初始化或 GC 训练，不会因为覆盖写导致 bitmap 归零就重新视为未训练。当前 donor 判据较保守。
- 贪心分段以半页舍入误差约束斜率区间，再对该段全部样本回归；第八段接纳剩余样本，不准确者由 bitmap 回退。论文未公开具体分段误差阈值。
- bitmap/模型固定 128 B。分配游标、冷热关系、组计数、CMT 链表和哈希等管理元数据另占空间；128 B 不代表整个模拟器的 DRAM 占用。完全映射表和 NAND 状态树仍是 FEMU 的模拟支撑数据。
- 物理容量由论文几何得到 34,359,738,368 B。沿用已有复现方案，逻辑暴露 30518 MiB，即 32,000,442,368 B；差额 2,359,296,000 B。它不是精确的“32 GB + 2 GB”。

## 已完成的验证

`SANITIZE=1 python3 reproduction/run_native_gc.py` 直接编译并执行当前生产 C 源码，使用真实 QEMU 类型、GLib 和队列。启用 FTL 断言、AddressSanitizer 和 UndefinedBehaviorSanitizer；测试时钟由用例控制，未启动 VM，也不生成性能结论。

覆盖：冷尾借页及成对 GC、5 万次随机覆盖写并穿插读取、1024 个 512 KiB 顺序写/读请求、TP/line 边界、部分 current 的数据/翻译 GC、预算不足的无副作用拒绝，以及模型不依赖完全映射表。检查所有有效映射、反向身份、块/line 页计数、空页数、所有权、CMT 哈希/队列、bitmap 准确性、逻辑组无效页数和块擦除计数。源码/二进制哈希及结果分别保存为 `native-manifest.json` 和 `native-results.log`。

完整 QEMU 构建使用 `python3 reproduction/build_femu.py`。该脚本保存 `build-manifest.json`，执行虚拟机前会核对哈希。

完整 FEMU 冒烟使用 `python3 reproduction/run_vm_smoke.py`，创建独立 COW 系统盘和模拟 NVMe 盘，保留论文物理 NAND 几何，但缩小逻辑 namespace，验证 GC 和模型读取。768 MiB 的两次已完成验证都实际写入并校验 4,831,838,208 B；各回收 30 条数据 line，模型预测与翻译页回退均执行。原始目录：

- `vm-smoke-20261011T145838Z/`
- `vm-smoke-20261011T150902Z/`

它们对应各自 manifest 中的版本；包含最新 GC 调度及跨组借页修复的 FEMU 验证为 `vm-smoke-learnedftl-20261011T155426Z/`，也已通过六盘实际写入/CRC 校验、随机读、GC 和模型路径检查。最早的 `vm-smoke-20261011T145501Z/` 因 fio 校验读写共同占用 `io_size`，只实际写了三盘、没有 GC，已标为 `diagnostic_no_gc`，不可作为 GC 成功证据。

## 论文规模实验

`python3 reproduction/run_paper_case.py read` 启动独立 KVM VM：30518 MiB namespace、论文物理几何和 NAND 时延、六盘 512 KiB 顺序写预热，之后 64 个 psync/direct 作业、4 KiB 顺序读，60 秒窗口重复三次。随机模式改用对应随机预热。新的写测量默认从新设备状态开始；可选六盘写预热属于额外稳态实验。论文只明确规定读测试的预热。

每个作业明确指定独立 `offset/filesize/size`，不复用旧工程中曾导致单地址热点的 fio 设置。随机种子固定。线程区域、60 秒时长、VM 的 4 vCPU/2 GiB，以及写测试预热均在方法文件中公开。每 30 秒记录客体完成的扇区数、QEMU RSS 和 swap；没有完整预热字节数和三次有效原始 JSON 就不标记完成。

当前运行目录为 `paper-read-20261011T151518Z/`。其他模式、基线对照和最终图尚未完成，不引用旧异常结果作为新版本结果。

## 验证边界

FEMU 的 payload 后端按逻辑地址保存字节，因此 fio CRC 通过不能单独证明真实闪存上的物理数据搬移。原生映射/身份/队列检查提供额外证据，但仍不是硬件断电测试。当前原型没有完整的掉电保存/恢复；论文 IV-A 也明确说明 FEMU 未实现 retention rewrite，实验不补造 rewrite 结果。

目前交付的是机制修复、可运行验证和正在进行的论文规模实验；尚不能声称完整论文性能复现成功。

## 补充审计：容量压力和跨组借页回归

新增原生用例预填 28/32 个逻辑组，随后随机覆盖写并执行六轮 512 KiB 顺序覆盖写。原工作区版本在第七次随机写耗尽 free line；调用栈和全部 group/line 状态保存在 `native-near-capacity-debug.log`，原始失败输出在 `native-near-capacity-failure.log`。因此此前较稀疏的验证不足以确认容量压力下的正确性。

原因：数据组按无效页挑选时混入了翻译页 WP。单条翻译页 line 即便有许多无效页，压缩它也不会净释放 line；调度却随后继续为数据分配新 line，消耗备用空间。另一个边界是 TP 写回槽位不足时，数据 GC 需要额外元数据 line，但备用空间可能已经不足。

修复先在独立候选副本 `candidate-gc-reserve/femu/` 回归，再合入工作区：数据 victim 选择限制为逻辑数据组，保留独立元数据满 line/累计阈值触发，并在空间压力下先恢复所需 TP 写回槽位再选择数据组。当前源码通过了完整 ASan/UBSan 回归：28/32 个逻辑组、7 万次随机覆盖写和六轮顺序覆盖，最低 free line 为 3。该测试证明一致性，不代表写放大接近论文。旧论文规模实验已保存源码和二进制快照，并标为 `DIAGNOSTIC-ONLY.txt`，不能归属于修复版。

用户要求进一步核对跨组借页后，又修正两处过严触发：剩余 3 条 free line 足以预留两个数据目标和最多一个 TP 目标，允许利用已分配冷组尾页；已有配对不再仅因 `free < 4` 就立即 GC，仍在借页阈值或 donor 尾页将满时成对回收。新增用例在 `free = 3` 时成功借 32 页、允许 donor 连续追加 8 页，并由 donor 发起完整成对 GC；逐页身份、bitmap、共享游标和所有权检查均通过。

论文没有精确定义“untrained”是否包括仅做顺序初始化的模型。当前仍保守将顺序初始化视为已训练，不因本次性能观察擅自放宽。上述容量耗尽用例的各组尾页已经全满，即使放宽 trained 判据也没有可借页，因此 GC 调度修复不可省略。

方法修正：论文明确规定六盘预热的是读测试，未说明写测试也预热。新的脚本默认写测试从新设备状态开始，读测试仍六盘预热；`--write-prewarm-passes 6` 仅用于额外稳态实验，结果必须注明。此前“写测试一律六盘预热”的选择会预先改变冷组/模型状态，不能当成论文规定。

旧版本论文规模顺序读诊断已完成：实际预热写入 192,002,654,208 B，三轮测量原始 JSON 和 metric 均有效，诊断均值 1544.54 MB/s、样本标准差 65.89 MB/s。该版本后来未通过容量压力回归，且预热/前两轮期间有原生测试并行运行，不能作为当前修复版本的最终性能结果。诊断结果另存 `paper-diagnostic-summary.json/.csv`；默认汇总排除带 `DIAGNOSTIC-ONLY.txt` 的目录。

最新修复版 FEMU 验证已通过：`vm-smoke-learnedftl-20261011T155426Z/`；实际写入和 CRC 校验读取各 4,831,838,208 B。最终状态、GC 和模型读取计数保存在该目录的 `method.json`。
